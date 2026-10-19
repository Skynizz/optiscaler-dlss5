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
// the other does not -- the blend leans toward the sample closer to this frame's pixel. A pixel the same in
// both frames (the HUD, anything standing still) is this frame's. Every one of these is a sliding weight, never
// a switch: a switch flickers on foliage.
//
// The HUD. The motion vectors are the scene's, so a pixel next to the crosshair or the weapon panel fetched them
// from where the scene came from -- and copied them there: a trail of ghost HUD at every camera move. A sample
// whose colour stands still while the scene under it moves is HUD, and the other frame's sample (which sees the
// scene there) is taken instead. When OptiScaler hands over the frame without its HUD (HUDFix), the scene is
// interpolated from that alone and this frame's HUD laid back on top, untouched.
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

    UINT hasHudless;   // t4 / t5 hold the frames without their HUD
    float uiThreshold; // the difference, relative, between the frame and its HUD-less copy that marks HUD
    UINT pad0;
    UINT pad1;
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
    uint hasHudless;
    float uiThreshold;
    uint pad0;
    uint pad1;
};

Texture2D<float4> PrevColor : register(t0);
Texture2D<float4> CurColor : register(t1);
Texture2D<float4> Velocity : register(t2);
Texture2D<float4> Depth : register(t3);
Texture2D<float4> HudlessPrev : register(t4);
Texture2D<float4> HudlessCur : register(t5);
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

// The motion at the pixel itself, undilated: the surface it shows, not the nearest one around it.
float2 MotionHere(float2 uv)
{
    const int2 mc = clamp(int2(uv * float2(mvWidth, mvHeight)), int2(0, 0), int2(mvWidth, mvHeight) - 1);
    return Velocity.Load(int3(mc, 0)).xy * float2(mvScaleX, mvScaleY) / float2(mvWidth, mvHeight);
}

// The difference between the two frames at a pixel, relative to its brightness.
float ChangeAt(int2 c)
{
    const float4 a = PrevColor.Load(int3(c, 0));
    const float4 b = CurColor.Load(int3(c, 0));
    return MaxOf(abs(b.rgb - a.rgb)) / (max(MaxOf(a.rgb), MaxOf(b.rgb)) + 0.02);
}

// How much a sample looks like HUD, 0 to 1: a colour that stands still where the scene under it moves. The scene's
// motion is the pixel's own -- the dilated one took a leaf's motion for the background between the leaves, which
// stands still under a slow move, and foliage passed for HUD, flipping from one frame to the other.
float HudScore(float2 uv)
{
    const int2 c = clamp(int2(uv * float2(outWidth, outHeight)), int2(0, 0), int2(outWidth, outHeight) - 1);
    const float2 motion = MotionHere(uv) * float2(outWidth, outHeight);
    const float moving = saturate((length(motion) - 1.5) / 1.5);
    const float still = saturate(1.0 - ChangeAt(c) / (staticThreshold * 3.0));
    return moving * still;
}

// Catmull-Rom, five bilinear taps, clamped to the four texels around: as sharp as the real frames. Bilinear
// softened every generated frame a little, and fine detail -- leaves, branches -- went sharp, soft, sharp: a shimmer.
float4 SampleSharp(Texture2D<float4> tex, float2 uv)
{
    const float2 size = float2(outWidth, outHeight);
    const float2 pos = uv * size;
    const float2 c1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (c1 - 1.0) / size;
    const float2 t3 = (c1 + 2.0) / size;
    const float2 t12 = (c1 + w2 / w12) / size;

    const float a = w12.x * w0.y;
    const float b = w0.x * w12.y;
    const float c = w12.x * w12.y;
    const float d = w3.x * w12.y;
    const float e = w12.x * w3.y;

    float4 r = tex.SampleLevel(Linear, float2(t12.x, t0.y), 0) * a + tex.SampleLevel(Linear, float2(t0.x, t12.y), 0) * b +
               tex.SampleLevel(Linear, t12, 0) * c + tex.SampleLevel(Linear, float2(t3.x, t12.y), 0) * d +
               tex.SampleLevel(Linear, float2(t12.x, t3.y), 0) * e;
    r /= a + b + c + d + e;

    const int2 hi = int2(outWidth, outHeight) - 1;
    const int2 base = int2(c1 - 0.5);
    const float4 s00 = tex.Load(int3(clamp(base, int2(0, 0), hi), 0));
    const float4 s10 = tex.Load(int3(clamp(base + int2(1, 0), int2(0, 0), hi), 0));
    const float4 s01 = tex.Load(int3(clamp(base + int2(0, 1), int2(0, 0), hi), 0));
    const float4 s11 = tex.Load(int3(clamp(base + int2(1, 1), int2(0, 0), hi), 0));

    return clamp(r, min(min(s00, s10), min(s01, s11)), max(max(s00, s10), max(s01, s11)));
}

// Every decision below is a weight that slides, never a switch. A switch flips from one pixel to its neighbour and
// from one generated frame to the next wherever the two frames half agree -- foliage above all, fine, contrasted,
// with noisy motion -- and the leaves trembled and slid.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outWidth || id.y >= outHeight)
        return;

    const float2 uv = (float2(id.xy) + 0.5) / float2(outWidth, outHeight);
    const float4 prevHere = PrevColor.Load(int3(id.xy, 0));
    const float4 curHere = CurColor.Load(int3(id.xy, 0));

    // HUD-less frames: where this frame differs from its HUD-less copy is HUD, shown as it is now.
    if (hasHudless != 0)
    {
        const float4 sceneHere = HudlessCur.Load(int3(id.xy, 0));
        const float uiScale = max(MaxOf(curHere.rgb), MaxOf(sceneHere.rgb)) + 0.02;

        if (MaxOf(abs(curHere.rgb - sceneHere.rgb)) / uiScale > uiThreshold)
        {
            Output[id.xy] = curHere;
            return;
        }
    }

    // Unchanged: this frame's pixel. Fading out of it rather than dropping it at a threshold.
    const float change = MaxOf(abs(curHere.rgb - prevHere.rgb)) / (max(MaxOf(curHere.rgb), MaxOf(prevHere.rgb)) + 0.02);

    if (change <= staticThreshold * 0.5)
    {
        Output[id.xy] = curHere;
        return;
    }

    const float unchanged = saturate(1.0 - (change - staticThreshold * 0.5) / (staticThreshold * 1.5));

    float2 m = MotionAt(uv);

    [unroll] for (int i = 0; i < 3; ++i)
        m = MotionAt(uv - m * (1.0 - t));

    const float2 q = uv - m * (1.0 - t);
    const float2 p = uv + m * t;

    float4 fromCur;
    float4 fromPrev;

    if (hasHudless != 0)
    {
        // The scene alone: nothing of the HUD can be dragged along.
        fromCur = SampleSharp(HudlessCur, q);
        fromPrev = SampleSharp(HudlessPrev, p);
    }
    else
    {
        fromCur = SampleSharp(CurColor, q);
        fromPrev = SampleSharp(PrevColor, p);
    }

    float w = t;      // how much of this frame's sample
    float here = 0.0; // how much of the pixel as this frame shows it

    if (!Inside(p))
        w = 1.0;
    else if (!Inside(q))
        w = 0.0;
    else
    {
        float hud = 0.0;

        if (hasHudless == 0)
        {
            // A piece of HUD fetched from beside the pixel on one side: the other side's sample. On both: the
            // pixel itself.
            const float hc = HudScore(q);
            const float hp = HudScore(p);
            w = lerp(w, 0.0, hc * (1.0 - hp));
            w = lerp(w, 1.0, hp * (1.0 - hc));
            here = hc * hp;
            hud = max(hc, hp);
        }

        const float norm = max(MaxOf(fromCur.rgb), MaxOf(fromPrev.rgb)) + 0.02;
        const float disagree = MaxOf(abs(fromCur.rgb - fromPrev.rgb)) / norm;

        // The two frames disagree (one sees what the other does not): lean toward the sample closer to what is
        // really at this pixel now -- by how much closer, not which.
        const float toCur = MaxOf(abs(fromCur.rgb - curHere.rgb));
        const float toPrev = MaxOf(abs(fromPrev.rgb - curHere.rgb));
        const float side = smoothstep(0.0, 1.0, toPrev / (toCur + toPrev + 1e-4));
        w = lerp(w, side, saturate((disagree - 0.12) / 0.3) * (1.0 - hud));

        // Screen-anchored -- a transparent HUD, its colour moving with the scene behind it: the pixel matches
        // itself in the last frame far better than the scene moved along its motion does, while that motion is
        // real (a couple of pixels at least, the pixel's own).
        if (hasHudless == 0)
        {
            const float2 motion = MotionHere(uv) * float2(outWidth, outHeight);
            const float moving = saturate((length(motion) - 2.0) / 2.0);
            const float anchored = saturate((disagree * 0.25 - change) / max(disagree * 0.15, 1e-3));
            here = max(here, moving * anchored * saturate((disagree - 0.2) / 0.15));
        }
    }

    float4 result = lerp(fromPrev, fromCur, w);
    result = lerp(result, curHere, max(here, unchanged));

    if (debugView != 0)
        result.rgb *= float3(0.6, 1.0, 0.6);

    Output[id.xy] = result;
}
)";
