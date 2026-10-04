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
    ID3D12Resource* depthSource = nullptr; // the game's own depth, which may carry a stencil plane

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
};

// Spread refresh: one band of this frame, already run through the model and resolved, to merge into the
// carried history. resolved is band-local (row 0 = y0) and in NON_PIXEL_SHADER_RESOURCE.
struct DlssNrCacheBand
{
    ID3D12Resource* resolved = nullptr;
    unsigned int y0 = 0;
    unsigned int height = 0;
    unsigned int feather = 0;
    unsigned int edges = 0; // bit 0 touches the top of the frame, bit 1 the bottom
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
        bool stencilAvailable = false;
        const char* lastRefreshReason = "";
        int regime = 1; // 0 still, 1 moving, 2 fast
        unsigned int dumpWritten = 0;
        bool dumpActive = false;
    };

    explicit DlssNrEditCache_Dx12(ID3D12Device* device);
    ~DlssNrEditCache_Dx12();

    // Decides this frame. True means the model must run (a refresh); false means a cached frame.
    // Call once per frame while the cache is active, before anything else here.
    bool BeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width, unsigned int height,
                    DXGI_FORMAT format, bool reset);

    // Forget the history: the next active frame is a refresh.
    void Invalidate();

    // A cached frame: target (UNORDERED_ACCESS, holding the upscaler's frame) is rewritten as that frame
    // times the carried edit. keep (UNORDERED_ACCESS) receives the untouched frame on the way.
    bool RunCached(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                   ID3D12Resource* keep, const DlssNrCacheInputs& in, const DlssNrCacheBand* band = nullptr);

    // Spread refresh: the model runs on one horizontal band of every frame instead of the whole frame
    // one frame in N, so every frame costs about the same. Latched in BeginFrame.
    bool Spread() const { return _spread; }
    unsigned int Bands() const { return _bands; }
    unsigned int NextBand() { return (unsigned int) (_bandCounter++ % _bands); }

    // Spread refresh, every frame: keeps each band's accumulated motion current.
    void AccumulateBands(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const DlssNrCacheInputs& in);

    // Spread refresh: rows [offsetY, offsetY + height) of depth and of motion (the band's accumulated
    // motion when the model history policy asks for it, else the game's), for the band's model. Both
    // come back in NON_PIXEL_SHADER_RESOURCE; FinishCrop returns them to rest.
    bool CropGuides(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const DlssNrCacheInputs& in,
                    unsigned int band, unsigned int offsetY, unsigned int height, ID3D12Resource** depthOut,
                    ID3D12Resource** motionOut);
    void FinishCrop(ID3D12GraphicsCommandList* cmd);

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

    // Writes a run of consecutive frames for the offline measurement script. The model runs on every
    // one of them, so each frame has its own ground truth.
    void RequestDump(unsigned int frames);
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

    static constexpr uint32_t kSrvCount = 11;
    static constexpr uint32_t kUavCount = 6;

    // The frame-sized history, two of each so one is read while the other is written.
    ID3D12Resource* _histEdit[2] = {};
    ID3D12Resource* _histGuide[2] = {};
    unsigned int _cur = 0;

    ID3D12Resource* _level[kDlssNrCachePyramidLevels] = {};
    ID3D12Resource* _levelGuide = nullptr;

    // One accumulator per band (slot 0 is the whole frame's when not spreading), two textures each.
    ID3D12Resource* _accMv[kDlssNrCacheMaxBands][2] = {};
    unsigned int _accCur[kDlssNrCacheMaxBands] = {};
    bool _accReset[kDlssNrCacheMaxBands] = { true, true, true, true };
    bool _accInModelState = false;
    DXGI_FORMAT _accFormat = DXGI_FORMAT_UNKNOWN;
    unsigned int _accWidth = 0;
    unsigned int _accHeight = 0;

    ID3D12Resource* _stats = nullptr;
    ID3D12Resource* _statsReadback[kDlssNrCacheStatSlots] = {};
    unsigned long long _statsFrame[kDlssNrCacheStatSlots] = {};
    bool _statsPending[kDlssNrCacheStatSlots] = {};
    unsigned int _statsSlot = 0;

    ID3D12Resource* _stencilClone = nullptr;
    bool _stencilBound = false;

    ID3D12Resource* _modelUp = nullptr;

    ID3D12Resource* _bandDepth = nullptr;
    ID3D12Resource* _bandMotion = nullptr;
    bool _cropInUse = false;

    bool _spread = false;
    unsigned int _bands = 2;
    unsigned long long _bandCounter = 0;

    ID3D12Resource* _dummySrv = nullptr;
    ID3D12Resource* _dummyUav = nullptr;
    ID3D12Resource* _dummyStencil = nullptr;

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
    bool _stencilWanted = false;   // read the plane at all: priority on, or the stencil debug view
    bool _stencilPriority = false; // treat matching pixels as priority
    unsigned int _stencilMask = 0;
    unsigned int _stencilRef = 0;

    // The measurement dump.
    struct DumpFrame
    {
        ID3D12Resource* readback[3] = {}; // frame, model's frame, geometry
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout[3] = {};
        float whitePoint = 1.0f;
    };

    unsigned int _dumpWanted = 0;
    unsigned int _dumpCaptured = 0;
    unsigned long long _dumpWriteAt = 0;
    unsigned int _dumpWritten = 0;
    std::vector<DumpFrame> _dumpFrames;
    ID3D12Resource* _dumpTex[3] = {};

    bool EnsureResources(ID3D12Device* device, unsigned int width, unsigned int height, DXGI_FORMAT format);
    void ReleaseAll(bool immediately);
    void Park(ID3D12Resource*& res);
    void TickRetired();

    void PrepareStencil(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, const DlssNrCacheInputs& in);
    void RestoreStencil(ID3D12GraphicsCommandList* cmd);

    void Accumulate(ID3D12GraphicsCommandList* cmd, const DlssNrCacheInputs& in, unsigned int slot);
    bool EnsureAccumulatorSlot(ID3D12Device* device, ID3D12Resource* motion, unsigned int slot);
    void BuildCoarseLevels(ID3D12GraphicsCommandList* cmd);
    void ApplyPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target, ID3D12Resource* original,
                   const DlssNrCacheInputs& in);

    void ConsumeStats();
    unsigned int EffectiveInterval(unsigned int interval, bool adaptive, float threshold);

    float _motion = 0.0f;
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
