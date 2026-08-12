cmake_minimum_required(VERSION 3.24)
project(haori_server LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    # /MP: 並列コンパイル、/utf-8: ソースの日本語コメントを正しく扱う
    add_compile_options(/MP /utf-8)
    # Windows.h の min/max マクロが std::min/max と衝突するのを防ぐ
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# 依存は tools\setup_deps.ps1 が external/thirdparty へ展開済みであることを前提にする。
# CMake の FetchContent にネットワーク取得させない (docs/decisions.md D-006):
#   PATH 上の MinGW 版 CMake は CA 証明書を持たず、https の TLS 検証に失敗する。
#   PowerShell 側で取得すればこの問題を回避でき、2回目以降はオフラインでビルドできる。
set(HAORI_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/external/thirdparty")

foreach(dep cpp-httplib-0.18.3 json-3.11.3 spdlog-1.14.1)
    if (NOT EXISTS "${HAORI_THIRDPARTY}/${dep}")
        message(FATAL_ERROR
            "依存 '${dep}' が見つからない。先に tools\\setup_deps.ps1 を実行すること。\n"
            "  期待するパス: ${HAORI_THIRDPARTY}/${dep}")
    endif()
endforeach()

set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

add_subdirectory("${HAORI_THIRDPARTY}/json-3.11.3"       ${CMAKE_BINARY_DIR}/_deps/json   EXCLUDE_FROM_ALL)
add_subdirectory("${HAORI_THIRDPARTY}/spdlog-1.14.1"     ${CMAKE_BINARY_DIR}/_deps/spdlog EXCLUDE_FROM_ALL)
add_subdirectory("${HAORI_THIRDPARTY}/cpp-httplib-0.18.3" ${CMAKE_BINARY_DIR}/_deps/httplib EXCLUDE_FROM_ALL)

# ---- プロトコル層 (Gaia にも HTTP にも依存しない。テスト対象) ----------------
add_library(haori_core STATIC
    src/codec/codec.cpp
    src/jobs/job_queue.cpp
    src/sim/dummy_simulator.cpp
)
target_include_directories(haori_core PUBLIC src)
target_link_libraries(haori_core PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog)

# ---- サーバー本体 -----------------------------------------------------------
add_executable(haori-server
    src/main.cpp
    src/http/server.cpp
)
target_link_libraries(haori-server PRIVATE haori_core httplib::httplib)

if (WIN32)
    target_link_libraries(haori-server PRIVATE ws2_32 crypt32)
endif()

# ---- ユニットテスト ---------------------------------------------------------
enable_testing()

add_executable(haori_tests tests/unit/test_codec.cpp)
target_link_libraries(haori_tests PRIVATE haori_core)
add_test(NAME unit COMMAND haori_tests)
