"""haori-server の E2E テスト。

実際に haori-server.exe を起動し、docs/protocol.md の全エンドポイントを叩く。

    pytest tests/e2e -v

サーバーの実行ファイルは build/server/<Config>/haori-server.exe を自動で探す。
HAORI_SERVER_EXE 環境変数で明示指定もできる。
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from haori_client import Client, HaoriError, Scene, decode_result  # noqa: E402


def find_server_exe() -> Path:
    explicit = os.environ.get("HAORI_SERVER_EXE")
    if explicit:
        return Path(explicit)
    for config in ("Release", "RelWithDebInfo", "Debug"):
        candidate = REPO_ROOT / "build" / "server" / config / "haori-server.exe"
        if candidate.exists():
            return candidate
    pytest.skip("haori-server.exe が見つからない。tools\\build_server.ps1 を先に実行すること。")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="session")
def server():
    """テストセッション中だけサーバーを起動する。"""
    exe = find_server_exe()
    port = free_port()
    proc = subprocess.Popen(
        [str(exe), "--port", str(port), "--engine", "dummy", "--log", "warn"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    client = Client(f"http://127.0.0.1:{port}", timeout=60.0)

    # 起動待ち
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            out = proc.stdout.read().decode("utf-8", "replace") if proc.stdout else ""
            pytest.fail(f"サーバーが起動直後に終了した (exit {proc.returncode}):\n{out}")
        try:
            client.health()
            break
        except OSError:
            time.sleep(0.1)
    else:
        proc.kill()
        pytest.fail("サーバーが 20 秒以内に応答しなかった")

    yield client

    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()


def make_scene(frames: int = 5, grid: int = 4, pinned=None) -> Scene:
    """三角形1枚のボディと、格子状の布からなる最小シーン。"""
    body_rest = [-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 0.0, 1.0, 0.0]
    body_frames = [list(body_rest) for _ in range(frames)]

    cloth_pos: list[float] = []
    for iy in range(grid):
        for ix in range(grid):
            cloth_pos.extend([ix * 0.1, iy * 0.1, 2.0])

    cloth_tri: list[int] = []
    for iy in range(grid - 1):
        for ix in range(grid - 1):
            a = iy * grid + ix
            cloth_tri.extend([a, a + grid, a + 1, a + 1, a + grid, a + grid + 1])

    return Scene(
        fps=24.0,
        frame_start=1,
        frame_end=frames,
        substeps=2,
        body_frames=body_frames,
        body_triangles=[0, 1, 2],
        cloth_positions=cloth_pos,
        cloth_triangles=cloth_tri,
        pinned_vertices=list(pinned or []),
        sim={"warmup_frames": 0},
    )


# --- §2 health ---------------------------------------------------------------


def test_health(server):
    h = server.health()
    assert h["status"] == "ok"
    assert h["engine"] == "dummy"
    assert "version" in h
    assert "gpu" in h


# --- §2 ジョブのライフサイクル ------------------------------------------------


def test_full_job_roundtrip(server):
    scene = make_scene(frames=5, grid=4)
    job_id = server.submit(scene)
    assert job_id

    final = server.wait(job_id, timeout=60.0)
    assert final["state"] == "done", final
    assert final["progress"] == pytest.approx(1.0)
    assert final["frames_done"] == scene.num_frames

    payload = server.result(job_id)
    num_frames, num_vertices, data = decode_result(payload)

    assert num_frames == scene.num_frames
    assert num_vertices == len(scene.cloth_positions) // 3
    assert len(data) == num_frames * num_vertices * 3
    assert all(x == x for x in data), "NaN が含まれている"


def test_result_topology_matches_input(server):
    """§4: 結果の頂点数は入力トポロジーと同一であること。"""
    scene = make_scene(frames=3, grid=6)
    job_id = server.submit(scene)
    assert server.wait(job_id, timeout=60.0)["state"] == "done"

    _, num_vertices, _ = decode_result(server.result(job_id))
    assert num_vertices == len(scene.cloth_positions) // 3


def test_pinned_vertices_do_not_move(server):
    """pinned_vertices が固定されていること。"""
    scene = make_scene(frames=4, grid=4, pinned=[0, 3])
    job_id = server.submit(scene)
    assert server.wait(job_id, timeout=60.0)["state"] == "done"

    num_frames, num_vertices, data = decode_result(server.result(job_id))
    for f in range(num_frames):
        base = f * num_vertices * 3
        for v in (0, 3):
            for c in range(3):
                assert data[base + v * 3 + c] == pytest.approx(scene.cloth_positions[v * 3 + c])

    # 固定していない頂点は落ちている
    assert data[(num_frames - 1) * num_vertices * 3 + 1 * 3 + 2] < scene.cloth_positions[1 * 3 + 2]


def test_gravity_is_applied(server):
    scene = make_scene(frames=6, grid=4)
    job_id = server.submit(scene)
    assert server.wait(job_id, timeout=60.0)["state"] == "done"

    num_frames, num_vertices, data = decode_result(server.result(job_id))

    def centroid_z(f: int) -> float:
        base = f * num_vertices * 3
        return sum(data[base + i * 3 + 2] for i in range(num_vertices)) / num_vertices

    assert centroid_z(num_frames - 1) < centroid_z(0), "布が落ちていない"


# --- §2 キャンセル -------------------------------------------------------------


def test_cancel_unknown_job_is_404(server):
    with pytest.raises(HaoriError) as e:
        server.cancel("ffffffffffffffffffffffffffffffff")
    assert e.value.code == "job_not_found"
    assert e.value.status == 404


def test_status_unknown_job_is_404(server):
    with pytest.raises(HaoriError) as e:
        server.status("ffffffffffffffffffffffffffffffff")
    assert e.value.code == "job_not_found"


def test_cancel_finished_job_returns_false(server):
    scene = make_scene(frames=3, grid=4)
    job_id = server.submit(scene)
    assert server.wait(job_id, timeout=60.0)["state"] == "done"
    assert server.cancel(job_id)["cancelled"] is False


# --- §5 エラー ------------------------------------------------------------------


def test_result_before_done_is_409(server):
    """完了前に /result を取ると 409。

    ダミーシミュレータは速いので、フレーム数ではなく substeps と warmup_frames で
    計算量だけを増やす。こうすると結果バイナリは数 MB のまま実行時間だけ伸びるので、
    完了前に /result を叩ける。
    """
    scene = make_scene(frames=20, grid=100)
    scene.substeps = 300
    scene.sim = {"warmup_frames": 500}

    job_id = server.submit(scene)
    try:
        assert server.status(job_id)["state"] in ("queued", "running")

        with pytest.raises(HaoriError) as e:
            server.result(job_id)
        assert e.value.code == "job_not_done"
        assert e.value.status == 409
    finally:
        server.cancel(job_id)


def test_cancel_running_job(server):
    """実行中のジョブをキャンセルすると cancelled で終わること。"""
    scene = make_scene(frames=20, grid=100)
    scene.substeps = 300
    scene.sim = {"warmup_frames": 500}

    job_id = server.submit(scene)
    assert server.cancel(job_id)["cancelled"] is True

    final = server.wait(job_id, timeout=60.0)
    assert final["state"] == "cancelled", final

    # キャンセル済みジョブの結果は取れない
    with pytest.raises(HaoriError) as e:
        server.result(job_id)
    assert e.value.code == "job_not_done"


def test_missing_part_is_rejected(server):
    """必須パートが欠けていれば 400 / missing_part。"""
    from haori_client import build_multipart

    scene = make_scene()
    parts = [p for p in scene.parts() if p[0] != "body_frames"]
    body, content_type = build_multipart(parts)

    with pytest.raises(HaoriError) as e:
        server._request("POST", "/api/v1/jobs", body, content_type)
    assert e.value.code == "missing_part"
    assert e.value.status == 400


def test_part_size_mismatch_is_rejected(server):
    """manifest の頂点数とバイナリ長が食い違えば 400。"""
    from haori_client import build_multipart

    scene = make_scene()
    parts = scene.parts()
    parts = [(n, c, p[:-4] if n == "body_frames" else p) for n, c, p in parts]
    body, content_type = build_multipart(parts)

    with pytest.raises(HaoriError) as e:
        server._request("POST", "/api/v1/jobs", body, content_type)
    assert e.value.code == "part_size_mismatch"


def test_bad_manifest_is_rejected(server):
    from haori_client import build_multipart

    scene = make_scene()
    parts = [
        (n, c, b"{ this is not json" if n == "manifest" else p) for n, c, p in scene.parts()
    ]
    body, content_type = build_multipart(parts)

    with pytest.raises(HaoriError) as e:
        server._request("POST", "/api/v1/jobs", body, content_type)
    assert e.value.code == "manifest_parse_error"


def test_out_of_range_topology_is_rejected(server):
    """三角形が範囲外の頂点を指していれば 400。"""
    from haori_client import build_multipart, pack_u32

    scene = make_scene()
    parts = [
        (n, c, pack_u32([0, 1, 999]) if n == "body_topology" else p) for n, c, p in scene.parts()
    ]
    body, content_type = build_multipart(parts)

    with pytest.raises(HaoriError) as e:
        server._request("POST", "/api/v1/jobs", body, content_type)
    assert e.value.code == "topology_out_of_range"


def test_invalid_frame_range_is_rejected(server):
    import json as _json

    from haori_client import build_multipart

    scene = make_scene()
    manifest = scene.manifest()
    manifest["frame_start"] = 100
    manifest["frame_end"] = 1
    parts = [
        (n, c, _json.dumps(manifest).encode() if n == "manifest" else p)
        for n, c, p in scene.parts()
    ]
    body, content_type = build_multipart(parts)

    with pytest.raises(HaoriError) as e:
        server._request("POST", "/api/v1/jobs", body, content_type)
    assert e.value.code == "invalid_frame_range"
