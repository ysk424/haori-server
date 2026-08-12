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

## D-006: C++ 依存も FetchContent ではなく自前取得にする

**状況**
haori-server 本体は cpp-httplib / nlohmann-json / spdlog を使う。当初 CMake の
`FetchContent` で取得しようとしたが、**すべてのダウンロードが TLS エラーで失敗した**。

```
status_code: 60
status_string: "SSL peer certificate or SSH remote key was not OK."
```

原因は D-003 と同根で、PATH 上の `cmake` が **MinGW 同梱版**
(`WinLibs...\mingw64\share\cmake-4.3`)であり、バンドルされた curl が CA 証明書ストアを
持っていないため。

**選択肢**
- (a) `CMAKE_TLS_VERIFY=0` を設定する → 検証を黙って無効化するので却下
- (b) Visual Studio 同梱の CMake 3.31.6-msvc6 を使う → どの cmake が拾われるかに依存し脆い
- (c) PowerShell (`Invoke-WebRequest`) で取得し、CMake は展開済みディレクトリを
      `add_subdirectory` する

**判断: (c)**
`tools\setup_deps.ps1` に3つを追加した。利点:
- .NET の TLS スタックを使うので証明書問題が起きない
- どの CMake でビルドしても同じ結果になる
- 2回目以降はネットワーク不要(オフラインビルド可)
- Gaia 側の依存と入手方法が統一される

CMakeLists は依存が無ければ `FATAL_ERROR` で `setup_deps.ps1` の実行を促す。

---

## D-007: cpp-httplib の `get_file_value()` は値返し

**状況(実装中に踏んだバグ)**
multipart のパートを次のように取り出したところ、POST するたびにサーバーが即クラッシュした。

```cpp
const std::string& require_part(...) {
    return req.get_file_value(name).content;   // ダングリング参照
}
```

`httplib::Request::get_file_value()` は `MultipartFormData` を**値で返す**ため、
その `.content` を参照で受けると一時オブジェクトへの参照になる。

**判断**
`req.files`(`MultipartFormDataMap`)を直接引いて、保持されている実体への参照を返す。
値返しを受けてもよいが、`body_frames` は数十 MB になるのでコピーは避けたい。

```cpp
auto it = req.files.find(name);
if (it == req.files.end()) throw ProtocolError("missing_part", ...);
return it->second.content;
```

---

## D-008: ボディは Deformer ではなく ColliderTriMeshBase として入れる

**状況**
Phase 0 では `VBDClothBaseDeformer` を使って布メッシュの頂点を書き換える方針(D-005)だったが、
実装時に `Modules/SpatialQuery/` を読み直したところ、Gaia には
**`ColliderTriMeshBase`(キネマティックコライダー)という一級の仕組み**があった。
さらに `ColliderTrimeshSequence` は `meshFiles` + `keyFrames` を受け取り、
**サブステップ／イテレーション単位で補間する**アニメーションコライダーそのものだった。

**選択肢**
- (a) 当初どおり Deformer で布を全頂点固定にして書き換える
- (b) `ColliderTrimeshSequence` をそのまま使う
- (c) `ColliderTriMeshBase` を継承した独自コライダーを haori-server 側に書く

**判断: (c)**
(b) は フレームごとに `.obj` を `loadObj()` で読む実装なので、
120 フレーム × 22 万頂点ではファイル I/O だけで実用にならない。
`update(frameId, substepId, iter, numSubsteps, numIters)` を実装するだけでよいので、
ベイク済み頂点をメモリ上で補間する `HaoriBodyCollider` を書いた。
トポロジー確定のために1フレーム目だけ `.obj` に書き出して読ませている。

`initializeCollider()` は仮想関数なので、`VBDClothSimulationFramework` を継承した
`HaoriClothFramework` で差し替えるだけで済み、**Gaia 本体へのパッチは不要**。

**warmup の実現方法**: コライダーのキーフレームを `warmup_frames` だけ後ろにずらす。
`frameId < keyFrames.front()` の間はフレーム1の姿勢に留まるので、
指示書 §7 の「フレーム1のボディ姿勢で布を落ち着かせる」がそのまま表現できる。

---

## D-009: Gaia 側の nlohmann/json (3.10.5) を使う

**状況**
`gaia_simulator.cpp` で `<nlohmann/json.hpp>`(external/thirdparty の 3.11.3)を
include したところ、Gaia の関数がリンクできなかった。

```
error LNK2019: 未解決の外部シンボル "TriMeshParams::fromJson(...json_abi_v3_11_3...)"
```

nlohmann/json はバージョンごとに `json_abi_v3_11_3` のような inline namespace を切るため、
Gaia (同梱 3.10.5) と別版を掴むと同じ関数が別シンボルになる。

**判断**
Gaia に渡す JSON を組む箇所では **`<Json/json.hpp>`(Gaia 同梱 3.10.5)** を使う。
codec 層は 3.11.3 のままでよい。`codec.h` は json 型を境界に出していない
(`parse_manifest` は `std::string` を取る)ので、2つの版が混ざることはない。

---

## D-010: bendingStiffness はメッシュ寸法で正規化する

**状況(実測で判明)**
Gaia 統合の直後、布がほとんど落ちなかった。ボディを遥か下に置いた**自由落下だけの試験**で
軌跡を測ると、加速せず**等速**で落ちていた(理論値の 2.3%)。

パラメータを振った結果:

| 条件 | 自由落下の再現率 |
|---|---|
| substeps 4, stretch 1e4 / 1e3 / 1e2 / 1e1 | **すべて 0.023**(剛性が全く効かない) |
| substeps 4 → 20 → 50 | 0.023 → 0.600 → 0.780 |
| iterations 20 → 100 | 0.023 → 0.227 |
| **bend_stiffness 0.5 → 0.05 → 0.005** | **0.023 → 0.171 → 0.652** |
| **density 0.2 → 2 → 20** | **0.023 → 0.246 → 1.011** |

犯人は**曲げ剛性と慣性の比**だった。Gaia の `bendingStiffness` は辺あたりの絶対量で、
メッシュの寸法にも解像度にも追従しない。Gaia のサンプルは cm 単位・辺長 1.0 前後だが、
haori のプロトコルはメートル単位で辺長が 0.005〜0.1 のオーダーになる。
そこへ既定値 0.5 をそのまま渡すと曲げ項が慣性項を圧倒し、
VBD が既定の反復回数で収束せず、布が平行移動すらできなくなる。

**判断**
`bendingStiffness = bend_stiffness × (平均辺長)²` として正規化する。
これで自由落下の再現率は 1.00 になり、`stretch_stiffness` も期待どおり効くようになった。

**併せて**: 収束の見込みを表す無次元数
`stretch_stiffness · dt² / (density · 平均辺長²)` を計算し、
100 を超えたらジョブの `message` で「substeps をいくつ以上にすべきか」を返す。
利用者のパラメータを黙って書き換えるのは避け、警告に留めた。

---

## D-011: contactRadius はメッシュの平均辺長で頭打ちにする

**状況(実機の服で判明)**
実在のキャラクター(ボディ 225,184 頂点 / 服 31,926 頂点)で流したところ、
シミュレーションは完走するが**服が全面的に毛羽立って破綻**した。

原因は接触半径。`contactRadius = thickness + collision_margin = 0.005 m` に対し、
この服の**平均辺長は 0.0048 m**。接触半径がほぼ辺長と同じになり、
隣り合う頂点同士が常時「接触」と判定されて反発し合っていた。
球の試験で問題が出なかったのは、あちらの辺長が 0.053 m と粗く、接触半径が辺長の 9% だったため。

**判断**
`contactRadius = min(thickness + collision_margin, 0.3 × 平均辺長)` とする。
0.3 は Gaia のサンプル(辺長 1.0 前後に対し contactRadius 0.3)に合わせた。
制限した場合はログに警告を出す。

**副次効果**: 偽の接触対が激減し、同じジョブの所要時間が **270 秒 → 45 秒** になった。

---

## 未解決 / 保留

- **メッシュのメモリ内構築**: `TriMeshFEM` はファイルパスからの読み込み前提。
  頂点・面配列の直接差し替えが可能かは M3 で確認する。だめなら一時 `.obj` 経由。
- **`stretch_stiffness` → `miu`/`lambda` の換算**: プロトコルは単一値、Gaia は2値。
  換算式は M3 で決めて `docs/gaia-survey.md` の対応表に追記する。
- **GPU パスでの固定頂点マスク同期**: `setFixedPointCloth()` 内の `toGPU()` が
  コメントアウトされている。布パスは CPU 中心のため当面影響しない見込みだが、
  `useGPU` を有効にする場合は要検証。
