#pragma once

#include <string>

#include "../jobs/job_queue.h"

namespace haori {

struct ServerOptions {
    std::string host    = "127.0.0.1";  ///< 既定はローカルのみ (指示書 §9)
    int         port    = 8787;
    std::string gpu     = "unknown";    ///< health に出す GPU 名
    std::string version = "0.1.0";
};

/// ブロッキング。Ctrl-C まで動き続ける。
/// 戻り値は 0 が正常、非 0 が起動失敗。
int run_server(JobQueue& queue, const ServerOptions& options);

}  // namespace haori
