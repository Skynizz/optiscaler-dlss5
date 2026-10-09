# OptiScaler DLSS 5

<p align="center">
  <a href="https://github.com/Skynizz/optiscaler-dlss5/releases/download/v1.1.0/OptiScaler-DLSS5-1.1.0-win-x64.zip"><img src="https://img.shields.io/badge/Download-v1.1.0-2ea44f?style=for-the-badge&logo=github" alt="Download"></a>
  <a href="https://github.com/Skynizz/optiscaler-dlss5/releases"><img src="https://img.shields.io/github/downloads/Skynizz/optiscaler-dlss5/total?style=for-the-badge&label=Downloads&color=blue" alt="Downloads"></a>
  <a href="https://github.com/Skynizz/optiscaler-dlss5/releases/latest"><img src="https://img.shields.io/github/v/release/Skynizz/optiscaler-dlss5?style=for-the-badge&label=Latest" alt="Latest release"></a>
</p>

A fork of [OptiScaler](https://github.com/optiscaler/OptiScaler), built on Dagherbou's
[OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR), that runs DLSS 5 Neural Rendering
for a fraction of its usual cost and with less flicker. OptiScaler base: `master` as of 6 October 2026.

NVIDIA's model (`nvngx_dlssnr.dll`) is not included.

## Results

Control Resonant, RTX 4070, 2560x1440, DLSS Balanced (1484x835 internal). FPS is frames the game
actually renders, frame generation excluded. Same scene, fixed camera, 3 s warm-up then 8 s measured
per mode.

| Mode | FPS | 1% low | DLSS 5 cost | Flicker | Added detail |
|---|---:|---:|---:|---:|---:|
| DLSS 5 off | 62.2 | 28.3 | - | 0.30% | - |
| DLSS 5 as OptiScaler ships it | 34.4 | 15.1 | 13.0 ms | 0.57% | +12% |
| **Quality**: pre-SR, model every frame | **46.7** (+36%) | 23.3 | 5.5 ms | 0.40% | +13% |
| Balanced: after SR, model at 67%, every other frame | 48.1 (+40%) | 18.5 | 4.7 ms | 0.42% | +15% |
| **Performance**: pre-SR, every other frame | **55.3** (+61%) | 24.3 | 2.9 ms | 0.47% | +10% |

- **Flicker**: average frame-to-frame brightness change with the camera still, ignoring the 5% of
  pixels that move the most (the "off" row is the game's own noise).
- **Added detail**: local contrast of the image compared with DLSS 5 off, on the static part of the
  scene.

### Against the other DLSS 5 builds

Same scene, each tool on its fastest settings. Cost is what each tool logs for its own pass; FPS is
estimated the same way for all of them (see the method in the comparison doc).

| | DLSS 5 cost | Est. FPS | Flicker | Detail |
|---|---:|---:|---:|---:|
| **This fork, Performance** | **2.89 ms** | **52.7** | 0.40% | x1.19 |
| F5 v0.1.27, DetailReuse | 3.50 ms | 51.1 | 0.52% | x1.23 |
| ShyVortex v0.9.34, pre-SR 75% | 3.74 ms | 50.5 | 0.37% | x1.11 |
| F5 v0.1.27, pre-SR | 4.85 ms | 47.8 | 0.41% | x1.25 |
| ShyVortex v0.9.34, pre-SR 100% | 5.35 ms | 46.7 | 0.45% | x1.34 |
| **This fork, Quality** | 5.53 ms | 46.3 | 0.36% | x1.32 |

The RenoDX DLSS5 add-on (19 September build) never engaged in Control Resonant, or froze the game with
its Streamline hooks on, so it could not be measured.

### Model styles and multi-pass

Same scene, Quality preset (pre-SR, model every frame):

| | FPS | DLSS 5 cost | Flicker | Detail | Effect |
|---|---:|---:|---:|---:|---:|
| DLSS 5 as OptiScaler ships it | 35.0 | 13.1 ms | 0.43-0.52% | x1.11 | 0.29 stop |
| Style Default, 1 pass | 47.7 | 5.5 ms | 0.38-0.40% | x1.12 | 0.29 stop |
| Style Natural | 47.6 | 5.6 ms | 0.36% | x1.19 | 0.28 stop |
| Style Cinematic | 47.1 | 5.6 ms | 0.42% | x0.87 | 0.25 stop |
| 2 passes | 37.6 | 10.8 ms | 0.47% | x1.25 | 0.44 stop |
| 2 passes, model every other frame | 48.0 | 5.5 ms | 0.63% | x1.18 | 0.43 stop |

Two passes in Performance give you the stronger multi-pass look for the cost of a normal single pass,
with a bit more flicker.

Full numbers and method: [docs/comparison.md](docs/comparison.md).

![Comparison](docs/benchmark/zoom.jpg)

### Why the in-game FPS counter may not show it

With frame generation (MFG x2 to x6), the counter multiplies the frames the game renders and then
stops at your refresh rate. At x6 on a 240 Hz screen, 31 base fps shows as 186 and 48 would be 288,
but the screen caps around 225, so both look about the same even though one renders 55% more real
frames. Dynamic MFG makes it worse: it picks the multiplier that hits its target no matter what. Press
F6 in game to see the real number.

## What it changes

- **Edit cache**: the model only runs every N frames. What it changed (a per-pixel ratio, never the
  image itself) is reprojected with the motion vectors, validated per pixel (depth, colour) and laid
  back onto the game's fresh frame.
- **Pre-SR**: the model runs on the render-resolution image right before DLSS Super Resolution, which
  then upscales the result. About three times fewer pixels to process, and DLSS's own temporal
  accumulation smooths what the model adds.
- **Automatic white point**: measured from each scene before the model sees it. No more paper white
  slider to babysit.
- **Anti-flicker**: temporal stabiliser on the edit, smoothed updates between model runs, despeckle,
  regional luminance stabilisation (helps a lot on OLED).
- **Anti-ghosting at long intervals** (model every 4 to 8 frames): a fingerprint of the frame the model
  worked on travels with its edit and drops it once the frame no longer matches, rejected pixels only
  borrow from their own surface, smoothed updates are capped at 3 frames, and the interval shortens with
  camera speed. Guided filter and aging are there too, off by default. Debug views show the rejection mask
  and the edit's age; with ShowStats the overlay gives each cache pass's GPU time.
- **Reduced model resolution** with edge-aware upscaling of the result (after-SR placement).
- **Multi-pass** (1 to 3 passes): the model runs again on its own answer, each pass with its own
  history, composed once. Each pass is built one frame before it is used, which avoids the GPU hang
  that got multi-pass removed from the original fork.
- **Model style** (Default, Natural, Cinematic) right next to the presets.
- **Strong preset**: after the upscaler (works with Ray Reconstruction), Natural style, 2 model passes,
  model every other frame. The strongest look for about the cost of one pass every frame.
- **Motion priority and GPU budget**: ghosting only exists in motion, so with a long interval the model now
  runs at the long interval standing still and about every other frame as soon as the camera moves (priority
  0.5 by default). Or give DLSS 5 a budget in ms and the interval adapts to stay under it.
- **Model in the background (async compute)**, off by default: on the frames the model runs, it runs on a
  GPU queue of its own, in parallel with the game's next frame, and its answer is laid down two frames later.
  At 8 frames between runs (Control, 4070): frame pacing halved, 1% lows +14%, slightly faster. No gain at
  3 frames between runs.
- **Steady light at long intervals**: at 8 frames between runs the light seemed to pulse with the model.
  Measured frame by frame, the frame the model ran on moved twice as much as the others, almost all of it on
  things that move without motion vectors (paper, debris, hands) that got the model's answer for one frame.
  They now fade to it like everything else (soft refresh), and the fine detail no longer fades between runs:
  the model's frame moves no more than the others, with 6% more detail.
- **Light pop-in smoothing**: when a light enters the frame, the model re-grades the whole picture at once.
  Where the game's own image did not change, that change now fades in (1.5 stops/s by default) instead of
  landing in one frame; real light changes still pass at once.
- **Live comparison**: F6 cycles optimised / vanilla / off without touching your settings and shows
  the real rendered FPS on screen.
- **Built-in benchmark**: runs the modes back to back, captures each one and writes an HTML report.

Everything can be turned on and off from the OptiScaler menu and from `OptiScaler.ini`. Turned off,
it behaves exactly like the original.

## Install

Download `OptiScaler-DLSS5-1.1.0-win-x64.zip` from the [Releases](https://github.com/Skynizz/optiscaler-dlss5/releases)
page, unzip it, run `INSTALLER.bat` and drop the game folder (the one with the exe) onto it.
The installer picks a free file name (`winmm.dll`, or `OptiScaler.asi` if there is an ASI loader) and
never overwrites anything. `DESINSTALLER.bat` puts everything back.

You need an RTX card, a recent driver, Python, and `nvngx_dlssnr.dll` (the installer looks for it in
the game folder and in `%LOCALAPPDATA%\RHI\DLSS-NR`).

In game: turn DLSS on, open the OptiScaler menu with Insert, then the **DLSS 5** tab.

## Build

Full source with every submodule: `OptiScaler-DLSS5-1.1.0-source.zip` on the release page (GitHub's
own "Source code" archives leave the submodules out, so they will not build). Or clone it:

```
git clone --recursive https://github.com/Skynizz/optiscaler-dlss5.git
```

Visual Studio 2026 Build Tools (toolset v145):

```
msbuild OptiScaler\OptiScaler.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
```

The cache shader is compiled with `fxc`, the composition shader with `dxc`: see
[OptiScaler/dlssnr/README.md](OptiScaler/dlssnr/README.md) and
[OptiScaler/dlssnr/design/edit-cache.md](OptiScaler/dlssnr/design/edit-cache.md).
`deploy/install.py` installs a build into a game.

## License and credits

GPL-3.0, like OptiScaler. The original OptiScaler README is in
[README_OptiScaler.md](README_OptiScaler.md).

- OptiScaler: cdozdil and the OptiScaler team.
- DLSS 5 Neural Rendering integration: Dagherbou.
- Colour composition taken from the RenoDX DLSS 5 add-on (clshortfuse, MIT license): see
  [Licenses/RenoDX_ATTRIBUTION.txt](Licenses/RenoDX_ATTRIBUTION.txt).

Unofficial project, not affiliated with NVIDIA. It uses an undocumented driver feature;
`nvngx_dlssnr.dll` belongs to NVIDIA and is not redistributed here.
