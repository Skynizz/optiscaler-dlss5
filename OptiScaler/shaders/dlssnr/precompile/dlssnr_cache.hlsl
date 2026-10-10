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
    uint  gStatsSlot;
    uint  gAccReset;
    uint  gPassthrough;
    float gJbuSigma;
    uint  gSrcW;
    uint  gSrcH;
    uint  gFrameIndex;
    uint  gUseGameExposure; // the game's live exposure is bound at t10: white = gExposurePreMul / exposure
    float gExposurePreMul;
    float gMaxLumaEdit;     // the most, in stops, the composition can move a pixel's luminance
    float gStabilize;       // anti-flicker: the most a refresh may move a still-valid pixel's edit, in stops (0 off)
    uint  gDespeckle;       // bound each fresh edit by its eight neighbours' (isolated dark specks)
    uint  gCrossfadeOn;     // keyframe crossfade: the shown edit walks toward the model's latest answer
    float gCrossfade;       // this frame's step: 1 / (frames left until the model runs again)
    float gTemporal;        // temporal stabiliser: weight of the reprojected, clamped previous edit (0 off)
    uint  gTemporalValid;   // the previous stabilised edit exists and belongs to this raster
    float gLowTemporal;     // luminance stability: weight of the reprojected regional (low band) edit (0 off)
    float gJitterDeltaX;    // pre-SR: this frame's change of camera jitter, in uv, added to every reprojection
    float gJitterDeltaY;
    uint  gGhostFlags;      // anti-ghosting switches (DlssNrCacheGhostFlag); 0 is the cache as it was
    float gPrintTol;        // fingerprint: stops outside this frame's 3x3 range before doubt starts
    float gContextTol;      // fingerprint: stops of change of the surroundings
    float gAgeHalfLife;     // aging: frames of motion over which the carried detail halves
    float gStalePx;         // aging: pixels of motion that count as one frame
    float gGuidedEps;       // guided filter: regularisation, log luma squared
    float gGuidedStrength;  // guided filter: share of the rebuilt edit taken
    uint  gCtxW;            // the context grid, a quarter of the frame
    uint  gCtxH;
    uint  gHalfW;           // the guided filter's grid, half the frame
    uint  gHalfH;
    float gAntiPopStep;     // anti light pop-in: max change of the regional edit per frame where the frame is still (0 off)
    float gAntiPopFrameTol; // anti light pop-in: the frame's own regional change, in stops, that counts as a real change
    uint  gNoiseAware;      // colour test against this frame's 3x3 range, anti-flicker faded and soft-limited
    uint  gDilatedReady;    // this frame's dilated motion is at t19
    uint  gRegionalReady;   // mode 10's regional lows are at t20 (this frame's edit) and t21 (last frame's)
    uint  gAsyncWarp;       // CacheAsync: the capture reads the background answer carried here by mode 18
    uint  gProbeContext;    // mode 19: this frame's and last frame's surroundings are at t13 / t18
    uint  gSoftRefresh;     // crossfade: a pixel without a carried edit starts the walk from what was on screen
    uint  gSoftReveal;      // soft refresh: a revealed or moving pixel walks from last frame's regional light
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
// Anti-ghosting. The fingerprint is what the frame looked like where and when the model computed the
// edit -- carried with the edit and never updated -- so a slow drift is caught as surely as a jump; the
// meta is the edit's age, how much of it is still believed, and how far it has travelled.
Texture2D<float4>   gHistPrint : register(t9);  // normalised log luma, log chroma r/g, b/g, surroundings
Texture2D<float4>   gExposure  : register(t10); // the game's 1x1 exposure, when it supplies one
Texture2D<float4>   gHistTarget : register(t11); // keyframe crossfade: the model's latest answer, carried
Texture2D<float4>   gHistMeta  : register(t12); // age (frames), validity, staleness (frames of motion), tap validity
Texture2D<float4>   gContext   : register(t13); // this frame's surroundings: normalised log luma, 1/4 size
Texture2D<float4>   gGuide0    : register(t14); // guided filter: half-size sums, or the coefficients a
Texture2D<float4>   gGuide1    : register(t15); // guided filter: half-size sums, or the coefficients b
Texture2D<float4>   gAux4      : register(t16); // L2 guide (log2 depth, log luma), for the same-surface fill
Texture2D<float4>   gAux5      : register(t17); // L3 guide
Texture2D<float4>   gContextPrev : register(t18); // anti pop-in: last frame's surroundings
Texture2D<float4>   gDilated   : register(t19); // per depth texel: the nearest surface's motion, uv (mode 16)
Texture2D<float4>   gRegNow    : register(t20); // mode 10: the 12 px tent of this frame's edit, quarter size
Texture2D<float4>   gRegPrev   : register(t21); // mode 10: the same of last frame's shown edit

RWTexture2D<float4> gOut0  : register(u0);
RWTexture2D<float4> gOut1  : register(u1);
RWTexture2D<float4> gOut2  : register(u2);
RWTexture2D<float4> gOut3  : register(u3);
RWTexture2D<float4> gOut4  : register(u4);
RWTexture2D<uint>   gStats : register(u5); // x: frame slot; y: 0 depth rejects, 1 fingerprint rejects, 2 motion (1/8 px)
RWTexture2D<float4> gOut6  : register(u6); // keyframe crossfade: the carried target, written
RWTexture2D<float4> gOut7  : register(u7); // anti-ghosting: the fingerprint, written
RWTexture2D<float4> gOut8  : register(u8); // anti-ghosting: the meta, written

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

// The ratio floor, from the same white point the composition used on this very frame.
//
// It was paper white / 512 from the CPU's white point, which in a game that supplies its exposure is
// read back three frames late -- and Control's exposure swings by three orders of magnitude within a
// second after a cut. A floor a thousand times too small makes every ratio in the shadows explode:
// near-black pixels become black or bright, and with bands refreshing in turn they pop on and off.
// Reading the live exposure here, exactly as dlssnr.hlsl does, keeps the floor where the composition's
// own guard put it.
float WhitePoint()
{
    float white = max(gEpsilon * 512.0, 1e-4);

    if (gUseGameExposure != 0)
    {
        const float e = gExposure.Load(int3(0, 0, 0)).r;

        if (e > 1e-6 && e < 1e6)
            white = clamp(gExposurePreMul / e, 0.01, 4096.0);
    }

    return white;
}

float Eps() { return WhitePoint() / 512.0; }

float LogLuma(float3 c) { return log2(dot(max(c, 0.0), kLuma) + Eps()); }

// --- Anti-ghosting ---------------------------------------------------------------------------------
//
// At long intervals the carried edit trails: validation compared each frame only with the one before it,
// so whatever drifted a little every frame -- a shadow sliding, debris without motion vectors, the light
// an object threw on the ground after it moved away -- never failed, and the low band was never rejected
// at all. Each switch below attacks one part of it; with GhostFlags at 0 none of this code runs.

static const uint kGhostPrint = 1u;
static const uint kGhostContext = 2u;
static const uint kGhostAging = 4u;
static const uint kGhostAgeNeutral = 8u;
static const uint kGhostGuided = 16u;
static const uint kGhostSurface = 32u;

bool Ghost(uint flag) { return (gGhostFlags & flag) != 0u; }

// Log luma against this frame's white point: the same surface reads the same whatever the exposure does.
float NormLogLuma(float3 c) { return LogLuma(c) - log2(WhitePoint()); }

// What the frame looks like at a pixel: normalised log luma and two log chroma ratios. Stored where the
// model computed the edit and carried with it, untouched, to be compared with the frame every frame.
// white is WhitePoint(), read once by the caller (it is a texture read).
float3 PrintOf(float3 c, float white)
{
    const float eps = white / 512.0;
    const float3 p = max(c, 0.0) + eps;
    return float3(log2(dot(p - eps, kLuma) + eps) - log2(white), log2(p.r / p.g), log2(p.b / p.g));
}

// This frame's surroundings at uv: the quarter-size mean, tent-filtered by four bilinear reads, so it
// moves smoothly with the content instead of snapping to a grid of blocks.
float ContextAt(float2 uv)
{
    const float2 t = 0.5 / float2(max(gCtxW, 1u), max(gCtxH, 1u));
    return 0.25 * (gContext.SampleLevel(gLinear, uv + float2(-t.x, -t.y), 0).r +
                   gContext.SampleLevel(gLinear, uv + float2(t.x, -t.y), 0).r +
                   gContext.SampleLevel(gLinear, uv + float2(-t.x, t.y), 0).r +
                   gContext.SampleLevel(gLinear, uv + float2(t.x, t.y), 0).r);
}

// The same, of last frame's surroundings (the anti pop-in's "did the frame change here").
float ContextPrevAt(float2 uv)
{
    const float2 t = 0.5 / float2(max(gCtxW, 1u), max(gCtxH, 1u));
    return 0.25 * (gContextPrev.SampleLevel(gLinear, uv + float2(-t.x, -t.y), 0).r +
                   gContextPrev.SampleLevel(gLinear, uv + float2(t.x, -t.y), 0).r +
                   gContextPrev.SampleLevel(gLinear, uv + float2(-t.x, t.y), 0).r +
                   gContextPrev.SampleLevel(gLinear, uv + float2(t.x, t.y), 0).r);
}

// Full belief inside the tolerance, none at twice it: the same soft edge as the depth test, so a pixel
// sitting on the threshold does not flicker between the two.
float Soft(float d, float tol) { return saturate((2.0 * tol - d) / max(tol, 1e-4)); }

float Outside(float v, float lo, float hi) { return max(max(lo - v, v - hi), 0.0); }

// Whether the frame still looks like what the edit was computed for. The source is compared with the
// range this frame spans over the pixel's 3x3 neighbourhood rather than with the pixel alone, as TAA
// clamps its history: a sub-pixel shift, aliasing on fine detail or the bilinear read of the print stay
// inside that range; a shadow that moved, a particle, a surface that is no longer there do not.
float PrintMatch(float4 src, float3 lo, float3 hi, float contextNow)
{
    float m = Soft(Outside(src.x, lo.x, hi.x), gPrintTol);
    m *= Soft(max(Outside(src.y, lo.y, hi.y), Outside(src.z, lo.z, hi.z)), gPrintTol);

    // The surroundings: the light the model gave a patch of ground depended on what stood on it. When that
    // left, the patch itself did not change, but its neighbourhood did.
    if (Ghost(kGhostContext))
        m *= Soft(abs(contextNow - src.w), gContextTol);

    return m;
}

// An edit bounded to what the composition can actually produce: its luminance within the highlight
// guard (plus a little for the soft knee), its colour within a stop of its luminance. Anything beyond
// is not a model verdict but a ratio against a near-black pixel, and carrying it is what flickers.
float3 ClampEdit(float3 e, float scale)
{
    const float limit = gMaxLumaEdit * max(scale, 1.0);
    const float l = dot(e, kLuma);
    const float lc = clamp(l, -limit, limit);
    e += lc - l;
    return clamp(e, lc - 1.0, lc + 1.0);
}

// Anti-flicker. Where the carried edit still belongs to this surface, a refresh may move its luminance
// by at most gStabilize stops. The model re-decides small things every run -- that is detail, and it
// passes -- but now and then it re-decides a dark patch by a stop or more and back again, which is
// the black popping. A limit on the step lets the first through and holds the second.
//
// Noise-aware: in a noisy area (path-traced shadows, Ray Reconstruction) the trust of neighbouring pixels
// hovers around the threshold, and with a switch and a hard clamp neighbours flipped between limited and
// not from frame to frame -- the limiter made a flicker of its own, worst at its default (0 never limits,
// a large value almost never binds). So it fades in with trust, and limits with a soft knee: small steps
// pass untouched, large ones are compressed toward the bound instead of cut at it.
float3 Stabilize(float3 fresh, float3 carried, float trust)
{
    if (gStabilize <= 0.0)
        return fresh;

    const float lf = dot(fresh, kLuma);
    const float lc = dot(carried, kLuma);

    if (gNoiseAware == 0)
    {
        if (trust < 0.5)
            return fresh;

        const float l = lc + clamp(lf - lc, -gStabilize, gStabilize);
        return fresh + (l - lf);
    }

    const float w = smoothstep(0.2, 0.8, trust);
    const float x = (lf - lc) / gStabilize;
    const float l = lc + gStabilize * x / sqrt(1.0 + x * x); // soft limit, tends to +-gStabilize
    return fresh + (l - lf) * w;
}

// The model's edit for one pixel, and whether to believe it. A model answer that is black where the
// frame is not is a failed evaluate (a band's first frame, a reset), not a verdict to carry for N frames.
float3 FreshEdit(float3 nr, float3 orig, out float ok)
{
    const float eps = Eps();
    nr = max(nr, 0.0);
    orig = max(orig, 0.0);

    const float lo = dot(orig, kLuma);
    const float ln = dot(nr, kLuma);
    ok = (lo > 8.0 * eps && ln < 0.05 * lo) ? 0.0 : 1.0;

    return ClampEdit(log2((nr + eps) / (orig + eps)), 1.0);
}

// The model's fresh edit at a pixel, with its eight neighbours' edits bounding its luminance.
//
// The model's characteristic failure in shadows is a speck: a few pixels it suddenly darkens by a stop
// or more while everything around them stays put, and next run they are back. An edit that is darker
// (or brighter) than every one of its neighbours is that speck, not structure -- a real edge has
// neighbours on its own side that agree with it -- so it is brought back to the range they span.
//
// nr is read from gAux0 at nrPos (band-local or frame coordinates), the frame from gColour at framePos.
float3 FreshEditAt(int2 framePos, int2 nrPos, int2 nrSize, out float ok)
{
    const float3 e = FreshEdit(gAux0.Load(int3(nrPos, 0)).rgb, gColour.Load(int3(framePos, 0)).rgb, ok);

    if (gDespeckle == 0)
        return e;

    float lo = 1e9, hi = -1e9;

    [unroll] for (int k = 0; k < 9; ++k)
    {
        if (k == 4)
            continue;

        const int2 o = int2(k % 3 - 1, k / 3 - 1);
        const int2 pn = clamp(nrPos + o, int2(0, 0), nrSize - 1);
        const int2 pf = clamp(framePos + o, int2(0, 0), int2(gWidth, gHeight) - 1);

        float okn;
        const float3 en = FreshEdit(gAux0.Load(int3(pn, 0)).rgb, gColour.Load(int3(pf, 0)).rgb, okn);

        if (okn > 0.5)
        {
            const float l = dot(en, kLuma);
            lo = min(lo, l);
            hi = max(hi, l);
        }
    }

    if (hi < lo)
        return e;

    const float l = dot(e, kLuma);
    const float lc = clamp(l, lo - 0.1, hi + 0.1);
    return e + (lc - l);
}


float RelDepthDiff(float a, float b) { return abs(a - b) / max(min(a, b), 1e-7); }

int2 DepthTexel(float2 uv)
{
    return clamp(int2(uv * float2(gDepthW, gDepthH)), int2(0, 0), int2(gDepthW, gDepthH) - 1);
}

int2 MotionTexel(float2 uv)
{
    return clamp(int2(uv * float2(gMotionW, gMotionH)), int2(0, 0), int2(gMotionW, gMotionH) - 1);
}

// Where this pixel was last frame, as an offset in uv.
//
// The vector is taken from the nearest surface in a 3x3 neighbourhood rather than the pixel itself,
// as every TAA does: at a silhouette the render-resolution vector under a display pixel can belong to
// the background while the pixel shows the foreground, and the foreground is the one that moved.
// The search below, for one depth texel. Every pixel that falls on the same depth texel gets the same
// answer, so mode 16 runs it once per texel per frame and the passes read the result.
float2 NearestSurfaceMotion(int2 c)
{
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

float2 MotionUvOffset(float2 uv)
{
    // Read from this frame's precomputed field when it is there (mode 16), searched here otherwise -- the
    // same answer either way.
    const float2 motion = gDilatedReady != 0 ? gDilated.Load(int3(DepthTexel(uv), 0)).xy
                                             : NearestSurfaceMotion(DepthTexel(uv));

    // Pre-SR works on the game's jittered render: each frame samples the scene a fraction of a pixel
    // elsewhere and the motion vectors leave that out, so the change of jitter is added here (0 after
    // the upscaler, where the frame is not jittered).
    return motion + float2(gJitterDeltaX, gJitterDeltaY);
}

// CacheAsync: where this pixel was on the frame the model last saw -- the motion chained since then by mode
// 5 (gAux3, in the game's units at the motion texture's resolution) -- read at the nearest surface, as above.
float2 AccumulatedUvOffset(int2 c)
{
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
    const float2 mv = gAux3.Load(int3(MotionTexel(bestUv), 0)).xy * float2(gMvScaleX, gMvScaleY);
    return mv / float2(gMotionW, gMotionH);
}

// The history at q, with each of the four bilinear taps admitted only if its depth agrees with this
// pixel's. Taps on another surface are dropped and the rest renormalised, so an edit never bleeds
// across a silhouette -- the foreground's verdict stays on the foreground.
struct History
{
    float3 edit;
    float confidence;
    float3 target;     // keyframe crossfade: the model's latest answer, reprojected like the shown edit
    float targetConfidence;
    float logLuma;
    float valid; // the fraction of the bilinear weight that passed, 0..1
    float4 print; // anti-ghosting: the source's fingerprint, carried
    float4 meta;  // anti-ghosting: age, validity, staleness, tap validity
    float3 shown; // soft refresh (capture only): last frame's edit on screen, at the same taps
};

// A history texture at uv q, Catmull-Rom filtered in five bilinear taps (the usual TAA arrangement).
float3 CatmullRom(Texture2D<float4> tex, float2 q)
{
    const float2 size = float2(gWidth, gHeight);
    const float2 samplePos = q * size;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    const float2 f = samplePos - texPos1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-6);

    const float2 tc0 = (texPos1 - 1.0) / size;
    const float2 tc3 = (texPos1 + 2.0) / size;
    const float2 tc12 = (texPos1 + offset12) / size;

    float3 r = 0.0;
    r += tex.SampleLevel(gLinear, float2(tc12.x, tc0.y), 0).rgb * (w12.x * w0.y);
    r += tex.SampleLevel(gLinear, float2(tc0.x, tc12.y), 0).rgb * (w0.x * w12.y);
    r += tex.SampleLevel(gLinear, float2(tc12.x, tc12.y), 0).rgb * (w12.x * w12.y);
    r += tex.SampleLevel(gLinear, float2(tc3.x, tc12.y), 0).rgb * (w3.x * w12.y);
    r += tex.SampleLevel(gLinear, float2(tc12.x, tc3.y), 0).rgb * (w12.x * w3.y);

    const float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return r / max(wsum, 1e-6);
}

History ReadHistory(float2 q, float linC, float tol)
{
    History h;
    h.edit = 0.0;
    h.confidence = 0.0;
    h.target = 0.0;
    h.targetConfidence = 0.0;
    h.logLuma = 0.0;
    h.valid = 0.0;
    h.print = 0.0;
    h.meta = 0.0;
    h.shown = 0.0;
    const bool readShown = gMode == 2 && gSoftRefresh != 0 && gTemporalValid != 0;

    const float2 pos = q * float2(gWidth, gHeight) - 0.5;
    const int2 i0 = (int2) floor(pos);
    const float2 f = pos - floor(pos);

    float wsum = 0.0;
    float allValid = 1.0;
    float3 lo = 1e9, hi = -1e9;
    float3 tlo = 1e9, thi = -1e9;
    float bestW = 0.0;
    int2 bestT = int2(0, 0);

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
        allValid = min(allValid, wd);

        const float4 e = gHistEdit.Load(int3(t, 0));
        h.edit += e.rgb * w;
        h.confidence += e.a * w;

        if (readShown)
            h.shown += gAux1.Load(int3(t, 0)).rgb * w;
        h.logLuma += g.y * w;
        wsum += w;
        lo = min(lo, e.rgb);
        hi = max(hi, e.rgb);

        if (gCrossfadeOn != 0)
        {
            const float4 g4 = gHistTarget.Load(int3(t, 0));
            h.target += g4.rgb * w;
            h.targetConfidence += g4.a * w;
            tlo = min(tlo, g4.rgb);
            thi = max(thi, g4.rgb);
        }

        if (w > bestW)
        {
            bestW = w;
            bestT = t;
        }
    }

    // The fingerprint and the meta travel with the edit, read at its strongest valid tap: the fingerprint
    // is compared with a 3x3 range, which a pixel's worth of placement does not move, and two reads cost
    // a quarter of eight.
    if (gGhostFlags != 0u && bestW > 0.0)
    {
        h.print = gHistPrint.Load(int3(bestT, 0));
        h.meta = gHistMeta.Load(int3(bestT, 0));
    }

    if (wsum > 1e-4)
    {
        h.edit /= wsum;
        h.confidence /= wsum;
        h.logLuma /= wsum;
        h.target /= wsum;
        h.targetConfidence /= wsum;
        h.shown /= wsum;
    }

    // Where all four taps are the same surface, the edit is read with Catmull-Rom instead of bilinear.
    // A bilinear read is a small blur, and one blur per carried frame adds up: the detail softened a
    // little more each frame and came back sharp when the model ran -- distant, fine things visibly
    // breathed at the refresh rate. Catmull-Rom keeps the detail; clamping it to the four taps' range
    // keeps its overshoot from inventing any.
    //
    // Blended in by how surely all four are that surface, not switched at a threshold: on a thin, distant
    // thing that surety flickers from frame to frame, and a hard switch made its sharpness flicker with it.
    if (allValid > 0.0)
    {
        h.edit = lerp(h.edit, clamp(CatmullRom(gHistEdit, q), lo, hi), allValid);

        if (gCrossfadeOn != 0)
            h.target = lerp(h.target, clamp(CatmullRom(gHistTarget, q), tlo, thi), allValid);
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
groupshared uint sRejectedPrint; // anti-ghosting: rejected by the fingerprint, not by depth
groupshared uint sMotion;        // the group's motion, in eighths of a pixel

// Anti-ghosting: this frame's fingerprint over the group and a one-pixel border (10x10), so each pixel's
// 3x3 range is nine reads of shared memory rather than nine of the frame.
groupshared float3 sPrint[100];

// The fresh edit's luma over the group and a one-pixel border, and whether the model answered there, for
// the despeckle: each pixel's eight neighbours are read here instead of computed again from the frame.
groupshared float2 sFresh[100];

// Mode 10: the edit's luma over the group and a one-pixel border, for its 3x3 variance.
groupshared float sEditLuma[100];

void LoadFreshTile(uint3 gid, uint3 gtid)
{
    const int2 origin = int2(gid.xy) * 8 - 1;
    const uint li = gtid.y * 8 + gtid.x;

    [unroll] for (uint k = 0; k < 2; ++k)
    {
        const uint j = li + k * 64;

        if (j < 100)
        {
            const int2 t = clamp(origin + int2(j % 10, j / 10), int2(0, 0), int2(gWidth, gHeight) - 1);
            float ok;
            const float3 e = FreshEdit(gAux0.Load(int3(t, 0)).rgb, gColour.Load(int3(t, 0)).rgb, ok);
            sFresh[j] = float2(dot(e, kLuma), ok);
        }
    }
}

void LoadEditLumaTile(uint3 gid, uint3 gtid)
{
    const int2 origin = int2(gid.xy) * 8 - 1;
    const uint li = gtid.y * 8 + gtid.x;

    [unroll] for (uint k = 0; k < 2; ++k)
    {
        const uint j = li + k * 64;

        if (j < 100)
        {
            const int2 t = clamp(origin + int2(j % 10, j / 10), int2(0, 0), int2(gWidth, gHeight) - 1);
            sEditLuma[j] = dot(gAux0.Load(int3(t, 0)).rgb, kLuma);
        }
    }
}

// FreshEditAt for the capture, its neighbours from the tile above: the same result, a ninth of the work.
float3 FreshEditTiled(uint3 gtid, int2 pos, out float ok)
{
    const float3 e = FreshEdit(gAux0.Load(int3(pos, 0)).rgb, gColour.Load(int3(pos, 0)).rgb, ok);

    if (gDespeckle == 0)
        return e;

    float lo = 1e9, hi = -1e9;

    [unroll] for (int k = 0; k < 9; ++k)
    {
        if (k == 4)
            continue;

        const float2 n = sFresh[(gtid.y + k / 3) * 10 + gtid.x + k % 3];

        if (n.y > 0.5)
        {
            lo = min(lo, n.x);
            hi = max(hi, n.x);
        }
    }

    if (hi < lo)
        return e;

    const float l = dot(e, kLuma);
    const float lc = clamp(l, lo - 0.1, hi + 0.1);
    return e + (lc - l);
}

void LoadPrintTile(uint3 gid, uint3 gtid)
{
    const int2 origin = int2(gid.xy) * 8 - 1;
    const uint li = gtid.y * 8 + gtid.x;
    const float white = WhitePoint();

    [unroll] for (uint k = 0; k < 2; ++k)
    {
        const uint j = li + k * 64;

        if (j < 100)
        {
            const int2 t = clamp(origin + int2(j % 10, j / 10), int2(0, 0), int2(gWidth, gHeight) - 1);
            sPrint[j] = PrintOf(gColour.Load(int3(t, 0)).rgb, white);
        }
    }
}

void PrintRange(uint3 gtid, out float3 lo, out float3 hi)
{
    lo = 1e9;
    hi = -1e9;

    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const float3 v = sPrint[(gtid.y + k / 3) * 10 + gtid.x + k % 3];
        lo = min(lo, v);
        hi = max(hi, v);
    }
}

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

// The low band from the same surface only, for the anti-ghosting fill. Each pyramid level is read
// jointly-bilaterally -- more loosely the coarser it is -- from the coarsest to the finest, each finer
// level taking over where it has support. Where no level holds anything like this pixel there is no edit:
// borrowing another surface's light is how a character ended up wearing the wall behind them.
float3 SurfaceLow(float2 uv, float logD, float logL)
{
    float3 low = 0.0;
    float have = 0.0;

    [unroll] for (int lvl = 2; lvl >= 0; --lvl)
    {
        const uint2 size = LevelSize((uint) lvl);
        const float2 pos = uv * float2(size) - 0.5;
        const int2 i0 = (int2) floor(pos);
        const float2 f = pos - floor(pos);
        const float sd = 0.15 * (1.0 + lvl); // ~11% in depth at the first level
        const float sl = 1.0 * (1.0 + lvl);  // a stop of luma at the first level

        float3 acc = 0.0;
        float ws = 0.0;

        [unroll] for (int k = 0; k < 4; ++k)
        {
            const int2 o = int2(k & 1, k >> 1);
            const int2 t = clamp(i0 + o, int2(0, 0), int2(size) - 1);
            const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);

            float4 e;
            float2 g;

            if (lvl == 0)
            {
                e = gAux0.Load(int3(t, 0));
                g = gAux1.Load(int3(t, 0)).xy;
            }
            else if (lvl == 1)
            {
                e = gAux2.Load(int3(t, 0));
                g = gAux4.Load(int3(t, 0)).xy;
            }
            else
            {
                e = gAux3.Load(int3(t, 0));
                g = gAux5.Load(int3(t, 0)).xy;
            }

            const float dd = (g.x - logD) / sd;
            const float dl = (g.y - logL) / sl;
            const float w = wb * e.a * exp(-(dd * dd + dl * dl));
            acc += e.rgb * w;
            ws += w;
        }

        const float a = saturate(ws * 4.0);
        low = lerp(low, ws > 1e-6 ? acc / ws : low, a);
        have = lerp(have, 1.0, a);
    }

    return low * have;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const bool inside = id.x < gWidth && id.y < gHeight;
    const float2 uv = (float2(id.xy) + 0.5) / float2(max(gWidth, 1u), max(gHeight, 1u));

    if (gMode == 11)
    {
        // Pre-SR: the game's render-resolution colour, read where DLSS would read it, into the texture
        // the pass rewrites and DLSS is then handed instead. Nothing of the game's own is written.
        if (inside)
            gOut0[id.xy] = gColour.Load(int3(id.xy, 0));

        return;
    }

    if (gMode == 0)
    {
        if (id.x == 0 && id.y < 3u)
            gStats[uint2(gStatsSlot, id.y)] = 0u;

        return;
    }

    // Mode 10's 3x3 variance reads its tile; every thread of the group loads it before any may leave.
    if (gMode == 10)
    {
        LoadEditLumaTile(gid, gtid);
        GroupMemoryBarrierWithGroupSync();
    }

    // Reproject (1) and capture (2) share the history read and the shared-memory reduction, so no
    // thread may leave before the barrier inside it -- out-of-range threads contribute zero weight.
    if (gMode == 1 || gMode == 2)
    {
        if (gtid.x == 0 && gtid.y == 0)
        {
            sRejected = 0u;
            sRejectedPrint = 0u;
            sMotion = 0u;
        }

        if (Ghost(kGhostPrint) || gNoiseAware != 0)
            LoadPrintTile(gid, gtid);

        if (gMode == 2 && gDespeckle != 0)
            LoadFreshTile(gid, gtid);

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

            const float depthTol = gDepthTol;
            const float colourTol = max(gColourTol, 1e-3);

            const float2 q = uv + MotionUvOffset(uv);
            const bool onScreen = all(q >= 0.0) && all(q <= 1.0);

            History h = (History) 0;

            if (gHistValid != 0 && onScreen)
                h = ReadHistory(q, linC, depthTol);

            // Colour: the same surface should look roughly the same. A leaf that swayed, a particle,
            // water: the frame under the edit is no longer the frame it was computed for. This
            // rejects only the high band -- the low band is a property of the region, and survives.
            float colourDiff = abs(logLuma - h.logLuma);

            // Noise-aware: last frame's value against the range this frame spans over the 3x3 neighbourhood,
            // as TAA clamps its history. Noise and sub-pixel shimmer stay inside the range; a surface that
            // really changed does not. Against the pixel alone, the noise of path-traced shadows failed the
            // test every frame, so the carried detail was dimmed every other frame -- thinner shadows that
            // flickered at half the frame rate.
            if (gNoiseAware != 0)
            {
                float3 nlo, nhi;
                PrintRange(gtid, nlo, nhi);
                const float logWhite = log2(WhitePoint());
                colourDiff = Outside(h.logLuma - logWhite, nlo.x, nhi.x);
            }

            const float vColour = saturate((2.0 * colourTol - colourDiff) / colourTol);

            // Anti-ghosting. The fingerprint: does the frame still look like what the edit was computed
            // for? Its verdicts multiply and only a model run restores them, so an edit rejected once stays
            // rejected instead of flickering back.
            float3 printNow = 0.0;
            float contextNow = 0.0;
            float match = 1.0;

            if (Ghost(kGhostPrint))
            {
                printNow = sPrint[(gtid.y + 1) * 10 + gtid.x + 1];
                contextNow = Ghost(kGhostContext) ? ContextAt(uv) : 0.0;

                float3 lo, hi;
                PrintRange(gtid, lo, hi);

                if (gHistValid != 0 && onScreen)
                    match = PrintMatch(h.print, lo, hi, contextNow);
            }

            const float validity = !Ghost(kGhostPrint) ? 1.0 : (gHistValid != 0 && onScreen ? saturate(h.meta.y) * match : 0.0);

            // How far this frame moved the pixel, camera jitter left out: what ages a carried edit.
            const float motionPx = length((q - uv - float2(gJitterDeltaX, gJitterDeltaY)) * float2(gWidth, gHeight));

            if (gMode == 1)
            {
                gOut2[id.xy] = colour; // the untouched frame, kept for the apply

                // Keyframe crossfade: the shown edit takes this frame's share of the way to the model's
                // latest answer, so that it arrives exactly when the model runs again -- the change spread
                // evenly over the frames between runs instead of landing on one of them as a step.
                float3 shownEdit = h.edit;
                float shownConfidence = h.confidence;

                if (gCrossfadeOn != 0)
                {
                    shownEdit = lerp(h.edit, h.target, saturate(gCrossfade));
                    shownConfidence = lerp(h.confidence, h.targetConfidence, saturate(gCrossfade));
                }

                edit = shownEdit;
                w = h.valid * validity;

                // Confidence decays with age and with every doubt -- depth or colour -- and only
                // ever comes back on a refresh.
                // A colour failure costs confidence rather than all of it. Fine distant things alias a
                // little from frame to frame and passed or failed the colour test at random, so their
                // detail switched on and off; now a single failure dims it and only a run of them --
                // a surface that really changed, like grass in the wind -- takes it away.
                const float doubt = lerp(0.6, 1.0, vColour) * saturate(2.0 * h.valid - 1.0) * gHighDecay * validity;
                float confidence = shownConfidence * doubt;

                if (gCrossfadeOn != 0)
                    gOut6[id.xy] = float4(h.target, saturate(h.targetConfidence * doubt));

                gOut0[id.xy] = float4(edit, saturate(confidence));

                if (h.valid < 0.5)
                    InterlockedAdd(sRejected, 1u);
                else if (validity < 0.5)
                    InterlockedAdd(sRejectedPrint, 1u);

                // The print is carried as it is; the meta ages: a frame older, what is still believed,
                // and the motion it has been through (a still camera does not age it).
                if (gGhostFlags != 0u)
                {
                    gOut7[id.xy] = h.print;
                    gOut8[id.xy] = float4(h.meta.x + 1.0, validity, h.meta.z + saturate(motionPx / max(gStalePx, 0.1)),
                                          h.valid);
                }

                InterlockedAdd(sMotion, (uint) (min(motionPx, 32.0) * 8.0));
            }
            else
            {
                float ok;
                // An edit the fingerprint rejected is not something to hold the new answer to.
                const float3 fresh = Stabilize(FreshEditTiled(gtid, int2(id.xy), ok),
                                               h.edit, gHistValid != 0 ? h.valid * vColour * validity : 0.0);

                // Optional temporal smoothing of the refresh: where the carried edit is still valid,
                // move only part of the way to the new one. 1 takes the new answer whole. A pixel the
                // model returned black for keeps what was carried.
                float keep = (gHistValid != 0) ? (1.0 - gRefreshBlend) * h.valid * vColour * validity : 0.0;

                if (ok < 0.5)
                    keep = (gHistValid != 0 && h.valid > 0.5 && validity > 0.5) ? 1.0 : 0.0;

                // The fingerprint and meta of what is stored: this frame's, unless the model failed here
                // and the carried edit was kept, in which case its own go on with it.
                if (gGhostFlags != 0u)
                {
                    const bool kept = ok < 0.5 && keep > 0.5;
                    gOut7[id.xy] = kept ? h.print : float4(printNow, contextNow);
                    gOut8[id.xy] = kept ? float4(h.meta.x + 1.0, validity, h.meta.z, h.valid) : float4(0.0, 1.0, 0.0, 1.0);
                }

                edit = lerp(fresh, h.edit, saturate(keep));
                w = 1.0;

                if (gCrossfadeOn != 0)
                {
                    // The model's answer becomes the target, whole. What is shown takes the first step of
                    // the walk toward it, from what was shown before -- where that is still the same
                    // surface; elsewhere there is nothing to walk from, and the answer is shown at once.
                    const bool carry = gHistValid != 0 && h.valid > 0.5 && ok > 0.5 && validity > 0.5;
                    float a = carry ? saturate(gCrossfade) : 1.0;
                    float3 from = h.edit;

                    // Soft refresh. A pixel without a carried edit to walk from -- the fingerprint dropped it, or
                    // something moved in without motion vectors (a paper in the wind, debris, a hand) -- showed
                    // the region's light on the frames before, and took the model's answer at once here; the
                    // frame after, it is dropped again. On a long interval that is a flash on every run, and it
                    // was most of what moved on the frame the model ran. It now walks from what was on screen,
                    // like everything else: read at the history's taps where they hold this surface, and where
                    // they do not and the view is still, at the pixel itself -- only where something came in front
                    // (this surface no farther than last frame's): where the background reappears, walking from what
                    // stood there would drag its light along, so that takes the answer at once, as before.
                    if (gSoftRefresh != 0 && gTemporalValid != 0 && gHistValid != 0 && !carry && ok > 0.5 && onScreen)
                    {
                        const bool atTaps = h.valid > 0.5;
                        const float lastDepth = gHistGuide.Load(int3(clamp(int2(q * float2(gWidth, gHeight)), int2(0, 0),
                                                                           int2(gWidth, gHeight) - 1), 0)).x;

                        if (atTaps || (motionPx < 1.0 && linC <= lastDepth * (1.0 + gDepthTol)))
                        {
                            from = atTaps ? h.shown : gAux1.SampleLevel(gLinear, q, 0).rgb;
                            a = saturate(gCrossfade);
                        }
                        else if (gSoftReveal != 0)
                        {
                            // Soft reveal. What is left took the answer at once: the background reappearing behind
                            // something that moved, and anything moving with nothing carried. Foliage in the wind
                            // does both all the time -- it keeps uncovering what is behind it, and on the frames in
                            // between the fingerprint drops its carried edit -- so every run flashed across it
                            // (The Last of Us: almost all of what jumped on the model's frame). It walks from last
                            // frame's regional light there instead: the 12 px tent of what was on screen around the
                            // spot, close to what the region's fill showed it, and too broad to drag the light of
                            // the thing that moved away along.
                            const float2 spread = 6.0 / float2(gWidth, gHeight);
                            float3 regional = 0.0;

                            [unroll] for (int r = 0; r < 9; ++r)
                            {
                                const float2 o = float2(r % 3 - 1, r / 3 - 1);
                                const float wt = (o.x == 0 ? 2.0 : 1.0) * (o.y == 0 ? 2.0 : 1.0) / 16.0;
                                regional += gAux1.SampleLevel(gLinear, q + o * spread, 0).rgb * wt;
                            }

                            from = regional;
                            a = saturate(gCrossfade);
                        }
                    }

                    gOut6[id.xy] = float4(edit, 1.0);
                    edit = lerp(from, edit, a);
                    gOut0[id.xy] = float4(edit, carry ? lerp(h.confidence, 1.0, a) : 1.0);
                }
                else
                {
                    gOut0[id.xy] = float4(edit, 1.0);
                }

                // CacheAsync: where the background answer could not be carried to this pixel and no carried
                // edit stands either, the pixel holds nothing of its own and the apply fills it from its
                // region, as it fills a rejected pixel of a cached frame. Read as an answer, the gap would be
                // a dark hole.
                if (gAsyncWarp != 0 && ok < 0.5 && keep < 0.5)
                {
                    edit = 0.0;
                    w = 0.0;
                    gOut0[id.xy] = float4(0.0, 0.0, 0.0, 0.0);

                    if (gCrossfadeOn != 0)
                        gOut6[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
                }
            }

            gOut1[id.xy] = float4(linC, logLuma, 0.0, 0.0);
        }

        ReduceToFirstLevel(gtid, id, w, edit, linC, logLuma);

        if (gMode == 1)
        {
            GroupMemoryBarrierWithGroupSync();

            if (gtid.x == 0 && gtid.y == 0)
            {
                if (sRejected > 0u)
                    InterlockedAdd(gStats[uint2(gStatsSlot, 0)], sRejected);

                if (sRejectedPrint > 0u)
                    InterlockedAdd(gStats[uint2(gStatsSlot, 1)], sRejectedPrint);

                if (sMotion > 0u)
                    InterlockedAdd(gStats[uint2(gStatsSlot, 2)], sMotion);
            }
        }

        return;
    }

    if (gMode == 19)
    {
        // The pulse probe (ShowStats only), at a quarter of the frame. The regional light of the edit on screen --
        // the 12 px tent of log2(shown / untouched), gAux0 against gColour -- is kept (gOut0) and compared with last
        // frame's (gAux1) moved here, where the frame's own surroundings stayed put; and the detail around it, the
        // edit's distance from that regional light over the 4x4 block under the texel. Summed per frame into u5:
        // row 0 the weighted step (1e-4 stop), row 1 the weight (1e-3), row 2 the detail (1e-4 stop). The CPU
        // sorts the frames by how long since the model ran: a pulse is a step, or a detail, that follows that cycle.
        float4 part = 0.0;

        if (inside)
        {
            const float2 texel = 12.0 / float2(gSrcW, gSrcH);
            const float eps = Eps();
            float reg = 0.0;

            [unroll] for (int r = 0; r < 9; ++r)
            {
                const float2 o = float2(r % 3 - 1, r / 3 - 1);
                const float wt = (o.x == 0 ? 2.0 : 1.0) * (o.y == 0 ? 2.0 : 1.0) / 16.0;
                const float2 p = uv + o * texel;
                const float ys = dot(max(gAux0.SampleLevel(gLinear, p, 0).rgb, 0.0), kLuma);
                const float yo = dot(max(gColour.SampleLevel(gLinear, p, 0).rgb, 0.0), kLuma);
                reg += log2((ys + eps) / (yo + eps)) * wt;
            }

            gOut0[id.xy] = float4(reg, 0.0, 0.0, 0.0);

            const int2 base = int2(id.xy) * 4;
            float detail = 0.0;

            [unroll] for (int k = 0; k < 16; ++k)
            {
                const int2 t = min(base + int2(k & 3, k >> 2), int2(gSrcW, gSrcH) - 1);
                const float ys = dot(max(gAux0.Load(int3(t, 0)).rgb, 0.0), kLuma);
                const float yo = dot(max(gColour.Load(int3(t, 0)).rgb, 0.0), kLuma);
                detail += abs(log2((ys + eps) / (yo + eps)) - reg);
            }

            part.z = detail / 16.0;

            if (gHistValid != 0)
            {
                const float2 q = uv + MotionUvOffset(uv);

                if (all(q >= 0.0) && all(q <= 1.0))
                {
                    float w = 1.0;

                    if (gProbeContext != 0)
                        w = saturate(1.0 - abs(ContextAt(uv) - ContextPrevAt(q)) / 0.15);

                    part.x = w * min(abs(reg - gAux1.SampleLevel(gLinear, q, 0).r), 0.2);
                    part.y = w;
                }
            }
        }

        const uint li = gtid.y * 8 + gtid.x;
        sEdit[li] = part;
        GroupMemoryBarrierWithGroupSync();

        if (li == 0)
        {
            float4 sum = 0.0;

            for (uint j = 0; j < 64; ++j)
                sum += sEdit[j];

            InterlockedAdd(gStats[uint2(gStatsSlot, 0)], (uint) (sum.x * 1e4));
            InterlockedAdd(gStats[uint2(gStatsSlot, 1)], (uint) (sum.y * 1e3));
            InterlockedAdd(gStats[uint2(gStatsSlot, 2)], (uint) (sum.z * 1e4));
        }

        return;
    }

    if (!inside)
        return;

    if (gMode == 3)
    {
        // One level from the one above it: each texel is the coverage-weighted mean of a 4x4 block.
        float3 acc = 0.0;
        float2 gacc = 0.0;
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

                    if (Ghost(kGhostSurface))
                        gacc += gAux1.Load(int3(s, 0)).xy * v.a;
                }
            }
        }

        gOut0[id.xy] = float4(wsum > 1e-6 ? acc / wsum : 0.0, wsum / 16.0);

        // The same-surface fill reads every level bilaterally, so every level carries its guide.
        if (Ghost(kGhostSurface))
            gOut1[id.xy] = float4(wsum > 1e-6 ? gacc / wsum : 0.0, 0.0, 0.0);

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

        // Anti-ghosting fill: the low band from the same surface at every level, or none.
        if (Ghost(kGhostSurface))
            low = SurfaceLow(uv, logD, guide.y);

        // The edit's age, what is still believed of it and how far it has travelled.
        const float4 meta = gGhostFlags != 0u ? gHistMeta.Load(int3(id.xy, 0)) : float4(0.0, 1.0, 0.0, 1.0);

        // Aging: the carried detail fades with the motion it has been through -- toward the edit's broad
        // part, or the whole edit toward none. A still camera does not age it, so nothing pulses standing
        // still; the model's next run brings it back whole.
        float highWeight = hist.a;
        float ageScale = 1.0;

        if (Ghost(kGhostAging))
        {
            const float a = exp2(-meta.z / max(gAgeHalfLife, 0.1));

            if (Ghost(kGhostAgeNeutral))
                ageScale = a;
            else
                highWeight *= a;
        }

        // The high band is what the pixel's own history says beyond its region, and it is only as
        // good as its confidence. Where the history was rejected the pixel takes the low band alone.
        const float3 high = hist.rgb - low;
        const float3 edit = ClampEdit(gLowGain * low + gHighGain * highWeight * high, max(gLowGain, gHighGain)) * ageScale;

        const float eps = Eps();
        float3 result = max((max(original.rgb, 0.0) + eps) * exp2(edit) - eps, 0.0);

        // With the temporal stabiliser, the anti pop-in or the guided filter on, this pass only hands its
        // edit on; mode 15 or mode 10 writes the frame.
        if ((gTemporal > 0.0 || gAntiPopStep > 0.0 || Ghost(kGhostGuided)) && gDebugView == 0)
        {
            gOut3[id.xy] = float4(edit, 1.0);
            return;
        }

        // Debug views, in the frame's own units.
        const float white = WhitePoint();

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
            // The rejection mask. Green: the carried edit is believed. Red: the fingerprint rejected it --
            // the frame changed under it. Orange: rejected by depth -- revealed, or another surface.
            // In between, how much of it is still believed.
            const float3 c = meta.w < 0.5 ? float3(1.0, 0.5, 0.0) : lerp(float3(1.0, 0.08, 0.08), float3(0.1, 1.0, 0.25), meta.y);
            result = c * white * 0.5;
        }
        else if (gDebugView == 5 || gDebugView == 6)
        {
            // The edit's age in frames since the model ran (5), or the motion it has travelled, in frames of
            // staleness (6): black, blue, green, yellow, red at eight.
            const float a = saturate((gDebugView == 5 ? meta.x : meta.z) / 8.0);
            float3 c = lerp(float3(0.0, 0.0, 0.0), float3(0.0, 0.3, 1.0), saturate(a * 4.0));
            c = lerp(c, float3(0.0, 1.0, 0.3), saturate(a * 4.0 - 1.0));
            c = lerp(c, float3(1.0, 1.0, 0.0), saturate(a * 4.0 - 2.0));
            c = lerp(c, float3(1.0, 0.0, 0.0), saturate(a * 4.0 - 3.0));
            result = c * white * 0.5;
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

    if (gMode == 10)
    {
        // The edit's regional light at uv: a 3x3 tent of bilinear taps a dozen pixels apart, which is
        // the region a few dozen pixels across that a shift of overall brightness covers.
        // The temporal stabiliser: the shown edit, blended with last frame's shown edit reprojected --
        // with that history clamped to the range this frame's own 3x3 neighbourhood spans (variance
        // clipping, as every TAA does). Whatever the model re-decides from frame to frame within that
        // range -- the flicker, the specks, the swimming of synthesised detail -- is averaged out; a
        // history that no longer fits (a surface revealed, a light switched on) is pulled back to the
        // present instead of trailing. The edit then moves with the motion vectors, which is also what
        // frame generation assumes when it builds the frames in between.
        const float4 original = gColour.Load(int3(id.xy, 0));
        const float3 e = gAux0.Load(int3(id.xy, 0)).rgb;

        float m1 = 0.0, m2 = 0.0;

        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float l = sEditLuma[(gtid.y + k / 3) * 10 + gtid.x + k % 3];
            m1 += l;
            m2 += l * l;
        }

        const float mu = m1 / 9.0;
        const float sigma = sqrt(max(m2 / 9.0 - mu * mu, 0.0));

        float3 outEdit = e;

        // The regional light of this frame's edit, for the luminance stability and the anti pop-in below.
        const float2 texel = 12.0 / float2(gWidth, gHeight);
        float3 lowNow = 0.0;
        const bool split = gLowTemporal > 0.0 || gAntiPopStep > 0.0;

        if (split && gRegionalReady != 0)
        {
            // Precomputed at a quarter of the frame (mode 17): the tent is 36 pixels wide, so one bilinear
            // read of it is the nine taps' answer.
            lowNow = gRegNow.SampleLevel(gLinear, uv, 0).rgb;
        }
        else if (split)
        {
            [unroll] for (int r = 0; r < 9; ++r)
            {
                const float2 o = float2(r % 3 - 1, r / 3 - 1);
                const float wt = (o.x == 0 ? 2.0 : 1.0) * (o.y == 0 ? 2.0 : 1.0) / 16.0;
                lowNow += gAux0.SampleLevel(gLinear, uv + o * texel, 0).rgb * wt;
            }
        }

        if (gTemporalValid != 0)
        {
            const float linC = gHistGuide.Load(int3(id.xy, 0)).x;
            const float2 q = uv + MotionUvOffset(uv);

            if (all(q >= 0.0) && all(q <= 1.0))
            {
                const float2 pos = q * float2(gWidth, gHeight) - 0.5;
                const int2 i0 = (int2) floor(pos);
                const float2 f = pos - floor(pos);

                float3 hist = 0.0;
                float wsum = 0.0;

                [unroll] for (int j = 0; j < 4; ++j)
                {
                    const int2 o = int2(j & 1, j >> 1);
                    const int2 t = clamp(i0 + o, int2(0, 0), int2(gWidth, gHeight) - 1);
                    const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
                    const float rel = RelDepthDiff(gAux2.Load(int3(t, 0)).x, linC);
                    const float w = wb * saturate((2.0 * gDepthTol - rel) / max(gDepthTol, 1e-6));
                    hist += gAux1.Load(int3(t, 0)).rgb * w;
                    wsum += w;
                }

                if (wsum > 0.25)
                {
                    hist /= wsum;
                    const float lh = dot(hist, kLuma);
                    const float lc = clamp(lh, mu - sigma - 0.02, mu + sigma + 0.02);
                    const float valid = saturate(2.0 * wsum - 1.0);

                    if (!split)
                    {
                        hist += lc - lh;
                        outEdit = lerp(e, hist, gTemporal * valid);
                    }
                    else
                    {
                        // Luminance stability. The variance clip above holds a pixel to what its eight
                        // neighbours span -- and when a whole region of the model's answer brightens and
                        // darkens together, the neighbours move with it, so the clip lets it through.
                        // That regional breathing is what an OLED, black around it, shows most.
                        //
                        // So the edit is split: its regional light (the tent above) is eased in time on
                        // its own, strongly, while the detail on top keeps the clipped blend. A real change
                        // of the edit's light -- a step of a third of a stop or more -- loosens the easing at
                        // once, so the light follows the scene and only the trembling is held.
                        float3 lowHist = 0.0;

                        if (gRegionalReady != 0)
                        {
                            lowHist = gRegPrev.SampleLevel(gLinear, q, 0).rgb;
                        }
                        else
                        {
                            [unroll] for (int r = 0; r < 9; ++r)
                            {
                                const float2 o = float2(r % 3 - 1, r / 3 - 1);
                                const float wt = (o.x == 0 ? 2.0 : 1.0) * (o.y == 0 ? 2.0 : 1.0) / 16.0;
                                lowHist += gAux1.SampleLevel(gLinear, q + o * texel, 0).rgb * wt;
                            }
                        }

                        const float step = dot(lowHist - lowNow, kLuma) / 0.35;

                        // Full strength standing still and in slow motion, where the breathing shows; let
                        // go as the view moves faster, where new content legitimately changes the light and
                        // holding it would only make it trail (measured: +15% change in a pan without this).
                        const float motionPx = length((q - uv) * float2(gWidth, gHeight)) / 6.0;
                        const float lowW = gLowTemporal * valid * exp(-step * step) * exp(-motionPx * motionPx);
                        float3 lowOut = lerp(lowNow, lowHist, lowW);

                        // Anti light pop-in. The model sees only the frame: a bright light entering it makes
                        // the model re-grade regions far from the light, whose own picture did not change at
                        // all -- the whole image shifts in one frame. A change of the edit's regional light
                        // where the frame's own regional light stayed put is that re-grade, and it is let
                        // through at a bounded rate, as an eye adapts. Where the frame did change (a light
                        // switched on right there, a cut, a revealed area) the change passes at once.
                        if (gAntiPopStep > 0.0)
                        {
                            // The frame's own regional light, now and last frame where this pixel was: the
                            // quarter-size surroundings, normalised by the white point the model is shown.
                            const float frameNow = ContextAt(uv);
                            const float framePrev = ContextPrevAt(q);
                            const float still = saturate(1.0 - abs(frameNow - framePrev) / max(gAntiPopFrameTol, 1e-3)) * valid;
                            const float dl = dot(lowOut - lowHist, kLuma);
                            lowOut += (clamp(dl, -gAntiPopStep, gAntiPopStep) - dl) * still;
                        }

                        hist += lc - lh;
                        const float3 detail = lerp(e - lowNow, hist - lowHist, gTemporal * valid);
                        outEdit = lowOut + detail;
                    }
                }
            }
        }

        const float eps = Eps();
        gOut0[id.xy] = float4(max((max(original.rgb, 0.0) + eps) * exp2(outEdit) - eps, 0.0), original.a);
        gOut3[id.xy] = float4(outEdit, 1.0);
        return;
    }

    if (gMode == 16)
    {
        // Once per frame, per depth texel (dispatched over the depth's valid region): the nearest-surface
        // motion every pass reads, instead of each pixel of each pass searching the 3x3 again.
        gOut0[id.xy] = float4(NearestSurfaceMotion(int2(id.xy)), 0.0, 0.0);
        return;
    }

    if (gMode == 18)
    {
        // CacheAsync. The model ran in the background on the frame two before this one (or more): gAux0 is
        // that frame composed with its answer, gAux1 that frame as the upscaler wrote it, gAux2 its depth. Its
        // edit is carried here along the motion chained since (gAux3), each tap admitted only where that
        // frame's depth agrees with this pixel's -- a cached frame's reprojection, the frames between at once.
        // Written as this frame times the carried edit (gOut1), so the capture reads it exactly as it reads the
        // model's own frame on a frame the model runs; black where nothing could be carried, which the capture
        // reads as no answer. gOut0 keeps this frame untouched for the capture and the apply.
        const float4 colour = gColour.Load(int3(id.xy, 0));
        gOut0[id.xy] = colour;

        const float linC = LinDepth(gDepth.Load(int3(DepthTexel(uv), 0)).r);
        const float2 q = uv + AccumulatedUvOffset(DepthTexel(uv));
        float3 carried = 0.0;

        if (all(q >= 0.0) && all(q <= 1.0))
        {
            const float2 size = float2(gWidth, gHeight);
            const float2 pos = q * size - 0.5;
            const int2 i0 = (int2) floor(pos);
            const float2 f = pos - floor(pos);
            float3 acc = 0.0;
            float wsum = 0.0;

            [unroll] for (int k = 0; k < 4; ++k)
            {
                const int2 o = int2(k & 1, k >> 1);
                const int2 t = clamp(i0 + o, int2(0, 0), int2(gWidth, gHeight) - 1);
                const float wb = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
                const float linThen = LinDepth(gAux2.Load(int3(DepthTexel((float2(t) + 0.5) / size), 0)).r);
                const float wd = saturate((2.0 * gDepthTol - RelDepthDiff(linThen, linC)) / max(gDepthTol, 1e-6));

                float ok;
                const float3 e = FreshEdit(gAux0.Load(int3(t, 0)).rgb, gAux1.Load(int3(t, 0)).rgb, ok);
                const float w = wb * wd * ok;
                acc += e * w;
                wsum += w;
            }

            if (wsum > 0.5)
            {
                const float eps = Eps();
                carried = max((max(colour.rgb, 0.0) + eps) * exp2(acc / wsum) - eps, 0.0);
            }
        }

        gOut1[id.xy] = float4(carried, colour.a);
        return;
    }

    if (gMode == 17)
    {
        // The 12 px tent of gAux0 at the centres of a quarter-size grid (gSrcW/H is the frame).
        const float2 texel = 12.0 / float2(gSrcW, gSrcH);
        float3 acc = 0.0;

        [unroll] for (int r = 0; r < 9; ++r)
        {
            const float2 o = float2(r % 3 - 1, r / 3 - 1);
            const float wt = (o.x == 0 ? 2.0 : 1.0) * (o.y == 0 ? 2.0 : 1.0) / 16.0;
            acc += gAux0.SampleLevel(gLinear, uv + o * texel, 0).rgb * wt;
        }

        gOut0[id.xy] = float4(acc, 0.0);
        return;
    }

    if (gMode == 12)
    {
        // Anti-ghosting: this frame's surroundings, each texel the mean normalised log luma of a 4x4
        // block of the frame (dispatched at a quarter size; gSrcW/H is the frame).
        float acc = 0.0;
        float n = 0.0;
        const float white = WhitePoint();

        [unroll] for (uint y = 0; y < 4; ++y)
        {
            [unroll] for (uint x = 0; x < 4; ++x)
            {
                const uint2 s = id.xy * 4u + uint2(x, y);

                if (s.x < gSrcW && s.y < gSrcH)
                {
                    acc += PrintOf(gColour.Load(int3(s, 0)).rgb, white).x;
                    n += 1.0;
                }
            }
        }

        gOut0[id.xy] = float4(n > 0.0 ? acc / n : 0.0, 0.0, 0.0, 0.0);
        return;
    }

    if (gMode == 13)
    {
        // The guided filter models the carried edit, window by window, as a linear function of the frame's
        // own log luma: a = cov(I, edit) / (var(I) + eps), b = mean(edit) - a mean(I). Where the frame has
        // structure the edit keeps the structure that follows it; where the frame is flat (the wall a
        // character walked away from) a goes to zero and the edit to its local mean -- the ghost's
        // outline has nothing in the frame to hold on to. He et al., computed at half size ("fast").
        //
        // This pass: 2x2 means of I, I^2, the edit and I x edit, at half size (gSrcW/H is the frame).
        float I1 = 0.0, I2 = 0.0, n = 0.0;
        float3 p1 = 0.0, Ip = 0.0;
        const float white = WhitePoint();

        [unroll] for (uint y = 0; y < 2; ++y)
        {
            [unroll] for (uint x = 0; x < 2; ++x)
            {
                const uint2 s = id.xy * 2u + uint2(x, y);

                if (s.x < gSrcW && s.y < gSrcH)
                {
                    const float I = PrintOf(gColour.Load(int3(s, 0)).rgb, white).x;
                    const float3 e = gAux0.Load(int3(s, 0)).rgb;
                    I1 += I;
                    I2 += I * I;
                    p1 += e;
                    Ip += I * e;
                    n += 1.0;
                }
            }
        }

        n = max(n, 1.0);
        gOut0[id.xy] = float4(I1 / n, I2 / n, p1.r / n, p1.g / n);
        gOut1[id.xy] = float4(p1.b / n, Ip / n);
        return;
    }

    if (gMode == 14)
    {
        // The window means, then the coefficients (gWidth/H is the half size).
        static const int R = 1; // kDlssNrCacheGuidedRadius
        float4 s0 = 0.0, s1 = 0.0;

        [unroll] for (int dy = -R; dy <= R; ++dy)
        {
            [unroll] for (int dx = -R; dx <= R; ++dx)
            {
                const int2 t = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(gWidth, gHeight) - 1);
                s0 += gGuide0.Load(int3(t, 0));
                s1 += gGuide1.Load(int3(t, 0));
            }
        }

        s0 /= (float) ((2 * R + 1) * (2 * R + 1));
        s1 /= (float) ((2 * R + 1) * (2 * R + 1));

        const float mI = s0.x;
        const float varI = max(s0.y - mI * mI, 0.0);
        const float3 mp = float3(s0.z, s0.w, s1.x);
        const float3 a = (s1.yzw - mI * mp) / (varI + max(gGuidedEps, 1e-5));

        gOut0[id.xy] = float4(a, 0.0);
        gOut1[id.xy] = float4(mp - a * mI, 0.0);
        return;
    }

    if (gMode == 15)
    {
        // The edit rebuilt from this frame, a and b averaged over the windows covering the pixel (a tent
        // of four bilinear reads), and blended in by how far the edit has travelled: a fresh edit is the
        // model's own and is left alone, so nothing changes standing still.
        const float4 original = gColour.Load(int3(id.xy, 0));
        const float3 raw = gAux0.Load(int3(id.xy, 0)).rgb;
        const float4 meta = gHistMeta.Load(int3(id.xy, 0));

        const float2 t = 0.5 / float2(max(gHalfW, 1u), max(gHalfH, 1u));
        float3 A = 0.0, B = 0.0;

        [unroll] for (int k = 0; k < 4; ++k)
        {
            const float2 o = float2(k & 1 ? t.x : -t.x, k & 2 ? t.y : -t.y);
            A += gGuide0.SampleLevel(gLinear, uv + o, 0).rgb;
            B += gGuide1.SampleLevel(gLinear, uv + o, 0).rgb;
        }

        const float3 rebuilt = 0.25 * (A * NormLogLuma(original.rgb) + B);
        const float weight = saturate(gGuidedStrength) * saturate(meta.z / 2.0);
        const float3 edit = ClampEdit(lerp(raw, rebuilt, weight), max(gLowGain, gHighGain));

        if (gTemporal > 0.0 || gAntiPopStep > 0.0)
        {
            gOut3[id.xy] = float4(edit, 1.0);
            return;
        }

        const float eps = Eps();
        gOut0[id.xy] = float4(max((max(original.rgb, 0.0) + eps) * exp2(edit) - eps, 0.0), original.a);
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
