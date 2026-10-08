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
    DlssNrCacheMode_Temporal = 10,    // the temporal stabiliser: shown edit vs last frame's, clamped
    DlssNrCacheMode_Copy = 11,        // pre-SR: the game's colour into the texture the pass rewrites
    DlssNrCacheMode_Context = 12,     // anti-ghosting: this frame's surroundings (log luma, 1/4 size)
    DlssNrCacheMode_GuidedDown = 13,  // guided filter: the frame and the edit's local sums (1/2 size)
    DlssNrCacheMode_GuidedCoef = 14,  // guided filter: per-window linear model of the edit on the frame
    DlssNrCacheMode_GuidedApply = 15  // guided filter: the edit rebuilt from the frame, then composed
};

// Anti-ghosting switches (GhostFlags). Each one off is the cache exactly as it was before it.
enum DlssNrCacheGhostFlag : uint32_t
{
    DlssNrCacheGhost_Fingerprint = 1u,  // reject a carried edit whose source no longer matches the frame
    DlssNrCacheGhost_Context = 2u,      // ... including its surroundings (an object that moved away)
    DlssNrCacheGhost_Aging = 4u,        // carried detail fades with how far it has travelled
    DlssNrCacheGhost_AgeNeutral = 8u,   // ... the whole edit, rather than toward its broad part
    DlssNrCacheGhost_Guided = 16u,      // guided filter: the carried edit keeps structure only where the frame has it
    DlssNrCacheGhost_SurfaceFill = 32u  // rejected pixels borrow only from the same surface, at every scale
};

// The guided filter's window, in half-size texels each side (1 = 6 pixels across at full size).
constexpr uint32_t kDlssNrCacheGuidedRadius = 1;

// Per-frame counters, one row each: rejected by depth, rejected by the fingerprint, motion (1/8 px).
constexpr uint32_t kDlssNrCacheStatRows = 3;

// The first pyramid level is a quarter of the frame on each side, and each level below a quarter of
// the one above. Three levels reach 1/64, coarse enough that a disocclusion the size of a character
// still finds valid neighbours to borrow from.
constexpr uint32_t kDlssNrCachePyramidStep = 4;
constexpr uint32_t kDlssNrCachePyramidLevels = 3;

// One counter per frame slot and row (see kDlssNrCacheStatRows) and four slots, matching the readback
// ring.
constexpr uint32_t kDlssNrCacheStatSlots = 4;
constexpr uint32_t kDlssNrCacheStatsWidth = kDlssNrCacheStatSlots;

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

    // The game's live exposure (bound at t10), so the ratio floor is the composition's own on this frame.
    uint32_t UseGameExposure;
    float ExposurePreMul;

    // log2 of the composition's highlight guard, plus half a stop for the soft knee.
    float MaxLumaEdit;

    // Anti-flicker: the largest step, in stops, a refresh may make on a still-valid pixel. 0 is off.
    float Stabilize;

    // Bound each fresh edit by its eight neighbours' (removes isolated specks the model invents).
    uint32_t Despeckle;

    // Keyframe crossfade: on, and this frame's step toward the model's latest answer.
    uint32_t CrossfadeOn;
    float Crossfade;

    // Temporal stabiliser: the weight of last frame's clamped edit, and whether it exists.
    float Temporal;
    uint32_t TemporalValid;

    // Luminance stability: the weight of last frame's regional (low band) edit, eased on its own.
    float LowTemporal;

    // Pre-SR: this frame's change of camera jitter, in uv, added to every reprojection. 0 otherwise.
    float JitterDeltaX;
    float JitterDeltaY;

    // Anti-ghosting (DlssNrCacheGhostFlag). 0 is the cache exactly as it was without it.
    uint32_t GhostFlags;
    // Fingerprint: stops outside this frame's 3x3 range (luma, chroma) before doubt starts; none left at twice.
    float PrintTolerance;
    // Fingerprint: stops of change of the surroundings (a ~10 pixel neighbourhood's mean luma).
    float ContextTolerance;
    // Aging: frames of motion over which the carried detail halves.
    float AgeHalfLife;
    // Aging: pixels of motion that count as one frame of staleness.
    float StalePixels;
    // Guided filter: regularisation (log luma squared) and how much of the rebuilt edit is taken.
    float GuidedEps;
    float GuidedStrength;
    // The context grid (a quarter of the frame) and the guided filter's (a half).
    uint32_t ContextWidth;
    uint32_t ContextHeight;
    uint32_t HalfWidth;
    uint32_t HalfHeight;

    // Anti light pop-in: the most, in stops, the edit's regional light may move in one frame where the
    // frame itself did not change there (0 is off), and how much the frame's own regional luma may change,
    // in stops, before the region counts as changed.
    float AntiPopStep;
    float AntiPopFrameTolerance;

    // Noise-aware checks: the colour test against the 3x3 range of this frame (not the pixel alone), and the
    // anti-flicker faded in with trust and soft-limited rather than switched on and clamped. 0 is as before.
    uint32_t NoiseAware;
};
