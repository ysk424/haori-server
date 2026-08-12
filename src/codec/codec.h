// プロトコル (docs/protocol.md) のエンコード/デコード。
// この層は Gaia にも HTTP ライブラリにも依存しない(単体テスト可能にするため)。
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace haori {

/// プロトコル §5 のエラー。code はクライアントが機械判定に使う。
class ProtocolError : public std::runtime_error {
public:
    ProtocolError(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}

    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

/// manifest.sim.cloth
struct ClothParams {
    double density           = 0.2;
    double stretch_stiffness = 1.0e4;
    double bend_stiffness    = 0.5;
    double friction          = 0.3;
    double damping           = 0.01;
    double thickness         = 0.002;
};

/// manifest.sim
struct SimParams {
    std::array<double, 3> gravity = {0.0, 0.0, -9.8};
    int         iterations        = 20;
    ClothParams cloth;
    double      collision_margin  = 0.003;
    int         warmup_frames     = 10;
};

/// manifest 全体 (docs/protocol.md §3.1)
struct Manifest {
    int    version     = 1;
    double fps         = 24.0;
    int    frame_start = 1;
    int    frame_end   = 120;
    int    substeps    = 4;

    SimParams sim;

    std::uint32_t body_num_vertices   = 0;
    std::uint32_t body_num_triangles  = 0;
    std::uint32_t cloth_num_vertices  = 0;
    std::uint32_t cloth_num_triangles = 0;

    std::vector<std::uint32_t> pinned_vertices;

    /// frame_start..frame_end は両端を含む
    int num_frames() const { return frame_end - frame_start + 1; }
};

/// ジョブ1件分の入力。デコード済みの生データ。
struct JobInput {
    Manifest manifest;

    std::vector<std::uint32_t> body_topology;    ///< num_triangles * 3
    std::vector<float>         body_frames;      ///< num_frames * num_vertices * 3
    std::vector<float>         cloth_positions;  ///< num_vertices * 3 (静止位置)
    std::vector<std::uint32_t> cloth_topology;   ///< num_triangles * 3
};

/// manifest (JSON テキスト) をパースする。未知キーは無視する (§3.1)。
Manifest parse_manifest(const std::string& json_text);

/// body_topology パート: uint32 × num_triangles × 3
std::vector<std::uint32_t> decode_body_topology(const std::string& bytes, const Manifest& m);

/// body_frames パート: float32 × num_frames × num_vertices × 3
std::vector<float> decode_body_frames(const std::string& bytes, const Manifest& m);

/// cloth_mesh パート: float32 × nv × 3 の静止位置 + uint32 × nt × 3
void decode_cloth_mesh(const std::string& bytes, const Manifest& m,
                       std::vector<float>& out_positions,
                       std::vector<std::uint32_t>& out_topology);

/// 結果バイナリ (§4) を組み立てる。
/// data は num_frames * num_vertices * 3 の float32。
std::string encode_result(std::uint32_t num_frames, std::uint32_t num_vertices,
                          const std::vector<float>& data);

/// 結果バイナリを解く。テストとツールが使う。
void decode_result(const std::string& bytes, std::uint32_t& out_num_frames,
                   std::uint32_t& out_num_vertices, std::vector<float>& out_data);

/// 三角形インデックスが頂点数の範囲に収まっているか検査する。
void validate_topology(const std::vector<std::uint32_t>& topology, std::uint32_t num_vertices,
                       const char* what);

}  // namespace haori
