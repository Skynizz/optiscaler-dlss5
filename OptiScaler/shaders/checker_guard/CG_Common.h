#pragma once

#include "SysUtils.h"

// Speckle cleanup, on the game's render-resolution colour just before the upscaler reads it.
//
// Ray-traced effects (ambient occlusion above all, in The Witcher 3) trace a few rays per pixel and count on
// their denoiser's history to smooth the result. When the camera turns fast that history is thrown away and the
// raw result reaches the image: isolated dark and bright pixels, one in seven on a dirt path in The Witcher 3.
// The upscaler then blows each noisy render pixel up by 1.5x on a regular grid -- the mesh of dots -- and frame
// generation copies it into every frame it makes, five out of six at 6x.
//
// A speckle is a pixel far outside what its eight neighbours agree on. Their median and median deviation say
// what that agreement is; a pixel beyond it (by a margin, relative to its brightness) is pulled to the
// neighbours' colour at their median brightness. Edges survive -- along an edge half the neighbours agree with
// the pixel -- and so does everything that is not moving fast: a still image is the effect's own denoiser's
// job, and detail the upscaler builds up from jitter is left alone.
//
// Only on smooth surfaces. Ambient occlusion noise lies on the ground, paths, rocks and walls; a leaf against the
// sky or a branch against the background is also a pixel unlike its neighbours, and the first version flattened
// foliage into a cartoon. Depth tells them apart: on any plane, depth stored as 1/z (reversed, or 1 - d for the
// standard convention) is affine in screen space, so its second difference is zero. Where it is not -- a
// silhouette, a blade, a leaf -- the pixel is real detail and left alone.
struct alignas(256) CGConstants
{
    UINT width; // the texture
    UINT height;
    UINT rectWidth; // the rendered region; the rest is copied untouched
    UINT rectHeight;

    UINT mvWidth; // the motion texture
    UINT mvHeight;
    float mvScaleX; // motion vector * scale = pixels of the motion texture
    float mvScaleY;

    float strength;  // 0..1
    float threshold; // the margin beyond the neighbours' spread, relative to brightness
    float minMotion; // render pixels per frame below which nothing is touched
    UINT debugView;  // 1 tints what is cleaned

    UINT depthWidth; // the depth texture (0: no depth, the surface test is skipped)
    UINT depthHeight;
    UINT depthInverted; // 1: reversed Z
    float planarTolerance; // second difference of 1/z over its largest value, beyond which it is not a surface
};

inline static std::string cgCode = R"(
cbuffer Params : register(b0)
{
    uint width;
    uint height;
    uint rectWidth;
    uint rectHeight;
    uint mvWidth;
    uint mvHeight;
    float mvScaleX;
    float mvScaleY;
    float strength;
    float threshold;
    float minMotion;
    uint debugView;
    uint depthWidth;
    uint depthHeight;
    uint depthInverted;
    float planarTolerance;
};

Texture2D<float4> Color : register(t0);
Texture2D<float4> Motion : register(t1);
Texture2D<float4> Depth : register(t2);
RWTexture2D<float4> Output : register(u0);

// 1/z up to a constant, whatever the convention: reversed Z stores it, standard Z stores about 1 - it.
float InvZ(int2 c, int2 hi)
{
    const float d = Depth.Load(int3(clamp(c, int2(0, 0), hi), 0)).r;
    return depthInverted != 0 ? d : 1.0 - d;
}

// 1 on a smooth surface, 0 on a silhouette, a blade, a leaf.
float Surface(int2 p)
{
    if (depthWidth == 0 || depthHeight == 0)
        return 1.0;

    const int2 hi = int2((int) depthWidth, (int) depthHeight) - 1;
    const int2 c = clamp(int2((float2(p) + 0.5) * float2(depthWidth, depthHeight) / float2(rectWidth, rectHeight)),
                         int2(0, 0), hi);
    const float z0 = InvZ(c, hi);
    const float zl = InvZ(c - int2(1, 0), hi);
    const float zr = InvZ(c + int2(1, 0), hi);
    const float zu = InvZ(c - int2(0, 1), hi);
    const float zd = InvZ(c + int2(0, 1), hi);
    const float zmax = max(max(max(z0, zl), max(zr, zu)), zd);

    // The sky (nothing there): not a surface either.
    if (zmax <= 1e-6)
        return 0.0;

    const float curvature = max(abs(zl + zr - 2.0 * z0), abs(zu + zd - 2.0 * z0)) / zmax;
    return 1.0 - smoothstep(planarTolerance, planarTolerance * 3.0, curvature);
}

float Luma(float3 c) { return dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722)); }

#define CS(a, b) { const float lo = min(v[a], v[b]); const float hi = max(v[a], v[b]); v[a] = lo; v[b] = hi; }

// Batcher's odd-even merge sort, 19 comparisons.
void Sort8(inout float v[8])
{
    CS(0, 1) CS(2, 3) CS(4, 5) CS(6, 7)
    CS(0, 2) CS(1, 3) CS(4, 6) CS(5, 7)
    CS(1, 2) CS(5, 6)
    CS(0, 4) CS(1, 5) CS(2, 6) CS(3, 7)
    CS(2, 4) CS(3, 5)
    CS(1, 2) CS(3, 4) CS(5, 6)
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);

    if (p.x >= int(width) || p.y >= int(height))
        return;

    const float4 colour = Color.Load(int3(p, 0));
    const int2 hi = int2(rectWidth, rectHeight) - 1;

    if (p.x > hi.x || p.y > hi.y)
    {
        Output[p] = colour;
        return;
    }

    // How far the pixel moved since the last frame, in render pixels: nothing still is touched.
    const int2 mc = clamp(int2((float2(p) + 0.5) * float2(mvWidth, mvHeight) / float2(rectWidth, rectHeight)),
                          int2(0, 0), int2(mvWidth, mvHeight) - 1);
    const float2 mv = Motion.Load(int3(mc, 0)).xy * float2(mvScaleX, mvScaleY);
    const float motion = length(mv * float2(rectWidth, rectHeight) / float2(mvWidth, mvHeight));
    const float moving = smoothstep(minMotion, minMotion * 4.0, motion);

    float w = 0.0;
    float3 result = colour.rgb;

    const float surface = moving > 0.0 ? Surface(p) : 0.0;

    if (moving > 0.0 && strength > 0.0 && surface > 0.0)
    {
        float ring[8];
        float v[8];
        float3 sum = 0.0;
        int n = 0;

        [unroll] for (int y = -1; y <= 1; ++y)
        {
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0)
                    continue;

                const float3 c = Color.Load(int3(clamp(p + int2(x, y), int2(0, 0), hi), 0)).rgb;
                ring[n] = Luma(c);
                v[n] = ring[n];
                sum += c;
                n++;
            }
        }

        Sort8(v);
        const float median = 0.5 * (v[3] + v[4]);

        [unroll] for (int i = 0; i < 8; ++i)
            v[i] = abs(ring[i] - median);

        Sort8(v);
        const float spread = 0.5 * (v[3] + v[4]);

        const float L = Luma(colour.rgb);
        const float margin = 2.5 * spread + threshold * (median + 0.02);
        const float outlier = saturate((abs(L - median) - margin) / (margin + 1e-6));

        w = outlier * moving * surface * saturate(strength);

        if (w > 0.0)
        {
            // The neighbours' colour, at their median brightness.
            const float3 mean = sum / 8.0;
            const float3 target = mean * (median / (Luma(mean) + 1e-4));
            result = lerp(colour.rgb, target, w);
        }
    }

    // Red: what is cleaned. Blue: moving but protected as real detail. Green: depth not readable (zero).
    if (debugView != 0)
    {
        result = lerp(result, float3(1.0, 0.1, 0.1) * max(Luma(result), 0.3), w * 0.8);

        if (debugView == 2 && moving > 0.0)
            result = lerp(result, float3(0.1, 0.3, 1.0) * max(Luma(result), 0.3), (1.0 - surface) * 0.6);

        if (debugView == 3)
        {
            const int2 dhi = int2((int) max(depthWidth, 1u), (int) max(depthHeight, 1u)) - 1;
            const float d = Depth.Load(int3(clamp(int2((float2(p) + 0.5) * float2(depthWidth, depthHeight) /
                                                       float2(rectWidth, rectHeight)), int2(0, 0), dhi), 0)).r;
            result = float3(frac(d * 64.0), d > 0.0 ? 0.0 : 1.0, saturate(d * 8.0));
        }
    }

    Output[p] = float4(result, colour.a);
}
)";
