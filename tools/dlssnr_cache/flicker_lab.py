"""Flicker lab: replay the edit cache on a real dump and measure the flicker it adds.

The dump has the model's answer for every frame, so "vanilla" -- the model every frame -- is known. This
replays the cache with the model run one frame in N, frame by frame as the shader would, and reports for
each strategy:

  error    mean |cache - vanilla| of the displayed picture (how far from running the model every frame)
  flicker  temporal change the cache adds beyond vanilla's own: |out_t - warp(out_t-1)| minus the same for
           vanilla, motion compensated, on pixels that stayed the same surface. What reads as "it blinks".
  far      the same flicker, on the farthest third of the depth range only (distant objects).

Usage:
  python flicker_lab.py <dump> [--crop x0 y0 w h] [--intervals 2 3 5]
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, replace

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrcache_common as nc  # noqa: E402


@dataclass
class Strategy:
    name: str
    stabilize: float = 0.5   # luminance step limit on refresh (stops), 0 off
    despeckle: bool = True
    detail_mix: float = 0.0  # on refresh, how much of the carried high band survives where it disagrees
    decay: float = 0.97
    colour_floor: float = 0.6


def crop_frame(f: nc.Frame, x0, y0, w, h, W, H) -> nc.Frame:
    sl = (slice(y0, y0 + h), slice(x0, x0 + w))
    mv = f.mv[sl].copy()
    mv[..., 0] *= W / w
    mv[..., 1] *= H / h
    return nc.Frame(orig=f.orig[sl], nr=f.nr[sl], mv=mv, depth=f.depth[sl], log_luma=f.log_luma[sl],
                    white_point=f.white_point)


def despeckle(fresh: np.ndarray) -> np.ndarray:
    l = fresh @ nc.LUMA
    p = np.pad(l, 1, mode="edge")
    neigh = [p[1 + dy:p.shape[0] - 1 + dy, 1 + dx:p.shape[1] - 1 + dx]
             for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dx, dy) != (0, 0)]
    lo = np.min(neigh, axis=0) - 0.1
    hi = np.max(neigh, axis=0) + 0.1
    return fresh + (np.clip(l, lo, hi) - l)[..., None]


def warp(img: np.ndarray, frame: nc.Frame) -> np.ndarray:
    h, w = frame.depth.shape
    return nc.sample_bilinear(img, np.clip(nc.pixel_uv(h, w) + frame.mv, 0, 1))


def run(frames, n: int, s: Strategy):
    p = nc.CacheParams(high_decay=s.decay)
    out = []
    hist = None

    for t, f in enumerate(frames):
        fresh = nc.edit_of(f.orig, f.nr, f.eps)
        if s.despeckle:
            fresh = despeckle(fresh)

        if hist is None or t % n == 0:
            if hist is not None:
                # Refresh, with the carried edit reprojected for the stabiliser and the detail mix.
                carried, conf, valid = nc.reproject(hist, f, p)
                trust = (valid > 0.5).astype(np.float32)

                if s.stabilize > 0:
                    lf, lc = fresh @ nc.LUMA, carried @ nc.LUMA
                    fresh = fresh + ((lc + np.clip(lf - lc, -s.stabilize, s.stabilize)) - lf)[..., None] * trust[..., None] \
                        + 0 * fresh
                    fresh = np.where(trust[..., None] > 0, fresh, nc.edit_of(f.orig, f.nr, f.eps))

                if s.detail_mix > 0:
                    lowf = nc.gaussian_blur(fresh, 2.0)
                    lowc = nc.gaussian_blur(carried, 2.0)
                    hf, hc = fresh - lowf, carried - lowc
                    fresh = lowf + hf + (hc - hf) * (s.detail_mix * trust)[..., None]

            hist = nc.History(edit=fresh, confidence=np.ones(f.depth.shape, np.float32), depth=f.depth.copy(),
                              log_luma=f.log_luma.copy())
            final = fresh
        else:
            final, hist, _ = nc.cached_edit(hist, f, p)

        out.append(nc.display(nc.apply_edit(f.orig, final, f.eps), f.white_point))

    return out


def measure(frames, outs):
    vanilla = [nc.display(f.nr, f.white_point) for f in frames]
    err = np.mean([np.abs(o - v).mean() for o, v in zip(outs, vanilla)])

    flick, far = [], []
    for t in range(1, len(frames)):
        f = frames[t]
        same = np.abs(np.log2(np.maximum(warp(frames[t - 1].depth, f), 1e-6)) - np.log2(np.maximum(f.depth, 1e-6))) < 0.1
        far_mask = same & (f.depth > np.quantile(f.depth, 0.66))
        dc = np.abs(outs[t] - warp(outs[t - 1], f)).mean(axis=-1)
        dv = np.abs(vanilla[t] - warp(vanilla[t - 1], f)).mean(axis=-1)
        flick.append(float((dc - dv)[same].mean()))
        far.append(float((dc - dv)[far_mask].mean()) if far_mask.any() else 0.0)

    return err, float(np.mean(flick)), float(np.mean(far))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--crop", type=int, nargs=4, default=None)
    ap.add_argument("--intervals", type=int, nargs="+", default=[2, 3, 5])
    args = ap.parse_args()

    dump = nc.load_dump(args.dump)
    frames = dump.frames

    if args.crop:
        x0, y0, w, h = args.crop
        frames = [crop_frame(f, x0, y0, w, h, dump.width, dump.height) for f in frames]

    strategies = [
        Strategy("current (stab 0.5)"),
        Strategy("stab 0.25", stabilize=0.25),
        Strategy("stab 0.12", stabilize=0.12),
        Strategy("detail mix 0.5", detail_mix=0.5),
        Strategy("stab 0.25 + mix 0.5", stabilize=0.25, detail_mix=0.5),
        Strategy("no stab, no despeckle", stabilize=0.0, despeckle=False),
    ]

    print(f"{len(frames)} frames {frames[0].depth.shape[1]}x{frames[0].depth.shape[0]}")
    for n in args.intervals:
        print(f"\ninterval {n}")
        print(f"  {'strategy':<24} {'error':>8} {'flicker':>9} {'far':>9}")
        for s in strategies:
            e, fl, fa = measure(frames, run(frames, n, s))
            print(f"  {s.name:<24} {e:8.5f} {fl:9.5f} {fa:9.5f}")


if __name__ == "__main__":
    main()
