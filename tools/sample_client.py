"""動作確認用の最小クライアント: 上下する球コライダー + その上に浮かぶ布。

使い方:
    python tools\\sample_client.py [--url http://127.0.0.1:8787] [--dump-obj out_dir]

サーバーが起動していること。engine=dummy でも通る(布が落ちるだけの結果になる)。
"""

from __future__ import annotations

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from haori_client import Client, HaoriError, Scene, decode_result  # noqa: E402


def make_uv_sphere(radius: float, rings: int, segments: int, center=(0.0, 0.0, 0.0)):
    """UV 球を (positions, triangles) で返す。positions は平坦な [x,y,z,...]。"""
    positions: list[float] = []
    cx, cy, cz = center

    # 極を含めた rings+1 段
    for i in range(rings + 1):
        theta = math.pi * i / rings
        for j in range(segments):
            phi = 2.0 * math.pi * j / segments
            positions.extend(
                [
                    cx + radius * math.sin(theta) * math.cos(phi),
                    cy + radius * math.sin(theta) * math.sin(phi),
                    cz + radius * math.cos(theta),
                ]
            )

    triangles: list[int] = []
    for i in range(rings):
        for j in range(segments):
            a = i * segments + j
            b = i * segments + (j + 1) % segments
            c = (i + 1) * segments + j
            d = (i + 1) * segments + (j + 1) % segments
            triangles.extend([a, c, b, b, c, d])
    return positions, triangles


def make_grid(size: float, n: int, z: float):
    """XY 平面の格子布を (positions, triangles) で返す。"""
    positions: list[float] = []
    half = size * 0.5
    for iy in range(n):
        for ix in range(n):
            positions.extend(
                [-half + size * ix / (n - 1), -half + size * iy / (n - 1), z]
            )

    triangles: list[int] = []
    for iy in range(n - 1):
        for ix in range(n - 1):
            a = iy * n + ix
            b = a + 1
            c = a + n
            d = c + 1
            triangles.extend([a, c, b, b, c, d])
    return positions, triangles


def write_obj(path: str, positions: list[float], triangles: list[int]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for i in range(0, len(positions), 3):
            f.write(f"v {positions[i]:.6f} {positions[i+1]:.6f} {positions[i+2]:.6f}\n")
        for i in range(0, len(triangles), 3):
            f.write(f"f {triangles[i]+1} {triangles[i+1]+1} {triangles[i+2]+1}\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://127.0.0.1:8787")
    ap.add_argument("--frames", type=int, default=24, help="シミュレートするフレーム数")
    ap.add_argument("--grid", type=int, default=21, help="布の1辺の頂点数")
    ap.add_argument("--dump-obj", metavar="DIR", help="結果を .obj 連番で書き出す")
    args = ap.parse_args()

    client = Client(args.url)

    try:
        health = client.health()
    except (HaoriError, OSError) as e:
        print(f"サーバーに接続できない ({args.url}): {e}", file=sys.stderr)
        print("haori-server.exe を起動しているか確認すること。", file=sys.stderr)
        return 1
    print(f"サーバー: engine={health['engine']} version={health['version']} gpu={health['gpu']}")

    # --- シーン構築 -----------------------------------------------------------
    body_rest, body_tri = make_uv_sphere(radius=0.5, rings=16, segments=24, center=(0, 0, 0.5))
    cloth_pos, cloth_tri = make_grid(size=1.6, n=args.grid, z=1.4)

    # 球を上下に動かす(アニメーションするコライダーの検証)
    body_frames = []
    for f in range(args.frames):
        t = f / max(args.frames - 1, 1)
        dz = 0.3 * math.sin(2.0 * math.pi * t)
        frame = list(body_rest)
        for i in range(2, len(frame), 3):
            frame[i] += dz
        body_frames.append(frame)

    scene = Scene(
        fps=24.0,
        frame_start=1,
        frame_end=args.frames,
        substeps=4,
        body_frames=body_frames,
        body_triangles=body_tri,
        cloth_positions=cloth_pos,
        cloth_triangles=cloth_tri,
        pinned_vertices=[],
    )

    nv_body = len(body_rest) // 3
    nv_cloth = len(cloth_pos) // 3
    payload_mb = (args.frames * nv_body * 12) / (1024 * 1024)
    print(
        f"ボディ {nv_body} 頂点 / {len(body_tri)//3} 面, "
        f"布 {nv_cloth} 頂点 / {len(cloth_tri)//3} 面, "
        f"{args.frames} フレーム (約 {payload_mb:.1f} MB 送信)"
    )

    # --- 投入・待機 -----------------------------------------------------------
    job_id = client.submit(scene)
    print(f"job_id = {job_id}")

    def on_progress(st):
        print(f"  {st['state']:9s} {st['progress']*100:5.1f}%  frames_done={st['frames_done']}")

    final = client.wait(job_id, on_progress=on_progress)
    if final["state"] != "done":
        print(f"ジョブが完了しなかった: {final}", file=sys.stderr)
        return 1
    if final.get("message"):
        print(f"message: {final['message']}")

    payload = client.result(job_id)
    num_frames, num_vertices, data = decode_result(payload)
    print(f"結果: {num_frames} フレーム × {num_vertices} 頂点 ({len(payload)} bytes)")

    if num_vertices != nv_cloth:
        print(f"頂点数が入力と一致しない: {num_vertices} != {nv_cloth}", file=sys.stderr)
        return 1

    # 最初と最後のフレームの重心 Z を出して、動いていることを確かめる
    def centroid_z(frame_index: int) -> float:
        base = frame_index * num_vertices * 3
        return sum(data[base + 2 + i * 3] for i in range(num_vertices)) / num_vertices

    print(f"布の重心 Z: 先頭 {centroid_z(0):.4f} → 末尾 {centroid_z(num_frames-1):.4f}")

    if args.dump_obj:
        os.makedirs(args.dump_obj, exist_ok=True)
        for f in range(num_frames):
            base = f * num_vertices * 3
            write_obj(
                os.path.join(args.dump_obj, f"cloth_{f:04d}.obj"),
                data[base : base + num_vertices * 3],
                cloth_tri,
            )
        print(f"{num_frames} 個の .obj を {args.dump_obj} に書き出した")

    return 0


if __name__ == "__main__":
    sys.exit(main())
