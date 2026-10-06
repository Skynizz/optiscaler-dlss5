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
    };

    explicit DlssNrEditCache_Dx12(ID3D12Device* device);
    ~DlssNrEditCache_Dx12();

    // Decides this frame. True means the model must run (a refresh); false means a cached frame.
    // Call once per frame while the cache is active, before anything else here.
    // preSr: the frame is the game's jittered render, before its upscaler (see BeginFrame).
    bool BeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width, unsigned int height,
                    DXGI_FORMAT format, bool reset, bool preSr = false);

    // Forget the history: the next active frame is a refresh.
    void Invalidate();

    // A cached frame: target (UNORDERED_ACCESS, holding the upscaler's frame) is rewritten as that frame
    // times the carried edit. keep (UNORDERED_ACCESS) receives the untouched frame on the way.
    bool RunCached(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                   ID3D12Resource* keep, const DlssNrCacheInputs& in);

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
    bool CaptureRefresh(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                        ID3D12Resource* original, const DlssNrCacheInputs& in);

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

    static constexpr uint32_t kSrvCount = 12;
    static constexpr uint32_t kUavCount = 7;

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

    void TemporalPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                      const DlssNrCacheInputs& in);

    // Keyframe crossfade: the model's latest answer, carried alongside what is shown.
    ID3D12Resource* _histTarget[2] = {};
    bool _crossfadeOn = false;
    float _crossfade = 1.0f;
    unsigned int _cur = 0;

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
