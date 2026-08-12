// ジョブキュー。同時実行は1本 (GPU 1枚を前提、指示書 §7)。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "../codec/codec.h"
#include "../sim/simulator.h"

namespace haori {

enum class JobState { Queued, Running, Done, Error, Cancelled };

const char* to_string(JobState state);

/// 1ジョブの状態。ワーカースレッドと HTTP スレッドの双方から触るので mutex で守る。
struct Job {
    std::string id;
    JobInput    input;

    mutable std::mutex mtx;
    JobState    state       = JobState::Queued;
    double      progress    = 0.0;
    int         frames_done = 0;
    std::string message;
    std::string error_code;

    std::string result;  ///< エンコード済みの結果バイナリ (§4)

    std::atomic<bool> cancel_requested{false};

    /// 終了時刻。一定時間後の破棄判定に使う。
    std::chrono::steady_clock::time_point finished_at{};
};

using JobPtr = std::shared_ptr<Job>;

/// 状態のスナップショット (ロックを跨がずに HTTP 応答を作るため)
struct JobSnapshot {
    JobState    state;
    double      progress;
    int         frames_done;
    std::string message;
    std::string error_code;
    bool        has_result;
};

class JobQueue {
public:
    /// retention: 完了したジョブを保持する時間。過ぎたものは破棄する (指示書 §7)。
    explicit JobQueue(std::unique_ptr<ISimulator> simulator,
                      std::chrono::seconds retention = std::chrono::minutes(30));
    ~JobQueue();

    JobQueue(const JobQueue&)            = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    /// ジョブを積んで job_id を返す。
    std::string submit(JobInput input);

    /// 見つからなければ nullptr。
    JobPtr find(const std::string& id) const;

    static JobSnapshot snapshot(const Job& job);

    /// キャンセル要求。すでに終了していれば false。
    bool cancel(const std::string& id);

    /// 結果を取り出す。まだ done でなければ false。
    bool take_result(const std::string& id, std::string& out);

    const char* engine_name() const { return simulator_->name(); }

private:
    void worker_loop();
    void run_job(const JobPtr& job);
    void collect_garbage();

    std::unique_ptr<ISimulator> simulator_;
    std::chrono::seconds        retention_;

    mutable std::mutex                            mtx_;
    std::condition_variable                       cv_;
    std::deque<JobPtr>                            pending_;
    std::unordered_map<std::string, JobPtr>       jobs_;
    bool                                          stopping_ = false;

    std::thread worker_;
};

}  // namespace haori
