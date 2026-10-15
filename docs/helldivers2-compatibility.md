# HELLDIVERS 2: DLSSNR, frame generation and Insert menu

This is a targeted backport onto Skynizz `6bfc4389`, not a full upgrade to
OptiScaler nightly. Runtime validation in HELLDIVERS 2 is still required.

## Evidence and changes

The failing installation's log records an RTX 3080, `FGInput=Upscaler`,
`FGOutput=DLSSG`, `FGNvngxReplacement=Arturs`, and `InterpolationCount=3`.
It also records `CreateSwapChainForHwnd Overlay call!`, the local
`OptiScaler/streamline/sl.interposer.dll` being hooked, and DLSS-G being ignored
because it was not requested by the host. These observations match the affected
paths below; they do not by themselves prove every runtime issue is fixed.

Backports from official OptiScaler, all included by nightly-20261014:

* [a676a402](https://github.com/optiscaler/OptiScaler/commit/a676a402a0cba255162dd673591a8ea760c473cc):
  zero swapchain width/height means use the window size. Do not classify it as
  a tiny overlay and bypass the wrapped swapchain. Applies to all four DXGI
  creation paths.
* [d14b6303](https://github.com/optiscaler/OptiScaler/commit/d14b6303a4475489d7595fddc152b5080aa18bba),
  **LibraryLoad_Hooks.cpp only**: exclude the local Streamline interposer from
  the already-loaded game-module scan, while retaining the NvngxFG exception.
  Otherwise game-input hooks can remove DLSS-G from our own initialization.
* [880402ba](https://github.com/optiscaler/OptiScaler/commit/880402ba204578cccd97a5b72e2b275dc7390a05),
  **Util.cpp only**: make subdirectory checks lexical and case insensitive,
  and handle an empty relative result before dereferencing it.
* [2d91b8e3](https://github.com/optiscaler/OptiScaler/commit/2d91b8e33f5bfd7e241a9736660246ba694f505e):
  make the D3D-device creation guard thread-local so overlapping device creation
  on separate threads cannot leave hooks suppressed.

The DLSSNR implementation, its precompiled shaders and its menu remain intact.
No anti-cheat component is modified by this patch.

## Build

Clone with submodules. Use Visual Studio C++ Build Tools and a Windows SDK.
The original project targets v143; the fork recommends building with v145:

```powershell
git submodule update --init --recursive
msbuild OptiScaler.sln /m:2 /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
```

The Release post-build event moves OptiScaler.dll into `x64/Release/a/`.
The forwarder is `x64/Release/a/nvngx.dll_dlssnr.dll`.

## Testing on an existing OptiScalerClient installation

1. Close the game. Back up `dxgi.dll`, `OptiScaler.ini`, and the existing
   `nvngx.dll_dlssnr.dll`, if present.
2. Keep the existing `OptiScaler/` folder, Streamline files, Arturs provider and
   fakenvapi files. This patch is not a replacement for those dependencies.
3. Install the newly built OptiScaler.dll as **dxgi.dll** beside
   `helldivers2.exe`, and put the matching forwarder there as well. Avoid adding
   a second OptiScaler proxy under winmm.dll or an ASI name.
4. Supply your own `nvngx_dlssnr.dll`. It is not included in this project.
5. Preserve the working frame-generation configuration. In this RTX 3080
   setup that is `[FrameGen] Enabled=true, FGInput=upscaler, FGOutput=dlssg,
   FGNvngxReplacement=Arturs`, and `[DLSSG] InterpolationCount=3`.
   Arturs is a replacement provider, not proof of native NVIDIA DLSS-G support.
6. Set `[Menu] OverlayMenu=true, ShortcutKey=0x2D`, `[DlssNr] Enabled=true`,
   and `[Log] LogToFile=true, LogLevel=2`. Keep DLSS upscaling enabled in game.
7. First confirm Insert opens the menu with NR and FG disabled. Then test NR
   alone, FG alone, and both together. Check visual changes and frame pacing,
   not just the configured multiplier. Retain the new log for each run.

Success requires menu rendering, actual NR model evaluation, and actual frame
generation in the same session. Compilation and configuration checks cannot
establish that. Restore the backed-up files to roll back.
