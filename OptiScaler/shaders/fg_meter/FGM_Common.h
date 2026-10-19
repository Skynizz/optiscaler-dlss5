#pragma once

#include "SysUtils.h"

// Frame generation meter: a 64x36 luminance thumbnail of each frame about to be presented, so consecutive frames
// can be compared on the CPU. A generated frame identical to the one before it (every generated frame put at the
// midpoint between two real ones, the known fault of multi frame generation on an RTX 40 without its temporal
// fix) shows up as a near-zero difference while the scene moves.
struct alignas(256) FGMConstants
{
    UINT srcWidth;
    UINT srcHeight;
    UINT thumbWidth;
    UINT thumbHeight;
};

inline static std::string fgmCode = R"(
cbuffer Params : register(b0)
{
    uint srcWidth;
    uint srcHeight;
    uint thumbWidth;
    uint thumbHeight;
};

Texture2D<float4> Src : register(t0);
RWTexture2D<float> Thumb : register(u0);

[numthreads(8, 4, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= thumbWidth || id.y >= thumbHeight)
        return;

    const uint x0 = id.x * srcWidth / thumbWidth;
    const uint x1 = max((id.x + 1) * srcWidth / thumbWidth, x0 + 1);
    const uint y0 = id.y * srcHeight / thumbHeight;
    const uint y1 = max((id.y + 1) * srcHeight / thumbHeight, y0 + 1);

    float sum = 0.0;
    uint n = 0;

    // Every other pixel each way: a quarter of the block is plenty for an average.
    for (uint y = y0; y < y1; y += 2)
    {
        for (uint x = x0; x < x1; x += 2)
        {
            sum += dot(max(Src.Load(int3(x, y, 0)).rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
            n++;
        }
    }

    Thumb[id.xy] = sum / max(n, 1u);
}
)";
