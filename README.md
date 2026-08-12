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

```powershell
# 1. Gaia サブモジュールを取得 (約4GB、数分かかる)
git submodule update --init --recursive

# 2. 依存ライブラリ (Eigen 3.4.0 / oneTBB 2021.12.0 / Embree 3.13.1) を取得・展開
.\tools\setup_deps.ps1

# 3. Gaia に haori-server 用パッチを適用 (冪等。-Check で状況確認、-Revert で戻す)
.\tools\apply_patches.ps1

# 4. Gaia の VBDCloth をビルド (M0 の検証対象)
.\tools\build_gaia_cloth.ps1
#    → build\gaia-vbdcloth\Release\GAIA_VBDCloth.exe

# 5. 動作確認: 同梱の布サンプルを2フレームだけ走らせる
.\tools\run_m0_sample.ps1 -NumFrames 2
#    → build\m0_sample\out\*.ply が生成されれば OK
```

`-CudaArchitectures` で GPU アーキテクチャを変えられる(既定 `120` = Blackwell)。
Ampere なら `.\tools\build_gaia_cloth.ps1 -CudaArchitectures 86`。

### ⚠ ハマりどころ

- **PATH に MinGW GCC があると CMake が GCC を選ぶ**。
  この開発機では winget 版 MinGW-w64 GCC 16.1.0 と ninja が PATH にあり、
  何も指定しないと Ninja + GCC でコンフィグされて失敗する。
  付属スクリプトは `-G "Visual Studio 17 2022" -A x64` を明示しているので問題ないが、
  手で `cmake` を叩く場合は必ずジェネレータを指定すること(`docs/decisions.md` D-003)。
- **Embree は 3.13.1 を使うこと。4 系は非互換**。
- Gaia 本体には修正が必要な箇所が3つある(Blackwell 非対応の CUDA arch、C++11 指定、
  GUI 無効時の二重定義バグ)。すべて `patches/` で管理しており、fork はしない。
  内容は `docs/gaia-survey.md`「ビルドに必要だった修正」を参照。

## 進捗

| マイルストーン | 状態 |
|---|---|
| M0: Gaia のビルド環境確立 | **完了** — sm_120 でビルド・実行とも成功 |
| M1: Phase 0 調査 / Plan 決定 | **完了** — Plan A 採用 (`docs/gaia-survey.md`) |
| M2: HTTP + codec + ジョブ管理(ダミーシミュレータ) | 未着手 |
| M3: Gaia 統合(静的ボディ) | 未着手 |
| M4: アニメーションボディ対応 | 未着手 |
| M5: 安定化・README 完成 | 未着手 |
