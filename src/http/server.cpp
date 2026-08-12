// HTTP 層。docs/protocol.md §2 のエンドポイントを実装する。
#include "server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace haori {
namespace {

using nlohmann::json;

/// プロトコル §5 のエラー応答。
void send_error(httplib::Response& res, int status, const std::string& code,
                const std::string& message) {
    json j = {{"error", {{"code", code}, {"message", message}}}};
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

/// multipart のパートを取り出す。無ければ ProtocolError。
///
/// 注意: httplib::Request::get_file_value() は MultipartFormData を「値で」返すため、
/// 戻り値の .content を参照で受けると一時オブジェクトへのダングリング参照になる
/// (body_frames は数十 MB になるので値コピーも避けたい)。
/// req.files を直接引いて、保持されている実体への参照を返す。
const std::string& require_part(const httplib::Request& req, const char* name) {
    auto it = req.files.find(name);
    if (it == req.files.end()) {
        throw ProtocolError("missing_part",
                            std::string("multipart に '") + name + "' パートが無い");
    }
    return it->second.content;
}

void handle_submit(const httplib::Request& req, httplib::Response& res, JobQueue& queue) {
    JobInput input;
    input.manifest = parse_manifest(require_part(req, "manifest"));

    input.body_topology = decode_body_topology(require_part(req, "body_topology"), input.manifest);
    input.body_frames   = decode_body_frames(require_part(req, "body_frames"), input.manifest);
    decode_cloth_mesh(require_part(req, "cloth_mesh"), input.manifest, input.cloth_positions,
                      input.cloth_topology);

    const std::string id = queue.submit(std::move(input));

    res.status = 202;
    res.set_content(json{{"job_id", id}}.dump(), "application/json");
}

void handle_status(const httplib::Request& req, httplib::Response& res, JobQueue& queue) {
    const std::string id  = req.matches[1];
    JobPtr            job = queue.find(id);
    if (!job) {
        send_error(res, 404, "job_not_found", "ジョブが見つからない: " + id);
        return;
    }

    const JobSnapshot s = JobQueue::snapshot(*job);

    json j = {{"state", to_string(s.state)},
              {"progress", s.progress},
              {"frames_done", s.frames_done},
              {"message", s.message}};
    if (!s.error_code.empty()) j["code"] = s.error_code;

    res.set_content(j.dump(), "application/json");
}

void handle_result(const httplib::Request& req, httplib::Response& res, JobQueue& queue) {
    const std::string id  = req.matches[1];
    JobPtr            job = queue.find(id);
    if (!job) {
        send_error(res, 404, "job_not_found", "ジョブが見つからない: " + id);
        return;
    }

    const JobSnapshot s = JobQueue::snapshot(*job);
    if (s.state != JobState::Done) {
        send_error(res, 409, "job_not_done",
                   std::string("ジョブはまだ完了していない (state=") + to_string(s.state) + ")");
        return;
    }

    std::string payload;
    if (!queue.take_result(id, payload)) {
        send_error(res, 410, "result_discarded", "結果は既に破棄されている");
        return;
    }
    res.set_content(payload, "application/octet-stream");
}

void handle_cancel(const httplib::Request& req, httplib::Response& res, JobQueue& queue) {
    const std::string id = req.matches[1];
    if (!queue.find(id)) {
        send_error(res, 404, "job_not_found", "ジョブが見つからない: " + id);
        return;
    }
    const bool accepted = queue.cancel(id);
    res.set_content(json{{"cancelled", accepted}}.dump(), "application/json");
}

}  // namespace

int run_server(JobQueue& queue, const ServerOptions& options) {
    httplib::Server svr;

    // ボディのベイクは 120 フレーム × 数万頂点で数十 MB になる。既定の上限では足りない。
    svr.set_payload_max_length(2ull * 1024 * 1024 * 1024);

    // ハンドラ内で投げた ProtocolError をここで 4xx に落とす。
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                 std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const ProtocolError& e) {
            send_error(res, 400, e.code(), e.what());
        } catch (const std::exception& e) {
            spdlog::error("未処理の例外: {}", e.what());
            send_error(res, 500, "internal_error", e.what());
        } catch (...) {
            send_error(res, 500, "internal_error", "不明な内部エラー");
        }
    });

    svr.Get("/api/v1/health", [&](const httplib::Request&, httplib::Response& res) {
        json j = {{"status", "ok"},
                  {"engine", queue.engine_name()},
                  {"version", options.version},
                  {"gpu", options.gpu}};
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/api/v1/jobs",
             [&](const httplib::Request& req, httplib::Response& res) { handle_submit(req, res, queue); });

    // /result を先に登録する。httplib はパス全体に正規表現をかけるので取り違えは起きないが、
    // 意図を明示するために並び順も合わせておく。
    svr.Get(R"(/api/v1/jobs/([0-9a-fA-F]+)/result)",
            [&](const httplib::Request& req, httplib::Response& res) { handle_result(req, res, queue); });

    svr.Get(R"(/api/v1/jobs/([0-9a-fA-F]+))",
            [&](const httplib::Request& req, httplib::Response& res) { handle_status(req, res, queue); });

    svr.Delete(R"(/api/v1/jobs/([0-9a-fA-F]+))",
               [&](const httplib::Request& req, httplib::Response& res) { handle_cancel(req, res, queue); });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        spdlog::debug("{} {} -> {}", req.method, req.path, res.status);
    });

    spdlog::info("haori-server 起動: http://{}:{} (engine={})", options.host, options.port,
                 queue.engine_name());

    if (!svr.listen(options.host, options.port)) {
        spdlog::error("待ち受けに失敗した: {}:{} (ポートが使用中の可能性)", options.host,
                      options.port);
        return 1;
    }
    return 0;
}

}  // namespace haori
