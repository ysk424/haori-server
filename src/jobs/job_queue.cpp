#include "job_queue.h"

#include <cmath>
#include <random>
#include <spdlog/spdlog.h>

namespace haori {
namespace {

std::string make_job_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";

    std::string id(32, '0');
    for (int i = 0; i < 32; ++i) id[i] = kHex[rng() & 0xF];
    return id;
}

/// 結果に NaN/Inf が混じっていないか調べる (指示書 §7 の NaN 監視)。
bool has_non_finite(const std::vector<float>& v) {
    for (float x : v) {
        if (!std::isfinite(x)) return true;
    }
    return false;
}

}  // namespace

const char* to_string(JobState state) {
    switch (state) {
        case JobState::Queued:    return "queued";
        case JobState::Running:   return "running";
        case JobState::Done:      return "done";
        case JobState::Error:     return "error";
        case JobState::Cancelled: return "cancelled";
    }
    return "unknown";
}

JobQueue::JobQueue(std::unique_ptr<ISimulator> simulator, std::chrono::seconds retention)
    : simulator_(std::move(simulator)), retention_(retention) {
    worker_ = std::thread([this] { worker_loop(); });
}

JobQueue::~JobQueue() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = true;
        // 実行中のジョブにも抜けてもらう
        for (auto& [id, job] : jobs_) job->cancel_requested = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string JobQueue::submit(JobInput input) {
    auto job   = std::make_shared<Job>();
    job->id    = make_job_id();
    job->input = std::move(input);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        jobs_.emplace(job->id, job);
        pending_.push_back(job);
    }
    cv_.notify_one();

    spdlog::info("ジョブ受付 {} (frames={}, cloth_verts={}, body_verts={})", job->id,
                 job->input.manifest.num_frames(), job->input.manifest.cloth_num_vertices,
                 job->input.manifest.body_num_vertices);
    return job->id;
}

JobPtr JobQueue::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : it->second;
}

JobSnapshot JobQueue::snapshot(const Job& job) {
    std::lock_guard<std::mutex> lock(job.mtx);
    return JobSnapshot{job.state,      job.progress,   job.frames_done,
                       job.message,    job.error_code, !job.result.empty()};
}

bool JobQueue::cancel(const std::string& id) {
    JobPtr job = find(id);
    if (!job) return false;

    std::lock_guard<std::mutex> lock(job->mtx);
    if (job->state == JobState::Done || job->state == JobState::Error ||
        job->state == JobState::Cancelled) {
        return false;  // すでに終わっている
    }
    job->cancel_requested = true;

    // queued のままならワーカーが拾う前にここで確定させる
    if (job->state == JobState::Queued) {
        job->state       = JobState::Cancelled;
        job->message     = "キューから取り消した";
        job->finished_at = std::chrono::steady_clock::now();
    }
    spdlog::info("ジョブ キャンセル要求 {}", id);
    return true;
}

bool JobQueue::take_result(const std::string& id, std::string& out) {
    JobPtr job = find(id);
    if (!job) return false;

    std::lock_guard<std::mutex> lock(job->mtx);
    if (job->state != JobState::Done || job->result.empty()) return false;
    out = job->result;
    return true;
}

void JobQueue::worker_loop() {
    for (;;) {
        JobPtr job;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait_for(lock, std::chrono::seconds(30),
                         [this] { return stopping_ || !pending_.empty(); });
            if (stopping_) return;

            if (!pending_.empty()) {
                job = pending_.front();
                pending_.pop_front();
            }
        }
        collect_garbage();
        if (!job) continue;

        // キュー上でキャンセル済みなら走らせない
        {
            std::lock_guard<std::mutex> lock(job->mtx);
            if (job->state == JobState::Cancelled) continue;
            job->state = JobState::Running;
        }
        run_job(job);
    }
}

void JobQueue::run_job(const JobPtr& job) {
    const auto started = std::chrono::steady_clock::now();
    spdlog::info("ジョブ開始 {}", job->id);

    SimContext ctx;
    ctx.is_cancelled = [job] { return job->cancel_requested.load(); };
    ctx.on_progress  = [job](int frames_done, double progress) {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->frames_done = frames_done;
        job->progress    = progress;
    };
    ctx.warn = [job](const std::string& text) {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->message = job->message.empty() ? text : job->message + " / " + text;
        spdlog::warn("ジョブ {}: {}", job->id, text);
    };

    try {
        std::vector<float> frames = simulator_->simulate(job->input, ctx);

        if (has_non_finite(frames)) {
            throw ProtocolError("nan_detected",
                                "シミュレーション結果に NaN/Inf が含まれている。"
                                "パラメータ(剛性・サブステップ)を見直すこと。");
        }

        const auto& m = job->input.manifest;
        std::string encoded = encode_result(static_cast<std::uint32_t>(m.num_frames()),
                                            m.cloth_num_vertices, frames);

        const double secs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

        std::lock_guard<std::mutex> lock(job->mtx);
        job->result      = std::move(encoded);
        job->state       = JobState::Done;
        job->progress    = 1.0;
        job->finished_at = std::chrono::steady_clock::now();
        spdlog::info("ジョブ完了 {} ({:.1f} 秒)", job->id, secs);

    } catch (const SimulationCancelled&) {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->state       = JobState::Cancelled;
        job->message     = "キャンセルされた";
        job->finished_at = std::chrono::steady_clock::now();
        spdlog::info("ジョブ中断 {}", job->id);

    } catch (const ProtocolError& e) {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->state       = JobState::Error;
        job->error_code  = e.code();
        job->message     = e.what();
        job->finished_at = std::chrono::steady_clock::now();
        spdlog::error("ジョブ失敗 {} [{}]: {}", job->id, e.code(), e.what());

    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->state       = JobState::Error;
        job->error_code  = "internal_error";
        job->message     = e.what();
        job->finished_at = std::chrono::steady_clock::now();
        spdlog::error("ジョブ失敗 {}: {}", job->id, e.what());
    }
}

void JobQueue::collect_garbage() {
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mtx_);
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        const JobPtr& job = it->second;

        bool expired = false;
        {
            std::lock_guard<std::mutex> jlock(job->mtx);
            const bool finished = job->state == JobState::Done || job->state == JobState::Error ||
                                  job->state == JobState::Cancelled;
            expired = finished && job->finished_at.time_since_epoch().count() != 0 &&
                      (now - job->finished_at) > retention_;
        }
        if (expired) {
            spdlog::debug("ジョブ破棄 {}", it->first);
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace haori
