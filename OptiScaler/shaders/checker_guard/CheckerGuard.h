#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

// Speckle cleanup before the upscaler (FGSpeckleCleanup, off by default). See CG_Common.h for what it removes
// and how it tells it apart from detail.
//
// Runs on the game's render-resolution colour right before the upscaler reads it, before DLSS 5's pre-SR
// pass if that is on: the colour is filtered into a texture of ours and the parameter block handed that
// texture, then given its own pointer back after the upscaler. The game's colour is only read. Off: nothing
// here runs and the parameter block is untouched.
namespace CheckerGuard
{
// Before the upscaler: true when the colour was swapped for the cleaned copy.
bool BeginBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params);

// After the upscaler: puts the game's colour back if it was swapped.
void EndBeforeUpscale(NVSDK_NGX_Parameter* params);
} // namespace CheckerGuard
