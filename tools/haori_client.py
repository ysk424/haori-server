"""haori プロトコル (docs/protocol.md) の最小クライアント。

Python 標準ライブラリのみで実装する。理由は2つ:
  - haori-blender 側が同じ制約(標準ライブラリのみ)で動くので、実装の妥当性をここで確かめられる
  - E2E テストに外部依存を持ち込まない

サーバー開発用のツールなので bpy には依存しない。
"""

from __future__ import annotations

import json
import struct
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field

DEFAULT_URL = "http://127.0.0.1:8787"


class HaoriError(Exception):
    """サーバーが返した {"error": {"code", "message"}} を表す。"""

    def __init__(self, code: str, message: str, status: int = 0):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.status = status


# --- multipart ---------------------------------------------------------------


def build_multipart(parts: list[tuple[str, str, bytes]]) -> tuple[bytes, str]:
    """parts = [(name, content_type, payload), ...] を multipart/form-data に組む。

    戻り値は (body, content_type ヘッダ値)。
    """
    boundary = f"----haori{uuid.uuid4().hex}"
    out = bytearray()
    for name, content_type, payload in parts:
        out += f"--{boundary}\r\n".encode()
        out += f'Content-Disposition: form-data; name="{name}"\r\n'.encode()
        out += f"Content-Type: {content_type}\r\n\r\n".encode()
        out += payload
        out += b"\r\n"
    out += f"--{boundary}--\r\n".encode()
    return bytes(out), f"multipart/form-data; boundary={boundary}"


# --- codec (§3, §4) ----------------------------------------------------------


def pack_f32(values) -> bytes:
    """float32 リトルエンディアンの連続バイト列にする。"""
    return struct.pack(f"<{len(values)}f", *values)


def pack_u32(values) -> bytes:
    return struct.pack(f"<{len(values)}I", *values)


def decode_result(payload: bytes) -> tuple[int, int, list[float]]:
    """結果バイナリ (§4) を (num_frames, num_vertices, flat float list) に解く。"""
    if len(payload) < 16 or payload[:4] != b"HAOR":
        raise HaoriError("bad_result", "結果バイナリのマジックが 'HAOR' ではない")
    version, num_frames, num_vertices = struct.unpack_from("<III", payload, 4)
    if version != 1:
        raise HaoriError("bad_result", f"未対応の結果 version: {version}")

    count = num_frames * num_vertices * 3
    expected = 16 + count * 4
    if len(payload) != expected:
        raise HaoriError(
            "bad_result",
            f"本体長がヘッダと一致しない: {len(payload)} bytes (期待値 {expected} bytes)",
        )
    return num_frames, num_vertices, list(struct.unpack_from(f"<{count}f", payload, 16))


# --- ジョブ入力 ---------------------------------------------------------------


@dataclass
class Scene:
    """1ジョブ分の入力。座標はすべてワールド座標・メートル・Z-up (§1)。"""

    fps: float = 24.0
    frame_start: int = 1
    frame_end: int = 10
    substeps: int = 4

    body_frames: list[list[float]] = field(default_factory=list)  # フレームごとの [x,y,z,...]
    body_triangles: list[int] = field(default_factory=list)

    cloth_positions: list[float] = field(default_factory=list)
    cloth_triangles: list[int] = field(default_factory=list)
    pinned_vertices: list[int] = field(default_factory=list)

    sim: dict = field(default_factory=dict)

    @property
    def num_frames(self) -> int:
        return self.frame_end - self.frame_start + 1

    def manifest(self) -> dict:
        sim = {
            "gravity": [0.0, 0.0, -9.8],
            "iterations": 20,
            "cloth": {
                "density": 0.2,
                "stretch_stiffness": 1.0e4,
                "bend_stiffness": 0.5,
                "friction": 0.3,
                "damping": 0.01,
                "thickness": 0.002,
            },
            "collision_margin": 0.003,
            "warmup_frames": 10,
        }
        sim.update(self.sim)
        return {
            "version": 1,
            "fps": self.fps,
            "frame_start": self.frame_start,
            "frame_end": self.frame_end,
            "substeps": self.substeps,
            "sim": sim,
            "body": {
                "num_vertices": len(self.body_frames[0]) // 3 if self.body_frames else 0,
                "num_triangles": len(self.body_triangles) // 3,
            },
            "cloth": {
                "num_vertices": len(self.cloth_positions) // 3,
                "num_triangles": len(self.cloth_triangles) // 3,
                "pinned_vertices": self.pinned_vertices,
            },
        }

    def parts(self) -> list[tuple[str, str, bytes]]:
        flat_frames: list[float] = []
        for frame in self.body_frames:
            flat_frames.extend(frame)
        return [
            ("manifest", "application/json", json.dumps(self.manifest()).encode("utf-8")),
            ("body_topology", "application/octet-stream", pack_u32(self.body_triangles)),
            ("body_frames", "application/octet-stream", pack_f32(flat_frames)),
            (
                "cloth_mesh",
                "application/octet-stream",
                pack_f32(self.cloth_positions) + pack_u32(self.cloth_triangles),
            ),
        ]


# --- HTTP --------------------------------------------------------------------


class Client:
    def __init__(self, base_url: str = DEFAULT_URL, timeout: float = 30.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def _request(self, method: str, path: str, data=None, content_type=None) -> tuple[int, bytes]:
        req = urllib.request.Request(self.base_url + path, data=data, method=method)
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as res:
                return res.status, res.read()
        except urllib.error.HTTPError as e:
            body = e.read()
            try:
                err = json.loads(body)["error"]
                raise HaoriError(err["code"], err["message"], e.code) from None
            except (ValueError, KeyError):
                raise HaoriError("http_error", body.decode("utf-8", "replace"), e.code) from None

    def health(self) -> dict:
        _, body = self._request("GET", "/api/v1/health")
        return json.loads(body)

    def submit(self, scene: Scene) -> str:
        body, content_type = build_multipart(scene.parts())
        status, payload = self._request("POST", "/api/v1/jobs", body, content_type)
        if status != 202:
            raise HaoriError("unexpected_status", f"ジョブ投入の応答が {status}", status)
        return json.loads(payload)["job_id"]

    def status(self, job_id: str) -> dict:
        _, body = self._request("GET", f"/api/v1/jobs/{job_id}")
        return json.loads(body)

    def result(self, job_id: str) -> bytes:
        _, body = self._request("GET", f"/api/v1/jobs/{job_id}/result")
        return body

    def cancel(self, job_id: str) -> dict:
        _, body = self._request("DELETE", f"/api/v1/jobs/{job_id}")
        return json.loads(body)

    def wait(self, job_id: str, poll_interval: float = 0.2, timeout: float = 300.0,
             on_progress=None) -> dict:
        """done / error / cancelled になるまでポーリングして最後の status を返す。"""
        import time

        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            st = self.status(job_id)
            if on_progress and st != last:
                on_progress(st)
                last = st
            if st["state"] in ("done", "error", "cancelled"):
                return st
            time.sleep(poll_interval)
        raise HaoriError("timeout", f"ジョブ {job_id} が {timeout} 秒以内に終わらなかった")
