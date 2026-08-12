// シミュレータの抽象。ダミー実装と Gaia 実装をここで差し替える。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../codec/codec.h"

namespace haori {

/// シミュレータからサーバー側へのコールバック群。
struct SimContext {
    /// フレーム1枚が終わるたびに呼ぶ。progress は 0.0〜1.0。
    std::function<void(int frames_done, double progress)> on_progress;

    /// true を返したらシミュレータは速やかに中断すること。
    std::function<bool()> is_cancelled;

    /// 警告(めり込み検出など)。ジョブの message としてクライアントに返る。
    std::function<void(const std::string&)> warn;
};

/// 中断要求で抜けたことを示す。JobQueue が cancelled 状態に落とす。
class SimulationCancelled : public std::exception {
public:
    const char* what() const noexcept override { return "simulation cancelled"; }
};

class ISimulator {
public:
    virtual ~ISimulator() = default;

    /// health エンドポイントの "engine" に出る名前。
    virtual const char* name() const = 0;

    /// 服の全フレーム頂点位置を返す (num_frames * cloth_num_vertices * 3, ワールド座標)。
    virtual std::vector<float> simulate(const JobInput& input, const SimContext& ctx) = 0;
};

/// M2 用のダミー。Gaia を使わず、布を重力で落として床で止めるだけ。
/// HTTP・codec・ジョブ管理の貫通確認と、haori-blender 側の開発に使う。
std::unique_ptr<ISimulator> make_dummy_simulator();

}  // namespace haori
