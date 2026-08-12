# haori プロトコル仕様 v1

**このドキュメントが正本**。haori-blender 側の `docs/protocol.md` はこの写し。
実装の都合で変更したくなった場合も勝手に変えず、`docs/decisions.md` に提案として記録し、
haori-server / haori-blender 双方を合わせて変更すること。

HTTP/1.1、`http://127.0.0.1:8787` をデフォルトとする。ジョブベースの非同期 API。
サーバーは `127.0.0.1` バインドを既定とし、認証は実装しない(ローカル用途)。

## 1. 座標系・単位

- 右手系 **Z-up**(Blender ネイティブ)
- 単位 **メートル**
- **float32 リトルエンディアン**
- クライアントがワールド座標に変換済みの頂点を送る(サーバーは座標変換しない)

## 2. エンドポイント

| Method | Path | 説明 |
|---|---|---|
| GET | `/api/v1/health` | `{"status":"ok","engine":"gaia-vbd","version":...,"gpu":"..."}` |
| POST | `/api/v1/jobs` | ジョブ投入。multipart/form-data(§3)。→ `{"job_id":"..."}` (202) |
| GET | `/api/v1/jobs/{id}` | `{"state":"queued\|running\|done\|error\|cancelled","progress":0.0-1.0,"frames_done":N,"message":...}` |
| GET | `/api/v1/jobs/{id}/result` | 完了後のみ。`application/octet-stream`(§4) |
| DELETE | `/api/v1/jobs/{id}` | キャンセル |

## 3. ジョブ投入 (multipart parts)

### 3.1 `manifest` (application/json)

```json
{
  "version": 1,
  "fps": 24.0,
  "frame_start": 1,
  "frame_end": 120,
  "substeps": 4,
  "sim": {
    "gravity": [0, 0, -9.8],
    "iterations": 20,
    "cloth": {
      "density": 0.2,
      "stretch_stiffness": 1.0e4,
      "bend_stiffness": 0.5,
      "friction": 0.3,
      "damping": 0.01,
      "thickness": 0.002
    },
    "collision_margin": 0.003,
    "warmup_frames": 10
  },
  "body":  {"num_vertices": 0, "num_triangles": 0},
  "cloth": {"num_vertices": 0, "num_triangles": 0, "pinned_vertices": []}
}
```

- `warmup_frames`: フレーム1のボディ姿勢で布を落ち着かせるための助走ステップ数
- `pinned_vertices`: 布側の固定頂点インデックス(オプション、Blender の頂点グループ由来)
- `sim` 内のキーは Gaia 調査の結果に合わせて**サーバー側で妥当なデフォルトにマップ**する。
  未知キーは無視する。

### 3.2 `body_topology` (application/octet-stream)

`uint32 × num_triangles × 3`(三角形インデックス)

### 3.3 `body_frames` (application/octet-stream)

`float32 × num_frames × num_vertices × 3`
フレーム連続(`frame_start` → `frame_end`)、トポロジーは全フレーム不変。

### 3.4 `cloth_mesh` (application/octet-stream)

`float32 × num_vertices × 3` の静止位置 + 続けて `uint32 × num_triangles × 3`

## 4. 結果バイナリ

```
[header]
  magic   : "HAOR" (4 bytes)
  version : uint32 = 1
  num_frames   : uint32
  num_vertices : uint32   // 服。入力トポロジーと同一
[payload]
  float32 × num_frames × num_vertices × 3
```

## 5. エラー

```json
{"error": {"code": "...", "message": "..."}}
```

トポロジー不整合・NaN 検出・GPU 初期化失敗などは 4xx/5xx とコードで区別する。
`message` はクライアントの UI にそのまま表示される前提で書くこと。
