// Gaia (VBD Cloth) を組み込んだ実シミュレータ。
//
// 設計の要点 (docs/gaia-survey.md / decisions.md D-008 も参照):
//
//  * ボディは Gaia の **ColliderTriMeshBase** としてシーンに入れる。
//    Gaia には ColliderTrimeshSequence というアニメーションコライダーがあるが、
//    あれはフレームごとに .obj を読む実装なので、120 フレーム × 数万頂点だと
//    ファイル I/O だけで実用にならない。同じ更新インタフェースを実装した
//    HaoriBodyCollider を用意し、ベイク済み頂点をメモリ上で補間する。
//    Gaia 本体には手を入れない。
//
//  * warmup は「コライダーのキーフレームを warmup_frames だけ後ろへずらす」ことで表現する。
//    frameId < warmup の間はボディがフレーム1の姿勢に留まり、布だけが落ち着く (指示書 §7)。
//
//  * simulate() は全フレームを回し切るブロッキング呼び出しなので使わない。
//    runStep() を自分で1フレームずつ回し、その合間に進捗報告・キャンセル・結果吸い出しを行う。
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

// ⚠ JSON は **Gaia が同梱している 3.10.5** を使うこと (external/thirdparty の 3.11.3 ではない)。
//    nlohmann/json はバージョンごとに inline namespace (json_abi_v3_10_5 等) を切るので、
//    別版を掴むと Gaia の関数がシンボル不一致で未解決になる (docs/decisions.md D-009)。
//    codec 層は 3.11.3 のままでよい。json 型を境界に晒していないので混ざらない。
#include <Json/json.hpp>

// ⚠ include 順に意味がある。
//   Gaia の SpatialQuery/DynamicCollider.h は BasePhysicsParams を include せずに使っており、
//   先に読むと C2653 で落ちる。VBDClothPhysics.h が Framework 経由でそれを引き込むので、
//   必ずこちらを先に置くこと。
#include <VBDCloth/VBDClothPhysics.h>

#include <Parser/Parser.h>
#include <SpatialQuery/ColiiderTriMeshBase.h>
#include <SpatialQuery/DynamicCollider.h>
#include <TriMesh/TriMesh.h>

#include "mesh_utils.h"
#include "simulator.h"

namespace fs = std::filesystem;

namespace haori {
namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// ボディ: メモリ上のフレーム列を補間するキネマティックコライダー
// ---------------------------------------------------------------------------

struct HaoriBodyCollider : public GAIA::ColliderTriMeshBase {
    const std::vector<float>* frames = nullptr;  ///< num_frames * num_vertices * 3 (ワールド座標)
    std::uint32_t body_num_vertices = 0;
    int           num_body_frames   = 0;
    int           warmup_frames     = 0;

    void initialize(GAIA::ColliderTriMeshBaseParams::SharedPtr inObjectParams) override {
        GAIA::ColliderTriMeshBase::initialize(inObjectParams);
        pParams = inObjectParams;
        // path に置いた1フレーム目の .obj からトポロジーと近傍情報を作らせる。
        // 頂点位置は以降 update() で上書きする。
        GAIA::TriMeshFEM::initialize(inObjectParams, true);
    }

    void update(GAIA::IdType frameId, GAIA::IdType substepId, GAIA::IdType iter,
                std::size_t numSubsteps, std::size_t numIters) override {
        if (frames == nullptr || num_body_frames <= 0) {
            updated = false;
            return;
        }

        // 反復の途中でコライダーが動くと収束を乱すので、サブステップの頭でだけ動かす。
        if (iter != 0) {
            updated = false;
            return;
        }

        // warmup 中はフレーム1の姿勢で固定する
        const long long local = static_cast<long long>(frameId) - warmup_frames;

        double target;  // ボディフレーム空間での位置 (0 .. num_body_frames-1)
        if (local < 0) {
            target = 0.0;
            if (!first_update) {
                updated = false;  // 動いていないので BVH 再構築も要らない
                return;
            }
        } else {
            // サブステップの終端を狙う。numSubsteps 個目で丁度 local+1 に届く。
            const double t = (numSubsteps > 0)
                                 ? static_cast<double>(substepId + 1) / static_cast<double>(numSubsteps)
                                 : 1.0;
            target = static_cast<double>(local) + t;
        }

        target = std::clamp(target, 0.0, static_cast<double>(num_body_frames - 1));
        apply(target);
        first_update = false;
        updated      = true;
    }

private:
    bool first_update = true;

    /// ボディフレーム空間の位置 target (実数) の姿勢を頂点に書き込む。
    void apply(double target) {
        const int    f0 = static_cast<int>(std::floor(target));
        const int    f1 = std::min(f0 + 1, num_body_frames - 1);
        const double w  = target - f0;

        const std::size_t stride = static_cast<std::size_t>(body_num_vertices) * 3;
        const float* a = frames->data() + static_cast<std::size_t>(f0) * stride;
        const float* b = frames->data() + static_cast<std::size_t>(f1) * stride;

        auto pos = positions();
        const auto n = static_cast<std::uint32_t>(
            std::min<std::size_t>(body_num_vertices, static_cast<std::size_t>(pos.cols())));

        if (w <= 0.0) {
            for (std::uint32_t v = 0; v < n; ++v) {
                pos(0, v) = a[v * 3 + 0];
                pos(1, v) = a[v * 3 + 1];
                pos(2, v) = a[v * 3 + 2];
            }
        } else {
            const float wb = static_cast<float>(w);
            const float wa = 1.0f - wb;
            for (std::uint32_t v = 0; v < n; ++v) {
                pos(0, v) = wa * a[v * 3 + 0] + wb * b[v * 3 + 0];
                pos(1, v) = wa * a[v * 3 + 1] + wb * b[v * 3 + 1];
                pos(2, v) = wa * a[v * 3 + 2] + wb * b[v * 3 + 2];
            }
        }
    }
};

// ---------------------------------------------------------------------------
// フレームワーク: コライダーの作り方だけ差し替える
// ---------------------------------------------------------------------------

struct HaoriClothFramework : public GAIA::VBDClothSimulationFramework {
    std::shared_ptr<HaoriBodyCollider>            bodyCollider;
    GAIA::ColliderTriMeshBaseParams::SharedPtr    bodyColliderParams;

    void initializeCollider() override {
        // 既定実装は Parameters.json の ColliderMeshes から作るが、
        // ここではメモリ上のボディを使うので自前で組む。
        pDynamicCollider = std::make_shared<GAIA::DynamicCollider>(pDynamicColliderParameter);

        bodyCollider->initialize(bodyColliderParams);
        colliderTriMeshes.push_back(bodyCollider);

        pDynamicCollider->initialize(colliderTriMeshes);
    }
};

// ---------------------------------------------------------------------------
// 一時ディレクトリ (ジョブごと。抜けるときに消す)
// ---------------------------------------------------------------------------

class TempDir {
public:
    TempDir() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::ostringstream name;
        name << "haori-" << std::hex << rng();
        path_ = fs::temp_directory_path() / name.str();
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);  // 消せなくても致命的ではない
        if (ec) spdlog::warn("一時ディレクトリを削除できなかった: {}", path_.string());
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const char* name) const { return (path_ / name).string(); }
    std::string dir() const { return path_.string(); }

private:
    fs::path path_;
};

void write_json(const std::string& path, const json& j) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("JSON を書き出せない: " + path);
    out << j.dump(1);
    if (!out) throw std::runtime_error("JSON の書き込みに失敗した: " + path);
}

// ---------------------------------------------------------------------------
// プロトコル (§3.1) の sim パラメータ → Gaia のパラメータ
// ---------------------------------------------------------------------------
//
// 対応の根拠は docs/gaia-survey.md「材質パラメータ対応表」に書く。
// Gaia のサンプル S03 で実績のある値を基準にして、プロトコルの既定値がそこへ
// 落ちるように換算している。

json build_models_json(const JobInput& in, const std::string& cloth_obj,
                       const std::string& coloring_json, double mean_edge) {
    const ClothParams& c = in.manifest.sim.cloth;

    json model;
    model["materialName"] = "StVK_triMesh";
    model["path"]         = cloth_obj;
    model["verticesColoringCategoriesPath"] = coloring_json;

    model["density"] = c.density;

    // StVK のラメ定数。プロトコルは伸び剛性を1値で持つので両方に同じ値を入れる。
    model["miu"]    = c.stretch_stiffness;
    model["lambda"] = c.stretch_stiffness;

    // ⚠ Gaia の bendingStiffness は「辺あたりの絶対量」で、メッシュの寸法にも解像度にも
    //   追従しない。プロトコルの既定値 (0.5) をそのまま渡すと、1m 級の服・density 0.2 では
    //   曲げ項が慣性項を圧倒し、VBD が既定の反復回数で収束せず布が自由落下すらしなくなる
    //   (自由落下の再現率 0.023 = ほぼ静止。docs/decisions.md D-010 に実測を記録)。
    //   平均辺長の2乗で正規化して、解像度と単位に依らない量に直す。
    const double edge2 = mean_edge * mean_edge;
    model["bendingStiffness"] = c.bend_stiffness * edge2;

    // Gaia の減衰係数はプロトコルとスケールが違う。
    // サンプル S03 の実績値 2e-6 が、プロトコル既定の damping=0.01 に対応するよう換算する。
    const double damping = c.damping * 2.0e-4;
    model["dampingStVK"]    = damping;
    model["dampingBending"] = damping;

    model["fixedPoints"] = in.manifest.pinned_vertices;

    // 2 = 前フレーム位置から始める。衝突のある初期状態で安定しやすい (サンプルもこれ)。
    model["initializationType"] = 2;

    // クライアントはワールド座標で送ってくるので変換しない (プロトコル §1)
    model["scale"]                    = json::array({1.0, 1.0, 1.0});
    model["rotation"]                 = json::array({0.0, 0.0, 0.0});
    model["translation"]              = json::array({0.0, 0.0, 0.0});
    model["translationBeforeScaling"] = json::array({0.0, 0.0, 0.0});

    model["initialVelocity"]             = json::array({0.0, 0.0, 0.0});
    model["maxVelocityMagnitude"]        = -1.0;
    model["hasNoGravZone"]               = false;
    model["noGravZoneThreshold"]         = 0.0;
    model["shuffleParallelizationGroup"] = true;

    return json{{"Models", json::array({model})}};
}

json build_parameters_json(const JobInput& in, int total_steps, double mean_edge) {
    const Manifest&    m = in.manifest;
    const ClothParams& c = m.sim.cloth;

    // 接触まわり。単位はメートル (プロトコル §1)。
    //
    // contactRadius は「この距離まで近づいたら接触とみなす」範囲。厚み + 衝突マージンが基本だが、
    // ⚠ メッシュの平均辺長に対して大きすぎると、隣り合う頂点同士が常時接触判定になり、
    //   布が細かく毛羽立って破綻する。実際に 6mm ピッチの服へ 5mm の接触半径を与えると
    //   全面が刺々しく崩れた (docs/decisions.md D-011)。
    //   Gaia のサンプルは辺長のおよそ 0.3 倍を使っているので、それを上限にする。
    const double requested   = c.thickness + m.sim.collision_margin;
    const double edge_limit  = 0.3 * mean_edge;
    const double contact_radius = std::max(std::min(requested, edge_limit), 1.0e-5);
    const double max_query_dis  = contact_radius * 1.5;

    if (requested > edge_limit) {
        spdlog::warn(
            "contactRadius を {:.5f} m から {:.5f} m に制限した "
            "(平均辺長 {:.5f} m に対して大きすぎるため)",
            requested, contact_radius, mean_edge);
    }

    json physics;
    physics["timeStep"]    = 1.0 / m.fps;
    physics["numSubsteps"] = m.substeps;
    physics["iterations"]  = m.sim.iterations;
    physics["numFrames"]   = total_steps;
    physics["gravity"] =
        json::array({m.sim.gravity[0], m.sim.gravity[1], m.sim.gravity[2]});

    physics["contactStiffness"]           = c.stretch_stiffness * 10.0;
    physics["contactRadius"]              = contact_radius;
    physics["thickness"]                  = c.thickness;
    physics["handleCollision"]            = true;
    physics["conservativeStepRelaxation"] = 0.4;
    physics["contactBVHReconstructionSteps"] = 32;

    physics["boundaryFrictionDynamic"] = c.friction;
    physics["boundaryFrictionStatic"]  = c.friction;

    // 地面や境界ボックスは服のシミュレーションでは邪魔なので切る。
    physics["usePlaneGround"]            = false;
    physics["useBowlGround"]             = false;
    physics["checkAndUpdateWorldBounds"] = false;
    physics["worldBounds"] = json::array({json::array({-1.0e4, -1.0e4, -1.0e4}),
                                          json::array({1.0e4, 1.0e4, 1.0e4})});

    physics["useNewton"]          = false;
    physics["useLineSearch"]      = true;
    physics["applyAcceleration"]  = false;
    physics["evaluateConvergence"] = false;
    physics["usePreconditioner"]  = true;
    physics["stepSizeGD"]         = 1.0;
    physics["associateGravityWithInertia"] = true;
    physics["doCollDetectionOnlyForFirstIteration"] = true;
    physics["collisionDetectionSubSteps"] = 1;
    physics["perMeshParallelization"] = true;
    physics["smoothSurfaceNormal"]    = true;

    // 出力はサーバーがメモリ上で扱うので、Gaia 側のファイル書き出しは全部切る
    physics["saveOutputs"]              = false;
    physics["outputStatistics"]         = false;
    physics["outputRecoveryState"]      = false;
    physics["saveSimulationParameters"] = false;
    physics["outputIntermediateState"]  = false;
    physics["outputExt"]                = "ply";
    physics["debug"]                    = false;
    physics["debugVerboseLvl"]          = 0;
    physics["showTimeConsumption"]      = false;
    physics["showSubstepProgress"]      = false;
    physics["shaderFolderPath"]         = "";

    json collision;
    collision["allowCCD"]            = true;
    collision["allowDCD"]            = true;
    collision["handleSelfCollision"] = true;

    json params{
        {"PhysicsParams", physics},
        {"CollisionParams", collision},
        {"ContactDetectorParams", json{{"maxQueryDis", max_query_dis}}},
        {"ColliderParams", json{{"ColliderMeshes", json::array()}, {"maxQueryDis", max_query_dis}}},
        {"ViewerParams", json{{"enableViewer", false}}},
        {"Deformers", json::array()},
    };

    // 調査用の逃げ道。環境変数 HAORI_GAIA_OVERRIDES に
    //   {"PhysicsParams":{"numSubsteps":20}, "ContactDetectorParams":{...}}
    // のような JSON を入れると、上で組んだ設定に上書きマージする。
    // Gaia のどのパラメータが効くかを再ビルド無しで試すためのもので、通常の運用では使わない。
    if (const char* env = std::getenv("HAORI_GAIA_OVERRIDES")) {
        try {
            json overrides = json::parse(env);
            for (auto& [section, values] : overrides.items()) {
                if (!values.is_object()) continue;
                for (auto& [key, value] : values.items()) {
                    params[section][key] = value;
                    spdlog::warn("Gaia パラメータを上書き: {}.{} = {}", section, key, value.dump());
                }
            }
        } catch (const std::exception& e) {
            spdlog::error("HAORI_GAIA_OVERRIDES を解釈できない: {}", e.what());
        }
    }
    return params;
}

// ---------------------------------------------------------------------------
// シミュレータ本体
// ---------------------------------------------------------------------------

class GaiaSimulator final : public ISimulator {
public:
    const char* name() const override { return "gaia-vbd"; }

    std::vector<float> simulate(const JobInput& input, const SimContext& ctx) override {
        const Manifest& m          = input.manifest;
        const int       num_frames = m.num_frames();
        const int       warmup     = m.sim.warmup_frames;
        const auto      cloth_nv   = static_cast<std::size_t>(m.cloth_num_vertices);

        TempDir tmp;
        spdlog::debug("Gaia 作業ディレクトリ: {}", tmp.dir());

        // --- 1. メッシュ資材の書き出し ---------------------------------------
        const std::string cloth_obj     = tmp.file("cloth.obj");
        const std::string body_obj      = tmp.file("body.obj");
        const std::string coloring_json = tmp.file("cloth.coloring.json");

        write_obj(cloth_obj, input.cloth_positions.data(), cloth_nv,
                  input.cloth_topology.data(), input.cloth_topology.size() / 3);

        // ボディはトポロジー確定用に1フレーム目だけ書く。以降はメモリから補間する。
        write_obj(body_obj, input.body_frames.data(), m.body_num_vertices,
                  input.body_topology.data(), input.body_topology.size() / 3);

        // --- 2. 頂点彩色 (これが無いと VBD は無言で何も解かない) --------------
        {
            const auto adjacency = build_vertex_adjacency(
                input.cloth_topology.data(), input.cloth_topology.size() / 3,
                m.cloth_num_vertices, /*include_bending=*/true);
            const auto categories = greedy_coloring(adjacency);

            if (!validate_coloring(adjacency, categories, m.cloth_num_vertices)) {
                throw ProtocolError("coloring_failed",
                                    "布メッシュの頂点彩色に失敗した。"
                                    "メッシュが非多様体である可能性がある。");
            }
            write_coloring_json(coloring_json, categories);
            spdlog::info("頂点彩色: {} 色 ({} 頂点)", categories.size(), m.cloth_num_vertices);
        }

        // --- 3. Gaia の設定ファイル -------------------------------------------
        const int total_steps = warmup + std::max(num_frames - 1, 0);

        const double mean_edge =
            mean_edge_length(input.cloth_positions.data(), m.cloth_num_vertices,
                             input.cloth_topology.data(), input.cloth_topology.size() / 3);
        if (mean_edge <= 0.0) {
            throw ProtocolError("degenerate_cloth", "布メッシュの辺長がゼロである");
        }
        spdlog::info("布の平均辺長: {:.5f} m", mean_edge);

        const std::string models_path = tmp.file("Models.json");
        const std::string params_path = tmp.file("Parameters.json");
        write_json(models_path, build_models_json(input, cloth_obj, coloring_json, mean_edge));
        write_json(params_path,
                   build_parameters_json(input, std::max(total_steps, 1), mean_edge));

        // --- 4. 組み立て -------------------------------------------------------
        HaoriClothFramework physics;

        auto collider_params = std::make_shared<GAIA::ColliderTriMeshBaseParams>();
        {
            json cj;
            cj["path"]                    = body_obj;
            cj["colliderType"]            = "HaoriBody";
            cj["scale"]                   = json::array({1.0, 1.0, 1.0});
            cj["rotation"]                = json::array({0.0, 0.0, 0.0});
            cj["translation"]             = json::array({0.0, 0.0, 0.0});
            cj["translationBeforeScaling"] = json::array({0.0, 0.0, 0.0});
            collider_params->fromJson(cj);
        }

        auto collider               = std::make_shared<HaoriBodyCollider>();
        collider->frames            = &input.body_frames;
        collider->body_num_vertices = m.body_num_vertices;
        collider->num_body_frames   = num_frames;
        collider->warmup_frames     = warmup;

        physics.bodyCollider       = collider;
        physics.bodyColliderParams = collider_params;

        try {
            physics.loadRunningparameters(models_path, params_path, tmp.dir());
            physics.initialize();
        } catch (const std::exception& e) {
            throw ProtocolError("gaia_init_failed",
                                std::string("Gaia の初期化に失敗した: ") + e.what());
        }

        if (physics.baseTriMeshesForSimulation.empty()) {
            throw ProtocolError("gaia_init_failed", "Gaia に布メッシュが登録されなかった");
        }
        GAIA::TriMeshFEM& cloth = *physics.baseTriMeshesForSimulation[0];
        if (static_cast<std::size_t>(cloth.numVertices()) != cloth_nv) {
            throw ProtocolError("gaia_init_failed",
                                "Gaia が読んだ布の頂点数が入力と一致しない: " +
                                    std::to_string(cloth.numVertices()) + " != " +
                                    std::to_string(cloth_nv));
        }

        // 並列グループが空だと VBD はどの頂点も解かないまま「成功」してしまう。
        // 無言で誤った結果を返す最悪のパターンなので、ここで必ず検査する。
        std::size_t verts_in_groups = 0;
        for (const auto& group : physics.vertexParallelGroups) verts_in_groups += group.size() / 2;

        spdlog::info(
            "Gaia 初期化完了: 布 {} 頂点 / 彩色 {} 群 / 並列グループ {} 群 ({} 頂点), "
            "コライダー {} 頂点, substeps={} iterations={}",
            cloth.numVertices(), cloth.verticesColoringCategories().size(),
            physics.vertexParallelGroups.size(), verts_in_groups, collider->numVertices(),
            physics.basePhysicsParams->numSubsteps, physics.basePhysicsParams->iterations);

        {
            // Gaia に実際に届いた材質パラメータ。マッピングを検証するために出す。
            auto& op = physics.objectParamsList->getObjectParamAs<GAIA::VBDObjectParamsTriMeshStVk>(0);
            spdlog::info(
                "  材質: density={} miu={} lambda={} bending={} dampingStVK={} initType={} "
                "fixed={} / dt={} contactRadius={} thickness={} handleCollision={}",
                op.density, op.miu, op.lambda, op.bendingStiffness, op.dampingStVK,
                op.initializationType, op.fixedPoints.size(), physics.basePhysicsParams->dt,
                physics.physicsParams().contactRadius, physics.physicsParams().thickness,
                physics.physicsParams().handleCollision);
        }

        if (physics.vertexParallelGroups.empty() || verts_in_groups == 0) {
            throw ProtocolError("gaia_no_parallel_groups",
                                "Gaia が並列グループを構築できなかった。"
                                "頂点彩色が読み込まれていない可能性がある。");
        }
        if (verts_in_groups != cloth_nv) {
            spdlog::warn("並列グループに入った頂点数が布の頂点数と一致しない: {} != {}",
                         verts_in_groups, cloth_nv);
        }

        // --- 5. フレームループ -------------------------------------------------
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(num_frames) * cloth_nv * 3);

        auto check_cancel = [&ctx] {
            if (ctx.is_cancelled && ctx.is_cancelled()) throw SimulationCancelled();
        };
        auto record = [&](int frame_index) {
            const auto pos = cloth.positions();
            for (std::size_t v = 0; v < cloth_nv; ++v) {
                out.push_back(pos(0, static_cast<Eigen::Index>(v)));
                out.push_back(pos(1, static_cast<Eigen::Index>(v)));
                out.push_back(pos(2, static_cast<Eigen::Index>(v)));
            }
            for (std::size_t i = out.size() - cloth_nv * 3; i < out.size(); ++i) {
                if (!std::isfinite(out[i])) {
                    throw ProtocolError(
                        "nan_detected",
                        "フレーム " + std::to_string(frame_index + m.frame_start) +
                            " で発散した (NaN/Inf)。substeps か iterations を増やすか、"
                            "stretch_stiffness を下げること。");
                }
            }
        };

        // --- 収束の見込みを事前に判定して警告する ---------------------------
        // VBD はガウス・ザイデル反復なので、慣性項に対して材質が硬すぎると
        // 既定の反復回数では収束せず、布が「重すぎる板」のように振る舞う。
        // 実測 (docs/decisions.md D-010) では、下の無次元数がおよそ 100 を超えると
        // iterations=20 では足りなくなる。
        {
            const double dt    = 1.0 / (m.fps * m.substeps);
            const double ratio = m.sim.cloth.stretch_stiffness * dt * dt /
                                 (m.sim.cloth.density * mean_edge * mean_edge);
            spdlog::info("剛性/慣性比 = {:.1f} (100 以下が目安)", ratio);

            if (ratio > 100.0 && ctx.warn) {
                const int suggested =
                    static_cast<int>(std::ceil(m.substeps * std::sqrt(ratio / 100.0)));
                ctx.warn("材質が慣性に対して硬く、この substeps では収束しきらない可能性がある"
                         "(剛性/慣性比 " + std::to_string(static_cast<int>(ratio)) +
                         ")。substeps を " + std::to_string(suggested) +
                         " 以上にするか、stretch_stiffness を下げること。");
            }
        }

        // 助走: ボディをフレーム1に留めたまま布を落ち着かせる
        for (int i = 0; i < warmup; ++i) {
            check_cancel();
            physics.runStep();
            ++physics.frameId;
            if (ctx.on_progress) {
                // 助走は全体の 10% 分として進捗に載せる
                ctx.on_progress(0, 0.1 * static_cast<double>(i + 1) / std::max(warmup, 1));
            }
        }

        // 出力フレーム1 = 助走後の状態(ボディはフレーム1の姿勢)
        record(0);
        if (ctx.on_progress) ctx.on_progress(1, num_frames > 0 ? 0.1 + 0.9 / num_frames : 1.0);

        for (int i = 1; i < num_frames; ++i) {
            check_cancel();
            physics.runStep();
            ++physics.frameId;
            record(i);
            if (ctx.on_progress) {
                ctx.on_progress(i + 1,
                                0.1 + 0.9 * static_cast<double>(i + 1) / num_frames);
            }
        }

        if (warmup == 0 && ctx.warn) {
            ctx.warn("warmup_frames が 0 のため、最初のフレームは入力そのままの姿勢になる。");
        }
        return out;
    }
};

}  // namespace

std::unique_ptr<ISimulator> make_gaia_simulator() {
    return std::make_unique<GaiaSimulator>();
}

}  // namespace haori
