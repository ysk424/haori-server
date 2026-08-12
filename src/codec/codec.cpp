#include "codec.h"

#include <cstring>
#include <nlohmann/json.hpp>

namespace haori {
namespace {

using nlohmann::json;

// プロトコルは float32/uint32 のリトルエンディアン固定 (§1)。
// Windows/x86-64 と ARM64 はどちらも LE なので memcpy で足りる。
// ビッグエンディアン環境に移す場合はここだけ差し替えればよい。
static_assert(sizeof(float) == 4, "float32 を前提にしている");
static_assert(sizeof(std::uint32_t) == 4, "uint32 を前提にしている");

/// JSON から値を取り出す。キーが無い/型違いなら既定値のまま (§3.1「未知キーは無視」)。
template <typename T>
void pick(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (const json::exception&) {
        // 型が合わないものは既定値を維持する。壊れた値で走らせるより安全。
    }
}

/// 要素数 count の配列としてバイト列を検査する。
void expect_size(const std::string& bytes, std::size_t count, std::size_t elem_size,
                 const char* part) {
    const std::size_t expected = count * elem_size;
    if (bytes.size() != expected) {
        throw ProtocolError("part_size_mismatch",
                            std::string(part) + " のサイズが不正: " +
                                std::to_string(bytes.size()) + " bytes (期待値 " +
                                std::to_string(expected) + " bytes)");
    }
}

}  // namespace

Manifest parse_manifest(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        throw ProtocolError("manifest_parse_error",
                            std::string("manifest の JSON を解析できない: ") + e.what());
    }
    if (!j.is_object()) {
        throw ProtocolError("manifest_parse_error", "manifest はオブジェクトである必要がある");
    }

    Manifest m;
    pick(j, "version", m.version);
    if (m.version != 1) {
        throw ProtocolError("unsupported_version",
                            "未対応の manifest version: " + std::to_string(m.version));
    }
    pick(j, "fps", m.fps);
    pick(j, "frame_start", m.frame_start);
    pick(j, "frame_end", m.frame_end);
    pick(j, "substeps", m.substeps);

    if (auto sim = j.find("sim"); sim != j.end() && sim->is_object()) {
        pick(*sim, "gravity", m.sim.gravity);
        pick(*sim, "iterations", m.sim.iterations);
        pick(*sim, "collision_margin", m.sim.collision_margin);
        pick(*sim, "warmup_frames", m.sim.warmup_frames);

        if (auto c = sim->find("cloth"); c != sim->end() && c->is_object()) {
            pick(*c, "density", m.sim.cloth.density);
            pick(*c, "stretch_stiffness", m.sim.cloth.stretch_stiffness);
            pick(*c, "bend_stiffness", m.sim.cloth.bend_stiffness);
            pick(*c, "friction", m.sim.cloth.friction);
            pick(*c, "damping", m.sim.cloth.damping);
            pick(*c, "thickness", m.sim.cloth.thickness);
        }
    }

    if (auto b = j.find("body"); b != j.end() && b->is_object()) {
        pick(*b, "num_vertices", m.body_num_vertices);
        pick(*b, "num_triangles", m.body_num_triangles);
    }
    if (auto c = j.find("cloth"); c != j.end() && c->is_object()) {
        pick(*c, "num_vertices", m.cloth_num_vertices);
        pick(*c, "num_triangles", m.cloth_num_triangles);
        pick(*c, "pinned_vertices", m.pinned_vertices);
    }

    // --- 整合性チェック -----------------------------------------------------
    if (m.frame_end < m.frame_start) {
        throw ProtocolError("invalid_frame_range",
                            "frame_end (" + std::to_string(m.frame_end) + ") が frame_start (" +
                                std::to_string(m.frame_start) + ") より小さい");
    }
    if (m.fps <= 0.0) {
        throw ProtocolError("invalid_manifest", "fps は正の値である必要がある");
    }
    if (m.substeps < 1) {
        throw ProtocolError("invalid_manifest", "substeps は 1 以上である必要がある");
    }
    if (m.sim.warmup_frames < 0) {
        throw ProtocolError("invalid_manifest", "warmup_frames は 0 以上である必要がある");
    }
    if (m.cloth_num_vertices == 0 || m.cloth_num_triangles == 0) {
        throw ProtocolError("empty_cloth", "服のメッシュが空である");
    }
    if (m.body_num_vertices == 0 || m.body_num_triangles == 0) {
        throw ProtocolError("empty_body", "ボディのメッシュが空である");
    }
    for (std::uint32_t v : m.pinned_vertices) {
        if (v >= m.cloth_num_vertices) {
            throw ProtocolError("pinned_vertex_out_of_range",
                                "pinned_vertices に範囲外のインデックスがある: " +
                                    std::to_string(v));
        }
    }
    return m;
}

void validate_topology(const std::vector<std::uint32_t>& topology, std::uint32_t num_vertices,
                       const char* what) {
    for (std::uint32_t idx : topology) {
        if (idx >= num_vertices) {
            throw ProtocolError("topology_out_of_range",
                                std::string(what) + " の三角形が範囲外の頂点を参照している: " +
                                    std::to_string(idx) + " >= " + std::to_string(num_vertices));
        }
    }
}

std::vector<std::uint32_t> decode_body_topology(const std::string& bytes, const Manifest& m) {
    const std::size_t count = static_cast<std::size_t>(m.body_num_triangles) * 3;
    expect_size(bytes, count, sizeof(std::uint32_t), "body_topology");

    std::vector<std::uint32_t> out(count);
    if (count) std::memcpy(out.data(), bytes.data(), bytes.size());
    validate_topology(out, m.body_num_vertices, "body");
    return out;
}

std::vector<float> decode_body_frames(const std::string& bytes, const Manifest& m) {
    const std::size_t count = static_cast<std::size_t>(m.num_frames()) *
                              static_cast<std::size_t>(m.body_num_vertices) * 3;
    expect_size(bytes, count, sizeof(float), "body_frames");

    std::vector<float> out(count);
    if (count) std::memcpy(out.data(), bytes.data(), bytes.size());
    return out;
}

void decode_cloth_mesh(const std::string& bytes, const Manifest& m,
                       std::vector<float>& out_positions,
                       std::vector<std::uint32_t>& out_topology) {
    const std::size_t pos_count = static_cast<std::size_t>(m.cloth_num_vertices) * 3;
    const std::size_t idx_count = static_cast<std::size_t>(m.cloth_num_triangles) * 3;
    const std::size_t expected  = pos_count * sizeof(float) + idx_count * sizeof(std::uint32_t);

    if (bytes.size() != expected) {
        throw ProtocolError("part_size_mismatch",
                            "cloth_mesh のサイズが不正: " + std::to_string(bytes.size()) +
                                " bytes (期待値 " + std::to_string(expected) + " bytes)");
    }

    out_positions.resize(pos_count);
    out_topology.resize(idx_count);
    if (pos_count) std::memcpy(out_positions.data(), bytes.data(), pos_count * sizeof(float));
    if (idx_count) {
        std::memcpy(out_topology.data(), bytes.data() + pos_count * sizeof(float),
                    idx_count * sizeof(std::uint32_t));
    }
    validate_topology(out_topology, m.cloth_num_vertices, "cloth");
}

std::string encode_result(std::uint32_t num_frames, std::uint32_t num_vertices,
                          const std::vector<float>& data) {
    const std::size_t expected =
        static_cast<std::size_t>(num_frames) * static_cast<std::size_t>(num_vertices) * 3;
    if (data.size() != expected) {
        throw ProtocolError("internal_error",
                            "結果の要素数が不正: " + std::to_string(data.size()) + " (期待値 " +
                                std::to_string(expected) + ")");
    }

    std::string out;
    out.reserve(16 + data.size() * sizeof(float));
    out.append("HAOR", 4);

    const std::uint32_t header[3] = {1u, num_frames, num_vertices};
    out.append(reinterpret_cast<const char*>(header), sizeof(header));
    out.append(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
    return out;
}

void decode_result(const std::string& bytes, std::uint32_t& out_num_frames,
                   std::uint32_t& out_num_vertices, std::vector<float>& out_data) {
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "HAOR", 4) != 0) {
        throw ProtocolError("bad_result", "結果バイナリのマジックが 'HAOR' ではない");
    }
    std::uint32_t header[3];
    std::memcpy(header, bytes.data() + 4, sizeof(header));
    if (header[0] != 1u) {
        throw ProtocolError("bad_result",
                            "未対応の結果 version: " + std::to_string(header[0]));
    }
    out_num_frames   = header[1];
    out_num_vertices = header[2];

    const std::size_t count =
        static_cast<std::size_t>(out_num_frames) * static_cast<std::size_t>(out_num_vertices) * 3;
    if (bytes.size() != 16 + count * sizeof(float)) {
        throw ProtocolError("bad_result", "結果バイナリの本体長がヘッダと一致しない");
    }
    out_data.resize(count);
    if (count) std::memcpy(out_data.data(), bytes.data() + 16, count * sizeof(float));
}

}  // namespace haori
