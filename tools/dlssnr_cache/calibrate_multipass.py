"""Fit the edit cache's multi-pass gains from real one-pass and two-pass captures.

Running the model twice (feeding it its own output) costs twice as much. The cache can instead amplify
a single pass's edit, with separate gains on its low band (lighting, tone) and high band (detail), in
log space. This finds the gains that make one pass look most like two.

You need the same frame three times -- held still, so only the pass count differs:
  orig   the frame without Neural Rendering
  one    after one pass
  two    after two passes

Hold frame (Compare section of the menu) freezes the frame; capture it with one pass and with two using
whichever build or tool runs two passes (this fork removed its own multi-pass, see DlssNr_Dx12.cpp).

Accepted inputs: .npy arrays (H x W x 3 or 4, linear -- e.g. frame_000_orig.npy / frame_000_nr.npy from a
cache dump), or a cache dump directory for --orig/--one (frame 0 is used). 8-bit images are not
accepted: the bands have to be measured in linear light.

Several triples can be given (repeat the three flags) and are fitted together.

Usage:
  python calibrate_multipass.py --orig o.npy --one a.npy --two b.npy [--white 1.0] [--sigma 8]
"""

from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrcache_common as nc  # noqa: E402


def load(path: str, kind: str) -> np.ndarray:
    if os.path.isdir(path):
        path = os.path.join(path, f"frame_000_{kind}.npy")
    a = np.load(path).astype(np.float32)
    return np.maximum(a[..., :3], 0)


def shader_low_band(orig: np.ndarray, edit: np.ndarray, eps: float) -> np.ndarray:
    """The low band exactly as the shader builds it on a refresh: every pixel valid, the 4x4 pyramid.

    Without depth in a plain capture the bilateral read is off, which on a refresh -- where every pixel
    is valid -- changes little: the finest level then dominates everywhere."""
    h, w = orig.shape[:2]
    frame = nc.Frame(orig=orig, nr=orig, mv=np.zeros((h, w, 2), np.float32), depth=np.ones((h, w), np.float32),
                     log_luma=np.log2(nc.luma(orig) + eps).astype(np.float32))
    return nc.low_band(edit, np.ones((h, w), np.float32), frame, nc.CacheParams(bilateral=False))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--orig", action="append", required=True)
    ap.add_argument("--one", action="append", required=True)
    ap.add_argument("--two", action="append", required=True)
    ap.add_argument("--white", type=float, default=1.0, help="paper white of the captures (the ratio floor is white/512)")
    ap.add_argument("--sigma", type=float, default=0.0,
                    help="split the bands with a Gaussian of this many pixels instead of the shader's own "
                         "pyramid (default: the shader's, which is what the fitted gains will be applied to)")
    args = ap.parse_args()

    if not (len(args.orig) == len(args.one) == len(args.two)):
        sys.exit("give the same number of --orig, --one and --two")

    eps = max(args.white, 1e-4) / 512.0
    sums = np.zeros(4)  # L1.L2, L1.L1, H1.H2, H1.H1
    pairs = []

    for o_path, a_path, b_path in zip(args.orig, args.one, args.two):
        o = load(o_path, "orig")
        a = load(a_path, "nr")
        b = load(b_path, "nr")

        e1c = nc.edit_of(o, a, eps)
        e2c = nc.edit_of(o, b, eps)

        if args.sigma > 0:
            l1 = nc.gaussian_blur(e1c, args.sigma)
            l2 = nc.gaussian_blur(e2c, args.sigma)
        else:
            l1 = shader_low_band(o, e1c, eps)
            l2 = shader_low_band(o, e2c, eps)

        e1, e2 = e1c @ nc.LUMA, e2c @ nc.LUMA
        l1, l2 = l1 @ nc.LUMA, l2 @ nc.LUMA
        h1, h2 = e1 - l1, e2 - l2

        sums += [float((l1 * l2).sum()), float((l1 * l1).sum()), float((h1 * h2).sum()), float((h1 * h1).sum())]
        pairs.append((o, a, b, l1, h1, e2))

    g_low = sums[0] / max(sums[1], 1e-12)
    g_high = sums[2] / max(sums[3], 1e-12)

    split = f"Gaussian, sigma {args.sigma} px" if args.sigma > 0 else "the shader's pyramid"
    print(f"fitted on {len(pairs)} frame(s), band split: {split}")
    print(f"  low band  (lighting) gain: {g_low:.3f}")
    print(f"  high band (detail)   gain: {g_high:.3f}")

    for i, (o, a, b, l1, h1, e2) in enumerate(pairs):
        e1 = l1 + h1
        approx = g_low * l1 + g_high * h1
        err_one = float(np.abs(e1 - e2).mean())
        err_fit = float(np.abs(approx - e2).mean())
        print(f"  frame {i}: |one pass - two passes| {err_one:.4f} stops -> |fitted - two passes| {err_fit:.4f} "
              f"({100 * (1 - err_fit / max(err_one, 1e-12)):.0f}% of the gap closed)")

    print("\nOptiScaler.ini, [DlssNr]:")
    print(f"CacheLowGain={g_low:.3f}")
    print(f"CacheHighGain={g_high:.3f}")


if __name__ == "__main__":
    main()
