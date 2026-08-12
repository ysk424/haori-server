# 設計判断の記録

指示書 §9 に従い、不明点・Gaia 側の想定外仕様に対する選択肢と判断を記録する。
**プロトコル (§6 / `docs/protocol.md`) は勝手に変更しない。**変更提案もここに書く。

---

## D-001: M0 の検証対象を VBDDynamics ではなく VBDCloth にする

**状況**
指示書 §8 の M0 は `Simulator/VBDDynamics`(サンプル `S01_Experiment_HybridModelsDrop...`)を
ビルド対象に挙げていた。

**判明したこと**
`VBDDynamics` は**テトラメッシュ(ソリッド)用**で、材質は `NeoHookean`、入力は `.t` ファイル。
布に必要なのは別ターゲットの `Simulator/VBDCloth` で、
`VBDClothPhysics` / `VBDClothDeformer` / `ClothContactDetector` と、
布＋コライダーのサンプル `S03_MultiLayerClothOnCollider` を持つ。

**選択肢**
- (a) 指示書どおり VBDDynamics で M0 を通す → 環境問題は潰せるが、布パスの検証にならない
- (b) VBDCloth を M0 の対象にする
- (c) 両方ビルドする

**判断: (b)**
M0 の目的は「CUDA/sm_120・Embree3・TBB・Eigen の環境問題を全部潰す」ことであり、
依存関係は両ターゲットで共通。であれば、以降のマイルストーンで実際に使う VBDCloth を
検証したほうが得られる情報が多い。VBDDynamics は必要になった時点でビルドする。

---

## D-002: 依存ライブラリは vcpkg ではなく公式プリビルドを直接展開する

**状況**
Gaia は Eigen 3.4.0 / oneTBB 2021.12.0 / Embree **3.13.1**(4系は非互換)を要求する。
開発機に vcpkg は未導入だった。

**選択肢**
- (a) vcpkg を導入して `embree3` などのポートを使う
- (b) 各プロジェクトの公式 Windows プリビルド zip を展開する

**判断: (b)**
理由:
1. Embree は **3.13.1 ちょうど**を狙う必要がある。vcpkg のポートは版が動くので固定しづらい
2. vcpkg の bootstrap とビルドは時間がかかる。プリビルド展開なら数十秒
3. `tools\setup_deps.ps1` 一本で再現でき、README の手順が短くなる

`external/thirdparty/` に展開し `.gitignore` 済み。
検出した `Eigen3_DIR` / `TBB_DIR` / `embree_DIR` は
`external/deps.env.ps1` に自動生成する(環境変数を恒久的に汚さないため)。

**補足**: Eigen は zip がソース配布で `Eigen3Config.cmake.in`(テンプレート)しか入っていない。
`find_package(Eigen3 CONFIG)` が探す `Eigen3Config.cmake` は install 時に生成されるため、
セットアップスクリプト内で `cmake --install` を1回通している(ヘッダオンリーなので数秒)。

---

## D-003: CMake のジェネレータを必ず明示する

**状況**
この開発機の PATH には winget 導入の **MinGW-w64 GCC 16.1.0 と ninja** が入っている
(`%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs...`)。
CMake をオプション無しで実行すると **Ninja + GCC** を選んでしまう。

実際、Eigen の configure で `The C compiler identification is GNU 16.1.0` となり失敗した。

**判断**
すべてのビルドスクリプトで `-G "Visual Studio 17 2022" -A x64` を明示する。
Gaia は CUDA + MSVC 前提であり、GCC では通らない。

**波及**
この罠は haori-server 本体の CMake でも同じなので、README のビルド手順にも明記する。

---

## D-004: 頂点グラフ彩色はサーバーが実行時に計算する

**状況**
`Modules/TriMesh/TriMesh.cpp:742` は `verticesColoringCategoriesPath` が空のとき
彩色を読まず、フォールバックも無い。`VBDClothPhysics` は彩色カテゴリを
並列グループとして回すため、空だと**無言でどの頂点も解かれない**。

Gaia のサンプルには `*.obj.vertexColoring.withBendingEnergy.json` が同梱されているが、
Blender から来る任意メッシュには存在しない。

**選択肢**
- (a) 事前計算を必須にしてクライアントに送らせる → プロトコル §6 の変更が必要。却下
- (b) `Simulator/GraphColoring` の実行ファイルをサブプロセスとして呼ぶ
- (c) `Modules/GraphColoring/` をサーバーにリンクして実行時に計算する

**判断: (c)**
`TriMeshVertexGraph::fromMesh()` + `mcs`/`greedy` + `convertToColoredCategories()` +
`balanceColoredCategories()` がヘッダとして取り込める。
ジョブ受信時に布とボディそれぞれ1回計算すればよく、プロトコルは変更不要。

計算コストがメッシュ規模に対して問題になる場合は、同一トポロジーのキャッシュを検討する
(未着手。M4 以降の課題)。

---

## D-005: アーキテクチャは Plan A (Gaia をモジュールとして組み込む)

**根拠**
`Modules/VBDCloth/VBDClothDeformer.h` の `VBDClothBaseDeformer` フックが
サブステップ単位で呼ばれ、任意メッシュの頂点を直接書き換えられる。
一方 `loadClothDeformers()` は JSON から `"Rotator"` / `"Translator"` しか生成できず、
**アニメーションするボディを Models.json 経由で与えることはできない**。

したがって Plan B(`VBDDynamics.exe` をサブプロセス起動)では
指示書 §1 の中核要件「ボディのアニメーションを送って服をシミュレートする」が満たせない。
**Plan A のみが選択肢**である(指示書 §4 の予測どおり)。

詳細は `docs/gaia-survey.md` を参照。

---

## 未解決 / 保留

- **メッシュのメモリ内構築**: `TriMeshFEM` はファイルパスからの読み込み前提。
  頂点・面配列の直接差し替えが可能かは M3 で確認する。だめなら一時 `.obj` 経由。
- **`stretch_stiffness` → `miu`/`lambda` の換算**: プロトコルは単一値、Gaia は2値。
  換算式は M3 で決めて `docs/gaia-survey.md` の対応表に追記する。
- **GPU パスでの固定頂点マスク同期**: `setFixedPointCloth()` 内の `toGPU()` が
  コメントアウトされている。布パスは CPU 中心のため当面影響しない見込みだが、
  `useGPU` を有効にする場合は要検証。
