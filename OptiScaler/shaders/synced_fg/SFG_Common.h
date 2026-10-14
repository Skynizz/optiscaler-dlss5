#pragma once

#include "SysUtils.h"

// Synced frame generation: one generated frame between the last real frame and this one.
//
// The surface that sits at an output pixel at time t (0 the last frame, 1 this one) is found by walking the
// game's motion vectors back from that pixel: a surface moving from P (last frame) to Q (this frame) is at
// Q + m * (1 - t), m the uv offset to the last frame. A few steps of that fixed point land on the surface
// the pixel shows, and its colour is the two frames' colours at P and Q, blended by t.
//
// The motion is taken from the nearest surface of the 3x3 depth texels around (a silhouette carries its own
// motion past its edge, not the background's). Where the two frames disagree -- one of them sees something
// the other does not -- this frame's colour wins: its position was found from its own vectors. A pixel the
// same in both frames (the HUD, anything standing still) is this frame's, untouched.
struct alignas(256) SFGConstants
{
    UINT outWidth;
    UINT outHeight;
    float t;
    UINT invertedDepth;

    float mvScaleX; // motion vector * scale = pixels of the motion texture
    float mvScaleY;
    float mvWidth; // the motion texture's valid region
    float mvHeight;

    float depthWidth; // the depth texture's valid region
    float depthHeight;
    float staticThreshold; // the largest difference, relative, at which a pixel counts as unchanged
    UINT debugView;        // 1 tints generated frames, so their pacing shows
};

inline static std::string sfgCode = R"(
cbuffer Params : register(b0)
{
    uint outWidth;
    uint outHeight;
    float t;
    uint invertedDepth;
    float mvScaleX;
    float mvScaleY;
    float mvWidth;
    float mvHeight;
    float depthWidth;
    float depthHeight;
    float staticThreshold;
    uint debugView;
};

Texture2D<float4> PrevColor : register(t0);
Texture2D<float4> CurColor : register(t1);
Texture2D<float4> Velocity : register(t2);
Texture2D<float4> Depth : register(t3);
RWTexture2D<float4> Output : register(u0);
SamplerState Linear : register(s0);

float2 MotionAt(float2 uv)
{
    const int2 hi = int2(depthWidth, depthHeight) - 1;
    const int2 dc = clamp(int2(uv * float2(depthWidth, depthHeight)), int2(0, 0), hi);
    int2 best = dc;
    float bestD = invertedDepth != 0 ? -1.0 : 2.0;

    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 c = clamp(dc + int2(x, y), int2(0, 0), hi);
            const float d = Depth.Load(int3(c, 0)).r;
            const bool nearer = invertedDepth != 0 ? d > bestD : d < bestD;

            if (nearer)
            {
                bestD = d;
                best = c;
            }
        }
    }

    const float2 buv = (float2(best) + 0.5) / float2(depthWidth, depthHeight);
    const int2 mc = clamp(int2(buv * float2(mvWidth, mvHeight)), int2(0, 0), int2(mvWidth, mvHeight) - 1);
    const float2 mv = Velocity.Load(int3(mc, 0)).xy * float2(mvScaleX, mvScaleY);
    return mv / float2(mvWidth, mvHeight);
}

float MaxOf(float3 v) { return max(v.r, max(v.g, v.b)); }

bool Inside(float2 uv) { return all(uv >= 0.0) && all(uv <= 1.0); }

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outWidth || id.y >= outHeight)
        return;

    const float2 uv = (float2(id.xy) + 0.5) / float2(outWidth, outHeight);
    const float4 prevHere = PrevColor.Load(int3(id.xy, 0));
    const float4 curHere = CurColor.Load(int3(id.xy, 0));

    const float3 delta = abs(curHere.rgb - prevHere.rgb);
    const float scale = max(MaxOf(curHere.rgb), MaxOf(prevHere.rgb)) + 0.02;

    if (MaxOf(delta) / scale <= staticThreshold)
    {
        Output[id.xy] = curHere;
        return;
    }

    float2 m = MotionAt(uv);

    [unroll] for (int i = 0; i < 3; ++i)
        m = MotionAt(uv - m * (1.0 - t));

    const float2 q = uv - m * (1.0 - t);
    const float2 p = uv + m * t;

    const float4 fromCur = CurColor.SampleLevel(Linear, q, 0);
    const float4 fromPrev = PrevColor.SampleLevel(Linear, p, 0);

    float w = t;

    if (!Inside(p))
        w = 1.0;
    else if (!Inside(q))
        w = 0.0;
    else
    {
        const float disagree = MaxOf(abs(fromCur.rgb - fromPrev.rgb)) /
                               (max(MaxOf(fromCur.rgb), MaxOf(fromPrev.rgb)) + 0.02);
        w = lerp(t, 1.0, saturate((disagree - 0.1) / 0.2));
    }

    float4 result = lerp(fromPrev, fromCur, w);

    if (debugView != 0)
        result.rgb *= float3(0.6, 1.0, 0.6);

    Output[id.xy] = result;
}
)";
