// The temporal edit cache for Neural Rendering. See DlssNr_CacheCommon.h for what it is and why.
//
// The edit is stored as log2((edited + eps) / (original + eps)) per channel. A ratio rather than a
// difference because a ratio survives the lighting changing underneath it -- a torch flickering, the
// exposure adapting -- and log so that averaging it (the pyramid, the bilinear taps) is a geometric
// mean of ratios rather than an arithmetic one, which would favour brightening.
//
// Compiled with fxc cs_5_0 like the composition shader beside it; see README.md, "Editing the shader".

cbuffer Params : register(b0)
{
    uint  gMode;
    uint  gWidth;
    uint  gHeight;
    uint  gDepthW;
    uint  gDepthH;
    uint  gMotionW;
    uint  gMotionH;
    float gMvScaleX;
    float gMvScaleY;
    uint  gDepthInverted;
    float gEpsilon;
    float gDepthTol;
    float gColourTol;
    float gHighDecay;
    float gRefreshBlend;
    uint  gHistValid;
    float gLowGain;
    float gHighGain;
    uint  gBilateral;
    uint  gDebugView;
    uint  gStencilOn;
    uint  gStencilMask;
    uint  gStencilRef;
    uint  gStatsSlot;
    uint  gAccReset;
    uint  gPassthrough;
    float gJbuSigma;
    uint  gSrcW;
    uint  gSrcH;
    uint  gFrameIndex;
};

Texture2D<float4>   gHistEdit  : register(t0); // rgb: log2 edit, a: high-band confidence
Texture2D<float4>   gHistGuide : register(t1); // r: pseudo-linear depth, g: log2 luma of the frame
Texture2D<float4>   gColour    : register(t2); // the frame as the upscaler wrote it (or the full proxy)
Texture2D<float4>   gDepth     : register(t3);
Texture2D<float4>   gMotion    : register(t4);
Texture2D<float4>   gAux0      : register(t5); // per mode: NR result / L1 / small proxy / previous acc
Texture2D<float4>   gAux1      : register(t6); // per mode: L1 guide / small model answer
Texture2D<float4>   gAux2      : register(t7); // L2
Texture2D<float4>   gAux3      : register(t8); // L3
Texture2D<uint2>    gStencil   : register(t9); // the depth buffer's stencil plane, when there is one

RWTexture2D<float4> gOut0  : register(u0);
RWTexture2D<float4> gOut1  : register(u1);
RWTexture2D<float4> gOut2  : register(u2);
RWTexture2D<float4> gOut3  : register(u3);
RWTexture2D<float4> gOut4  : register(u4);
RWTexture2D<uint>   gStats : register(u5);

SamplerState gLinear : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Every stored edit is clamped to 16x either way. The composition's own guard is far tighter on
// luminance, so this binds only on chroma outliers -- a near-black channel against a lit one -- and
// keeps one bad texel from becoming a firefly that the pyramid then spreads.
static const float kMaxEdit = 4.0;

float LinDepth(float d)
{
    // Proportional to view depth for either convention, without the projection: reversed-Z stores
    // near/z, a forward buffer stores roughly 1 - near/z. Only ratios of this are ever taken.
    return gDepthInverted != 0 ? 1.0 / max(d, 1e-7) : 1.0 / max(1.0 - d, 1e-7);
}

float LogLuma(float3 c) { return log2(dot(max(c, 0.0), kLuma) + gEpsilon); }

float RelDepthDiff(float a, float b) { return abs(a - b) / max(min(a, b), 1e-7); }

int2 DepthTexel(float2 uv)
{
    return clamp(int2(uv * float2(gDepthW, gDepthH)), int2(0, 0), int2(gDepthW, gDepthH) - 1);
}

int2 MotionTexel(float2 uv)
{
    return clamp(int2(uv * float2(gMotionW, gMotionH)), int2(0, 0), int2(gMotionW, gMotionH) - 1);
}

bool IsPriority(float2 uv)
{
    // Bit 0: the depth buffer has a stencil plane and it is bound. Bit 1: the user turned priority on.
    if ((gStencilOn & 3u) != 3u)
        return false;

    const uint s = gStencil.Load(int3(DepthTexel(uv), 0)).g;
    return (s & gStencilMask) == gStencilRef;
}

// Where this pixel was last frame, as an offset in uv.
//
// The vector is taken from the nearest surface in a 3x3 neighbourhood rather than the pixel itself,
// as every TAA does: at a silhouette the render-resolution vector under a display pixel can belong to
// the background while the pixel shows the foreground, and the foreground is the one that moved.
float2 MotionUvOffset(float2 uv)
{
    const int2 c = DepthTexel(uv);
    int2 best = c;
    float bestLin = 3.4e38;

    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 t = clamp(c + int2(dx, dy), int2(0, 0), int2(gDepthW, gDepthH) - 1);
            const float l = LinDepth(gDepth.Load(int3(t, 0)).r);

            if (l < bestLin)
            {
                bestLin = l;
                best = t;
            }
        }
    }

    const float2 bestUv = (float2(best) + 0.5) / float2(gDepthW, gDepthH);
    const float2 mv = gMotion.Load(int3(MotionTexel(bestUv), 0)).xy * float2(gMvScaleX, gMvScaleY);
    return mv / float2(gMotionW, gMotionH);
}

// The history at q, with each of the four bilinear taps admitted only if its depth agrees with this
// pixel's. Taps on another surface are dropped and the rest renormalised, so an edit never bleeds
// across a silhouette -- the foreground's verdict stays on the foreground.
struct History
{
    float3 edit;
    float confidence;
    float logLuma;
    float valid; // the fraction of the bilinear weight that passed, 0..1
};

History ReadHistory(float2 q, float linC, float tol)
{
    History h;
    h.edit = 0.0;
    h.confidence = 0.0;
    h.logLuma = 0.0;
    h.valid = 0.0;

    const float2 pos = q * float2(gWidth, gHeight) - 0.5;
    const int2 i0 = (int2) floor(pos);
    const float2 f = pos - floor(pos);

    float wsum = 0.0;

    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 t = clamp(i0 + o, int2(0, 0), int2(gWidth, gHeight) - 1);
        const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);

        const float2 g = gHistGuide.Load(int3(t, 0)).xy;
        const float rel = RelDepthDiff(g.x, linC);

        // Full weight inside the tolerance, fading to none at twice it, so the decision does not
        // flicker for a surface sitting right on the threshold.
        const float wd = saturate((2.0 * tol - rel) / max(tol, 1e-6));
        const float w = wb * wd;

        const float4 e = gHistEdit.Load(int3(t, 0));
        h.edit += e.rgb * w;
        h.confidence += e.a * w;
        h.logLuma += g.y * w;
        wsum += w;
    }

    if (wsum > 1e-4)
    {
        h.edit /= wsum;
        h.confidence /= wsum;
        h.logLuma /= wsum;
    }

    h.valid = saturate(wsum);
    return h;
}

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}

// Scale a residual so the result cannot leave the unit cube, without changing its direction -- the
// same as the composition shader's, for the same reason.
float3 CubeScaleResidual(float3 P, float3 T)
{
    float3 d = T - P;
    float alpha = 1.0;

    [unroll] for (int c = 0; c < 3; ++c)
    {
        if (d[c] > 1e-6)
            alpha = min(alpha, (1.0 - P[c]) / d[c]);
        else if (d[c] < -1e-6)
            alpha = min(alpha, (0.0 - P[c]) / d[c]);
    }

    return P + saturate(alpha) * d;
}

// A coverage-weighted bilinear read of one pyramid level (rgb mean, a coverage). Plain bilinear
// would blend toward the zeros stored where nothing was valid.
float4 FetchLevel(Texture2D<float4> tex, uint2 size, float2 uv)
{
    const float2 pos = uv * float2(size) - 0.5;
    const int2 i0 = (int2) floor(pos);
    const float2 f = pos - floor(pos);

    float3 acc = 0.0;
    float wsum = 0.0;

    [unroll] for (int k = 0; k < 4; ++k)
    {
        const int2 o = int2(k & 1, k >> 1);
        const int2 t = clamp(i0 + o, int2(0, 0), int2(size) - 1);
        const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        const float4 s = tex.Load(int3(t, 0));
        const float w = wb * s.a;
        acc += s.rgb * w;
        wsum += w;
    }

    return float4(wsum > 1e-6 ? acc / wsum : 0.0, wsum);
}

uint2 LevelSize(uint level)
{
    uint2 s = uint2(gWidth, gHeight);

    for (uint i = 0; i <= level; ++i)
        s = (s + 3u) / 4u;

    return s;
}

// --- The first pyramid level, built in the same pass that writes the history ---------------------
//
// An 8x8 group covers exactly 2x2 texels of a quarter-size level, so the reduction never leaves the
// group: each thread parks its weighted edit and guide in shared memory, and one thread per 4x4
// block sums them. That saves a full-resolution read of the history it just wrote.

groupshared float4 sEdit[64];  // rgb: w * edit, a: w
groupshared float2 sGuide[64]; // w * log2 depth, w * log2 luma
groupshared uint sRejected;
groupshared uint sRejectedPriority;

void ReduceToFirstLevel(uint3 gtid, uint3 id, float w, float3 edit, float linC, float logLuma)
{
    const uint li = gtid.y * 8 + gtid.x;
    sEdit[li] = float4(edit * w, w);
    sGuide[li] = float2(log2(linC) * w, logLuma * w);

    GroupMemoryBarrierWithGroupSync();

    if ((gtid.x & 3u) == 0 && (gtid.y & 3u) == 0)
    {
        float4 e = 0.0;
        float2 g = 0.0;

        [unroll] for (uint y = 0; y < 4; ++y)
        {
            [unroll] for (uint x = 0; x < 4; ++x)
            {
                const uint j = (gtid.y + y) * 8 + gtid.x + x;
                e += sEdit[j];
                g += sGuide[j];
            }
        }

        const uint2 l1 = id.xy / 4u;
        const uint2 l1Size = LevelSize(0);

        if (l1.x < l1Size.x && l1.y < l1Size.y)
        {
            // Coverage is the fraction of the 16 pixels that carried weight, so a block half on a
            // disocclusion counts for half when the levels are combined.
            gOut3[l1] = float4(e.a > 1e-6 ? e.rgb / e.a : 0.0, e.a / 16.0);
            gOut4[l1] = float4(e.a > 1e-6 ? g / e.a : 0.0, 0.0, 0.0);
        }
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    const bool inside = id.x < gWidth && id.y < gHeight;
    const float2 uv = (float2(id.xy) + 0.5) / float2(max(gWidth, 1u), max(gHeight, 1u));

    if (gMode == 0)
    {
        if (id.x == 0 && id.y == 0)
        {
            gStats[uint2(gStatsSlot * 2u, 0)] = 0u;
            gStats[uint2(gStatsSlot * 2u + 1u, 0)] = 0u;
        }

        return;
    }

    // Reproject (1) and capture (2) share the history read and the shared-memory reduction, so no
    // thread may leave before the barrier inside it -- out-of-range threads contribute zero weight.
    if (gMode == 1 || gMode == 2)
    {
        if (gtid.x == 0 && gtid.y == 0)
        {
            sRejected = 0u;
            sRejectedPriority = 0u;
        }

        GroupMemoryBarrierWithGroupSync();

        float w = 0.0;
        float3 edit = 0.0;
        float linC = 1.0;
        float logLuma = 0.0;

        if (inside)
        {
            const float4 colour = gColour.Load(int3(id.xy, 0));
            linC = LinDepth(gDepth.Load(int3(DepthTexel(uv), 0)).r);
            logLuma = LogLuma(colour.rgb);

            const bool priority = IsPriority(uv);

            // Characters get half the slack: on them a stale edit is the most visible.
            const float tolScale = priority ? 0.5 : 1.0;
            const float depthTol = gDepthTol * tolScale;
            const float colourTol = max(gColourTol * tolScale, 1e-3);

            const float2 q = uv + MotionUvOffset(uv);
            const bool onScreen = all(q >= 0.0) && all(q <= 1.0);

            History h = (History) 0;

            if (gHistValid != 0 && onScreen)
                h = ReadHistory(q, linC, depthTol);

            // Colour: the same surface should look roughly the same. A leaf that swayed, a particle,
            // water: the frame under the edit is no longer the frame it was computed for. This
            // rejects only the high band -- the low band is a property of the region, and survives.
            const float colourDiff = abs(logLuma - h.logLuma);
            const float vColour = saturate((2.0 * colourTol - colourDiff) / colourTol);

            if (gMode == 1)
            {
                gOut2[id.xy] = colour; // the untouched frame, kept for the apply

                edit = h.edit;
                w = h.valid;

                // Confidence decays with age and with every doubt -- depth or colour -- and only
                // ever comes back on a refresh.
                const float confidence = h.confidence * vColour * saturate(2.0 * h.valid - 1.0) * gHighDecay;

                gOut0[id.xy] = float4(edit, saturate(confidence));

                if (h.valid < 0.5)
                {
                    InterlockedAdd(sRejected, 1u);

                    if (priority)
                        InterlockedAdd(sRejectedPriority, 1u);
                }
            }
            else
            {
                const float3 nr = max(gAux0.Load(int3(id.xy, 0)).rgb, 0.0);
                const float3 fresh =
                    clamp(log2((nr + gEpsilon) / (max(colour.rgb, 0.0) + gEpsilon)), -kMaxEdit, kMaxEdit);

                // Optional temporal smoothing of the refresh: where the carried edit is still valid,
                // move only part of the way to the new one. 1 takes the new answer whole.
                const float keep = (gHistValid != 0) ? (1.0 - gRefreshBlend) * h.valid * vColour : 0.0;

                edit = lerp(fresh, h.edit, saturate(keep));
                w = 1.0;
                gOut0[id.xy] = float4(edit, 1.0);
            }

            gOut1[id.xy] = float4(linC, logLuma, 0.0, 0.0);
        }

        ReduceToFirstLevel(gtid, id, w, edit, linC, logLuma);

        if (gMode == 1)
        {
            GroupMemoryBarrierWithGroupSync();

            if (gtid.x == 0 && gtid.y == 0 && sRejected > 0u)
            {
                InterlockedAdd(gStats[uint2(gStatsSlot * 2u, 0)], sRejected);
                InterlockedAdd(gStats[uint2(gStatsSlot * 2u + 1u, 0)], sRejectedPriority);
            }
        }

        return;
    }

    if (!inside)
        return;

    if (gMode == 3)
    {
        // One level from the one above it: each texel is the coverage-weighted mean of a 4x4 block.
        float3 acc = 0.0;
        float wsum = 0.0;

        [unroll] for (uint y = 0; y < 4; ++y)
        {
            [unroll] for (uint x = 0; x < 4; ++x)
            {
                const uint2 s = id.xy * 4u + uint2(x, y);

                if (s.x < gSrcW && s.y < gSrcH)
                {
                    const float4 v = gAux0.Load(int3(s, 0));
                    acc += v.rgb * v.a;
                    wsum += v.a;
                }
            }
        }

        gOut0[id.xy] = float4(wsum > 1e-6 ? acc / wsum : 0.0, wsum / 16.0);
        return;
    }

    if (gMode == 4)
    {
        const float4 original = gColour.Load(int3(id.xy, 0));
        const float4 hist = gHistEdit.Load(int3(id.xy, 0));
        const float2 guide = gHistGuide.Load(int3(id.xy, 0)).xy;

        // The low band: the history pulled from the pyramid, finest level that has something to say.
        //
        // The first level is read jointly-bilaterally -- each tap weighted by how much its depth and
        // luma resemble this pixel's -- so the borrowed edit comes from the same surface rather than
        // from whatever is nearest. Coarser levels only fill what the finer ones could not, which is
        // the push-pull reconstruction: a hole the size of a character still gets its region's edit.
        const uint2 l1Size = LevelSize(0);
        const float2 pos = uv * float2(l1Size) - 0.5;
        const int2 i0 = (int2) floor(pos);
        const float2 f = pos - floor(pos);
        const float logD = log2(max(guide.x, 1e-7));

        float3 acc1 = 0.0;
        float w1 = 0.0;

        [unroll] for (int k = 0; k < 4; ++k)
        {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = clamp(i0 + o, int2(0, 0), int2(l1Size) - 1);
            const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
            const float4 s = gAux0.Load(int3(t, 0));
            float w = wb * s.a;

            if (gBilateral != 0)
            {
                const float2 g = gAux1.Load(int3(t, 0)).xy;
                const float dd = (g.x - logD) / 0.15; // ~11% in depth
                const float dl = (g.y - guide.y) / 1.0; // one stop of luma
                w *= exp(-(dd * dd + dl * dl));
            }

            acc1 += s.rgb * w;
            w1 += w;
        }

        const float4 l2 = FetchLevel(gAux2, LevelSize(1), uv);
        const float4 l3 = FetchLevel(gAux3, LevelSize(2), uv);

        float3 low = l3.a > 1e-6 ? l3.rgb : 0.0;
        low = lerp(low, l2.rgb, saturate(l2.a * 4.0));
        low = lerp(low, w1 > 1e-6 ? acc1 / w1 : low, saturate(w1 * 4.0));

        // The high band is what the pixel's own history says beyond its region, and it is only as
        // good as its confidence. Where the history was rejected the pixel takes the low band alone.
        const float3 high = hist.rgb - low;
        const float3 edit = clamp(gLowGain * low + gHighGain * hist.a * high, -kMaxEdit, kMaxEdit);

        float3 result = max((max(original.rgb, 0.0) + gEpsilon) * exp2(edit) - gEpsilon, 0.0);

        // Debug views, in the frame's own units (paper white = epsilon * 512).
        const float white = gEpsilon * 512.0;

        if (gDebugView == 1)
        {
            // Green: the pixel's own history is trusted. Red: it was rejected and the low band stands in.
            const float c = hist.a;
            result = float3(1.0 - c, c, 0.15) * white * 0.5;
        }
        else if (gDebugView == 2)
        {
            result = saturate(0.5 + dot(low, kLuma) * 2.0).xxx * white * 0.5;
        }
        else if (gDebugView == 3)
        {
            result = saturate(0.5 + dot(hist.a * high, kLuma) * 8.0).xxx * white * 0.5;
        }
        else if (gDebugView == 4)
        {
            // The stencil plane, one hue per value, so the bit a game uses for characters can be found
            // by looking. Grey where the depth buffer has no stencil.
            if ((gStencilOn & 1u) != 0)
            {
                const uint s = gStencil.Load(int3(DepthTexel(uv), 0)).g;
                const float3 hue = frac(float3(s * 0.137, s * 0.311, s * 0.519));
                result = (s == 0u ? 0.1 : 0.3 + 0.7 * hue) * white * 0.5;

                if (IsPriority(uv))
                    result = lerp(result, float3(1.0, 0.0, 1.0) * white * 0.5, 0.5);
            }
            else
            {
                result = 0.25 * white;
            }
        }

        gOut0[id.xy] = float4(result, original.a);
        return;
    }

    if (gMode == 5)
    {
        // The motion since the model last ran, chained frame to frame at the motion texture's own
        // resolution and in the game's own units, so the model's scale still applies. Without this the
        // model, run one frame in N, is told only one frame of motion and reprojects its history to the
        // wrong place.
        const float2 mv = gMotion.Load(int3(id.xy, 0)).xy;
        float2 acc = mv;

        if (gAccReset == 0)
        {
            const float2 prevPos = float2(id.xy) + 0.5 + mv * float2(gMvScaleX, gMvScaleY);

            if (all(prevPos >= 0.0) && prevPos.x <= (float) gWidth && prevPos.y <= (float) gHeight)
                acc += gAux0.SampleLevel(gLinear, prevPos / float2(gSrcW, gSrcH), 0).xy;
        }

        gOut0[id.xy] = float4(acc, 0.0, 0.0);
        return;
    }

    if (gMode == 6)
    {
        // Joint bilateral upsampling of the model's residual, for a model that ran below the frame.
        //
        // The bilinear enlargement the resolve does reads the four nearest small texels whatever is in
        // them, so the edit smears across every edge the small raster could not resolve. Here each tap
        // is weighted by how much the small proxy under it resembles the full proxy at this pixel --
        // guided by the native image -- so the edit lands on the surface it was computed for.
        const float4 full = gColour.Load(int3(id.xy, 0));
        const float3 P = gPassthrough != 0 ? full.rgb : SrgbToLinear(full.rgb);
        const float lFull = dot(full.rgb, kLuma);

        const float2 pos = uv * float2(gSrcW, gSrcH) - 0.5;
        const int2 i0 = (int2) floor(pos);
        const float2 f = pos - floor(pos);
        const float inv = 1.0 / max(gJbuSigma, 1e-4);

        float3 acc = 0.0, accB = 0.0;
        float wsum = 0.0;

        [unroll] for (int k = 0; k < 4; ++k)
        {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = clamp(i0 + o, int2(0, 0), int2(gSrcW, gSrcH) - 1);
            const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);

            const float3 ps = gAux0.Load(int3(t, 0)).rgb;
            const float3 ms = gAux1.Load(int3(t, 0)).rgb;
            const float3 e = gPassthrough != 0 ? ms - ps : SrgbToLinear(ms) - SrgbToLinear(ps);

            const float dl = (lFull - dot(ps, kLuma)) * inv;
            const float w = wb * exp(-dl * dl);

            acc += e * w;
            accB += e * wb;
            wsum += w;
        }

        // Nothing resembles this pixel (a sub-texel highlight): fall back to plain bilinear.
        const float3 e = wsum > 1e-4 ? acc / wsum : accB;

        float3 M;

        if (gPassthrough != 0)
            M = max(P + e, 0.0);
        else
            M = LinearToSrgb(CubeScaleResidual(saturate(P), saturate(P) + e));

        gOut0[id.xy] = float4(M, full.a);
        return;
    }

    if (gMode == 7)
    {
        // The measurement dump: the frame, the model's frame, and the geometry the offline script
        // needs to reproject -- motion as a uv offset to the previous frame, pseudo-linear depth, and
        // the frame's log luma -- all at display resolution and in fixed formats.
        const float4 colour = gColour.Load(int3(id.xy, 0));
        gOut0[id.xy] = float4(max(colour.rgb, 0.0), 1.0);
        gOut3[id.xy] = float4(max(gAux0.Load(int3(id.xy, 0)).rgb, 0.0), 1.0);

        const float linC = LinDepth(gDepth.Load(int3(DepthTexel(uv), 0)).r);
        gOut1[id.xy] = float4(MotionUvOffset(uv), linC, LogLuma(colour.rgb));
        return;
    }
}
