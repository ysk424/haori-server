// haori-server エントリポイント。
#include <cstring>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string>

#include "http/server.h"
#include "jobs/job_queue.h"
#include "sim/simulator.h"

namespace {

constexpr const char* kVersion = "0.1.0";

void print_usage() {
    std::cout << R"(haori-server - Gaia VBD ベースのローカル布シミュレーションサーバー

使い方:
  haori-server [オプション]

オプション:
  --host <addr>     待ち受けアドレス (既定: 127.0.0.1)
  --port <n>        待ち受けポート (既定: 8787)
  --engine <name>   gaia | dummy   (既定: gaia)
                    gaia  は Gaia (VBD Cloth) を使う本番シミュレータ
                    dummy は布を重力落下させるだけの検証用実装
  --log <level>     trace|debug|info|warn|error (既定: info)
  -h, --help        このヘルプ
)";
}

/// 次の引数を取り出す。無ければ false。
bool next_arg(int argc, char** argv, int& i, std::string& out) {
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    haori::ServerOptions options;
    options.version = kVersion;

#ifdef HAORI_WITH_GAIA
    std::string engine = "gaia";
#else
    std::string engine = "dummy";
#endif
    std::string log_level = "info";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--host") {
            if (!next_arg(argc, argv, i, options.host)) { std::cerr << "--host に値が無い\n"; return 2; }
            continue;
        }
        if (arg == "--port") {
            std::string v;
            if (!next_arg(argc, argv, i, v)) { std::cerr << "--port に値が無い\n"; return 2; }
            try {
                options.port = std::stoi(v);
            } catch (const std::exception&) {
                std::cerr << "--port の値が数値でない: " << v << "\n";
                return 2;
            }
            continue;
        }
        if (arg == "--engine") {
            if (!next_arg(argc, argv, i, engine)) { std::cerr << "--engine に値が無い\n"; return 2; }
            continue;
        }
        if (arg == "--log") {
            if (!next_arg(argc, argv, i, log_level)) { std::cerr << "--log に値が無い\n"; return 2; }
            continue;
        }
        std::cerr << "不明なオプション: " << arg << "\n";
        print_usage();
        return 2;
    }

    spdlog::set_level(spdlog::level::from_str(log_level));
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::unique_ptr<haori::ISimulator> simulator;
    if (engine == "dummy") {
        simulator = haori::make_dummy_simulator();
    } else if (engine == "gaia") {
#ifdef HAORI_WITH_GAIA
        simulator = haori::make_gaia_simulator();
#else
        std::cerr << "このビルドには Gaia が含まれていない "
                     "(-DHAORI_WITH_GAIA=ON でビルドし直すこと)。\n";
        return 2;
#endif
    } else {
        std::cerr << "不明な engine: " << engine << " (dummy | gaia)\n";
        return 2;
    }

    haori::JobQueue queue(std::move(simulator));
    return haori::run_server(queue, options);
}
