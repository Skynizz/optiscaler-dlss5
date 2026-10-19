#pragma once

#include <dxgi.h>

// The real swapchain's Present, hooked beneath every layer (Streamline, ReShade, OptiScaler's own wrapper): every
// frame frame generation hands to the display passes here, generated ones included, whether the game ships DLSS
// Frame Generation or OptiScaler injects it. Used by the frame generation meter and the auto multiplier (FGMeter);
// with neither on, nothing is hooked.
namespace PresentPacer
{
// Hooks the real swapchain beneath this one (through Streamline's proxy if there is one), once.
void Hook(IDXGISwapChain* swapChain);
} // namespace PresentPacer
