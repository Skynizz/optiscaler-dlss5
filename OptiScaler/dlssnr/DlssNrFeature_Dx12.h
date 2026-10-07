#pragma once

#include <d3d12.h>

#include <optional>
#include <string>

#include <shaders/dlssnr/DlssNr_Common.h>
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;

namespace DlssNr
{
// The model runs immediately after the game's upscaler, before the interface is drawn. It is shown a
// display-referred proxy of that frame -- the sort of picture it was trained on -- and its answer is
// composed back over the untouched original.
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// timingQueue is the queue this command list will be executed on, when the caller knows it.
// State::currentCommandQueue only exists once a D3D12 swapchain has been created, which a Vulkan
// game never does -- so without this the pass runs and never reports what it cost.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr);

// Pre-SR placement (DlssNrPreSr): called just before the game's DLSS Super Resolution evaluate. Runs the
// pass on a copy of the game's render-resolution colour and swaps that copy into the parameter block,
// so the upscaler upscales the enhanced frame. True when it swapped; EndBeforeUpscale must then be
// called with the same block once the upscaler has been evaluated, to put the game's own colour back.
// EvaluateAfterUpscale is still called after the upscaler as usual and knows not to run the model again.
bool EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue = nullptr);
void EndBeforeUpscale(NVSDK_NGX_Parameter* params);

// Hot-swap comparison: what Neural Rendering runs as, live, without touching any setting. Cycled by the
// comparison key (DlssNrCompareKey) or chosen in the menu.
enum class CompareMode : int
{
    Yours = 0,   // the saved settings
    Vanilla = 1, // as OptiScaler ships it: the model every frame, full size, after the upscaler
    Off = 2
};

CompareMode GetCompareMode();
void SetCompareMode(CompareMode mode);
void CycleCompareMode();
const char* CompareModeName(CompareMode mode);
double SecondsSinceCompareSwitch();

// What is running right now, for the line on screen and the menu.
struct LiveStats
{
    bool valid = false;
    bool enabled = false;
    bool preSr = false;
    bool cache = false;
    bool benchmark = false;
    const char* mode = "";
    CompareMode compare = CompareMode::Yours;
    double renderedFps = 0.0; // frames the game renders per second -- frame generation excluded
    double nrMs = 0.0;        // the pass on the GPU, averaged
    unsigned int modelWidth = 0;
    unsigned int modelHeight = 0;
    unsigned int passes = 1;  // model passes run on the last frame the model ran
    unsigned int style = 0;   // 0 default, 1 natural, 2 cinematic
};

LiveStats GetLiveStats();

// The line on screen, drawn by the menu every frame it is wanted.
bool OverlayWanted();
void RenderOverlay(float alpha);



// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.




// The settings panel, drawn inside OptiScaler's menu.
void RenderMenu(::Config* config, float menuResScale);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();


// Asks the model whether it will work on Direct3D 11 at all, once, and logs the answer.
//
// The bridge exists because of a claim nobody tested: "the model refuses on DX11, it answers
// FeatureNotSupported". Nothing in this project has ever called the snippet's own D3D11 entry points
// -- it exports ten of them, implemented in ngx_d3d11.cpp and sharing CreateFeatureCommon and
// EvaluateFeatureCommon with the D3D12 path. Nothing is created and nothing changes; it resolves the
// entry points and initialises on the game's own device, which is where a refusal would appear.
void ProbeD3D11(void* d3d11Device);

// What scale this game's buffer is on, measured from the untouched copy of each frame.
//
// A suggestion only. Nothing applies it: the menu shows it and the user takes it or does not, which
// keeps the number visible and adjustable rather than a value that moved on its own. Confidence is
// how settled recent readings are -- 1 means they agree, 0 means the scene is changing under the
// measurement and no single value would serve.
struct CalibrationReading
{
    float suggestion = 0.0f;

    // How much recent readings agree. This is steadiness, not correctness: a frozen frame agrees with
    // itself perfectly, so a loading screen scores full marks for a number that means nothing. Read it
    // together with usable.
    float steadiness = 0.0f;

    unsigned long long samples = 0;

    // Whether the scene is worth measuring at all. False when the frame is already tone mapped -- the
    // divisor does nothing there and the reading would be a meaningless 0.9 -- or when too little of
    // the picture is lit to say where the top of the range is. A dark cave gives a small number very
    // steadily, which is the trap this exists to close.
    bool usable = false;
    const char* why = "";
};

CalibrationReading Calibration();

// Whether the model is loaded and running, for the overlay.
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// What the game offers by way of exposure. Observed every frame whether or not the setting is on, so
// the menu can say whether turning it on would do anything here.
struct ExposureStatus
{
    unsigned long long seenFrames = 0;   // evaluates observed; 0 means nothing has run yet
    bool offeredNow = false;             // a texture on the most recent frame
    bool everOffered = false;            // a texture on any frame so far
    float exposure = 0.0f;               // last value read back, 0 if none
    float preExposure = 1.0f;
    bool unreliable = false;             // it swung too far to be an exposure, and is being ignored
};

ExposureStatus GameExposureStatus();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();

// The automatic white point (source 3): the smoothed value in use, and the last raw reading. 0 until
// the first reading has come back.
float AutoWhitePoint();
float AutoWhiteMeasured();

// What the white point meter last settled on, or 0 when it is not running. For the menu.


// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

// The temporal edit cache, for the menu. D3D12 only.
struct CacheStatus
{
    bool exists = false;       // created at all (it is, the first time it is switched on)
    bool historyValid = false; // holding an edit to carry
    unsigned long long refreshes = 0;
    unsigned long long cached = 0;
    unsigned int framesSinceRefresh = 0;
    float lastRejected = 0.0f;
    float cumulativeRejected = 0.0f;
    const char* lastRefreshReason = "";
    int regime = 1; // 0 still, 1 moving, 2 fast
    unsigned int dumpWritten = 0;
    bool dumpActive = false;

    // The whole pass's cost averaged over recent frames, cache on or off.
    double averageMs = 0.0;

    // Anti-ghosting, and the cadence it follows.
    unsigned int ghostFlags = 0;  // DlssNrCacheGhostFlag, as running
    unsigned int intervalNow = 1; // frames between model runs right now
    float printRejected = 0.0f;   // the last frame's share rejected by the fingerprint
    float speed = 0.0f;           // camera motion, pixels per frame
    unsigned int budgetFloor = 0; // the shortest interval the GPU budget allows (0: no budget)
    double costRefresh = 0.0;     // the pass on a frame the model runs, ms
    double costCached = 0.0;      // the pass on a cached frame, ms

    // GPU time of the cache's own passes (ms, -1 when not measured: only while ShowStats is on).
    static constexpr int kStages = 7;
    double stageMs[kStages] = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0 };
    const char* stageName[kStages] = {};
};

CacheStatus GetCacheStatus();

// Writes CacheDumpFrames consecutive frames (frame, model's frame, motion, depth) for
// tools/dlssnr_cache/measure_reprojection.py. The model runs on every one of them.
void RequestCacheDump();

// The A/B benchmark: Neural Rendering off (optional), as OptiScaler ships it, with the user's settings
// and (optional) those settings before the upscaler, on the same scene one after the other. Rendered
// frames only -- frame generation is excluded on purpose. Each phase ends with a capture: a picture and
// a flicker figure. The page goes to dlssnr-benchmark/<date>/rapport.html beside OptiScaler, and
// dlssnr-benchmark.html always opens the latest.
// off, vanilla, yours, yours at the other placement, yours in styles Default / Natural / Cinematic,
// yours with the other pass count (2 if yours run one, else 1)
constexpr int kBenchmarkPhases = 8;

struct BenchmarkResult
{
    bool valid = false;
    double fps = 0.0;
    double low1 = 0.0;    // 1% low
    double frameMs = 0.0;
    double nrMs = 0.0;    // the Neural Rendering pass on the GPU, averaged
    unsigned int frames = 0;

    bool flickerValid = false;
    float flickerMean = 0.0f; // mean frame-to-frame brightness change, percent
    float flickerP95 = 0.0f;
    bool picture = false;
    unsigned int shotWidth = 0;
    unsigned int shotHeight = 0;

    // Frame pacing: how much the frame time changes from one frame to the next (a model run one frame in
    // two shows here, not in the average), mean and 99th percentile, ms.
    double pacingMs = 0.0;
    double pacingP99 = 0.0;

    // Where the pass actually ran during the phase: 0 nowhere, 1 after the upscaler, 2 before it. Pre-SR
    // falls back to after the upscaler when the game calls Ray Reconstruction rather than Super Resolution.
    int placement = 0;
    bool preSrFellBack = false;

    // The edit cache during the phase: the share of frames the model ran (-1 without the cache) and the
    // share of the frame without a believed edit at the end (-1 without the cache).
    float modelShare = -1.0f;
    float rejected = -1.0f;
};

struct BenchmarkStatus
{
    bool active = false;
    int phase = 0;
    int step = 0;
    int steps = 0;
    bool capturing = false;
    float phaseProgress = 0.0f;
    bool warmingUp = false;
    BenchmarkResult results[kBenchmarkPhases];
    std::string report; // the page of the last finished run, empty until there is one
};

void StartBenchmark(bool includeOff, bool includeOtherPlacement, bool captures, bool includeStyles = false,
                    bool includePasses = false);
void CancelBenchmark();
BenchmarkStatus GetBenchmarkStatus();
const char* BenchmarkPhaseName(int phase);

void Shutdown();
} // namespace DlssNr
