#include "mesh_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

namespace haori {
namespace {

/// 無向辺のキー(小さい方を先に)
inline std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<std::uint64_t>(a) << 32) | b;
}

void add_edge(std::vector<std::vector<std::uint32_t>>& adj, std::uint32_t a, std::uint32_t b) {
    if (a == b) return;
    adj[a].push_back(b);
    adj[b].push_back(a);
}

}  // namespace

void write_obj(const std::string& path, const float* positions, std::size_t num_vertices,
               const std::uint32_t* triangles, std::size_t num_triangles) {
    // ofstream の << は桁数の多い浮動小数で遅いので、snprintf で組んでまとめて書く。
    // ボディは毎フレーム分書く可能性があるため、ここの速度は効いてくる。
    std::string buffer;
    buffer.reserve(num_vertices * 48 + num_triangles * 32);

    char line[128];
    for (std::size_t v = 0; v < num_vertices; ++v) {
        const int n = std::snprintf(line, sizeof(line), "v %.7g %.7g %.7g\n",
                                    positions[v * 3 + 0], positions[v * 3 + 1],
                                    positions[v * 3 + 2]);
        buffer.append(line, n);
    }
    for (std::size_t t = 0; t < num_triangles; ++t) {
        // OBJ のインデックスは 1 始まり
        const int n = std::snprintf(line, sizeof(line), "f %u %u %u\n",
                                    triangles[t * 3 + 0] + 1u, triangles[t * 3 + 1] + 1u,
                                    triangles[t * 3 + 2] + 1u);
        buffer.append(line, n);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("OBJ を書き出せない: " + path);
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!out) throw std::runtime_error("OBJ の書き込みに失敗した: " + path);
}

std::vector<std::vector<std::uint32_t>> build_vertex_adjacency(
    const std::uint32_t* triangles, std::size_t num_triangles, std::uint32_t num_vertices,
    bool include_bending) {
    std::vector<std::vector<std::uint32_t>> adj(num_vertices);

    // 面の辺
    for (std::size_t t = 0; t < num_triangles; ++t) {
        const std::uint32_t a = triangles[t * 3 + 0];
        const std::uint32_t b = triangles[t * 3 + 1];
        const std::uint32_t c = triangles[t * 3 + 2];
        add_edge(adj, a, b);
        add_edge(adj, b, c);
        add_edge(adj, c, a);
    }

    if (include_bending) {
        // 各辺を共有する2面の「対頂点」同士も曲げエネルギーで結ばれる。
        // 辺 -> その辺を含む面の対頂点、を集めてから対にする。
        std::map<std::uint64_t, std::vector<std::uint32_t>> opposite;
        for (std::size_t t = 0; t < num_triangles; ++t) {
            const std::uint32_t v[3] = {triangles[t * 3 + 0], triangles[t * 3 + 1],
                                        triangles[t * 3 + 2]};
            opposite[edge_key(v[0], v[1])].push_back(v[2]);
            opposite[edge_key(v[1], v[2])].push_back(v[0]);
            opposite[edge_key(v[2], v[0])].push_back(v[1]);
        }
        for (const auto& [key, verts] : opposite) {
            // 多様体なら 2 つ。非多様体(3面以上が共有)でも総当たりで安全側に倒す。
            for (std::size_t i = 0; i < verts.size(); ++i) {
                for (std::size_t j = i + 1; j < verts.size(); ++j) {
                    add_edge(adj, verts[i], verts[j]);
                }
            }
        }
    }

    // 重複を落とす
    for (auto& neighbors : adj) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    return adj;
}

std::vector<std::vector<std::uint32_t>> greedy_coloring(
    const std::vector<std::vector<std::uint32_t>>& adjacency) {
    const std::uint32_t n = static_cast<std::uint32_t>(adjacency.size());

    // 次数の大きい順に塗る (Welsh-Powell)。色数が減りやすい。
    std::vector<std::uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&adjacency](std::uint32_t a, std::uint32_t b) {
        if (adjacency[a].size() != adjacency[b].size()) {
            return adjacency[a].size() > adjacency[b].size();
        }
        return a < b;  // 実行ごとに結果が変わらないようにする
    });

    constexpr std::uint32_t kNoColor = 0xFFFFFFFFu;
    std::vector<std::uint32_t> color(n, kNoColor);
    std::vector<char> used;

    for (std::uint32_t v : order) {
        used.assign(adjacency[v].size() + 1, 0);
        for (std::uint32_t nb : adjacency[v]) {
            const std::uint32_t c = color[nb];
            if (c != kNoColor && c < used.size()) used[c] = 1;
        }
        std::uint32_t chosen = 0;
        while (chosen < used.size() && used[chosen]) ++chosen;
        color[v] = chosen;
    }

    std::uint32_t num_colors = 0;
    for (std::uint32_t v = 0; v < n; ++v) num_colors = std::max(num_colors, color[v] + 1);

    std::vector<std::vector<std::uint32_t>> categories(num_colors);
    for (std::uint32_t v = 0; v < n; ++v) categories[color[v]].push_back(v);

    // Gaia 側も大きい順に並べ替えるので、あらかじめ揃えておく
    std::sort(categories.begin(), categories.end(),
              [](const auto& a, const auto& b) { return a.size() > b.size(); });
    return categories;
}

bool validate_coloring(const std::vector<std::vector<std::uint32_t>>& adjacency,
                       const std::vector<std::vector<std::uint32_t>>& categories,
                       std::uint32_t num_vertices) {
    constexpr std::uint32_t kNoColor = 0xFFFFFFFFu;
    std::vector<std::uint32_t> color(num_vertices, kNoColor);

    for (std::uint32_t c = 0; c < categories.size(); ++c) {
        for (std::uint32_t v : categories[c]) {
            if (v >= num_vertices) return false;
            if (color[v] != kNoColor) return false;  // 同じ頂点が2つの色に入っている
            color[v] = c;
        }
    }
    // 全頂点が塗られていること
    for (std::uint32_t v = 0; v < num_vertices; ++v) {
        if (color[v] == kNoColor) return false;
    }
    // 隣接が同色でないこと
    for (std::uint32_t v = 0; v < num_vertices; ++v) {
        for (std::uint32_t nb : adjacency[v]) {
            if (color[v] == color[nb]) return false;
        }
    }
    return true;
}

double mean_edge_length(const float* positions, std::uint32_t num_vertices,
                        const std::uint32_t* triangles, std::size_t num_triangles) {
    double total = 0.0;
    std::size_t count = 0;

    auto length = [&](std::uint32_t a, std::uint32_t b) {
        if (a >= num_vertices || b >= num_vertices) return 0.0;
        const double dx = static_cast<double>(positions[a * 3 + 0]) - positions[b * 3 + 0];
        const double dy = static_cast<double>(positions[a * 3 + 1]) - positions[b * 3 + 1];
        const double dz = static_cast<double>(positions[a * 3 + 2]) - positions[b * 3 + 2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    for (std::size_t t = 0; t < num_triangles; ++t) {
        const std::uint32_t a = triangles[t * 3 + 0];
        const std::uint32_t b = triangles[t * 3 + 1];
        const std::uint32_t c = triangles[t * 3 + 2];
        total += length(a, b) + length(b, c) + length(c, a);
        count += 3;
    }
    return count ? total / static_cast<double>(count) : 0.0;
}

void write_coloring_json(const std::string& path,
                         const std::vector<std::vector<std::uint32_t>>& categories) {
    std::string buffer;
    buffer.reserve(1024);
    buffer += '[';

    char number[16];
    for (std::size_t c = 0; c < categories.size(); ++c) {
        if (c) buffer += ',';
        buffer += '[';
        for (std::size_t i = 0; i < categories[c].size(); ++i) {
            if (i) buffer += ',';
            const int n = std::snprintf(number, sizeof(number), "%u", categories[c][i]);
            buffer.append(number, n);
        }
        buffer += ']';
    }
    buffer += ']';

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("彩色 JSON を書き出せない: " + path);
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (!out) throw std::runtime_error("彩色 JSON の書き込みに失敗した: " + path);
}

}  // namespace haori
