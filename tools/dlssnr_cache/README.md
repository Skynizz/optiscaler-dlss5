# DLSS-NR edit cache tools

Offline companions to the temporal edit cache (`OptiScaler/dlssnr/design/edit-cache.md`). numpy is
required, scipy is optional.

| Script | What it does |
|---|---|
| `measure_reprojection.py <dump>` | How well a carried edit matches the real one 1/2/4/8 frames on, globally and in unstable zones (rejected history, foliage-like texture), for: no edit, naive reprojection, low band only, and the shader's own algorithm. |
| `calibrate_multipass.py --orig --one --two` | Fits `CacheLowGain` / `CacheHighGain` from one-pass and two-pass captures of a held frame. |
| `make_synthetic_dump.py <out>` | A synthetic dump (panning background, occluding foreground, wind-blown "grass") to test the tools without a game. |
| `nrcache_common.py` | The numpy mirror of `dlssnr_cache.hlsl`. Keep it in step with the shader. |

## Getting a dump

1. Enable Neural Rendering and *Edit cache > Reuse the model's edit between runs*.
2. Move the camera (a still frame measures nothing) and press *Dump frames for measurement*, or drop an
   empty file named `dlssnr-cachedump.trigger` next to OptiScaler.dll.
3. The frames land in `dlssnr-cachedump/` next to OptiScaler.dll. Roughly 120 MB of RAM per frame at
   1440p while recording, and a stutter while writing.

```
python measure_reprojection.py "<game>/dlssnr-cachedump"
python measure_reprojection.py "<game>/dlssnr-cachedump" --depth-tol 0.15 --colour-tol 0.8   # try settings offline
```

Reading the output: *kept* is the share of Neural Rendering's visible effect that survives (100% means
indistinguishable from running the model on that frame). Pick the largest interval whose *kept* you are
happy with, and look at the *unstable* column for grass and hair.
