// Gaia に渡すメッシュ資材の生成。
// Gaia のヘッダには依存しないので、この層だけ単体テストできる。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace haori {

/// 三角メッシュを Wavefront OBJ で書き出す。
/// positions は num_vertices*3、triangles は num_triangles*3 (0-based)。
void write_obj(const std::string& path, const float* positions, std::size_t num_vertices,
               const std::uint32_t* triangles, std::size_t num_triangles);

/// 頂点の隣接グラフを組む。
///
/// VBD は「同じ色の頂点は並列に更新してよい」前提で解くので、
/// **同じエネルギー項に現れる頂点同士を必ず隣接扱いにしなければならない**。
/// StVK(面)だけなら三角形の辺で足りるが、曲げエネルギーは辺を挟む2つの対頂点も
/// 同じ項に入るため、include_bending=true でそれも辺として足す
/// (Gaia の TriMeshVertexGraph の addEdgeBasedBending と同じ扱い)。
std::vector<std::vector<std::uint32_t>> build_vertex_adjacency(
    const std::uint32_t* triangles, std::size_t num_triangles, std::uint32_t num_vertices,
    bool include_bending);

/// 貪欲法で頂点を彩色し、色ごとの頂点リスト(並列グループ)にして返す。
///
/// 次数の大きい頂点から順に、隣接が使っていない最小の色を割り当てる
/// (Welsh-Powell)。色数が少ないほど並列度が上がる。
std::vector<std::vector<std::uint32_t>> greedy_coloring(
    const std::vector<std::vector<std::uint32_t>>& adjacency);

/// 彩色が正しいか(隣接頂点が同色になっていないか)を検査する。
/// 破れていると VBD が黙って誤った結果を出すので、実行前に必ず確かめる。
bool validate_coloring(const std::vector<std::vector<std::uint32_t>>& adjacency,
                       const std::vector<std::vector<std::uint32_t>>& categories,
                       std::uint32_t num_vertices);

/// 三角形の辺長の平均を返す。材質パラメータをメッシュ解像度に依らない量に直すのに使う。
double mean_edge_length(const float* positions, std::uint32_t num_vertices,
                        const std::uint32_t* triangles, std::size_t num_triangles);

/// Gaia の verticesColoringCategoriesPath が読む形式で書き出す。
/// 中身は素朴な配列の配列: [[0,5,9],[1,2],...]
void write_coloring_json(const std::string& path,
                         const std::vector<std::vector<std::uint32_t>>& categories);

}  // namespace haori
