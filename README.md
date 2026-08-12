# haori-server

[Gaia](https://github.com/AnkaChan/Gaia) (VBD: Vertex Block Descent) を使ったローカル布シミュレーションサーバー。

クライアント(Blender 拡張 [haori-blender](../haori-blender) など)から
**ボディのアニメーション(フレームごとの頂点位置)**と**静止状態の服メッシュ**を受け取り、
服のシミュレーション結果を返す。Blender には依存しない独立プロセス。

- プロトコル仕様: [`docs/protocol.md`](docs/protocol.md) (正本)
- Gaia 調査結果: [`docs/gaia-survey.md`](docs/gaia-survey.md)
- 設計判断の記録: [`docs/decisions.md`](docs/decisions.md)
- ライセンス: Apache-2.0 (Gaia と同じ)

## 動作環境

| 項目 | 要件 | 開発機での実測 |
|---|---|---|
| OS | Windows 11 | Windows 11 Home 26200 |
| GPU | NVIDIA (Blackwell 対応が必要な場合は CUDA 12.8+) | RTX 5070 Ti (sm_120) |
| CUDA | 12.8 以降 | 12.9 (V12.9.41) |
| コンパイラ | Visual Studio 2022 (MSVC v143) | Build Tools 2022 17.14 / MSVC 14.44 |
| CMake | 3.24+ | 4.3.2 |

## ビルド手順

準備中 (M0)。手順が確定次第ここに記載する。

```powershell
# 1. 依存ライブラリ (Eigen 3.4.0 / oneTBB 2021.12.0 / Embree 3.13.1) を取得
.\tools\setup_deps.ps1

# 2. Gaia サブモジュールの取得とパッチ適用
git submodule update --init --recursive
.\tools\apply_patches.ps1
```
