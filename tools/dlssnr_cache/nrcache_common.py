"""Shared pieces of the DLSS-NR edit cache tools.

Everything here mirrors OptiScaler/shaders/dlssnr/precompile/dlssnr_cache.hlsl, so that what the offline
scripts measure is what the shader would do. Where the two must agree it says so; if you change one,
change the other.

numpy only. scipy is used for the Gaussian blur when present, with a numpy fallback.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
MAX_EDIT = 4.0  # kMaxEdit in the shader


# --------------------------------------------------------------------------------------------------
# Loading
# --------------------------------------------------------------------------------------------------

@dataclass
class Frame:
    orig: np.ndarray  # H x W x 3, linear, the upscaler's frame
    nr: np.ndarray    # H x W x 3, linear, after Neural Rendering
    mv: np.ndarray    # H x W x 2, uv offset to the PREVIOUS frame (prev_uv = uv + mv)
    depth: np.ndarray  # H x W, pseudo-linear depth (proportional to view depth)
    log_luma: np.ndarray  # H x W, log2(luma(orig) + eps)
    white_point: float = 1.0

    @property
    def eps(self) -> float:
        return max(self.white_point, 1e-4) / 512.0


@dataclass
class Dump:
    frames: list = field(default_factory=list)
    width: int = 0
    height: int = 0


def load_dump(directory: str) -> Dump:
    with open(os.path.join(directory, "manifest.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)

    dump = Dump(width=manifest["width"], height=manifest["height"])
    wps = manifest.get("white_points", [1.0] * manifest["frames"])

    for k in range(manifest["frames"]):
        def load(kind):
            return np.load(os.path.join(directory, f"frame_{k:03d}_{kind}.npy")).astype(np.float32)

        orig = load("orig")[..., :3]
        nr = load("nr")[..., :3]
        geo = load("geo")
        dump.frames.append(Frame(orig=np.maximum(orig, 0), nr=np.maximum(nr, 0), mv=geo[..., 0:2],
                                 depth=geo[..., 2], log_luma=geo[..., 3], white_point=float(wps[k])))

    return dump


# --------------------------------------------------------------------------------------------------
# The edit
# --------------------------------------------------------------------------------------------------

MAX_LUMA_EDIT = 1.5  # log2(highlight guard 2.0) + 0.5, the shader's default gMaxLumaEdit


def clamp_edit(e: np.ndarray, limit: float = MAX_LUMA_EDIT) -> np.ndarray:
    """ClampEdit in the shader: luminance within the guard, colour within a stop of it."""
    l = e @ LUMA
    lc = np.clip(l, -limit, limit)
    e = e + (lc - l)[..., None]
    return np.clip(e, (lc - 1)[..., None], (lc + 1)[..., None]).astype(np.float32)


def edit_of(orig: np.ndarray, edited: np.ndarray, eps: float) -> np.ndarray:
    """log2((edited + eps) / (orig + eps)) per channel, bounded like the shader's FreshEdit."""
    e = np.log2((np.maximum(edited, 0) + eps) / (np.maximum(orig, 0) + eps))
    return clamp_edit(e)


def apply_edit(orig: np.ndarray, edit: np.ndarray, eps: float) -> np.ndarray:
    return np.maximum((np.maximum(orig, 0) + eps) * np.exp2(edit) - eps, 0)


def luma(x: np.ndarray) -> np.ndarray:
    return x[..., :3] @ LUMA


# --------------------------------------------------------------------------------------------------
# Sampling and filtering
# --------------------------------------------------------------------------------------------------

def bilinear_taps(uv: np.ndarray, width: int, height: int):
    """The four taps of a bilinear read at uv (pixel centres at (i + 0.5) / size), clamped.

    Returns a list of (y_index, x_index, weight) for the four corners."""
    px = uv[..., 0] * width - 0.5
    py = uv[..., 1] * height - 0.5
    x0 = np.floor(px).astype(np.int64)
    y0 = np.floor(py).astype(np.int64)
    fx = (px - x0).astype(np.float32)
    fy = (py - y0).astype(np.float32)

    taps = []
    for oy in (0, 1):
        for ox in (0, 1):
            xi = np.clip(x0 + ox, 0, width - 1)
            yi = np.clip(y0 + oy, 0, height - 1)
            w = (fx if ox else 1 - fx) * (fy if oy else 1 - fy)
            taps.append((yi, xi, w))
    return taps


def sample_bilinear(img: np.ndarray, uv: np.ndarray) -> np.ndarray:
    h, w = img.shape[:2]
    out = None
    for yi, xi, wt in bilinear_taps(uv, w, h):
        v = img[yi, xi]
        term = v * (wt[..., None] if v.ndim == 3 else wt)
        out = term if out is None else out + term
    return out


def pixel_uv(height: int, width: int) -> np.ndarray:
    ys, xs = np.mgrid[0:height, 0:width].astype(np.float32)
    return np.stack([(xs + 0.5) / width, (ys + 0.5) / height], axis=-1)


def gaussian_blur(img: np.ndarray, sigma: float, weight: np.ndarray | None = None) -> np.ndarray:
    """Normalised (optionally weighted) Gaussian blur over the first two axes."""
    try:
        from scipy.ndimage import gaussian_filter

        def blur(a):
            if a.ndim == 3:
                return np.stack([gaussian_filter(a[..., c], sigma, mode="nearest") for c in range(a.shape[2])], -1)
            return gaussian_filter(a, sigma, mode="nearest")
    except ImportError:  # three box passes approximate a Gaussian
        r = max(1, int(round(sigma * 0.8)))

        def box(a, axis):
            pad = [(0, 0)] * a.ndim
            pad[axis] = (r, r)
            p = np.pad(a, pad, mode="edge")
            c = np.cumsum(p, axis=axis, dtype=np.float64)
            c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
            n = a.shape[axis]
            hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
            lo = np.take(c, np.arange(0, n), axis=axis)
            return ((hi - lo) / (2 * r + 1)).astype(np.float32)

        def blur(a):
            for _ in range(3):
                a = box(box(a, 0), 1)
            return a

    if weight is None:
        return blur(img)

    w = weight if img.ndim == 2 else weight[..., None]
    num = blur(img * w)
    den = blur(weight)
    den = den if img.ndim == 2 else den[..., None]
    return num / np.maximum(den, 1e-6)


# --------------------------------------------------------------------------------------------------
# The cache, as the shader runs it
# --------------------------------------------------------------------------------------------------

@dataclass
class CacheParams:
    depth_tol: float = 0.10
    colour_tol: float = 0.50
    high_decay: float = 0.92
    low_gain: float = 1.0
    high_gain: float = 1.0
    bilateral: bool = True


@dataclass
class History:
    edit: np.ndarray       # H x W x 3
    confidence: np.ndarray  # H x W
    depth: np.ndarray      # H x W
    log_luma: np.ndarray   # H x W


def capture(frame: Frame) -> History:
    """Mode 2 of the shader with the refresh blend at 1: the model's edit, whole."""
    h, w = frame.depth.shape
    return History(edit=edit_of(frame.orig, frame.nr, frame.eps), confidence=np.ones((h, w), np.float32),
                   depth=frame.depth.copy(), log_luma=frame.log_luma.copy())


def reproject(hist: History, frame: Frame, p: CacheParams):
    """Mode 1: last frame's history onto this one. Returns (edit, confidence, valid)."""
    h, w = frame.depth.shape
    uv = pixel_uv(h, w)
    q = uv + frame.mv
    on_screen = np.all((q >= 0) & (q <= 1), axis=-1)

    lin_c = frame.depth
    edit = np.zeros((h, w, 3), np.float32)
    conf = np.zeros((h, w), np.float32)
    lum = np.zeros((h, w), np.float32)
    wsum = np.zeros((h, w), np.float32)

    for yi, xi, wb in bilinear_taps(q, w, h):
        g = hist.depth[yi, xi]
        rel = np.abs(g - lin_c) / np.maximum(np.minimum(g, lin_c), 1e-7)
        wd = np.clip((2 * p.depth_tol - rel) / max(p.depth_tol, 1e-6), 0, 1)
        wt = wb * wd
        edit += hist.edit[yi, xi] * wt[..., None]
        conf += hist.confidence[yi, xi] * wt
        lum += hist.log_luma[yi, xi] * wt
        wsum += wt

    ok = wsum > 1e-4
    edit[ok] /= wsum[ok][..., None]
    conf[ok] /= wsum[ok]
    lum[ok] /= wsum[ok]
    valid = np.clip(wsum, 0, 1) * on_screen

    edit *= on_screen[..., None]
    colour_tol = max(p.colour_tol, 1e-3)
    v_colour = np.clip((2 * colour_tol - np.abs(frame.log_luma - lum)) / colour_tol, 0, 1)
    conf = np.clip(conf * (0.6 + 0.4 * v_colour) * np.clip(2 * valid - 1, 0, 1) * p.high_decay, 0, 1) * on_screen
    return edit, conf, valid


def _reduce4(values: np.ndarray, weight: np.ndarray):
    """One pyramid step: coverage-weighted 4x4 means. Returns (mean, coverage)."""
    h, w = weight.shape
    H, W = (h + 3) // 4, (w + 3) // 4
    pw = np.zeros((H * 4, W * 4), np.float32)
    pw[:h, :w] = weight
    if values.ndim == 3:
        pv = np.zeros((H * 4, W * 4, values.shape[2]), np.float32)
        pv[:h, :w] = values
        num = (pv * pw[..., None]).reshape(H, 4, W, 4, -1).sum(axis=(1, 3))
    else:
        pv = np.zeros((H * 4, W * 4), np.float32)
        pv[:h, :w] = values
        num = (pv * pw).reshape(H, 4, W, 4).sum(axis=(1, 3))
    den = pw.reshape(H, 4, W, 4).sum(axis=(1, 3))
    mean = num / np.maximum(den if values.ndim == 2 else den[..., None], 1e-6)
    return mean.astype(np.float32), (den / 16.0).astype(np.float32)


def _fetch_level(mean: np.ndarray, cov: np.ndarray, uv: np.ndarray):
    h, w = cov.shape
    acc = 0
    wsum = 0
    for yi, xi, wb in bilinear_taps(uv, w, h):
        wt = wb * cov[yi, xi]
        acc = acc + mean[yi, xi] * wt[..., None]
        wsum = wsum + wt
    out = np.where(wsum[..., None] > 1e-6, acc / np.maximum(wsum, 1e-6)[..., None], 0)
    return out.astype(np.float32), wsum


def low_band(edit: np.ndarray, weight: np.ndarray, frame: Frame, p: CacheParams) -> np.ndarray:
    """Modes 1 (reduction), 3 and 4 (the low band): the push-pull pyramid with the bilateral L1 read."""
    h, w = weight.shape
    l1, c1 = _reduce4(edit, weight)
    gd1, _ = _reduce4(np.log2(np.maximum(frame.depth, 1e-7)), weight)
    gl1, _ = _reduce4(frame.log_luma, weight)
    l2, c2 = _reduce4(l1, c1)  # coarser levels are weighted by coverage, as in mode 3
    l3, c3 = _reduce4(l2, c2)

    uv = pixel_uv(h, w)
    logd = np.log2(np.maximum(frame.depth, 1e-7))

    H1, W1 = c1.shape
    acc1 = 0
    w1 = 0
    for yi, xi, wb in bilinear_taps(uv, W1, H1):
        wt = wb * c1[yi, xi]
        if p.bilateral:
            dd = (gd1[yi, xi] - logd) / 0.15
            dl = (gl1[yi, xi] - frame.log_luma) / 1.0
            wt = wt * np.exp(-(dd * dd + dl * dl))
        acc1 = acc1 + l1[yi, xi] * wt[..., None]
        w1 = w1 + wt

    e2, w2 = _fetch_level(l2, c2, uv)
    e3, w3 = _fetch_level(l3, c3, uv)

    low = np.where(w3[..., None] > 1e-6, e3, 0)
    low = low + (e2 - low) * np.clip(w2 * 4, 0, 1)[..., None]
    e1 = np.where(w1[..., None] > 1e-6, acc1 / np.maximum(w1, 1e-6)[..., None], low)
    low = low + (e1 - low) * np.clip(w1 * 4, 0, 1)[..., None]
    return low.astype(np.float32)


def cached_edit(hist: History, frame: Frame, p: CacheParams):
    """One cached frame end to end. Returns (final edit, new history, valid)."""
    edit, conf, valid = reproject(hist, frame, p)
    low = low_band(edit, valid, frame, p)
    final = clamp_edit(p.low_gain * low + p.high_gain * conf[..., None] * (edit - low),
                       MAX_LUMA_EDIT * max(p.low_gain, p.high_gain, 1.0))
    new_hist = History(edit=edit, confidence=conf, depth=frame.depth.copy(), log_luma=frame.log_luma.copy())
    return final, new_hist, valid


# --------------------------------------------------------------------------------------------------
# Metrics
# --------------------------------------------------------------------------------------------------

def display(x: np.ndarray, white: float) -> np.ndarray:
    """A neutral tone map for comparing pictures: Reinhard on luma, then sRGB-ish gamma."""
    y = luma(x) / max(white, 1e-6)
    scale = 1.0 / (1.0 + y)
    return np.clip((x / max(white, 1e-6)) * scale[..., None], 0, 1) ** (1 / 2.2)


def psnr(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    d = (a - b) ** 2
    if mask is not None:
        if mask.sum() < 1:
            return float("nan")
        mse = float(d[mask].mean())
    else:
        mse = float(d.mean())
    return 99.0 if mse <= 1e-12 else 10 * np.log10(1.0 / mse)


def edit_error(a: np.ndarray, b: np.ndarray, mask: np.ndarray | None = None) -> float:
    """Mean absolute difference of two edits' luminance component, in stops."""
    d = np.abs((a - b) @ LUMA)
    if mask is not None:
        return float("nan") if mask.sum() < 1 else float(d[mask].mean())
    return float(d.mean())


def unstable_mask(frame: Frame, valid: np.ndarray, sigma_px: float = 2.0, texture_thr: float = 0.35):
    """Where a carried edit is expected to struggle: rejected history, or busy fine texture (foliage).

    Busy texture is a high local standard deviation of log luma -- grass, leaves, hair, gravel."""
    ll = frame.log_luma
    mean = gaussian_blur(ll, sigma_px)
    var = gaussian_blur(ll * ll, sigma_px) - mean * mean
    busy = np.sqrt(np.maximum(var, 0)) > texture_thr
    return busy | (valid < 0.5)
