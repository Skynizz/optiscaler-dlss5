#pragma once

// The temporal edit cache: what its shader reads.
//
// The model is the expensive part of Neural Rendering -- 98% of the pass -- and it is run every frame
// to produce something that mostly does not change from one frame to the next. The cache stores what
// the model CHANGED, as a per-channel log ratio of the edited frame to the frame it was given, and
// carries that edit forward along the game's motion vectors onto each new frame. The model only runs
// to refresh it.
//
// Only the correction is ever cached, never the picture. The frame underneath is the game's own,
// rendered fresh every frame -- grass, hair and characters are never reprojected, only the model's
// verdict about how they should be lit and detailed.
//
// One shader, several modes, like the composition pass beside it. The struct below and the Params
// cbuffer in dlssnr_cache.hlsl are one ordered list of 4-byte scalars: append only, to both, in the
// same order (DEVELOPMENT.md rule 5).

#include <cstdint>

enum DlssNrCacheMode : uint32_t
{
    DlssNrCacheMode_ClearStats = 0, // zero this frame's counters
    DlssNrCacheMode_Reproject = 1,  // history -> this frame, validated; builds the first pyramid level
    DlssNrCacheMode_Capture = 2,    // a frame the model ran on -> fresh history
    DlssNrCacheMode_Downsample = 3, // one pyramid level -> the next, coverage weighted
    DlssNrCacheMode_Apply = 4,      // the frame x the reconstructed edit -> output
    DlssNrCacheMode_AccumulateMv = 5, // motion since the model last ran, for its own history
    DlssNrCacheMode_JbuUpsample = 6,  // a below-size model answer -> full size, guided by the frame
    DlssNrCacheMode_DumpPack = 7,     // the measurement dump's per-frame images
    DlssNrCacheMode_CropGuides = 8,   // one band of depth and motion, for a model run on that band
    DlssNrCacheMode_LocalMap = 9      // the automatic white point's smoothed local luminance map
};

// Spread refresh: at most this many bands, so a band never has too little of the picture around it.
constexpr uint32_t kDlssNrCacheMaxBands = 4;

// The first pyramid level is a quarter of the frame on each side, and each level below a quarter of
// the one above. Three levels reach 1/64, coarse enough that a disocclusion the size of a character
// still finds valid neighbours to borrow from.
constexpr uint32_t kDlssNrCachePyramidStep = 4;
constexpr uint32_t kDlssNrCachePyramidLevels = 3;

// Two counters per frame slot -- pixels whose history was rejected, and how many of those were
// priority (stencil) pixels -- and four slots, matching the readback ring.
constexpr uint32_t kDlssNrCacheStatSlots = 4;
constexpr uint32_t kDlssNrCacheStatsWidth = kDlssNrCacheStatSlots * 2;

struct alignas(256) DlssNrCacheConstants
{
    uint32_t Mode;
    uint32_t Width;  // the dispatch: the frame, or a pyramid level
    uint32_t Height;

    // How much of the depth and motion textures is real, as the main pass worked it out.
    uint32_t DepthWidth;
    uint32_t DepthHeight;
    uint32_t MotionWidth;
    uint32_t MotionHeight;

    // The game's own motion vector scale: vector * scale = pixels of the motion texture.
    float MvScaleX;
    float MvScaleY;
    uint32_t DepthInverted;

    // The floor added above and below every ratio, in the buffer's own units -- paper white / 512, the
    // same floor the composition uses, so a pixel with no light in it carries no edit.
    float Epsilon;

    // Validation. Depth is relative (0.1 = ten percent); colour is in stops of the frame's own luma.
    float DepthTolerance;
    float ColourTolerance;

    // How much of the high band's confidence survives each cached frame.
    float HighDecay;

    // On a refresh, how much of the model's new answer replaces the carried one. 1 = all of it.
    float RefreshBlend;
    uint32_t HistoryValid;

    // The multi-pass approximation: gains on the low and high bands of the edit, in log space.
    float LowGain;
    float HighGain;

    uint32_t Bilateral;
    uint32_t DebugView;

    uint32_t StencilEnabled;
    uint32_t StencilMask;
    uint32_t StencilRef;
    uint32_t StatsSlot;

    // The first accumulation after the model ran starts from zero rather than from what it was given.
    uint32_t AccumulateReset;

    // The frame is already tone mapped (display-referred), as the main pass decided.
    uint32_t Passthrough;
    float JbuSigma;

    // The source of a downsample, a JBU or an accumulation, when it differs from the dispatch.
    uint32_t SourceWidth;
    uint32_t SourceHeight;

    uint32_t FrameIndex;

    // Spread refresh (see DlssNrCacheBand).
    uint32_t BandActive;
    uint32_t BandY0;
    uint32_t BandHeight;
    uint32_t BandFeather;
    uint32_t BandEdges;
    uint32_t CropOffsetY;

    // The game's live exposure (bound at t10), so the ratio floor is the composition's own on this frame.
    uint32_t UseGameExposure;
    float ExposurePreMul;

    // log2 of the composition's highlight guard, plus half a stop for the soft knee.
    float MaxLumaEdit;

    // Anti-flicker: the largest step, in stops, a refresh may make on a still-valid pixel. 0 is off.
    float Stabilize;

    // Bound each fresh edit by its eight neighbours' (removes isolated specks the model invents).
    uint32_t Despeckle;

    // The local luminance map's temporal smoothing: the share of this frame's reading taken.
    float MapBlend;

    // Keyframe crossfade: on, and this frame's step toward the model's latest answer.
    uint32_t CrossfadeOn;
    float Crossfade;
};
