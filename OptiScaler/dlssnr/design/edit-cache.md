# Temporal edit cache

Neural Rendering costs ~98% of the pass, and the model is run every frame to produce something that
mostly does not change between frames. The cache runs it one frame in N (or sooner, see *adaptive*)
and carries its last answer forward along the game's motion vectors in between.

**Only the correction is cached, never the picture.** The edit is stored as
`log2((edited + eps) / (original + eps))` per channel, `eps = paper white / 512` (the composition's own
ratio floor). Every frame the game's fresh frame is multiplied by the carried edit. Geometry -- grass,
characters, hair -- is never reprojected; only the model's verdict about how to light and detail it.

A ratio rather than a difference because it survives the lighting changing underneath it (a torch
flickers, exposure adapts). Log so averaging it is a geometric mean of ratios.

## How this differs from the accumulator that was removed

`dlssnr.hlsl` explains that an *edit accumulator* -- blending each frame's edit with its reprojected
history to stabilise it -- was tried twice and removed: the model re-decides its fine detail with the
framing, so an old answer does not belong to a new frame. That remains true and the cache does not
contradict it. It does two things differently:

1. It is a cost tool, not a stabiliser. Refresh blend defaults to 1: a refresh takes the model's new
   answer whole, and nothing is averaged over time unless asked for.
2. It does not trust the carried fine detail. The edit is split into a low band (lighting, tone --
   broad, slow, and what reprojects well) and a high band (synthesised detail). The low band is carried
   everywhere; the high band only where depth *and* colour validate, and it decays with age
   (`CacheHighDecay`). Where the old accumulator moved a disagreement around, the cache drops the part
   that disagrees and keeps the part that does not.

Whether the remainder is good enough is a measurement, not an argument -- see *Measuring* below.

## Frame flow (D3D12, `shaders/dlssnr/DlssNr_EditCache_Dx12.cpp`)

Refresh frame (the model runs):

```
encode -> [downsample] -> model -> [JBU] -> resolve      (unchanged)
                                      -> Capture   edit = log ratio(resolved, original) -> history
                                      -> [Apply]   only if refresh blend < 1, gains != 1 or a debug view
```

Cached frame (the model does not run):

```
ClearStats -> Reproject (history -> this frame, validated; first pyramid level in shared memory;
              rejected-pixel counters) -> Downsample x2 -> Apply (frame x edit -> output)
```

| Pass | What it does |
|---|---|
| Reproject | `q = uv + motion` (vector taken from the nearest surface in 3x3, as TAA does). History read bilinearly with each tap admitted only if its depth agrees (relative, `CacheDepthTolerance`). Colour check on the frame's own log luma (`CacheColourTolerance`, stops). Confidence = previous x colour x depth x decay. |
| Pyramid | Three levels at 1/4, 1/16, 1/64, coverage weighted. The first is built inside Reproject. |
| Apply | Low band = first level read jointly-bilaterally (depth + luma of the texel vs the pixel), coarser levels filling what it cannot (push-pull). `edit = LowGain * low + HighGain * confidence * (history - low)`. |
| AccumulateMv | Motion since the model last ran, chained frame to frame at the motion texture's resolution, in the game's own units. Handed to the model on a refresh so its own history lands where the scene went (`CacheModelHistory` = 1). |

Refresh triggers, in order: no history; the game's reset or a feature rebuild; a measurement dump;
the interval; adaptive -- the fraction of pixels rejected (depth) summed since the last refresh, read
back three frames late, priority (stencil) pixels counted 4x. An early run never comes sooner than
`max(2, interval / 2)` frames after the last: otherwise a steady pan triggers one every frame and the
cache saves nothing exactly when frame rate matters.

## Robustness: vegetation, water, hair

They fail validation constantly -- wind animation absent from motion vectors, transparency, specular
churn -- and fall back to the low band of their own surface, borrowed from validated neighbours at a
similar depth and luma. They keep the model's lighting and lose its synthesised micro-detail until the
next refresh. That trade is deliberate: smearing the model's grass detail over moving grass is the
artefact this must not produce.

## Character priority (stencil)

If the game's depth buffer is `R24G8_TYPELESS` / `R32G8X24_TYPELESS`, the cache copies it and reads the
stencil plane. Pixels with `(stencil & CacheStencilMask) == CacheStencilRef` get half the tolerances and
count 4x toward an adaptive refresh. Which bits a game uses is undocumented: cache debug view 4 colours
each stencil value so it can be found by looking. Whether The Witcher 3's DLSS depth carries stencil at
all is logged on first use.

## Multi-pass approximation

`CacheLowGain` / `CacheHighGain` scale the two bands in log space (2 = twice the effect in stops). With
`CacheInterval = 1` this is a cheap stand-in for running the model twice.
`tools/dlssnr_cache/calibrate_multipass.py` fits the gains from real one-pass / two-pass captures of a
held frame, using the shader's own band split.

## Joint bilateral enlargement (`JbuUpsample`)

Independent of the cache. Below 100% model resolution, the model's residual is brought to full size
guided by the full-size proxy (each small texel weighted by how much it resembles the full-size pixel),
and the resolve then composes two full-size pictures (its classic path). Off is the resolve unchanged.

## Measuring

`RequestCacheDump` (menu button, or `dlssnr-cachedump.trigger` beside OptiScaler) writes
`CacheDumpFrames` consecutive frames with the model running on every one: the frame, the model's frame,
motion (uv offset to the previous frame, dilated exactly as the shader does), pseudo-linear depth and log
luma. `tools/dlssnr_cache/measure_reprojection.py` replays the shader's algorithm on them (its numpy
mirror is `nrcache_common.py` -- keep the two in step) and reports, for lags 1/2/4/8, edit error, PSNR
and the share of Neural Rendering's effect kept, globally and in unstable zones, against no edit, naive
reprojection and low band only.

## Known limits

* **Frame pacing.** Refresh frames cost what they always did and cached frames very little, so frame
  time alternates. Frame generation and VRR hide part of it. The fix is running the model on an async
  compute queue, one or two frames of latency, which the cache already tolerates (the edit is
  reprojected forward anyway) -- not done yet.
* **Tiles.** The model is evaluated whole; there is no partial-frame evaluate in the forwarder, so
  "priority tiles" are realised as priority-weighted refresh scheduling instead.
* **Depth without the projection.** Validation compares a pseudo-linear depth (`1/d` reversed, `1/(1-d)`
  forward), exact up to a constant for reversed-Z and close for forward Z. Close geometry while walking
  forward changes depth by several percent a frame; raise the tolerance if it rejects there.
* **Vulkan** is untouched: the cache is D3D12 only.

## Default-identical

`CacheEnabled = false` and `JbuUpsample = false` (the defaults) take none of the new code: the branch in
`DlssNr_Dx12::Dispatch` is skipped, no resource is created, the resolve gets the same inputs.
The cache also stands aside -- running the model every frame, unchanged -- while Hold frame, Compare, a
Debug view, the proxy path or a capture is active.

Removal: delete `DlssNr_EditCache_Dx12.*`, `DlssNr_CacheCommon.h`, `precompile/dlssnr_cache.hlsl` and
`DlssNr_Cache_Shader.h`, the `Cache*`/`Jbu*` keys in Config, the menu section, and the call sites in
`DlssNr_Dx12.cpp` (search `g_cache`).

Rebuild the shader after editing `dlssnr_cache.hlsl`:

```
cd OptiScaler/shaders/dlssnr/precompile
../../shader_tools/fxc.exe -T cs_5_0 -E CSMain -O3 dlssnr_cache.hlsl -Fo DlssNr_Cache_Shader.cso
python ../../shader_tools/create_header.py DlssNr_Cache_Shader.cso DlssNr_Cache_Shader.h DlssNrCache_cso
```
