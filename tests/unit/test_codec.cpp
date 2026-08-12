// codec と jobs のユニットテスト。
// 依存を増やしたくないので簡易なテストハーネスを自前で持つ。
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "codec/codec.h"
#include "jobs/job_queue.h"
#include "sim/simulator.h"

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cerr << "  FAIL " << file << ":" << line << "  " << expr << "\n";
    }
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

/// f が ProtocolError(code) を投げることを確認する。
template <typename F>
void check_throws(F&& f, const char* expected_code, const char* what, int line) {
    ++g_checks;
    try {
        f();
    } catch (const haori::ProtocolError& e) {
        if (e.code() != expected_code) {
            ++g_failures;
            std::cerr << "  FAIL " << __FILE__ << ":" << line << "  " << what
                      << " -> code=" << e.code() << " (期待 " << expected_code << ")\n";
        }
        return;
    } catch (...) {
    }
    ++g_failures;
    std::cerr << "  FAIL " << __FILE__ << ":" << line << "  " << what
              << " が ProtocolError(" << expected_code << ") を投げなかった\n";
}

#define CHECK_THROWS(expr, code) check_throws([&] { expr; }, code, #expr, __LINE__)

/// 最小の正常な manifest。テストごとに一部を差し替えて使う。
std::string minimal_manifest(int frame_start = 1, int frame_end = 2, int body_v = 3,
                             int body_t = 1, int cloth_v = 3, int cloth_t = 1) {
    return R"({"version":1,"fps":24.0,"frame_start":)" + std::to_string(frame_start) +
           R"(,"frame_end":)" + std::to_string(frame_end) + R"(,"substeps":2,
             "sim":{"gravity":[0,0,-9.8],"iterations":20,
                    "cloth":{"density":0.2,"stretch_stiffness":10000.0,"bend_stiffness":0.5,
                             "friction":0.3,"damping":0.01,"thickness":0.002},
                    "collision_margin":0.003,"warmup_frames":0},
             "body":{"num_vertices":)" + std::to_string(body_v) + R"(,"num_triangles":)" +
           std::to_string(body_t) + R"(},
             "cloth":{"num_vertices":)" + std::to_string(cloth_v) + R"(,"num_triangles":)" +
           std::to_string(cloth_t) + R"(,"pinned_vertices":[]}})";
}

std::string bytes_of(const std::vector<float>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float));
}
std::string bytes_of(const std::vector<std::uint32_t>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(std::uint32_t));
}

// ---------------------------------------------------------------------------

void test_manifest_defaults() {
    std::cout << "test_manifest_defaults\n";
    // sim を省略しても既定値が入ること (§3.1「未知キーは無視」/ 既定へのマップ)
    const std::string j =
        R"({"version":1,"body":{"num_vertices":3,"num_triangles":1},
            "cloth":{"num_vertices":3,"num_triangles":1}})";
    haori::Manifest m = haori::parse_manifest(j);

    CHECK(m.version == 1);
    CHECK(m.fps == 24.0);
    CHECK(m.substeps == 4);
    CHECK(m.sim.iterations == 20);
    CHECK(m.sim.warmup_frames == 10);
    CHECK(m.sim.gravity[2] == -9.8);
    CHECK(m.sim.cloth.thickness == 0.002);
    CHECK(m.num_frames() == 120);
}

void test_manifest_unknown_keys_ignored() {
    std::cout << "test_manifest_unknown_keys_ignored\n";
    const std::string j =
        R"({"version":1,"future_option":{"a":1},"fps":30.0,
            "sim":{"unknown_thing":true,"iterations":7},
            "body":{"num_vertices":3,"num_triangles":1},
            "cloth":{"num_vertices":3,"num_triangles":1}})";
    haori::Manifest m = haori::parse_manifest(j);
    CHECK(m.fps == 30.0);
    CHECK(m.sim.iterations == 7);
}

void test_manifest_validation() {
    std::cout << "test_manifest_validation\n";
    CHECK_THROWS(haori::parse_manifest("{ not json"), "manifest_parse_error");
    CHECK_THROWS(haori::parse_manifest(R"({"version":2})"), "unsupported_version");
    CHECK_THROWS(haori::parse_manifest(minimal_manifest(10, 5)), "invalid_frame_range");
    CHECK_THROWS(haori::parse_manifest(minimal_manifest(1, 2, 3, 1, 0, 0)), "empty_cloth");
    CHECK_THROWS(haori::parse_manifest(minimal_manifest(1, 2, 0, 0)), "empty_body");

    const std::string bad_pin =
        R"({"version":1,"body":{"num_vertices":3,"num_triangles":1},
            "cloth":{"num_vertices":3,"num_triangles":1,"pinned_vertices":[0,99]}})";
    CHECK_THROWS(haori::parse_manifest(bad_pin), "pinned_vertex_out_of_range");
}

void test_part_decoding() {
    std::cout << "test_part_decoding\n";
    haori::Manifest m = haori::parse_manifest(minimal_manifest());
    CHECK(m.num_frames() == 2);

    const std::vector<std::uint32_t> body_tri{0, 1, 2};
    auto decoded_tri = haori::decode_body_topology(bytes_of(body_tri), m);
    CHECK(decoded_tri == body_tri);

    // 2 フレーム × 3 頂点 × 3 成分 = 18 float
    std::vector<float> frames(18);
    for (std::size_t i = 0; i < frames.size(); ++i) frames[i] = static_cast<float>(i);
    auto decoded_frames = haori::decode_body_frames(bytes_of(frames), m);
    CHECK(decoded_frames == frames);

    // サイズ不一致は弾く
    CHECK_THROWS(haori::decode_body_frames(bytes_of(std::vector<float>(17)), m),
                 "part_size_mismatch");
    CHECK_THROWS(haori::decode_body_topology(bytes_of(std::vector<std::uint32_t>{0, 1}), m),
                 "part_size_mismatch");

    // 範囲外の頂点参照は弾く
    CHECK_THROWS(haori::decode_body_topology(bytes_of(std::vector<std::uint32_t>{0, 1, 9}), m),
                 "topology_out_of_range");
}

void test_cloth_mesh_decoding() {
    std::cout << "test_cloth_mesh_decoding\n";
    haori::Manifest m = haori::parse_manifest(minimal_manifest());

    const std::vector<float>         pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<std::uint32_t> tri{0, 1, 2};

    std::vector<float>         out_pos;
    std::vector<std::uint32_t> out_tri;
    haori::decode_cloth_mesh(bytes_of(pos) + bytes_of(tri), m, out_pos, out_tri);
    CHECK(out_pos == pos);
    CHECK(out_tri == tri);

    // 位置だけで三角形が欠けている
    CHECK_THROWS(haori::decode_cloth_mesh(bytes_of(pos), m, out_pos, out_tri),
                 "part_size_mismatch");
}

void test_result_roundtrip() {
    std::cout << "test_result_roundtrip\n";
    const std::uint32_t nf = 3, nv = 4;
    std::vector<float>  data(static_cast<std::size_t>(nf) * nv * 3);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<float>(i) * 0.5f;

    const std::string encoded = haori::encode_result(nf, nv, data);

    // ヘッダの並びが §4 どおりか
    CHECK(encoded.size() == 16 + data.size() * sizeof(float));
    CHECK(std::memcmp(encoded.data(), "HAOR", 4) == 0);

    std::uint32_t      out_nf = 0, out_nv = 0;
    std::vector<float> out_data;
    haori::decode_result(encoded, out_nf, out_nv, out_data);
    CHECK(out_nf == nf);
    CHECK(out_nv == nv);
    CHECK(out_data == data);

    // 要素数が合わなければエンコードを拒否する
    CHECK_THROWS(haori::encode_result(nf, nv, std::vector<float>(5)), "internal_error");

    // 壊れた結果を弾く
    CHECK_THROWS(haori::decode_result("XXXX000000000000", out_nf, out_nv, out_data), "bad_result");
    CHECK_THROWS(haori::decode_result(encoded.substr(0, encoded.size() - 4), out_nf, out_nv,
                                      out_data),
                 "bad_result");
}

void test_dummy_simulator_and_queue() {
    std::cout << "test_dummy_simulator_and_queue\n";

    haori::JobInput input;
    input.manifest = haori::parse_manifest(minimal_manifest(1, 5));
    input.manifest.pinned_vertices = {0};

    input.body_topology   = {0, 1, 2};
    input.body_frames.assign(static_cast<std::size_t>(input.manifest.num_frames()) * 3 * 3, 0.0f);
    input.cloth_positions = {0, 0, 5, 1, 0, 5, 0, 1, 5};
    input.cloth_topology  = {0, 1, 2};

    haori::JobQueue queue(haori::make_dummy_simulator());
    const std::string id = queue.submit(input);
    CHECK(!id.empty());
    CHECK(queue.find(id) != nullptr);

    // ワーカーの完了を待つ(ダミーは一瞬で終わる)
    std::string payload;
    for (int i = 0; i < 200 && !queue.take_result(id, payload); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!payload.empty());

    std::uint32_t      nf = 0, nv = 0;
    std::vector<float> data;
    haori::decode_result(payload, nf, nv, data);
    CHECK(nf == static_cast<std::uint32_t>(input.manifest.num_frames()));
    CHECK(nv == input.manifest.cloth_num_vertices);

    // pinned 頂点 0 は動かない
    CHECK(data[0] == input.cloth_positions[0]);
    CHECK(data[1] == input.cloth_positions[1]);
    CHECK(data[2] == input.cloth_positions[2]);

    // pinned でない頂点 1 は重力で下がる
    CHECK(data[5] < input.cloth_positions[5]);

    const haori::JobSnapshot s = haori::JobQueue::snapshot(*queue.find(id));
    CHECK(s.state == haori::JobState::Done);
    CHECK(s.frames_done == input.manifest.num_frames());

    // 未知のジョブ ID
    CHECK(queue.find("deadbeef") == nullptr);
    CHECK(queue.cancel("deadbeef") == false);
    // 完了済みジョブはキャンセルできない
    CHECK(queue.cancel(id) == false);
}

}  // namespace

int main() {
    test_manifest_defaults();
    test_manifest_unknown_keys_ignored();
    test_manifest_validation();
    test_part_decoding();
    test_cloth_mesh_decoding();
    test_result_roundtrip();
    test_dummy_simulator_and_queue();

    std::cout << "\n" << (g_checks - g_failures) << " / " << g_checks << " 件成功\n";
    if (g_failures) {
        std::cerr << g_failures << " 件失敗\n";
        return 1;
    }
    std::cout << "すべて成功\n";
    return 0;
}
