"""How well does a carried Neural Rendering edit match the real one, N frames later?

Reads a dump written by the edit cache (menu: Edit cache > Dump frames for measurement, or a file named
dlssnr-cachedump.trigger beside OptiScaler). The model ran on every dumped frame, so each frame has its
own ground truth. For each lag N (1, 2, 4, 8 by default) and each start frame t, the edit of frame t is
carried to frame t+N the way the cache would carry it, and compared with the edit the model actually
produced at t+N.

Strategies compared, all on the same frames:

  none        no edit at all -- the frame without Neural Rendering. The floor.
  naive       the whole edit reprojected, no validation. What a simple cache would do.
  low only    only the low band (lighting, tone), reprojected everywhere.
  cache       the shader's algorithm, step by step: validated reprojection, push-pull low band,
              high band where validated, decaying with age. What you get in game.

Reported globally and in the "unstable" zones: pixels whose history was rejected along the way, plus
busy fine texture (foliage, hair, gravel), where a carried edit is expected to struggle.

  edit err   mean |carried - true| of the edit's luminance, in stops. Lower is better.
  PSNR       of the tone-mapped picture against the true Neural Rendering frame. Higher is better.
  kept       how much of Neural Rendering's effect survives: 1 - err(strategy) / err(none), on the
             picture. 100% is indistinguishable from running the model; 0% is no better than off.

Usage:
  python measure_reprojection.py <dump dir> [--lags 1 2 4 8] [--depth-tol 0.1] [--colour-tol 0.5]
                                 [--decay 0.92] [--no-bilateral] [--heatmaps out_dir]
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrcache_common as nc  # noqa: E402


def carry(dump: nc.Dump, t: int, n: int, p: nc.CacheParams):
    """Carry frame t's edit to frame t+n with each strategy. Returns a dict of edits plus masks."""
    frames = dump.frames
    true_t = nc.edit_of(frames[t].orig, frames[t].nr, frames[t].eps)

    # The shader's algorithm, frame by frame.
    hist = nc.capture(frames[t])
    valid_all = np.ones(frames[t].depth.shape, bool)
    cache = None

    for k in range(t + 1, t + n + 1):
        cache, hist, valid = nc.cached_edit(hist, frames[k], p)
        valid_all &= valid >= 0.5

    # Naive and low-only: chain the motion back from t+n to t and sample once.
    h, w = frames[t].depth.shape
    uv = nc.pixel_uv(h, w)
    pos = uv.copy()
    on = np.ones((h, w), bool)

    for k in range(t + n, t, -1):
        mv = nc.sample_bilinear(frames[k].mv, pos)
        pos = pos + mv
        on &= np.all((pos >= 0) & (pos <= 1), axis=-1)

    naive = nc.sample_bilinear(true_t, np.clip(pos, 0, 1)) * on[..., None]
    low_t = nc.gaussian_blur(true_t, 8.0)
    low_only = nc.sample_bilinear(low_t, np.clip(pos, 0, 1)) * on[..., None]

    return {
        "none": np.zeros_like(true_t),
        "naive": naive,
        "low only": low_only,
        "cache": cache,
    }, valid_all


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--lags", type=int, nargs="+", default=[1, 2, 4, 8])
    ap.add_argument("--depth-tol", type=float, default=0.10)
    ap.add_argument("--colour-tol", type=float, default=0.50)
    ap.add_argument("--decay", type=float, default=0.92)
    ap.add_argument("--no-bilateral", action="store_true")
    ap.add_argument("--heatmaps", help="write per-lag error maps (.npy) for the cache strategy here")
    args = ap.parse_args()

    dump = nc.load_dump(args.dump)
    p = nc.CacheParams(depth_tol=args.depth_tol, colour_tol=args.colour_tol, high_decay=args.decay,
                       bilateral=not args.no_bilateral)

    print(f"{len(dump.frames)} frames, {dump.width}x{dump.height}")
    names = ["none", "naive", "low only", "cache"]

    for n in args.lags:
        if n >= len(dump.frames):
            print(f"\nlag {n}: not enough frames (need more than {n})")
            continue

        acc = {name: {"err": [], "err_u": [], "psnr": [], "psnr_u": [], "pic": [], "pic_u": []} for name in names}
        unstable_share = []

        for t in range(0, len(dump.frames) - n):
            s = t + n
            fs = dump.frames[s]
            true_s = nc.edit_of(fs.orig, fs.nr, fs.eps)
            carried, valid_all = carry(dump, t, n, p)

            unstable = nc.unstable_mask(fs, valid_all.astype(np.float32))
            unstable_share.append(float(unstable.mean()))

            ref = nc.display(fs.nr, fs.white_point)

            for name in names:
                pic = nc.display(nc.apply_edit(fs.orig, carried[name], fs.eps), fs.white_point)
                a = acc[name]
                a["err"].append(nc.edit_error(carried[name], true_s))
                a["err_u"].append(nc.edit_error(carried[name], true_s, unstable))
                a["psnr"].append(nc.psnr(pic, ref))
                a["psnr_u"].append(nc.psnr(pic, ref, unstable))
                d = np.abs(pic - ref).mean(axis=-1)
                a["pic"].append(float(d.mean()))
                a["pic_u"].append(float(d[unstable].mean()) if unstable.any() else float("nan"))

            if args.heatmaps:
                os.makedirs(args.heatmaps, exist_ok=True)
                err_map = np.abs((carried["cache"] - true_s) @ nc.LUMA).astype(np.float32)
                np.save(os.path.join(args.heatmaps, f"lag{n}_t{t:03d}_cache_err.npy"), err_map)

        def m(v):
            v = [x for x in v if np.isfinite(x)]
            return float(np.mean(v)) if v else float("nan")

        base, base_u = m(acc["none"]["pic"]), m(acc["none"]["pic_u"])
        print(f"\nlag {n} frame(s)  ({len(acc['none']['err'])} samples, unstable zones {100 * m(unstable_share):.1f}% of pixels)")
        print(f"  {'strategy':<10} {'edit err':>9} {'PSNR':>7} {'kept':>6}   | unstable: {'edit err':>9} {'PSNR':>7} {'kept':>6}")

        for name in names:
            a = acc[name]
            kept = 1 - m(a["pic"]) / base if base > 0 else float("nan")
            kept_u = 1 - m(a["pic_u"]) / base_u if base_u > 0 else float("nan")
            print(f"  {name:<10} {m(a['err']):9.4f} {m(a['psnr']):7.2f} {100 * kept:5.1f}%   |           "
                  f"{m(a['err_u']):9.4f} {m(a['psnr_u']):7.2f} {100 * kept_u:5.1f}%")


if __name__ == "__main__":
    main()
