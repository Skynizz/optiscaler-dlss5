"""How much does the edit cache ghost at long intervals, and how much do the anti-ghosting switches help?

Unlike measure_reprojection.py, which carries one edit forward from one frame, this runs the cache through
a dump frame by frame at a fixed cadence -- the model "runs" every N frames, the frames between are cached
-- exactly as the game sees it, keyframe crossfade included, and compares every shown frame with the
model's real answer on that frame (the dump ran the model on all of them).

It mirrors the anti-ghosting part of OptiScaler/shaders/dlssnr/precompile/dlssnr_cache.hlsl: the
fingerprint (normalised log luma, two log chroma ratios, the surroundings), read at the strongest valid tap
and compared with the range of the pixel's 3x3 neighbourhood; the same-surface fill; the crossfade cap;
aging; the fast guided filter (half size, 3x3 window, tent read). Keep the two in step.

Reported for each variant, on each dump:

  err      mean |shown - true| of the edit's luminance, in stops. Lower is better.
  p99.5    the 99.5th percentile of that: the strongest trails. Lower is better.
  harm     mean of max(0, |shown - true| - |true|) x 1000: where the carried edit is worse than none.
  ghost    share of pixels where that excess is above 0.15 stop.
  kept     share of Neural Rendering's visible effect that survives (100% = the model on every frame).

Usage:
  python measure_ghosting.py <dump dir> [<dump dir> ...] [--interval 8] [--phases 0 2]
                             [--variants before defaults guided aging] [--half]

--half works on a half-size copy (four times faster, same conclusions on the dumps tried). Each variant
takes a few minutes per dump at 1440p.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass, field

import numpy as np
from scipy.ndimage import convolve1d, maximum_filter, minimum_filter, uniform_filter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nrcache_common as nc  # noqa: E402


@dataclass
class Ghost:
    crossfade: bool = True
    xf_frames: int = 0           # CacheCrossfadeFrames (0 = the whole interval)
    fingerprint: bool = False    # CacheFingerprint
    context: bool = False        # CacheContext
    tolerance: float = 0.25      # CacheFingerprintTolerance
    surface_fill: bool = False   # CacheSurfaceFill
    aging: bool = False          # CacheAging
    half_life: float = 8.0       # CacheAgeHalfLife
    age_neutral: bool = False    # CacheAgeNeutral
    guided: bool = False         # CacheGuided
    guided_strength: float = 1.0  # CacheGuidedStrength
    guided_eps: float = 0.005    # the shader's GuidedEps
    stale_px: float = 4.0        # the shader's StalePixels


VARIANTS = {
    "before": Ghost(),
    "defaults": Ghost(xf_frames=3, fingerprint=True, context=True, surface_fill=True),
    "guided": Ghost(xf_frames=3, fingerprint=True, context=True, surface_fill=True, guided=True),
    "aging": Ghost(xf_frames=3, fingerprint=True, context=True, surface_fill=True, aging=True),
    "no-crossfade": Ghost(crossfade=False, fingerprint=True, context=True, surface_fill=True),
}


# --------------------------------------------------------------------------------------------------
# The shader's anti-ghosting pieces
# --------------------------------------------------------------------------------------------------

def fingerprint(f: nc.Frame):
    """PrintOf and mode 12: normalised log luma, log r/g, log b/g, and the tent-read quarter-size mean."""
    eps = f.eps
    nll = f.log_luma - np.log2(max(f.white_point, 1e-4))
    o = np.maximum(f.orig, 0) + eps
    u = np.log2(o[..., 0] / o[..., 1])
    v = np.log2(o[..., 2] / o[..., 1])

    h, w = nll.shape
    H, W = (h + 3) // 4, (w + 3) // 4
    p = np.pad(nll, ((0, H * 4 - h), (0, W * 4 - w)), mode="edge")
    small = p.reshape(H, 4, W, 4).mean(axis=(1, 3)).astype(np.float32)
    k = np.array([1, 2, 1], np.float32) / 4
    small = convolve1d(convolve1d(small, k, axis=0, mode="nearest"), k, axis=1, mode="nearest")
    ctx = nc.sample_bilinear(small, nc.pixel_uv(h, w))
    return np.stack([nll, u, v, ctx], -1).astype(np.float32)


def print_match(now: np.ndarray, src: np.ndarray, g: Ghost) -> np.ndarray:
    """PrintMatch: the source against this frame's 3x3 range, soft between the tolerance and twice it."""
    def outside(cur, s):
        lo = minimum_filter(cur, 3, mode="nearest")
        hi = maximum_filter(cur, 3, mode="nearest")
        return np.maximum(0, np.maximum(lo - s, s - hi))

    def soft(d):
        return np.clip((2 * g.tolerance - d) / g.tolerance, 0, 1)

    m = soft(outside(now[..., 0], src[..., 0]))
    m = m * soft(np.maximum(outside(now[..., 1], src[..., 1]), outside(now[..., 2], src[..., 2])))
    if g.context:
        m = m * soft(np.abs(now[..., 3] - src[..., 3]))
    return m


def surface_low(edit, weight, frame: nc.Frame) -> np.ndarray:
    """SurfaceLow: every pyramid level read jointly-bilaterally, coarse to fine; nothing similar, no edit."""
    h, w = weight.shape
    logd = np.log2(np.maximum(frame.depth, 1e-7))
    ll = frame.log_luma
    uv = nc.pixel_uv(h, w)

    levels = []
    e, c, d, l = edit, weight, logd, ll
    for _ in range(3):
        e1, c1 = nc._reduce4(e, c)
        d1, _ = nc._reduce4(d, c)
        l1, _ = nc._reduce4(l, c)
        levels.append((e1, c1, d1, l1))
        e, c, d, l = e1, c1, d1, l1

    low = np.zeros((h, w, 3), np.float32)
    have = np.zeros((h, w), np.float32)
    for lvl in (2, 1, 0):
        e1, c1, d1, l1 = levels[lvl]
        H1, W1 = c1.shape
        acc = 0
        ws = 0
        for yi, xi, wb in nc.bilinear_taps(uv, W1, H1):
            dd = (d1[yi, xi] - logd) / (0.15 * (1 + lvl))
            dl = (l1[yi, xi] - ll) / (1.0 * (1 + lvl))
            wt = wb * c1[yi, xi] * np.exp(-(dd * dd + dl * dl))
            acc = acc + e1[yi, xi] * wt[..., None]
            ws = ws + wt
        est = np.where(ws[..., None] > 1e-6, acc / np.maximum(ws, 1e-6)[..., None], low)
        a = np.clip(ws * 4, 0, 1)
        low = low + (est - low) * a[..., None]
        have = have + (1 - have) * a
    return (low * have[..., None]).astype(np.float32)


def guided_fast(I: np.ndarray, P: np.ndarray, eps: float) -> np.ndarray:
    """Modes 13-15: 2x2 means at half size, a 3x3 window there, a and b read back with a tent."""
    h, w = I.shape
    H, W = (h + 1) // 2, (w + 1) // 2

    def half(x):
        pad = [(0, H * 2 - h), (0, W * 2 - w)] + [(0, 0)] * (x.ndim - 2)
        x = np.pad(x, pad, mode="edge")
        return x.reshape(H, 2, W, 2, *x.shape[2:]).mean(axis=(1, 3))

    mI = uniform_filter(half(I), 3, mode="nearest")
    mII = uniform_filter(half(I * I), 3, mode="nearest")
    var = np.maximum(mII - mI * mI, 0)
    uv = nc.pixel_uv(h, w)
    out = np.empty_like(P)
    for ch in range(3):
        mp = uniform_filter(half(P[..., ch]), 3, mode="nearest")
        mIp = uniform_filter(half(I * P[..., ch]), 3, mode="nearest")
        a = (mIp - mI * mp) / (var + eps)
        b = mp - a * mI
        k = np.array([1, 2, 1], np.float32) / 4
        a = convolve1d(convolve1d(a, k, axis=0, mode="nearest"), k, axis=1, mode="nearest")
        b = convolve1d(convolve1d(b, k, axis=0, mode="nearest"), k, axis=1, mode="nearest")
        out[..., ch] = nc.sample_bilinear(a, uv) * I + nc.sample_bilinear(b, uv)
    return out


# --------------------------------------------------------------------------------------------------
# The cache at a cadence
# --------------------------------------------------------------------------------------------------

@dataclass
class Hist:
    edit: np.ndarray
    conf: np.ndarray
    target: np.ndarray
    tconf: np.ndarray
    depth: np.ndarray
    log_luma: np.ndarray
    prt: np.ndarray
    meta: np.ndarray  # age, validity, staleness, tap validity
    fields: tuple = field(default=("edit", "conf", "target", "tconf", "log_luma"))


def read_history(hist: Hist, frame: nc.Frame, p: nc.CacheParams):
    """ReadHistory: depth-admitted bilinear taps; the print and the meta from the strongest one."""
    h, w = frame.depth.shape
    uv = nc.pixel_uv(h, w)
    q = uv + frame.mv
    on = np.all((q >= 0) & (q <= 1), axis=-1)
    acc = {k: 0 for k in hist.fields}
    wsum = np.zeros((h, w), np.float32)
    best_w = np.zeros((h, w), np.float32)
    best_prt = np.zeros_like(hist.prt)
    best_meta = np.zeros_like(hist.meta)

    for yi, xi, wb in nc.bilinear_taps(q, w, h):
        g = hist.depth[yi, xi]
        rel = np.abs(g - frame.depth) / np.maximum(np.minimum(g, frame.depth), 1e-7)
        wt = wb * np.clip((2 * p.depth_tol - rel) / max(p.depth_tol, 1e-6), 0, 1)
        for k in acc:
            v = getattr(hist, k)[yi, xi]
            acc[k] = acc[k] + (v * (wt[..., None] if v.ndim == 3 else wt))
        wsum += wt
        better = wt > best_w
        best_w = np.where(better, wt, best_w)
        best_prt = np.where(better[..., None], hist.prt[yi, xi], best_prt)
        best_meta = np.where(better[..., None], hist.meta[yi, xi], best_meta)

    ok = wsum > 1e-4
    out = {}
    for k, v in acc.items():
        d = np.maximum(wsum, 1e-6)
        out[k] = np.where(ok[..., None] if v.ndim == 3 else ok, v / (d[..., None] if v.ndim == 3 else d), 0)
    valid = np.clip(wsum, 0, 1) * on
    motion_px = np.linalg.norm(frame.mv * np.array([w, h], np.float32), axis=-1)
    return out, best_prt, best_meta, valid, on, motion_px


def run(dump: nc.Dump, interval: int, phase: int, p: nc.CacheParams, g: Ghost):
    """Frames phase..end with the model every `interval`. Yields (refresh, shown edit, true edit, frame)."""
    hist = None
    for k in range(phase, len(dump.frames)):
        f = dump.frames[k]
        true = nc.edit_of(f.orig, f.nr, f.eps)
        now = fingerprint(f)
        h, w = f.depth.shape
        since = (k - phase) % interval
        refresh = since == 0
        span = min(interval, g.xf_frames) if g.xf_frames > 0 else interval
        step = 1.0 / span if refresh else (1.0 / (span - since) if since < span else 1.0)
        fresh_meta = np.stack([np.zeros((h, w)), np.ones((h, w)), np.zeros((h, w)), np.ones((h, w))], -1).astype(np.float32)

        if hist is None:
            hist = Hist(true, np.ones((h, w), np.float32), true, np.ones((h, w), np.float32), f.depth.copy(),
                        f.log_luma.copy(), now, fresh_meta)
            continue

        r, prt, meta, valid, on, motion_px = read_history(hist, f, p)
        colour_tol = max(p.colour_tol, 1e-3)
        v_colour = np.clip((2 * colour_tol - np.abs(f.log_luma - r["log_luma"])) / colour_tol, 0, 1)
        validity = meta[..., 1] * print_match(now, prt, g) * on if g.fingerprint else np.ones((h, w), np.float32)

        if refresh:
            carry = (valid > 0.5) & (validity > 0.5)
            if g.crossfade:
                a = np.where(carry, step, 1.0)
                shown = r["edit"] + (true - r["edit"]) * a[..., None]
                conf = np.where(carry, r["conf"] + (1 - r["conf"]) * a, 1.0)
            else:
                shown, conf = true, np.ones((h, w), np.float32)
            hist = Hist(shown, conf, true, np.ones((h, w), np.float32), f.depth.copy(), f.log_luma.copy(), now,
                        fresh_meta)
            weight, stale = np.ones((h, w), np.float32), np.zeros((h, w), np.float32)
        else:
            shown, sconf = r["edit"], r["conf"]
            if g.crossfade:
                shown = shown + (r["target"] - shown) * step
                sconf = sconf + (r["tconf"] - sconf) * step
            doubt = (0.6 + 0.4 * v_colour) * np.clip(2 * valid - 1, 0, 1) * p.high_decay * validity
            conf = np.clip(sconf * doubt, 0, 1) * on
            stale = meta[..., 2] + np.clip(motion_px / g.stale_px, 0, 1)
            new_meta = np.stack([meta[..., 0] + 1, validity, stale, valid], -1).astype(np.float32)
            hist = Hist(shown * on[..., None], conf, r["target"] * on[..., None],
                        np.clip(r["tconf"] * doubt, 0, 1) * on, f.depth.copy(), f.log_luma.copy(), prt, new_meta)
            shown = shown * on[..., None]
            weight = valid * validity

        low = surface_low(shown, weight, f) if g.surface_fill else nc.low_band(shown, weight, f, p)
        cf = conf * (np.exp2(-stale / g.half_life) if g.aging and not g.age_neutral else 1.0)
        final = nc.clamp_edit(low + cf[..., None] * (shown - low))
        if g.aging and g.age_neutral:
            final = final * np.exp2(-stale / g.half_life)[..., None]
        if g.guided and not refresh:
            q = guided_fast(now[..., 0], final, g.guided_eps)
            wgt = g.guided_strength * np.clip(stale / 2.0, 0, 1)
            final = nc.clamp_edit(final + (q - final) * wgt[..., None])
        yield refresh, final, true, f


def score(results):
    harm, ghost, err, p995, pic, pic0 = [], [], [], [], [], []
    for refresh, final, true, f in results:
        if refresh:
            continue
        e = np.abs((final - true) @ nc.LUMA)
        hm = np.maximum(0, e - np.abs(true @ nc.LUMA))
        harm.append(hm.mean())
        ghost.append((hm > 0.15).mean())
        err.append(e.mean())
        p995.append(np.percentile(e, 99.5))
        ref = nc.display(f.nr, f.white_point)
        pic.append(np.abs(nc.display(nc.apply_edit(f.orig, final, f.eps), f.white_point) - ref).mean())
        pic0.append(np.abs(nc.display(f.orig, f.white_point) - ref).mean())
    return dict(err=np.mean(err), p995=np.mean(p995), harm=1000 * np.mean(harm), ghost=100 * np.mean(ghost),
                kept=100 * (1 - np.mean(pic) / np.mean(pic0)))


def halve(dump: nc.Dump) -> nc.Dump:
    out = nc.Dump(width=dump.width // 2, height=dump.height // 2)
    for f in dump.frames:
        H, W = f.depth.shape[0] // 2, f.depth.shape[1] // 2

        def mean(x):
            x = x[:H * 2, :W * 2]
            return x.reshape(H, 2, W, 2, *x.shape[2:]).mean(axis=(1, 3)).astype(np.float32)

        orig = mean(f.orig)
        depth = f.depth[:H * 2, :W * 2].reshape(H, 2, W, 2).min(axis=(1, 3))
        out.frames.append(nc.Frame(orig=orig, nr=mean(f.nr), mv=mean(f.mv), depth=depth,
                                   log_luma=np.log2(orig @ nc.LUMA + f.eps).astype(np.float32),
                                   white_point=f.white_point))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dumps", nargs="+")
    ap.add_argument("--interval", type=int, default=8)
    ap.add_argument("--phases", type=int, nargs="+", default=[0, 2])
    ap.add_argument("--variants", nargs="+", default=list(VARIANTS), choices=list(VARIANTS))
    ap.add_argument("--half", action="store_true")
    args = ap.parse_args()

    p = nc.CacheParams(depth_tol=0.10, colour_tol=0.50, high_decay=0.97)
    dumps = {}
    for d in args.dumps:
        dump = nc.load_dump(d)
        dumps[os.path.basename(os.path.normpath(d))] = halve(dump) if args.half else dump

    print(f"interval {args.interval}, phases {args.phases}, {len(dumps)} dump(s)")
    for name in args.variants:
        t0 = time.time()
        for dn, dump in dumps.items():
            res = []
            for phase in args.phases:
                res += list(run(dump, args.interval, phase, p, VARIANTS[name]))
            s = score(res)
            print(f"  {name:<13} {dn:<20} err {s['err']:.4f}  p99.5 {s['p995']:.3f}  harm {s['harm']:.2f}  "
                  f"ghost {s['ghost']:.2f}%  kept {s['kept']:.1f}%", flush=True)
        print(f"  ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main()
