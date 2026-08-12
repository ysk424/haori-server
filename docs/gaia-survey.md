# Gaia 調査結果 (Phase 0)

対象コミット: `AnkaChan/Gaia` main (2025-10-04 push 時点)。
調査日: 2026-08-12。実機ビルド・実測を伴う調査。

指示書 §4 の5項目に回答し、最後にアーキテクチャ判断 (Plan A / Plan B) を示す。

---

## 結論(先に)

**Plan A(Gaia をモジュールとしてサーバープロセスに組み込む)を採用する。**

Gaia には、外部からフレームごとの頂点位置を与える用途にそのまま使える
`VBDClothBaseDeformer` というフックが存在する。コライダーも「全頂点を固定頂点にした
三角メッシュモデル」として表現されており、これは Gaia 自身の公式サンプルが採っている方法である。
指示書 §4-1 が「なければ」と書いていた代替案が、実際には Gaia の標準的なやり方だった。

---

## 1. アニメーションするキネマティックコライダーのサポート状況

### 1.1 コライダーの表現方法

Gaia の VBDCloth には「コライダー」という専用の型は無い。
公式サンプル `Simulator/VBDCloth/ParameterGen/Parameters/S03_MultiLayerClothOnCollider/`
を読むと、円柱コライダーは**布と同じ `StVK_triMesh` モデル**として登録され、
`fixedPoints` に全頂点インデックス (0〜1223) を列挙することで固定されている。

```jsonc
{
  "materialName": "StVK_triMesh",
  "path": "...\\CylinderCollider_tri.obj",
  "fixedPoints": [0, 1, 2, ..., 1223]   // 全頂点
}
```

つまり **「動かない布」= コライダー** という扱いである。

### 1.2 フレームごとに位置を書き換える仕組み

`Simulator/Modules/VBDCloth/VBDClothDeformer.h` に Deformer フックがある。

```cpp
struct VBDClothBaseDeformer {
    virtual void operator()(VBDClothSimulationFramework& physics, FloatingType curTime,
                            int iFrame, int iSubstep, int iIter, FloatingType dt) = 0;
};
```

- **シミュレーションループ内から、サブステップ／イテレーション単位で呼ばれる**
- 引数に `curTime`(秒)、`iFrame`、`iSubstep`、`dt` が渡る
- `physics.baseTriMeshesForSimulation[iMesh]->vertex(vId)` は `Vec3Block`(参照)を返すので、
  **頂点座標を直接書き換えられる**

同ヘッダの `setFixedPointCloth(physics, meshes, verts)` が `fixedMask[vertexId] = true` を立てて
固定頂点にする。既存実装 `ClothDeformerRotator` / `ClothDeformerTranslater` が手本になる。

`loadClothDeformers()` は JSON から `"Rotator"` / `"Translator"` を生成するだけなので、
**Models.json 経由で任意のアニメーションを与えることはできない**(= CLI だけでは不可能)。
C++ 側で独自 Deformer を書いて登録する必要がある。これが Plan A を選ぶ決定的な理由。

### 1.3 haori-server での実装方針

`HaoriBodyDeformer : public VBDClothBaseDeformer` を実装する。

- コンストラクタでボディメッシュの全頂点を `setFixedPointCloth()` で固定
- ベイク済みのボディフレーム列 `float32[num_frames][num_verts][3]` を保持
- `operator()` で `curTime` からフレーム位置 `t = curTime * fps` を求め、
  `floor(t)` と `floor(t)+1` のフレームを**線形補間**して頂点に書き込む
  (指示書 §7「サブステップで線形補間」に対応)
- `warmup_frames` の間は `t` を 0 に固定してフレーム1の姿勢を保つ

---

## 2. ステップ実行 API

`Simulator/VBDCloth/main.cpp` は次の3行しかない。

```cpp
inputHanlder.handleInput(inModelInputFile, inParameterFile, outFolder, parser, physics);
physics.initialize();
physics.simulate();
```

`simulate()` は全フレームを回し切る**ブロッキング呼び出し**で、
1ステップずつ外から進める公開 API はそのままでは無い。

ただし Deformer フックが毎サブステップ呼ばれるため、**実質的にステップ単位の制御点はある**。
haori-server では次のいずれかを採る(M3 で確定させる):

- **(a) Deformer をコールバック点として使う**:
  Deformer 内でボディを更新し、同じフックでフレーム完了を検知して結果頂点を吸い出す。
  進捗報告とキャンセル判定もここで行う。`simulate()` は別スレッドで回す。
- **(b) `simulate()` 相当を自前で書く**:
  `BaseClothSimPhsicsFramework` / `VBDClothPhysics` の step 相当メソッドを直接呼ぶ。
  制御性は高いが Gaia 内部への結合が強くなる。

まず (a) で組む。フレーム完了ごとの進捗更新・キャンセル・NaN 監視は (a) で足りる見込み。

---

## 3. 布の材質パラメータ

Models.json の各モデルエントリ(`Simulator/Modules/TriMesh/TriMesh.cpp` がパース)に以下がある。

| Gaia のキー | 意味 | プロトコル §6.3 の対応 |
|---|---|---|
| `density` | 面密度 | `cloth.density` |
| `miu`, `lambda` | StVK のラメ定数(伸び剛性) | `cloth.stretch_stiffness` から換算 |
| `bendingStiffness` | 曲げ剛性 | `cloth.bend_stiffness` |
| `dampingStVK`, `dampingBending` | 減衰 | `cloth.damping` |
| `fixedPoints` | 固定頂点インデックス | `cloth.pinned_vertices` |
| `initializationType` | 初期化方法 | (サーバー既定値) |
| `materialName` | `"StVK_triMesh"` | 固定 |

Parameters.json 側(`VBDClothPhysicsParameters.h`)に反復回数・サブステップ・重力・接触関連がある。
`friction` / `thickness` / `collision_margin` は接触パラメータ
(`Modules/CollisionDetector/CollisionDetertionParameters.h`, `ContactRelations.h`)側。
正確なキー名の対応表は M3 で実装しながら本ドキュメントに追記する。

> 注意: プロトコル §6.3 の `stretch_stiffness` は単一値だが Gaia は `miu` / `lambda` の2値。
> サーバー側で妥当な換算を行う(指示書 §6.3「サーバー側で妥当なデフォルトにマップ」に従う)。

## 4. メッシュ入出力形式

- 布・コライダー(三角メッシュ): **`.obj`**。`Models.json` の `path` で指定
- ソリッド(VBDDynamics 側): `.t` (テトラメッシュ)
- 出力: `.ply` 連番(`outFolder` 配下)

**メモリ上から直接メッシュを構築する公開 API は無い**。`TriMeshFEM` は
`pObjectParams->path` からファイルを読む前提。Plan A では次のどちらかを採る:

- `TriMeshFEM` の頂点・面配列を構築後に直接差し替える(初期化フローを1箇所読めば可能)
- 一時ディレクトリに `.obj` を書き出して読ませる(確実だが I/O 経由)

M3 で前者を試し、難しければ後者にフォールバックする。

### ⚠ 4.1 頂点カラーリング JSON が必須

これは事前に把握しておくべき重要な制約。

`Modules/TriMesh/TriMesh.cpp:742` は、`verticesColoringCategoriesPath` が空文字列のとき
**何もしない(フォールバックが無い)**。

```cpp
if (pObjectParams->verticesColoringCategoriesPath != "") {
    MF::loadJson(pObjectParams->verticesColoringCategoriesPath, vertsColoring);
    ...
}
```

`VBDClothPhysics.cpp:89` 以降は `verticesColoringCategories()` を並列グループとして回すため、
これが空だと**どの頂点も解かれない**(無言で失敗する)。

Gaia のサンプルデータには `*.obj.vertexColoring.withBendingEnergy.json` が同梱されているが、
Blender から来る任意のメッシュには当然そんなものは無い。
**サーバーが実行時に頂点グラフ彩色を計算する必要がある。**

幸い実装は Gaia 内にある(`Simulator/Modules/GraphColoring/`):

- `TriMeshVertexGraph::fromMesh(pMesh)` — 三角メッシュから頂点隣接グラフを構築
- `mcs` / `greedy` の彩色アルゴリズム
- `convertToColoredCategories()` / `balanceColoredCategories(ratio)` — グループ化と均等化

スタンドアロン版 `Simulator/GraphColoring/GraphColoring.cpp` が使用例になる。
これらはヘッダとして取り込めるので、**サーバーにリンクして実行時に彩色を計算する**方針とする。
布とボディの両方について、ジョブ受信時に1回計算すればよい。

## 5. CUDA 実装の範囲と CPU フォールバック

- GPU 実装は `Modules/VBD/` に厚い(`VBDPhysicsCompute.cu`, `VBD_GeneralCompute.cu`,
  `VBD_NeoHookeanGPU.h`, `BVH/*.cuh` による GPU LBVH)
- `Modules/VBDCloth/` 側には `.cu` が無く、**布パスは現状 CPU 中心**と見られる
- `VBDPhysicsParameters` に `useGPU` フラグがあり、CPU/GPU を切り替える設計
- CPU 並列は TBB

**布に関しては GPU 依存度が低い**ため、Blackwell 固有の実行時問題を踏む可能性は当初の想定より低い。
一方で `setFixedPointCloth()` 内の `vertexFixedMaskBuffer->toGPU()` が
コメントアウトされている点は、GPU パスで固定頂点マスクが同期されない疑いがある。
布パスが CPU 中心なら影響しないが、M4 で `useGPU` を有効にする場合は要確認。

---

## ビルドに必要だった修正 (patches/)

実機ビルドで判明した Gaia 側の問題。すべて `patches/*.patch` として管理し、
`tools\apply_patches.ps1` で適用する(fork しない / 指示書 §9)。

### `0001-cuda-arch-blackwell.patch`
`Simulator/CMake/GAIA-config.cmake` が `set(CMAKE_CUDA_ARCHITECTURES 75;80;86)` と
決め打ちしており、Blackwell (sm_120) を含まない。
外から `-DCMAKE_CUDA_ARCHITECTURES` を渡せるよう `if(NOT DEFINED ...)` で包んだ。

### `0002-cxx17-for-cuda129.patch`
`Simulator/VBDCloth/CMakeLists.txt` が `CMAKE_CXX_STANDARD 11` / `CMAKE_CUDA_STANDARD 11`。
CUDA 12.9 の nvcc は C++11 を受け付けない。17 に引き上げた。

### `0003-fix-viewer-nogui-dup.patch`
`Simulator/Modules/Viewer/Viewer.cpp` の `GAIA_NO_GUI` 側で
`GAIA::Viewer::frameTick()` が **2回定義されている**(276 行目と 308 行目)。
MSVC が C2084 で停止する。Gaia 本体のバグで、GUI 無効ビルドが一度も試されていないと思われる。
後半の重複定義を削除した。

### 修正不要だったもの

- **CMake 4.x 互換**: `cmake_minimum_required(VERSION 3.5)` 未満のものは
  polyscope の examples/tests、imgui の examples、cmake-git-version-tracking の tests など
  **ビルド経路に乗らないディレクトリのみ**だった。ビルド対象は 3.5 / 3.13 で問題なし。
  (念のためビルドスクリプトでは `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` を渡している)
- **Embree 4 非互換問題**: 指示どおり 3.13.1 を使ったので発生せず。

## ビルド実測結果

| 項目 | 値 |
|---|---|
| ターゲット | `GAIA_VBDCloth.exe` (`Simulator/VBDCloth`) |
| 生成物 | `build/gaia-vbdcloth/Release/GAIA_VBDCloth.exe` |
| CUDA アーキテクチャ | `sm_120` (Blackwell) |
| ジェネレータ | Visual Studio 17 2022 / x64 |
| 構成 | Release, `BUILD_GUI=OFF` (`GAIA_NO_GUI`) |
| 結果 | **ビルド成功 (exit 0)** |

## 実行検証結果 (M0)

`tools\run_m0_sample.ps1` で Gaia 同梱の `S03_MultiLayerClothOnCollider`
(布5層 + 円柱コライダー)を 2 フレームだけ実行した。

| 項目 | 値 |
|---|---|
| 終了コード | **0** |
| 出力 | `.ply` 7 件(布5層 × 各フレーム + コライダー) |
| 布の規模 | 4900 頂点 / 9522 面 × 5 層 = 約 24,500 頂点 |
| コライダー | 1224 頂点 / 2444 面(全頂点 `fixedPoints`) |
| ソルバ設定 | substeps 20 / iterations 15 / timeStep 1/60 |
| 所要時間 | **約 2.27 秒/フレーム** (CPU) |
| 内訳 | Material Solve 1982ms / DCD 279ms (BVH 27ms + 検出 252ms) |

Embree・TBB・メッシュ読み込み・BVH による離散衝突検出(DCD)まで
一通り正常に動作することを確認した。**M0 完了。**

### 実行時にハマった点

**`ViewerParams.enableViewer` を false にする必要がある。**
`BUILD_GUI=OFF`(`GAIA_NO_GUI`)でビルドすると、Gaia のサンプル設定は
`enableViewer: true` のままなので `VBDClothPhysics.cpp:40` から
`initializeViewer()` → `GUINoCompliationError()` に入り、
プロセスが `0xC0000409` で異常終了する。

Parameters.json 側のフラグなのでパッチは不要。
haori-server では常に `enableViewer: false` 相当で初期化する。

**サンプルデータのコライダー用彩色 JSON が同梱されていない。**
`CylinderCollider_tri.obj.vertexColoring.json` は repo に存在せず
`Fail to open` の警告が出る(コライダーは全頂点固定なので実害なし)。
D-004 の「彩色はサーバーが実行時に計算する」判断を裏付ける実例。

### 性能の目安

上記は CPU・布 24,500 頂点で 2.27 秒/フレーム。
服1着(数千〜1万頂点程度)なら 1 フレームあたり 1 秒未満が期待できるが、
substeps / iterations と衝突対象(ボディ)の規模に強く依存する。
120 フレームのジョブで数分オーダーになる見込みで、
指示書 §7 の「進捗はフレーム完了ごとに更新」は妥当な粒度。
