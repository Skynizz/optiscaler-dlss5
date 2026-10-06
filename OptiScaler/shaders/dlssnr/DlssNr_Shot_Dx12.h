#pragma once

// Captures of the finished frame for the benchmark report: one picture per mode, tone mapped for a
// screen, and a short run of consecutive frames that says how much the picture flickers.
//
// The frame is the upscaler's output after Neural Rendering -- before the interface and before frame
// generation -- copied on the game's own command list and read back several frames later, when the GPU
// is long done with it. Nothing here runs, and nothing is allocated, unless a capture is asked for.

#include <d3d12.h>

#include <filesystem>

namespace DlssNrShot
{
struct Result
{
    bool valid = false;

    // Mean (of the steadiest 95% of pixels) and 90th percentile of the frame-to-frame change of
    // brightness over the run, in percent. With the camera still, what is left is flicker; the
    // largest 5% -- animated objects, particles -- are left out. flickerP95 holds the 90th percentile.
    float flickerMean = 0.0f;
    float flickerP95 = 0.0f;

    bool picture = false; // the PNG was written
    unsigned int width = 0;
    unsigned int height = 0;
};

// Asks for `frames` consecutive frames (2 or more) from the next one on; the first is written to `png`.
// Ignored while another request is still busy.
void Request(int tag, unsigned int frames, const std::filesystem::path& png);

// A request is still recording, waiting on the GPU, or being written out.
bool Busy();

Result Get(int tag);

// Forgets every result and the exposure the pictures share, so one benchmark's pictures are all
// developed the same way and comparable with each other.
void Reset();

// Every rendered frame, once the output is final. state is the output's state on the way in, and the
// one it is left in.
void Tick(ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, D3D12_RESOURCE_STATES state);

void Shutdown();
} // namespace DlssNrShot
