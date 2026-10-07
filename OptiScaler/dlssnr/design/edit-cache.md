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
the interval. With `CacheAdaptive` the interval follows three steady motion regimes -- still (twice the
interval, up to 8), moving (as set), fast (half) -- decided from the share of pixels rejected, read back
three frames late, with hysteresis so the cadence never hops (an irregular cadence flickers by itself).

## Robustness: vegetation, water, hair

They fail validation constantly -- wind animation absent from motion vectors, transparency, specular
churn -- and fall back to the low band of their own surface, borrowed from validated neighbours at a
similar depth and luma. They keep the model's lighting and lose its synthesised micro-detail until the
next refresh. That trade is deliberate: smearing the model's grass detail over moving grass is the
artefact this must not produce.

## Anti-ghosting at long intervals

At 8 frames between runs the carried edit trailed, like early frame generation. Two causes, measured
offline on Control dumps (`tools/dlssnr_cache/measure_ghosting.py`):

* Validation compared each frame only with the one before it. Anything that changed a little every
  frame never failed: a sliding shadow, debris with no motion vectors, the light the model put on the
  ground under an object that has since moved. The low band was never rejected at all.
* The keyframe crossfade spread each new answer over the whole interval, so at 8 frames the previous
  answer stayed partly on screen for up to 16.

Five switches, each one off being the cache as it was:

| Switch | What it does | Default |
|---|---|---|
| `CacheFingerprint` | On a refresh, the frame's normalised log luma and two log chroma ratios are stored per pixel (`_histPrint`) and carried with the edit, never updated. Every cached frame compares them with the range this frame spans over the pixel's 3x3 neighbourhood (TAA-style, so aliasing and sub-pixel shifts pass). A mismatch multiplies a per-pixel validity (`_histMeta`) that only a refresh restores. A rejected pixel leaves the pyramid and keeps no detail; its surface's low band stands in. | on |
| `CacheContext` | The same test on the surroundings: a quarter-size mean log luma, tent-filtered (`_context`). Catches the light an object left behind on the ground. | on |
| `CacheSurfaceFill` | The low band is read jointly-bilaterally at every pyramid level, not only the first, and fades to no edit where nothing similar is near. Without it, a character whose edit was rejected took the wall's from the coarse levels. | on |
| `CacheCrossfadeFrames` | The crossfade reaches the model's answer within this many frames instead of over the whole interval. No effect at 3 or less. | 3 |
| `CacheAdaptiveSpeed` | With `CacheAdaptive`: the interval follows the camera's speed (mean motion, a GPU counter): `CacheInterval` at about 0.1% of the width per frame, shorter in proportion beyond, down to `CacheAdaptiveMin`; and the model runs at once when `CacheAdaptiveThreshold` of the frame has no believed edit left. | on |
| `CacheGuided` | A fast guided filter (He et al., half size, 6 px window) rebuilds the carried edit from the frame's log luma, weighted by how far the edit has travelled. | off |
| `CacheAging` | Carried detail fades with the motion it has travelled (`_histMeta.z`), toward the low band or (`CacheAgeNeutral`) toward no edit. | off |

Measured at an 8-frame interval on three dumps (pan, pan with the character and flying debris, almost
still), against the model's real answer on every frame, for the defaults against the cache before:

| | mean error | strongest trails (p99.5) | area where the edit hurts | effect kept |
|---|---:|---:|---:|---:|
| pan | -24% | -60% | -41% | 86.4 -> 89.3% |
| pan, character and debris | -16% | -26% | -23% | 82.6 -> 85.0% |
| almost still | -7% | -4% | -74% | 93.2 -> 93.6% |

On top of those, the guided filter and aging measured slightly worse on average (they also soften
detail that was right), which is why they are off; the guided filter does trim the strongest trails a
little further.

Debug views 4 to 6 (`CacheDebugView`): the rejection mask (green believed, red rejected by the
fingerprint, orange by depth), the edit's age in frames and its staleness (motion travelled). With
`ShowStats` the on-screen line adds the cadence, the rejected shares and the GPU time of each cache pass
(timestamps on the game's list, read back four frames later).

Memory, when on: two RGBA16F frame-size textures each for the fingerprint and the meta (about 60 MB at
1440p); the guided filter adds three more at full or half size.

## Pre-SR placement (`PreSr`)

Off by default. On, the pass runs before the game's DLSS Super Resolution instead of after it: in
`NVSDK_NGX_D3D12_EvaluateFeature`, ahead of the SuperSampling evaluate, `EvaluateBeforeUpscale` copies the
game's render-resolution colour into a texture of ours (cache shader mode 11, read as an SRV where DLSS
reads it -- the game's resource is never written or transitioned), runs the whole pass on the copy and
swaps the copy into the parameter block; `EndBeforeUpscale` puts the game's pointer back after the
evaluate. `EvaluateAfterUpscale` then only times the frame and feeds the benchmark capture.

The model works on the render resolution (1484x835 instead of 2560x1440 for DLSS Balanced at 1440p) and
the upscaler's temporal accumulation steadies what it adds. Model resolution and JBU do not apply.

The frame is then the jittered render: each frame samples the scene a fraction of a pixel elsewhere and
the motion vectors leave that out. The cache adds the change of the game's `Jitter_Offset` (in uv) to
every reprojection (`JitterDeltaX/Y`); without it the high band lands up to a pixel off each frame and the
upscaler averages it away (measured in Control: detail x1.05 with the cache, x1.12 without it).

## Hot-swap comparison (`CompareKey`, menu)

`ActiveCompare()` overrides what the pass runs as -- the saved settings, as OptiScaler ships it (model
every frame, full size, after the upscaler), or off -- without writing any setting, so nothing of it can
reach the ini. The key cycles it, the menu has radio buttons, and a line on screen (`RenderOverlay`) says
which mode runs and how many frames the game renders per second: with frame generation and a refresh cap
the game's own counter hides the difference.

## Benchmark with captures

Off / vanilla / your settings / your settings pre-SR (optional), 3 s warm-up and 8 s measured each,
through the same override. After each phase `DlssNr_Shot_Dx12` copies 6 consecutive output frames on the
game's list, reads them back 8 frames later, measures the frame-to-frame brightness change (trimmed mean
and p95, still camera) and writes the first frame as a PNG (WIC, one exposure for the whole run). The
page goes to `dlssnr-benchmark/<date>/rapport.html`; `dlssnr-benchmark.html` opens the latest.

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

`PreSr = false` (the default) never swaps anything; `CompareKey` can be unbound (-1).

Removal: delete `DlssNr_EditCache_Dx12.*`, `DlssNr_CacheCommon.h`, `DlssNr_Shot_Dx12.*`,
`precompile/dlssnr_cache.hlsl` and `DlssNr_Cache_Shader.h`, the `Cache*`/`Jbu*`/`PreSr`/`CompareKey`/
`ShowStats` keys in Config, the menu sections, the two calls in `NVNGX_DLSS_Dx12.cpp` and the call sites
in `DlssNr_Dx12.cpp` (search `g_cache`, `g_preSr`, `ActiveCompare`).

Rebuild the shader after editing `dlssnr_cache.hlsl`:

```
cd OptiScaler/shaders/dlssnr/precompile
../../shader_tools/fxc.exe -T cs_5_0 -E CSMain -O3 dlssnr_cache.hlsl -Fo DlssNr_Cache_Shader.cso
python ../../shader_tools/create_header.py DlssNr_Cache_Shader.cso DlssNr_Cache_Shader.h DlssNrCache_cso
```
