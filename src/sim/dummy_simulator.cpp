// M2 用のダミーシミュレータ。
//
// Gaia を使わず、布の頂点を重力で自由落下させ、ボディの下端 (最小 Z) を床として止める。
// 目的は「HTTP → codec → ジョブ管理 → 結果バイナリ」の貫通確認と、
// haori-server 完成前に haori-blender 側の開発を進められるようにすること。
// 物理的な正しさは一切求めていない。
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "simulator.h"

namespace haori {
namespace {

class DummySimulator final : public ISimulator {
public:
    const char* name() const override { return "dummy"; }

    std::vector<float> simulate(const JobInput& input, const SimContext& ctx) override {
        const Manifest& m  = input.manifest;
        const int   frames = m.num_frames();
        const auto  nv     = static_cast<std::size_t>(m.cloth_num_vertices);
        const double dt    = 1.0 / m.fps;

        // pinned は動かさない。プロトコルの pinned_vertices が効いていることの確認になる。
        std::unordered_set<std::uint32_t> pinned(m.pinned_vertices.begin(),
                                                 m.pinned_vertices.end());

        // 床の高さ = 全フレームのボディ頂点の最小 Z。
        // 布が落ちきって止まる位置が見えるので、Blender 側で結果を目視確認しやすい。
        const float floor_z = compute_floor_z(input);

        std::vector<float> pos = input.cloth_positions;          // 現在位置
        std::vector<float> vel(pos.size(), 0.0f);                // 速度
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(frames) * nv * 3);

        const float gz = static_cast<float>(m.sim.gravity[2]);
        const float gx = static_cast<float>(m.sim.gravity[0]);
        const float gy = static_cast<float>(m.sim.gravity[1]);
        const float damping = static_cast<float>(std::clamp(m.sim.cloth.damping, 0.0, 1.0));

        // warmup も1フレーム分の積分として消化する(結果には出さない)。
        for (int w = 0; w < m.sim.warmup_frames; ++w) {
            step(pos, vel, pinned, gx, gy, gz, damping, static_cast<float>(dt), floor_z);
        }

        for (int f = 0; f < frames; ++f) {
            if (ctx.is_cancelled && ctx.is_cancelled()) throw SimulationCancelled();

            // substeps に分けて積分する(実装の形を Gaia 版に寄せておく)
            const float sub_dt = static_cast<float>(dt) / static_cast<float>(m.substeps);
            for (int s = 0; s < m.substeps; ++s) {
                step(pos, vel, pinned, gx, gy, gz, damping, sub_dt, floor_z);
            }

            out.insert(out.end(), pos.begin(), pos.end());

            if (ctx.on_progress) {
                ctx.on_progress(f + 1, static_cast<double>(f + 1) / static_cast<double>(frames));
            }
        }

        if (ctx.warn) {
            ctx.warn("ダミーシミュレータで実行した(布は重力落下のみ)。物理的な結果ではない。");
        }
        return out;
    }

private:
    static float compute_floor_z(const JobInput& input) {
        float min_z = std::numeric_limits<float>::max();
        for (std::size_t i = 2; i < input.body_frames.size(); i += 3) {
            min_z = std::min(min_z, input.body_frames[i]);
        }
        return (min_z == std::numeric_limits<float>::max()) ? 0.0f : min_z;
    }

    static void step(std::vector<float>& pos, std::vector<float>& vel,
                     const std::unordered_set<std::uint32_t>& pinned, float gx, float gy, float gz,
                     float damping, float dt, float floor_z) {
        const std::size_t nv = pos.size() / 3;
        for (std::size_t v = 0; v < nv; ++v) {
            if (pinned.count(static_cast<std::uint32_t>(v))) continue;

            const std::size_t i = v * 3;
            vel[i + 0] = (vel[i + 0] + gx * dt) * (1.0f - damping);
            vel[i + 1] = (vel[i + 1] + gy * dt) * (1.0f - damping);
            vel[i + 2] = (vel[i + 2] + gz * dt) * (1.0f - damping);

            pos[i + 0] += vel[i + 0] * dt;
            pos[i + 1] += vel[i + 1] * dt;
            pos[i + 2] += vel[i + 2] * dt;

            if (pos[i + 2] < floor_z) {
                pos[i + 2] = floor_z;
                vel[i + 2] = 0.0f;
            }
        }
    }
};

}  // namespace

std::unique_ptr<ISimulator> make_dummy_simulator() {
    return std::make_unique<DummySimulator>();
}

}  // namespace haori
