"""measure_pulse.py <observation dump> <out prefix>: where the light on screen jumps on the frames the model ran.

Takes an observation dump (dlssnr-cacheobserve.trigger: what the cache showed, frame by frame, the model not
forced), whose manifest says how many frames since the model ran each one is. At half resolution: the shown edit's
log luma ratio, its regional light (gaussian, 6 px at half size = 12 px), and the step from the previous frame
moved here along the motion; the mean step by frame since the model ran (a pulse is the first above the rest),
by luma band, and two pictures: <out>_excess.png, where the frames the model ran moved more than the others
(red), and <out>_signed.png, which way (red brighter, blue darker)."""
import json
import os
import sys

import numpy as np
from PIL import Image

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def half(a):
    h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
    a = a[:h, :w]
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def blur(x, sigma):
    r = int(3 * sigma)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2).astype(np.float32)
    k /= k.sum()
    p = np.pad(x, ((0, 0), (r, r)), mode="edge")
    x = sum(k[i] * p[:, i:i + x.shape[1]] for i in range(2 * r + 1))
    p = np.pad(x, ((r, r), (0, 0)), mode="edge")
    return sum(k[i] * p[i:i + x.shape[0], :] for i in range(2 * r + 1))


def warp(img, offset):
    """img sampled at uv + offset (bilinear, clamped)."""
    h, w = img.shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    x = np.clip(xs + offset[..., 0] * w, 0, w - 1.001)
    y = np.clip(ys + offset[..., 1] * h, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    return ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x0 + 1] + (1 - fx) * fy * img[y0 + 1, x0] +
            fx * fy * img[y0 + 1, x0 + 1])


d = sys.argv[1]
out = sys.argv[2]
m = json.load(open(os.path.join(d, "manifest.json")))
since = m.get("frames_since_model", [])
n = m["frames"]
regional, detail, edits, offs, lumas = [], [], [], [], []

for k in range(n):
    orig = np.load(os.path.join(d, f"frame_{k:03d}_orig.npy")).astype(np.float32)[..., :3]
    nr = np.load(os.path.join(d, f"frame_{k:03d}_nr.npy")).astype(np.float32)[..., :3]
    geo = np.load(os.path.join(d, f"frame_{k:03d}_geo.npy")).astype(np.float32)
    eps = m["white_points"][k] / 512.0
    yo = half(np.maximum(orig, 0) @ LUMA)
    ys = half(np.maximum(nr, 0) @ LUMA)
    e = np.log2((ys + eps) / (yo + eps))
    edits.append(e)
    regional.append(blur(e, 6.0))
    offs.append(half(geo[..., :2]))
    lumas.append(np.log2(yo + eps) - np.log2(m["white_points"][k]))

steps = {}
signed = {}
maps_run, maps_other = [], []

for k in range(1, n):
    prev = warp(regional[k - 1], offs[k])
    s = regional[k] - prev
    p = since[k] if k < len(since) else -1
    steps.setdefault(p, []).append(float(np.abs(s).mean()))
    signed.setdefault(p, []).append(float(s.mean()))
    (maps_run if p == 0 else maps_other).append(np.abs(s))

print(f"{d}: {n} frames, since model {since}")
for p in sorted(steps):
    print(f"  {p}: step {np.mean(steps[p]):.4f}  signed {np.mean(signed[p]):+.4f}  ({len(steps[p])} frames)")

if maps_run and maps_other:
    excess = np.mean(maps_run, axis=0) - np.mean(maps_other, axis=0)
    print(f"  excess on run frames: mean {excess.mean():.4f}, p99 {np.percentile(excess, 99):.4f}")
    # Where: by luma band of the frame and by edit size
    luma = np.mean(lumas, axis=0)
    for lo, hi in ((-20, -6), (-6, -4), (-4, -2), (-2, 0), (0, 20)):
        sel = (luma >= lo) & (luma < hi)
        if sel.mean() > 0.001:
            print(f"    frame luma {lo:>3}..{hi:>3} stops ({100 * sel.mean():4.1f}% of the frame): "
                  f"excess {excess[sel].mean():.4f}")
    v = np.clip(excess / max(np.percentile(excess, 99.5), 1e-6), 0, 1)
    base = np.clip(np.mean([np.exp2(l) for l in lumas], axis=0) ** (1 / 2.2), 0, 1)
    rgb = np.stack([np.maximum(base * 0.6, v), base * 0.6 * (1 - v), base * 0.6 * (1 - v)], axis=-1)
    Image.fromarray((rgb * 255).astype(np.uint8)).save(out + "_excess.png")
    # the signed run-frame step, red brighter / blue darker
    sgn = np.mean([regional[k] - warp(regional[k - 1], offs[k]) for k in range(1, n) if k < len(since) and since[k] == 0],
                  axis=0)
    a = np.clip(sgn / 0.02, -1, 1)
    rgb = np.stack([base * 0.5 + np.maximum(a, 0) * 0.5, base * 0.5, base * 0.5 + np.maximum(-a, 0) * 0.5], axis=-1)
    Image.fromarray((np.clip(rgb, 0, 1) * 255).astype(np.uint8)).save(out + "_signed.png")
