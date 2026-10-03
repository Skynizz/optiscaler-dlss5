"""A synthetic dump in the edit cache's format, for testing the tools and the algorithm without a game.

The scene: a textured background panning left, a foreground block moving right in front of it (so it
occludes and reveals), and a patch of "grass" whose texture changes every frame without any motion
vectors describing it -- the case foliage swaying in the wind presents.

The "model" edit: a smooth lighting ratio attached to the world, plus fine detail attached to the
texture, plus a little per-frame noise standing in for the detail the real model re-decides each frame.

Usage:
  python make_synthetic_dump.py out_dir [--frames 12] [--width 320] [--height 180]
"""

from __future__ import annotations

import argparse
import json
import os

import numpy as np


def texture(u, v, seed):
    rng = np.random.default_rng(seed)
    acc = np.zeros_like(u)
    for octave in range(5):
        f = 2 ** octave * 3.0
        phase = rng.uniform(0, 6.28, 2)
        acc += np.sin(u * f * 6.28 + phase[0]) * np.cos(v * f * 6.28 + phase[1]) / (octave + 1)
    return acc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=180)
    args = ap.parse_args()

    W, H, N = args.width, args.height, args.frames
    os.makedirs(args.out, exist_ok=True)
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = (xs + 0.5) / W, (ys + 0.5) / H

    bg_speed = 2.0 / W   # uv per frame, background pans left in screen space
    fg_speed = 3.0 / W   # foreground moves right
    rng = np.random.default_rng(7)

    for k in range(N):
        # World coordinates of what each pixel shows.
        bg_u = u + bg_speed * k
        fg_x0 = 0.2 + fg_speed * k
        fg = (u > fg_x0) & (u < fg_x0 + 0.25) & (v > 0.3) & (v < 0.8)
        grass = (v > 0.82) & ~fg

        base_bg = 0.35 + 0.12 * texture(bg_u, v, 1)
        base_fg = 0.6 + 0.1 * texture(u - fg_x0, v, 2)
        base = np.where(fg, base_fg, base_bg)

        grass_tex = 0.3 + 0.2 * rng.random((H, W)).astype(np.float32)  # changes every frame
        base = np.where(grass, grass_tex, base)

        colour = np.stack([base * 1.0, base * 0.9, base * 0.75], -1).astype(np.float32)

        # The edit: smooth lighting tied to the world, detail tied to the texture, some re-decided noise.
        light_bg = 0.4 * np.sin(bg_u * 6.28 * 1.5) * np.cos(v * 3.14)
        light_fg = 0.3 + 0.0 * u
        light = np.where(fg, light_fg, light_bg)
        detail = 0.25 * np.where(fg, texture(u - fg_x0, v, 5), texture(bg_u, v, 6))
        detail = np.where(grass, 0.3 * (grass_tex - 0.4), detail)
        noise = 0.04 * rng.standard_normal((H, W)).astype(np.float32)
        edit = (light + detail + noise)[..., None] * np.array([1.0, 0.95, 0.9], np.float32)

        eps = 1.0 / 512.0
        nr = (colour + eps) * np.exp2(edit) - eps

        # Motion: uv offset to the previous frame.
        mv = np.zeros((H, W, 2), np.float32)
        mv[..., 0] = np.where(fg, -fg_speed, bg_speed)  # background content came from the right
        depth = np.where(fg, 5.0, 40.0).astype(np.float32)
        log_luma = np.log2(colour @ np.array([0.2126, 0.7152, 0.0722], np.float32) + eps)

        np.save(os.path.join(args.out, f"frame_{k:03d}_orig.npy"),
                np.concatenate([colour, np.ones((H, W, 1), np.float32)], -1).astype(np.float16))
        np.save(os.path.join(args.out, f"frame_{k:03d}_nr.npy"),
                np.concatenate([np.maximum(nr, 0), np.ones((H, W, 1), np.float32)], -1).astype(np.float16))
        np.save(os.path.join(args.out, f"frame_{k:03d}_geo.npy"),
                np.stack([mv[..., 0], mv[..., 1], depth, log_luma], -1).astype(np.float32))

    with open(os.path.join(args.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"frames": N, "width": W, "height": H, "white_points": [1.0] * N, "synthetic": True}, f, indent=2)

    print(f"wrote {N} synthetic frames to {args.out}")


if __name__ == "__main__":
    main()
