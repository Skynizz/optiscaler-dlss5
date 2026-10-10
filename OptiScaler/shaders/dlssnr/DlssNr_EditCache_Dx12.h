#pragma once

// The temporal edit cache for Neural Rendering, on Direct3D 12.
//
// The model runs one frame in N, or sooner when too much of the picture has been revealed since it
// last ran. Every frame in between, its last answer -- stored as an edit, never as a picture -- is
// carried forward along the game's motion vectors and laid over the game's fresh frame:
//
//   refresh frame   the composition pass runs as it always has; Capture then stores what it changed
//   cached frame    Reproject carries the stored edit onto this frame and validates it per pixel,
//                   the pyramid fills what could not be validated, Apply lays it on the fresh frame
//
// The edit is split in two on the way back. The low band -- the model's lighting and tone, broad and
// slow -- is reprojected everywhere and borrowed from valid neighbours where a pixel's own history was
// rejected. The high band -- the detail it synthesised -- is kept only where depth and colour both
// agree it still belongs to the same surface, and fades with age. Grass, hair and water fail that test
// constantly and fall back to the low band, which is the robust part.
//
// At long intervals the carried edit used to trail. Four anti-ghosting tools, each with its own switch:
//
//   fingerprint     the frame as it was where the model computed the edit, carried with the edit and
//                   compared with the frame every frame; a carried edit the frame no longer matches is
//                   dropped for the rest of the interval and the same surface's broad edit stands in
//   guided filter   the carried edit rebuilt from this frame, window by window, so it keeps structure only
//                   where the frame has it
//   aging           carried detail fades with the motion it has been through
//   adaptive        the model runs sooner the faster the camera moves and the more is rejected
//
// Also hosts the joint bilateral upsampler for a below-size model, which shares the shader.
//
// Off by default, and nothing here runs or allocates unless asked for.

#include "DlssNr_CacheCommon.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <filesystem>
#include <string>
#include <vector>

class Config;

// What the caller knows about this frame's guides. Every resource is in NON_PIXEL_SHADER_RESOURCE
// on the way in and is left there.
struct DlssNrCacheInputs
{
    ID3D12Resource* depth = nullptr;       // readable: the guide itself, or the main pass's typed clone
    ID3D12Resource* motion = nullptr;      // readable, likewise

    unsigned int depthWidth = 0; // the valid region of each, as the main pass worked it out
    unsigned int depthHeight = 0;
    unsigned int motionWidth = 0;
    unsigned int motionHeight = 0;

    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    bool depthInverted = false;

    // The buffer's own "1.0", so the ratio floor is in the right units.
    float whitePoint = 1.0f;
    bool passthrough = false;

    // The game's live exposure, exactly as the composition reads it, so both use one white point.
    ID3D12Resource* exposure = nullptr;
    bool useGameExposure = false;
    float exposurePreMul = 1.0f;

    // The composition's highlight guard: no carried edit may move a pixel further than it could.
    float maxRatio = 2.0f;

    // Pre-SR: the change of camera jitter since last frame, in uv (0 after the upscaler).
    float jitterDeltaX = 0.0f;
    float jitterDeltaY = 0.0f;
};

// Several dispatches a frame -- up to seven on a refresh with a dump -- and frame generation can keep
// the GPU many frames behind, so the ring is deep. See DLSSNR_NUM_OF_HEAPS for why this matters.
#define DLSSNR_CACHE_NUM_OF_HEAPS 128

class DlssNrEditCache_Dx12 : public Shader_Dx12
{
  public:
    struct Status
    {
        bool active = false;
        unsigned long long refreshes = 0;
        unsigned long long cached = 0;
        unsigned int framesSinceRefresh = 0;
        float lastRejected = 0.0f;       // the most recent frame's rejected fraction
        float cumulativeRejected = 0.0f; // since the last refresh
        const char* lastRefreshReason = "";
        int regime = 1; // 0 still, 1 moving, 2 fast
        unsigned int dumpWritten = 0;
        bool dumpActive = false;

        // Anti-ghosting.
        unsigned int ghostFlags = 0;      // DlssNrCacheGhostFlag, as running
        unsigned int intervalNow = 1;     // frames between model runs right now
        float printRejected = 0.0f;       // the most recent frame's share rejected by the fingerprint
        float speed = 0.0f;               // camera motion, pixels per frame, smoothed
        unsigned int budgetFloor = 0;     // the shortest interval the GPU budget allows (0: no budget)
        double costRefresh = 0.0;         // the pass on a frame the model runs, ms
        double costCached = 0.0;          // the pass on a cached frame, ms

        // The pulse probe (ShowStats): the regional step of the edit on screen by frame since the model ran
        // (stop, mean over the last measured stretch; negative where there was none), and its detail.
        static constexpr int kPulsePhases = 16;
        float pulseStep[kPulsePhases] = {};
        float pulseDetail[kPulsePhases] = {};
        unsigned int pulsePhases = 0; // how many of the above were measured

        // GPU time of the cache's own passes, ms, smoothed; negative when not measured.
        static constexpr int kStages = 7;
        double stageMs[kStages] = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0 };
    };

    // The passes timed for the overlay, in the order of Status::stageMs.
    static const char* StageName(int stage);

    explicit DlssNrEditCache_Dx12(ID3D12Device* device);
    ~DlssNrEditCache_Dx12();

    // Decides this frame. True means the model must run (a refresh); false means a cached frame.
    // Call once per frame while the cache is active, before anything else here.
    // preSr: the frame is the game's jittered render, before its upscaler (see BeginFrame).
    // async: the model runs in the background (CacheAsync) -- at least kAsyncMinInterval frames apart, and
    // asyncBusy (an answer on its way) holds the next run until it has landed.
    bool BeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width, unsigned int height,
                    DXGI_FORMAT format, bool reset, bool preSr = false, bool async = false, bool asyncBusy = false);

    // CacheAsync: a run launched on frame N lands on N + 2 at the earliest, so the next can start on N + 3.
    static constexpr unsigned int kAsyncMinInterval = 3;

    // Forget the history: the next active frame is a refresh.
    void Invalidate();

    // CacheAsync: the model could not run on the frame meant for it; the next frame is a refresh.
    void RefreshNext() { _refreshNext = true; }

    // The whole pass's GPU cost on a frame the model runs and on a cached frame, measured by the caller:
    // what the GPU budget (CacheBudgetMs) is held to. 0 while not yet measured.
    void SetFrameCosts(double refreshMs, double cachedMs)
    {
        _costRefresh = refreshMs;
        _costCached = cachedMs;
    }

    // A cached frame: target (UNORDERED_ACCESS, holding the upscaler's frame) is rewritten as that frame
    // times the carried edit. keep (UNORDERED_ACCESS) receives the untouched frame on the way.
    // accumulate: chain this frame's motion onto the model's (false where ModelMotion just did).
    bool RunCached(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                   ID3D12Resource* keep, const DlssNrCacheInputs& in, bool accumulate = true);

    // A refresh, before the model is evaluated: the motion vectors to hand it, so its own history is
    // reprojected across every frame it skipped. Returns nullptr to keep the game's own. resetModel is
    // set when the configured policy is to reset the model on each refresh instead.
    ID3D12Resource* ModelMotion(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                const DlssNrCacheInputs& in, bool& resetModel);

    // What the model history policy is: 0 the game's vectors, 1 accumulated, 2 reset every run.
    unsigned int ModelHistory() const { return _modelHistory; }

    // A refresh, after the resolve: target (UNORDERED_ACCESS) holds the model's frame, original
    // (NON_PIXEL_SHADER_RESOURCE) the upscaler's. Stores the edit; rewrites target only when the
    // refresh blend or the band gains say the stored edit differs from the model's own.
    // nrFrame: the model's frame when it is not target (CacheAsync); target is then always rewritten.
    bool CaptureRefresh(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                        ID3D12Resource* original, const DlssNrCacheInputs& in, ID3D12Resource* nrFrame = nullptr);

    // CacheAsync: the model's answer for an earlier frame lands on this one. composed is that frame composed
    // with the answer, launchFrame that frame as the upscaler wrote it, launchDepth its depth (all three
    // NON_PIXEL_SHADER_RESOURCE, left so). The answer is carried here along the motion chained since, then
    // captured as on a frame the model runs; target (UNORDERED_ACCESS, this frame) is rewritten with it and
    // keep (UNORDERED_ACCESS) receives the untouched frame on the way.
    bool ConsumeAsync(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                      ID3D12Resource* keep, ID3D12Resource* composed, ID3D12Resource* launchFrame,
                      ID3D12Resource* launchDepth, const DlssNrCacheInputs& in);

    // Puts back anything left in a transient state this frame. Every active frame ends with it.
    void EndFrame(ID3D12GraphicsCommandList* cmd);

    // Joint bilateral upsampling of a below-size model's answer to full size. Every input is in
    // NON_PIXEL_SHADER_RESOURCE; the result is returned in the same state, or nullptr on failure.
    // FinishUpsample returns it to rest once the resolve has read it.
    ID3D12Resource* UpsampleModel(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                  ID3D12Resource* fullProxy, ID3D12Resource* smallProxy,
                                  ID3D12Resource* smallModel, bool passthrough, float sigma);
    void FinishUpsample(ID3D12GraphicsCommandList* cmd);

    // Pre-SR: copies the game's render-resolution colour (src, readable as it is handed to DLSS) into dst
    // (UNORDERED_ACCESS, the pass's own texture), which stays in UNORDERED_ACCESS. Nothing of the game's
    // own is written or transitioned.
    bool CopyIn(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, ID3D12Resource* dst);

    // Writes a run of consecutive frames for the offline measurement script. The model runs on every
    // one of them, so each frame has its own ground truth.
    // observe: record what the cache actually shows, frame by frame, without forcing the model -- the
    // way to measure its flicker in the real game rather than in a replay.
    void RequestDump(unsigned int frames, bool observe = false);
    bool DumpActive() const { return _dumpWanted > 0; }

    Status GetStatus() const;

  private:
    struct Retired
    {
        ID3D12Resource* resource = nullptr;
        int framesLeft = 32;
    };

    FrameDescriptorHeap _frameHeaps[DLSSNR_CACHE_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_CACHE_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    static constexpr uint32_t kSrvCount = 22;
    static constexpr uint32_t kUavCount = 9;

    // The frame-sized history, two of each so one is read while the other is written.
    ID3D12Resource* _histEdit[2] = {};
    ID3D12Resource* _histGuide[2] = {};

    // Temporal stabiliser: this frame's edit before it, and the stabilised edits (ping-pong).
    ID3D12Resource* _finalRaw = nullptr;
    ID3D12Resource* _finalHist[2] = {};
    unsigned int _finalCur = 0;
    bool _finalValid = false;
    float _temporal = 0.5f;
    float _lowTemporal = 0.95f;
    bool _softRefresh = false;
    bool _softReveal = true;

    // Anti light pop-in: this frame's bound on the regional edit's step, from the rate and the real frame
    // time (0 is off).
    float _antiPopStep = 0.0f;
    long long _lastBeginTicks = 0;
    double _frameSeconds = 1.0 / 60.0;

    void TemporalPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                      const DlssNrCacheInputs& in, ID3D12Resource* edit);

    // CacheAsync: the background answer carried to this frame, as the capture reads it.
    bool _async = false;
    bool _refreshNext = false;
    ID3D12Resource* _asyncWarp = nullptr;

    // Keyframe crossfade: the model's latest answer, carried alongside what is shown.
    ID3D12Resource* _histTarget[2] = {};
    bool _crossfadeOn = false;
    float _crossfade = 1.0f;
    unsigned int _crossfadeSpan = 1; // this interval's walk length, for a background answer's landing
    unsigned int _cur = 0;
    unsigned int _crossfadeFrames = 0; // the walk's length cap, 0 = the whole interval

    // Anti-ghosting. Allocated only for the switches that need them, released with the rest.
    ID3D12Resource* _histPrint[2] = {};  // the source's fingerprint, carried (ping-pong with the edit)
    ID3D12Resource* _histMeta[2] = {};   // age, validity, staleness, tap validity
    ID3D12Resource* _context = nullptr;  // this frame's surroundings, a quarter of the frame
    ID3D12Resource* _contextPrev = nullptr; // last frame's (the anti pop-in compares the two)

    // Performance. The nearest-surface motion, once per depth texel per frame (every pass used to search
    // the 3x3 for every pixel), kept readable between passes; and mode 10's regional lows at a quarter of
    // the frame (it used to take eighteen taps per pixel).
    ID3D12Resource* _dilatedMv = nullptr;
    bool _dilatedIsSrv = false;
    bool _dilatedReady = false;
    ID3D12Resource* _regNow = nullptr;
    ID3D12Resource* _regPrev = nullptr;
    void DilatePass(ID3D12GraphicsCommandList* cmd, const DlssNrCacheInputs& in);
    unsigned long long _timingLogFrame = 0;
    unsigned long long _contextFrame = 0;
    bool _contextPrevValid = false;
    ID3D12Resource* _guideSum[2] = {};   // guided filter: half-size sums (32-bit: they are differenced)
    ID3D12Resource* _guideCoef[2] = {};  // guided filter: the coefficients a and b
    ID3D12Resource* _guidedOut = nullptr; // guided filter: its edit, for the temporal stabiliser
    ID3D12Resource* _levelGuideCoarse[2] = {}; // same-surface fill: the L2 and L3 guides

    uint32_t _ghostFlags = 0;      // this frame's
    uint32_t _ghostFlagsLast = 0;  // last frame's: a change invalidates the history
    float _printTol = 0.25f;
    float _ageHalfLife = 8.0f;
    float _guidedStrength = 1.0f;
    bool _guidedNow = false;       // the guided filter runs on this frame (cached frames only)

    bool EnsureGhostResources(ID3D12Device* device);
    void GuidedPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                    const DlssNrCacheInputs& in, bool toTemporal);

    // The camera-speed regime (CacheAdaptiveSpeed): the model runs sooner the faster the view moves.
    bool _adaptiveSpeed = false;
    unsigned int _adaptiveMin = 2;
    float _speed = -1.0f;          // pixels per frame, smoothed; negative until measured
    float _printRejected = 0.0f;
    float _staleNow = 0.0f;        // share of the frame without a believed edit, since the model ran

    // Motion priority (0-1): how much sooner the model runs as the camera moves -- 0 is the regime above,
    // 1 drops to the shortest interval at a fifth of the speed. And the GPU budget: the shortest interval
    // whose average cost stays under CacheBudgetMs, from the measured costs.
    float _motionPriority = 0.0f;
    bool _noiseAware = true;
    unsigned int _stillMax = 8;
    bool _stillHold = true;
    float _budgetMs = 0.0f;
    double _costRefresh = 0.0;
    double _costCached = 0.0;
    unsigned int _budgetFloor = 0;
    unsigned int _floorNow = 1;
    unsigned int _speedInterval = 0;
    unsigned int _speedCandidate = 0;
    unsigned int _speedFrames = 0;
    unsigned int SpeedInterval(unsigned int interval);

    // GPU timings of the cache's own passes, for the overlay (CacheTimings via ShowStats). Read back
    // several frames late, like the counters.
    static constexpr unsigned int kStampSlots = 8;
    static constexpr unsigned int kStampsPerFrame = 8; // the start, then one per pass, in order
    ID3D12QueryHeap* _stampHeap = nullptr;
    ID3D12Resource* _stampReadback = nullptr;
    bool _timing = false;
    unsigned int _stampSlot = 0;
    unsigned int _stampCount[kStampSlots] = {};
    int _stampStage[kStampSlots][kStampsPerFrame] = {};
    unsigned long long _stampFrame[kStampSlots] = {};
    bool _stampOpen = false;
    double _stageMs[Status::kStages] = {};
    bool _stageSeen[Status::kStages] = {};

    // The pulse probe: a quarter-size regional light of the edit on screen (this frame's and last frame's), its
    // per-frame sums, and their readback, sorted by frame since the model ran when they come home.
    static constexpr unsigned int kPulseSlots = 8;
    ID3D12Resource* _pulse[2] = {};
    unsigned int _pulseCur = 0;
    bool _pulsePrevValid = false;
    unsigned long long _pulsePrevFrame = 0;
    ID3D12Resource* _pulseStats = nullptr;
    ID3D12Resource* _pulseReadback[kPulseSlots] = {};
    unsigned long long _pulseFrame[kPulseSlots] = {};
    unsigned int _pulsePhase[kPulseSlots] = {};
    unsigned int _pulseSlot = 0;
    double _pulseSum[Status::kPulsePhases] = {};
    double _pulseWeight[Status::kPulsePhases] = {};
    double _pulseDetailSum[Status::kPulsePhases] = {};
    unsigned int _pulseDetailCount[Status::kPulsePhases] = {};
    unsigned int _pulseMeasured = 0;
    float _pulseShownStep[Status::kPulsePhases] = {};
    float _pulseShownDetail[Status::kPulsePhases] = {};
    unsigned int _pulseShownPhases = 0;
    void PulseProbe(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                    const DlssNrCacheInputs& in);
    void ConsumePulse();

    void StampBegin(ID3D12GraphicsCommandList* cmd);
    void Stamp(ID3D12GraphicsCommandList* cmd, int stage);
    void StampEnd(ID3D12GraphicsCommandList* cmd);
    void ConsumeStamps();

    ID3D12Resource* _level[kDlssNrCachePyramidLevels] = {};
    ID3D12Resource* _levelGuide = nullptr;

    // The motion since the model last ran, ping-ponged.
    ID3D12Resource* _accMv[2] = {};
    unsigned int _accCur = 0;
    bool _accReset = true;
    bool _accInModelState = false;
    DXGI_FORMAT _accFormat = DXGI_FORMAT_UNKNOWN;
    unsigned int _accWidth = 0;
    unsigned int _accHeight = 0;

    ID3D12Resource* _stats = nullptr;
    ID3D12Resource* _statsReadback[kDlssNrCacheStatSlots] = {};
    unsigned long long _statsFrame[kDlssNrCacheStatSlots] = {};
    bool _statsPending[kDlssNrCacheStatSlots] = {};
    unsigned int _statsSlot = 0;

    ID3D12Resource* _modelUp = nullptr;

    ID3D12Resource* _dummySrv = nullptr;
    ID3D12Resource* _dummyUav = nullptr;

    // This frame's exposure texture, bound to every pass at t10 (a stand-in when there is none).
    ID3D12Resource* _exposure = nullptr;

    std::vector<Retired> _retired;

    unsigned int _width = 0;
    unsigned int _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;

    bool _historyValid = false;
    unsigned long long _frame = 0;
    unsigned long long _lastRefresh = 0;
    unsigned long long _refreshes = 0;
    unsigned long long _cachedFrames = 0;
    float _cumulativeRejected = 0.0f;
    float _lastRejected = 0.0f;
    const char* _refreshReason = "";

    // This frame's settings, latched in BeginFrame so every pass of one frame agrees.
    float _depthTol = 0.1f;
    float _colourTol = 0.5f;
    float _highDecay = 0.92f;
    float _refreshBlend = 1.0f;
    float _lowGain = 1.0f;
    float _highGain = 1.0f;
    bool _bilateral = true;
    float _stabilize = 0.5f;
    bool _despeckle = true;
    unsigned int _debugView = 0;
    unsigned int _modelHistory = 1;

    // The measurement dump.
    struct DumpFrame
    {
        ID3D12Resource* readback[3] = {}; // frame, model's frame, geometry
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout[3] = {};
        float whitePoint = 1.0f;
        unsigned int sinceRun = 0; // frames since the model ran (0: it ran on this one)
    };

    unsigned int _dumpWanted = 0;
    bool _dumpObserve = false;
    unsigned int _dumpCaptured = 0;
    unsigned long long _dumpWriteAt = 0;
    unsigned int _dumpWritten = 0;
    std::vector<DumpFrame> _dumpFrames;
    ID3D12Resource* _dumpTex[3] = {};

    bool EnsureResources(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format);
    void ReleaseAll(bool immediately);
    void Park(ID3D12Resource*& res);
    void TickRetired();

    void Accumulate(ID3D12GraphicsCommandList* cmd, const DlssNrCacheInputs& in);
    bool EnsureAccumulator(ID3D12Device* device, ID3D12Resource* motion);
    void BuildCoarseLevels(ID3D12GraphicsCommandList* cmd);
    void ContextPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* frame, const DlssNrCacheInputs& in);
    void ApplyPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                   const DlssNrCacheInputs& in);

    void ConsumeStats();
    unsigned int EffectiveInterval(unsigned int interval, bool adaptive, float threshold);

    float _motion = 0.0f;
    unsigned int _intervalNow = 1;
    int _regime = 1;
    int _regimeCandidate = 1;
    unsigned int _regimeFrames = 0;

    void DumpRecord(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                    ID3D12Resource* original, const DlssNrCacheInputs& in);
    void DumpWrite();
    void DumpRelease();

    DlssNrCacheConstants BaseConstants(const DlssNrCacheInputs& in) const;

    // One dispatch. Null slots get a stand-in so every descriptor in the table is valid.
    bool Pass(ID3D12GraphicsCommandList* cmd, const DlssNrCacheConstants& constants,
              ID3D12Resource* const (&srv)[kSrvCount], ID3D12Resource* const (&uav)[kUavCount],
              unsigned int groupsX, unsigned int groupsY);
};
