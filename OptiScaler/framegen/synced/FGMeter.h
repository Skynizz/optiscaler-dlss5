#pragma once

#include <d3d12.h>
#include <dxgi.h>

// Frame generation meter and auto multiplier.
//
// The meter (FGMeter, off by default) measures what frame generation actually delivers, from the real swapchain's
// Present beneath Streamline: frames shown and rendered per second, the multiplier that results, how evenly the
// frames are handed to the display (on time, stacked on the one before, late), how many frames wait in the
// display queue, and -- from a 64x36 thumbnail of each frame -- how many generated frames are duplicates of the
// one before them while the scene moves.
//
// The auto multiplier (FGAutoMultiplier, off by default) picks 2x to 6x from the rendered frame rate so the frames
// shown stay just under a target (the display's refresh rate by default): fewer invented frames when they are not
// needed, never more frames than the display can show. Whole multipliers only, with hysteresis. It sets a
// temporary value: the multiplier saved in the ini is not touched and comes back when it is turned off.
namespace FGMeter
{
struct Snapshot
{
    bool valid = false;
    double shownFps = 0.0;
    double renderedFps = 0.0;
    double multiplier = 0.0;
    double onTime = 0.0;  // fractions of the intervals between frames handed to the display
    double stacked = 0.0;
    double late = 0.0;
    bool duplicatesKnown = false; // the scene moved enough to judge
    double duplicates = 0.0;      // fraction of frames identical to the one before
    int queued = -1;              // frames presented but not yet on screen
    double refresh = 0.0;         // display refresh rate, Hz
    double target = 0.0;          // the auto multiplier's target, frames shown per second
    int autoMultiplier = 0;       // 0: auto off or not running

    // Tearing: how frames are handed to the display.
    double tearingAllowed = 0.0; // fraction presented with tearing allowed (sync interval 0)
    double fasterThanScan = 0.0; // fraction arriving less than one refresh after the one before
    double tearRisk = 0.0;       // both: a flip in the middle of the screen being drawn -- a tear line
    int syncInterval = -1;       // the last one used
};

// The upscaler ran for a new frame (the game's render thread): rendered frame rate, and the auto multiplier.
void OnRealFrame();

// The real swapchain's Present, right before and right after the call (with the sync interval and flags it is
// called with).
void BeforePresent(UINT syncInterval, UINT flags);
void AfterPresent(IDXGISwapChain* swapChain);

// A thumbnail of the frame about to be presented, recorded on the queue that presents it.
void Thumbnail(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);

// Whether the real swapchain's Present needs hooking for the meter or the auto multiplier.
bool WantsPresentHook();

Snapshot Get();
} // namespace FGMeter
