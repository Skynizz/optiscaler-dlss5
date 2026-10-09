#include "pch.h"

#include <set>

#include <dlssnr/DlssNr.h>


#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include "DlssNr_Dx12.h"
#include "DlssNr_EditCache_Dx12.h"
#include "DlssNr_Shot_Dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>

#include <mutex>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include "precompile/DlssNr_Shader.h"
#include "../output_scaling/OS_Dx12.h"

namespace
{
// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00003: return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004: return "FAIL_FeatureNotFound";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00006: return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007: return "FAIL_NotInitialized";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD0000F: return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    case 0xBAD00012: return "FAIL_NotImplemented";
    default: return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control =
        (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})",
             result, NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright =
        result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*) (ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*,
                                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetExtras = void(__cdecl*) (void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                        unsigned int, unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.

struct NrState
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // CacheAsync: the feature was built on our compute queue, and is evaluated there and nowhere else.
    bool featureOnCompute = false;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to.
    void* passFeature[4] = {};

    // Multi-pass: each extra pass's first evaluate resets its history, and the copy of the previous
    // pass's answer the next pass reads (the model cannot read and write one resource).
    bool passReset[4] = { true, true, true, true };
    ID3D12Resource* passIn = nullptr;
    unsigned int passesRun = 1;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    bool meterExposureValid[4] = {};
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // The automatic white point (source 3): the scene's average luminance, measured on the frame as the
    // upscaler wrote it -- before this pass touches it, so nothing it writes can feed back into what it
    // measures -- and smoothed in log space. A fixed paper white cannot serve a bright exterior and a
    // dark interior at once: high enough for the first, it shows the model a black picture in the
    // second, and the model answers a black picture with speckle that pops on and off.
    bool autoWhiteValid = false;
    float autoWhiteLog = 0.0f;      // log2 of the smoothed white point
    float autoWhiteMeasured = 0.0f; // the last raw reading, for the menu
    LARGE_INTEGER autoWhiteLast {};
    bool autoWhiteSnap = true;

    // How long the scene has read far darker than the value in use: a fade, a loading screen, a menu --
    // or a genuinely dark place, if it lasts.
    double autoWhiteDarkFor = 0.0;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // Whether the game's exposure can be believed at all. Some games hand DLSS a texture under that name
    // that is not a stable exposure: Control's moved from 0.00025 to 116 within seconds, which put the
    // white point at the 0.01 floor one moment and the 4096 ceiling the next. The model was then shown
    // a black frame, then a blown one, and answered with black patches popping on and off. Once seen, it
    // is latched for the session and the paper white slider takes over.
    bool exposureUnreliable = false;
    float exposureLogWhite[120] = {};
    unsigned int exposureReadings = 0;
    unsigned int exposureOutOfRange = 0;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";
};

NrState g_nr;
std::unique_ptr<DlssNr_Dx12> g_compose;

// The temporal edit cache, and the joint bilateral upsampler that shares its shader. Created on first
// use only -- with both options off nothing of it exists.
std::unique_ptr<DlssNrEditCache_Dx12> g_cache;

// CacheAsync: the model on a compute queue of our own, in parallel with the game's frame. See AsyncLaunch for
// the frame-by-frame flow. Nothing of it exists until it is switched on.
struct AsyncState
{
    static constexpr unsigned int kAllocators = 3;
    static constexpr unsigned int kStampSlots = 4;

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc[kAllocators] = {};
    UINT64 allocDone[kAllocators] = {}; // the run after which each allocator is free again
    unsigned int allocCur = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* inputsReady = nullptr; // signalled on the game's queue after the launch frame
    ID3D12Fence* modelDone = nullptr;   // signalled on ours after the model
    UINT64 inputsValue = 0;
    UINT64 doneValue = 0;
    ID3D12CommandQueue* gameQueue = nullptr;

    ID3D12Resource* output = nullptr;   // the model's answer, at the working size
    ID3D12Resource* depth = nullptr;    // the launch frame's guides, as the model and the landing read them
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* keep = nullptr;     // the cache's untouched frame, while hdrCopy holds the launch frame
    ID3D12Resource* composed = nullptr; // the launch frame composed with the answer, at the landing

    // The run in flight: 0 none, 1 recorded (sent to our queue at the next frame), 2 on our queue.
    int phase = 0;
    unsigned long long launchFrame = 0;
    UINT64 job = 0;
    bool held = false;                   // the launch frame's proxy and frame kept readable until the landing
    ID3D12Resource* heldInput = nullptr; // what the model reads: colorSmall, or colorCopy at full size
    bool reduced = false;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int workW = 0;
    unsigned int workH = 0;
    float whitePoint = 1.0f;

    // The model's time on our queue.
    ID3D12QueryHeap* stamps = nullptr;
    ID3D12Resource* stampsBack = nullptr;
    UINT64 stampJob[kStampSlots] = {};
    unsigned int stampSlot = 0;
    double modelMs = 0.0;

    unsigned long long launches = 0;
    unsigned long long landed = 0;
    bool active = false;  // on this frame
    bool broken = false;  // something refused: the model runs in step for the rest of the session
    const char* why = ""; // why it is not running, when it is asked for
};

AsyncState g_async;

// A run launched on frame N lands on N + 2: the model runs alongside N + 1.
constexpr unsigned long long kAsyncLatency = 2;

// The pass's cost on a landing frame, for the GPU budget.
double g_costLand = 0.0;

// The pass's cost averaged over frames, because with the cache on consecutive frames cost very
// different amounts and the last reading alone says little.
double g_avgGpuTime = 0.0;

// The pass's cost by kind of frame, for the edit cache's GPU budget: a frame the model ran on under the
// cache (1) and a cached frame (2); 0 is a frame without the cache. The pass timer reads a frame two
// starts late from a ring of three, so the kind travels with its slot.
double g_costRefresh = 0.0;
double g_costCached = 0.0;
int g_frameKind = 0;
int g_timeKind[3] = {};
unsigned int g_timeStarts = 0;

// What the pass runs as right now. The saved settings, unless the comparison key or the benchmark says
// otherwise for the moment; neither ever writes a setting, so nothing of this can end up in the ini.
//   0 your settings, 1 as OptiScaler ships it (the model every frame, full size, after the upscaler),
//   2 off, 3 your settings at the other placement (the benchmark's last phase: before the upscaler if
//   yours run after it, after it if yours run before)
int g_userCompare = 0;
int g_benchCompare = -1;
constexpr int kCompareOtherPlacement = 3;

int ActiveCompare() { return g_benchCompare >= 0 ? g_benchCompare : g_userCompare; }

bool EffEnabled(const Config& cfg)
{
    const int m = ActiveCompare();

    if (m == 2)
        return false;

    return m == 1 || m == kCompareOtherPlacement || cfg.DlssNrEnabled.value_or_default();
}

bool EffPreSr(const Config& cfg)
{
    const int m = ActiveCompare();
    const bool saved = cfg.DlssNrPreSr.value_or_default();
    return m != 1 && (m == kCompareOtherPlacement ? !saved : saved);
}

bool EffCache(const Config& cfg) { return ActiveCompare() != 1 && cfg.DlssNrCacheEnabled.value_or_default(); }

float EffWorkScale(const Config& cfg) { return ActiveCompare() == 1 ? 1.0f : cfg.DlssNrWorkingScale.value_or_default(); }

bool EffJbu(const Config& cfg) { return ActiveCompare() != 1 && cfg.DlssNrJbuUpsample.value_or_default(); }

// The benchmark's style and pass-count phases override these two for the length of a phase.
int g_benchStyle = -1;
int g_benchPasses = -1;

// The model's style: the user's, everywhere, vanilla included -- it is a look, not an optimisation.
uint32_t EffStyle(const Config& cfg)
{
    return g_benchStyle >= 0 ? (uint32_t) g_benchStyle : std::min(cfg.DlssNrStyle.value_or_default(), 2u);
}

// How many times the model runs on a frame it runs on. Vanilla is one pass, as OptiScaler ships it.
unsigned int EffPasses(const Config& cfg)
{
    if (ActiveCompare() == 1)
        return 1;

    const unsigned int p = g_benchPasses > 0 ? (unsigned int) g_benchPasses : cfg.DlssNrPasses.value_or_default();
    return std::clamp(p, 1u, 3u);
}

// The state the upscaler leaves its output in, and every pass here hands it back in.
D3D12_RESOURCE_STATES OutputRestState()
{
    return Config::Instance()->OutputResourceBarrier.has_value()
               ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
               : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

// Pre-SR: the pass runs on a copy of the game's render-resolution colour, and the upscaler is handed the
// copy instead. The game's own colour is never written.
struct PreSrState
{
    ID3D12Resource* tex = nullptr; // the copy the pass rewrites; the upscaler reads it
    D3D12_RESOURCE_STATES texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* original = nullptr; // the game's colour, put back after the upscaler
    bool typed = true;                  // which slot of the parameter block it came through
    bool swapped = false;
    bool ranThisFrame = false;
    bool wasOn = false;
};

PreSrState g_preSr;

// Set around the pre-SR dispatch, so the pass knows its target is the copy rather than the output.
bool g_preSrDispatch = false;

// Frames the pass ran before and after the upscaler, for the benchmark's "where it really ran".
unsigned long long g_passBefore = 0;
unsigned long long g_passAfter = 0;

// The last pre-SR frame's jitter, for the change the cache moves its edit by; and which way the game's
// jitter runs against the image (+1 the sample sits at the pixel centre plus the jitter, -1 minus it,
// 0 no compensation). -1 measured steadiest in Control (flicker 0.47% against 0.54% uncompensated and
// 0.59% the other way); settable with JitterSign in dlssnr-set.txt for testing.
float g_lastJitterX = 0.0f;
float g_lastJitterY = 0.0f;
bool g_jitterValid = false;
int g_jitterSign = -1;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;

// A second timer, around the model's evaluate and nothing else.
//
// The first one brackets the whole pass, which is the number the menu shows and the right one for
// "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
// full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
// encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
// resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
// work to NGX overhead.
//
// Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
// we can actually do something about.
std::unique_ptr<GpuTime_Dx12> g_ngxTime;
std::optional<double> g_lastNgxTime;
std::optional<double> g_lastGpuTime;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
// Set by dlssnr-asyncprobe.trigger: the next frame the model runs also runs RunAsyncProbe.
bool g_asyncProbeWanted = false;

void CheckCaptureTrigger()
{
    // Every rendered frame reaches here, the pass on or off, so it keeps its own count.
    static unsigned long long calls = 0;

    if ((calls++ % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }

    // The async compute probe (RunAsyncProbe), the same way: run once on the next frame the model runs.
    const auto asyncTrigger = Util::DllPath().remove_filename() / "dlssnr-asyncprobe.trigger";

    if (std::filesystem::exists(asyncTrigger, ec))
    {
        std::filesystem::remove(asyncTrigger, ec);
        g_asyncProbeWanted = true;
        LOG_INFO("DLSS-NR async probe requested by trigger file");
    }

    // The edit cache's measurement dump, the same way.
    const auto cacheTrigger = Util::DllPath().remove_filename() / "dlssnr-cachedump.trigger";

    if (std::filesystem::exists(cacheTrigger, ec))
    {
        std::filesystem::remove(cacheTrigger, ec);
        DlssNr::RequestCacheDump();
        LOG_INFO("DLSS-NR edit cache dump requested by trigger file");
    }

    // An observation dump: what the cache shows, every frame, model not forced.
    const auto observeTrigger = Util::DllPath().remove_filename() / "dlssnr-cacheobserve.trigger";

    if (std::filesystem::exists(observeTrigger, ec))
    {
        std::filesystem::remove(observeTrigger, ec);

        if (g_cache != nullptr)
            g_cache->RequestDump(Config::Instance()->DlssNrCacheDumpFrames.value_or_default(), true);

        LOG_INFO("DLSS-NR edit cache observation dump requested by trigger file");
    }

    // Live settings, so a setting can be changed and compared without restarting the game: a file named
    // dlssnr-set.txt beside OptiScaler holding Key=Value lines (the [DlssNr] key names). Assigned, not
    // re-read -- OptiScaler's config reload keeps any value it already holds, so a reload changes nothing.
    const auto setFile = Util::DllPath().remove_filename() / "dlssnr-set.txt";

    if (std::filesystem::exists(setFile, ec))
    {
        Config* c = Config::Instance();
        FILE* f = _wfopen(setFile.wstring().c_str(), L"r");
        char line[256];

        while (f != nullptr && fgets(line, sizeof(line), f) != nullptr)
        {
            char key[128] = {};
            char value[64] = {};

            if (sscanf(line, " %127[^= ] = %63s", key, value) != 2)
                continue;

            const std::string k(key);
            const float v = (float) atof(value);
            const bool b = v != 0.0f || std::string(value) == "true";

            if (k == "CacheEnabled") c->DlssNrCacheEnabled = b;
            else if (k == "CacheInterval") c->DlssNrCacheInterval = (uint32_t) v;
            else if (k == "CacheAdaptive") c->DlssNrCacheAdaptive = b;
            else if (k == "CacheAdaptiveThreshold") c->DlssNrCacheAdaptiveThreshold = v;
            else if (k == "CacheDepthTolerance") c->DlssNrCacheDepthTolerance = v;
            else if (k == "CacheColourTolerance") c->DlssNrCacheColourTolerance = v;
            else if (k == "CacheHighDecay") c->DlssNrCacheHighDecay = v;
            else if (k == "CacheRefreshBlend") c->DlssNrCacheRefreshBlend = v;
            else if (k == "CacheStabilize") c->DlssNrCacheStabilize = v;
            else if (k == "CacheDespeckle") c->DlssNrCacheDespeckle = b;
            else if (k == "CacheCrossfade") c->DlssNrCacheCrossfade = b;
            else if (k == "CacheTemporal") c->DlssNrCacheTemporal = v;
            else if (k == "CacheLowTemporal") c->DlssNrCacheLowTemporal = v;
            else if (k == "CacheSoftRefresh") c->DlssNrCacheSoftRefresh = b;
            else if (k == "CacheDebugView") c->DlssNrCacheDebugView = (uint32_t) v;
            else if (k == "CacheModelHistory") c->DlssNrCacheModelHistory = (uint32_t) v;
            else if (k == "CacheDumpFrames") c->DlssNrCacheDumpFrames = (uint32_t) v;
            else if (k == "WhitePointSource") c->DlssNrWhitePointSource = (uint32_t) v;
            else if (k == "WhitePointScale") c->DlssNrWhitePointScale = v;
            else if (k == "WhitePointTrim") c->DlssNrWhitePointTrim = v;
            else if (k == "PreSr") c->DlssNrPreSr = b;
            else if (k == "Passes") c->DlssNrPasses = (uint32_t) v;
            else if (k == "Style") c->DlssNrStyle = (uint32_t) v;
            else if (k == "JitterSign") g_jitterSign = (int) v;
            else if (k == "ShowStats") c->DlssNrShowStats = b;
            else if (k == "Compare") DlssNr::SetCompareMode((DlssNr::CompareMode) (int) v);
            else if (k == "WorkingScale") c->DlssNrWorkingScale = v;
            else if (k == "JbuUpsample") c->DlssNrJbuUpsample = b;
            else if (k == "Enabled") c->DlssNrEnabled = b;
            else if (k == "CacheNoiseAware") c->DlssNrCacheNoiseAware = b;
            else if (k == "CacheAsync") c->DlssNrCacheAsync = b;
            else if (k == "CacheFingerprint") c->DlssNrCacheFingerprint = b;
            else if (k == "CacheContext") c->DlssNrCacheContext = b;
            else if (k == "CacheFingerprintTolerance") c->DlssNrCacheFingerprintTolerance = v;
            else if (k == "CacheSurfaceFill") c->DlssNrCacheSurfaceFill = b;
            else if (k == "CacheCrossfadeFrames") c->DlssNrCacheCrossfadeFrames = (uint32_t) v;
            else if (k == "CacheGuided") c->DlssNrCacheGuided = b;
            else if (k == "CacheAging") c->DlssNrCacheAging = b;
            else if (k == "CacheAdaptiveSpeed") c->DlssNrCacheAdaptiveSpeed = b;
            else if (k == "CacheAdaptiveMin") c->DlssNrCacheAdaptiveMin = (uint32_t) v;
            else if (k == "CacheMotionPriority") c->DlssNrCacheMotionPriority = v;
            else if (k == "CacheBudgetMs") c->DlssNrCacheBudgetMs = v;
            else if (k == "CacheStillMax") c->DlssNrCacheStillMax = (uint32_t) v;
            else if (k == "CacheAntiPop") c->DlssNrCacheAntiPop = b;
            else if (k == "CacheAntiPopRate") c->DlssNrCacheAntiPopRate = v;
            else
            {
                LOG_WARN("DLSS-NR dlssnr-set.txt: unknown key {}", k);
                continue;
            }

            LOG_INFO("DLSS-NR live setting {} = {}", k, value);
        }

        if (f != nullptr)
            fclose(f);

        std::filesystem::remove(setFile, ec);
    }

    // And the FPS comparison, so a test can be run without opening the menu.
    const auto benchTrigger = Util::DllPath().remove_filename() / "dlssnr-benchmark.trigger";

    if (std::filesystem::exists(benchTrigger, ec))
    {
        // Its content may ask for the extra phases: "styles" and/or "passes".
        std::string wants;

        if (FILE* tf = _wfopen(benchTrigger.wstring().c_str(), L"r"))
        {
            char buf[128] = {};
            fread(buf, 1, sizeof(buf) - 1, tf);
            fclose(tf);
            wants = buf;
        }

        std::filesystem::remove(benchTrigger, ec);
        DlssNr::StartBenchmark(true, wants.find("noother") == std::string::npos, true,
                               wants.find("styles") != std::string::npos, wants.find("passes") != std::string::npos);
        LOG_INFO("DLSS-NR benchmark requested by trigger file");
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf",         "Balanced",    "MaxQuality",
                                    "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = 32;
};

std::vector<NrRetired> g_nrRetired;

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (--g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (g_nrRetired[i].feature != nullptr && g_nr.release != nullptr)
            g_nr.release(g_nrRetired[i].feature);

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
{
    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
             (int) g_nr.output->GetDesc().Format, (int) needed);

    ForgetCalibration();

    ParkNrFeature(g_nr.feature);

    // The extras go with it: they were built for this raster and this tuning too.
    for (void*& f : g_nr.passFeature)
        ParkNrFeature(f);

    for (ID3D12Resource** r :
         { &g_nr.output, &g_nr.passIn, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall })
        ParkNrResource(*r);

    g_nr.reset = true;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
// texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                         bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureValid[slot] = exposureBound;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough  ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureValid[slot] && std::isfinite(src[0]) && src[0] > 0.0f)
    {
        g_nr.gameExposure = src[0];

        // The white point this exposure implies, judged for plausibility. A real exposure moves a few
        // stops between a cave and daylight and never asks for a white point near the clamps; a value
        // that does either is something else under the exposure's name.
        if (!g_nr.exposureUnreliable)
        {
            const float white = g_nr.gamePreExposure / src[0];
            const unsigned int n = sizeof(g_nr.exposureLogWhite) / sizeof(float);
            g_nr.exposureLogWhite[g_nr.exposureReadings % n] = std::log2(std::max(white, 1e-9f));
            g_nr.exposureReadings++;

            if (white < 0.02f || white > 512.0f)
                g_nr.exposureOutOfRange++;

            const unsigned int have = std::min(g_nr.exposureReadings, n);
            float lo = 1e9f, hi = -1e9f;

            for (unsigned int i = 0; i < have; ++i)
            {
                lo = std::min(lo, g_nr.exposureLogWhite[i]);
                hi = std::max(hi, g_nr.exposureLogWhite[i]);
            }

            if (g_nr.exposureOutOfRange >= 3 || (have >= 30 && hi - lo > 8.0f))
            {
                g_nr.exposureUnreliable = true;
                LOG_WARN("DLSS-NR: the game's exposure is not a usable exposure (white point {:.4f}..{:.1f}, {} "
                         "readings out of range) -- ignoring it for this session, paper white is used instead",
                         std::exp2(lo), std::exp2(hi), g_nr.exposureOutOfRange);
            }
        }
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Reads the tile grid written four frames ago and moves the automatic white point toward it.
//
// The statistic is the geometric mean of tile means with the darkest and brightest 5% of tiles left
// out: a sky or a lamp cannot decide it, nor can a black HUD bar. The white point then puts that
// average where the encode wants a mid-grey -- the same target the proxy has always aimed at.
void ConsumeAutoWhite(bool snap)
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;
    std::vector<float> logs;
    logs.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    // Tile 0 carries the exposure courier's value, not a tile mean; it is skipped.
    for (unsigned int i = 1; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-12f)
            logs.push_back(std::log2(src[i]));
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (logs.size() < 64)
        return;

    const size_t lo = logs.size() / 20;
    const size_t hi = logs.size() - 1 - logs.size() / 20;
    std::nth_element(logs.begin(), logs.begin() + lo, logs.end());
    const float lowCut = logs[lo];
    std::nth_element(logs.begin(), logs.begin() + hi, logs.end());
    const float highCut = logs[hi];

    double sum = 0.0;

    for (float v : logs)
        sum += std::clamp(v, lowCut, highCut);

    const float meanLuma = std::exp2((float) (sum / logs.size()));

    // WhitePointForMean caps at 10000 for its own purposes; the auto path needs the room some games
    // ask for (Control wants thousands in daylight).
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float target = std::clamp(meanLuma / (encoded / (1.0f - encoded)), 0.01f, 65536.0f);
    const float targetLog = std::log2(target);
    g_nr.autoWhiteMeasured = target;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Only the very first reading is taken whole. A reset used to snap too, but a reset is where the
    // fades are: the game cuts to black, the white point snapped to the black, and the next scene was
    // shown to the model blown out while the value climbed back -- a flash at every cut.
    (void) snap;

    if (!g_nr.autoWhiteValid || g_nr.autoWhiteSnap)
    {
        if (!g_nr.autoWhiteValid)
            g_nr.autoWhiteLog = targetLog;

        g_nr.autoWhiteValid = true;
        g_nr.autoWhiteSnap = false;
        g_nr.autoWhiteDarkFor = 0.0;
        g_nr.autoWhiteLast = now;
        return;
    }

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double dt = std::clamp((double) (now.QuadPart - g_nr.autoWhiteLast.QuadPart) / f.QuadPart, 0.0, 0.25);
    g_nr.autoWhiteLast = now;

    // A scene suddenly five stops or more darker than the value in use is a fade, a loading screen or a
    // menu far more often than a place: the value is held through it, and only follows once the dark
    // has lasted two seconds (a real cave, a night interior).
    if (targetLog < g_nr.autoWhiteLog - 5.0f)
    {
        g_nr.autoWhiteDarkFor += dt;

        if (g_nr.autoWhiteDarkFor < 2.0)
            return;
    }
    else
    {
        g_nr.autoWhiteDarkFor = 0.0;
    }

    // An eye adapting, not a meter jumping, and slower than it was (0.4 s): the white point decides what
    // the model is shown for the whole frame, so every lunge of it -- the sky panning into view -- made
    // the model's whole answer pump. About a second toward brighter, two toward darker, and never more
    // than three stops a second whatever the scene does.
    //
    // And a dead band, as a camera's exposure lock has: while the scene stays within 0.4 stop of the value
    // in use, the value does not move at all. Every small drift of the scene's average while walking
    // otherwise nudged what the model was shown, and the model's whole answer trembled with it.
    const float deadBand = 0.4f;
    const float excess = targetLog - g_nr.autoWhiteLog;

    if (std::abs(excess) <= deadBand)
        return;

    const float goal = targetLog - (excess > 0.0f ? deadBand : -deadBand);
    const float tau = targetLog > g_nr.autoWhiteLog ? 0.8f : 1.6f;
    const float a = 1.0f - (float) std::exp(-dt / tau);
    const float maxStep = 3.0f * (float) dt;
    g_nr.autoWhiteLog += std::clamp((goal - g_nr.autoWhiteLog) * a, -maxStep, maxStep);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
}

// Turns what the meter saw into the divisor the encode uses, or falls back to the slider.
//
// `cut` says the exposure may jump rather than drift, and it is the difference between this working
// and not. GTA V's character switch pulls the camera up through the sky: a linear HDR buffer's sky is
// tens of times brighter than the ground, the proxy clips to flat white, and the frame blows out until
// the camera comes back down. Easing across that at two percent a frame takes three and a half
// seconds, which is longer than the transition -- so a meter that only eases would lag through the
// whole thing and fix nothing.
//
// So a cut snaps and a drift eases. Walking out of a cave is a drift; a camera cut is not, and
// pretending otherwise to avoid pumping just moves the failure somewhere more visible.
float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const float slider = cfg.DlssNrWhitePointScale.value_or_default();

    // A frame the game already tone mapped is display-referred: white is at 1 by definition and there
    // is nothing to measure. The slider stays available as a manual exposure on that path.
    if (!isHdrBuffer)
        return slider;

    // The game's own exposure, where it supplies one.
    //
    // Exposure is the step that makes a cave and a field comparable: the renderer works in arbitrary
    // scene-referred units and multiplies by this before tone mapping, which is precisely why one
    // fixed paper white cannot serve both. FSR spells the relationship out -- frame / preExposure *
    // exposure -- so undoing it gives the divisor this pass wants, and paper white becomes a constant
    // on top rather than a value chasing the scene.
    //
    // Unlike anything measured off the frame this cannot be moved by what the pass writes, which is
    // what killed the statistical meter. It is the game's number, decided upstream.
    //
    // Held across the frames where the texture is absent -- GTA V dropped it three times in one
    // session -- because falling back to a default on those frames is a flicker, not a fallback.
    // The scan's anchor, where the game supplies no exposure of its own.
    //
    // Only ratios are used, so the units of the buffer never have to be known -- which is the whole
    // reason this is anchored rather than absolute. The anchor is the user's own white point at the
    // moment they pressed the button; everything after that is the scan moving it.
    //
    // Deliberately below the exposure texture in priority and mutually exclusive with it in the
    // menu. A game that hands over a real exposure has no business being driven by a buffer found by
    // its shape, and two sources fighting over one number is the class of bug worth making
    // unreachable rather than merely unlikely.
    // Automatic: the scene's own average, times the user's trim.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 3 && g_nr.autoWhiteValid)
    {
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        return std::clamp(std::exp2(g_nr.autoWhiteLog) * trim, 0.01f, 65536.0f);
    }

    if (cfg.DlssNrWhitePointSource.value_or_default() == 2)
    {
        // Multi-point: one or more calibration points the user placed, interpolated in log space by
        // the current scan value. One point is the original ratio law; more fit the buffer's actual
        // relationship so the white point holds across the whole range, not only near one anchor.
        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
            DlssNr::ExposureScan::BestValue(), cfg.DlssNrScanInverted.value_or_default(),
            cfg.DlssNrScanTrim.value_or_default());

        if (w > 0.0f)
            return w;
    }

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && g_nr.gameExposure > 1e-6f &&
        !g_nr.exposureUnreliable)
    {
        // Its own setting, not the manual divisor. See Config: they are different quantities with
        // different units and different sensible ranges, and sharing one value meant adjusting the
        // trim destroyed the divisor somebody had found by hand.
        //
        // Still bounded at the point of use rather than only in the menu that draws it.
        //
        // Bounding it at the slider would have been cosmetic: someone who found 64 by hand on the
        // manual path and then switched the exposure source on keeps that 64 in their ini, and the
        // composition would go on reading it until they happened to touch the control. The picture
        // would be wrong for a reason the menu was no longer showing.
        //
        // Their value is left in the config untouched, so switching back to manual restores the
        // number they arrived at. It is only what this path consumes that is limited.
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);

        return std::clamp(g_nr.gamePreExposure / g_nr.gameExposure * trim, 0.01f, 4096.0f);
    }

    // Otherwise the slider, and only the slider.
    //
    // Measuring white from the frame was tried and removed. It could not be made to work because the
    // pass writes the frame it measures: in Enshrouded one session walked the divisor from 0.010 to
    // 97.910, and toggling NR at a fixed spot read 41.31 off and 0.46 on. Two attempts to damp it --
    // a relative lit threshold, then a rate limit with a cut snap -- both treated a coupled system as
    // a noisy one and neither held. A constant cannot do that, which is the whole argument for it,
    // and is what RenoDX has always done.
    return slider;
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                              unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    // Colour formats, for pre-SR's copy of the game's colour.
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* source, ID3D12Resource** clone)
{
    if (source == nullptr || !IsTypeless(source->GetDesc().Format))
        return source;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}",
                  (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
// Async compute feasibility probe. Running the model on a compute queue of our own, in parallel with the
// game's rendering, only works if NGX accepts a feature built and evaluated on a COMPUTE command list. This
// answers that, once, without touching the normal pass: a separate feature on blank inputs of our own,
// recorded on our own compute list, executed on our own compute queue and waited for here (one hitch). The
// log says whether the feature was built, what the evaluate returned, whether the GPU finished, whether the
// device survived, and how long the model took on that queue.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight);

void RunAsyncProbe(ID3D12Device* device, const Config& cfg, DXGI_FORMAT format, unsigned int w, unsigned int h,
                   unsigned int gw, unsigned int gh)
{
    g_asyncProbeWanted = false;
    LOG_INFO("DLSS-NR async probe: model {}x{}, guides {}x{}, format {}", w, h, gw, gh, (int) format);

    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12QueryHeap* stamps = nullptr;
    ID3D12Resource* stampsBack = nullptr;
    ID3D12Resource* in = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* out = nullptr;
    void* feature = nullptr;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto cleanup = [&]()
    {
        if (feature != nullptr && g_nr.release != nullptr)
            g_nr.release(feature);

        for (ID3D12Resource* r : { in, depth, motion, out, stampsBack })
        {
            if (r != nullptr)
                r->Release();
        }

        if (stamps != nullptr)
            stamps->Release();

        if (list != nullptr)
            list->Release();

        if (alloc != nullptr)
            alloc->Release();

        if (fence != nullptr)
            fence->Release();

        if (queue != nullptr)
            queue->Release();

        if (done != nullptr)
            CloseHandle(done);
    };

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc, nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        LOG_ERROR("DLSS-NR async probe: could not create a compute queue, list or fence");
        cleanup();
        return;
    }

    D3D12_QUERY_HEAP_DESC hd {};
    hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = 2;
    device->CreateQueryHeap(&hd, IID_PPV_ARGS(&stamps));

    {
        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&stampsBack));
    }

    in = CreateScratch(device, format, w, h);
    depth = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, gw, gh);
    motion = CreateScratch(device, DXGI_FORMAT_R16G16_FLOAT, gw, gh);
    out = CreateScratch(device, format, w, h);

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (in == nullptr || depth == nullptr || motion == nullptr || out == nullptr || !snippet.has_value() ||
        g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        LOG_ERROR("DLSS-NR async probe: no inputs or no runtime");
        cleanup();
        return;
    }

    // Inputs as the model reads them; all three states are allowed on a compute queue.
    Barrier(list, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(list, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
    feature = g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device, list,
                          g_nr.capabilityParams, w, h, (int) cfg.DlssNrPreset.value_or_default(),
                          cfg.DlssNrIntensity.value_or_default(), (int) EffStyle(cfg),
                          cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                          cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

    if (feature == nullptr)
    {
        LOG_ERROR("DLSS-NR async probe: the feature could NOT be built on a compute command list");
        list->Close();
        cleanup();
        return;
    }

    if (stamps != nullptr)
        list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);

    const int result = g_nr.evaluate(list, feature, g_nr.capabilityParams, in, depth, motion, out, w, h, gw, gh,
                                     g_nr.guideDepthInverted ? 1 : 0, 1, cfg.DlssNrIntensity.value_or_default(),
                                     (int) EffStyle(cfg), cfg.DlssNrLocalStructure.value_or_default(),
                                     cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                                     cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1.0f, 1.0f);

    if (stamps != nullptr && stampsBack != nullptr)
    {
        list->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stampsBack, 0);
    }

    const HRESULT closed = list->Close();
    bool finished = false;

    if (SUCCEEDED(closed))
    {
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        fence->SetEventOnCompletion(1, done);
        finished = WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
    }

    const HRESULT removed = device->GetDeviceRemovedReason();
    double ms = -1.0;
    UINT64 freq = 0;

    if (finished && stampsBack != nullptr && SUCCEEDED(queue->GetTimestampFrequency(&freq)) && freq != 0)
    {
        UINT64* t = nullptr;
        D3D12_RANGE r { 0, 16 };

        if (SUCCEEDED(stampsBack->Map(0, &r, (void**) &t)) && t != nullptr)
        {
            if (t[1] >= t[0])
                ms = (double) (t[1] - t[0]) * 1000.0 / (double) freq;

            D3D12_RANGE none { 0, 0 };
            stampsBack->Unmap(0, &none);
        }
    }

    LOG_INFO("DLSS-NR async probe: feature built on a compute list, evaluate returned {} ({}), list close 0x{:X}, "
             "GPU {}, device {} (0x{:X}), model on the compute queue {:.2f} ms",
             result, NgxResultName((unsigned int) result), (uint32_t) closed, finished ? "finished" : "DID NOT FINISH",
             removed == S_OK ? "fine" : "REMOVED", (uint32_t) removed, ms);

    cleanup();
}

// ---------------------------------------------------------------------------------------------
// CacheAsync: the model on a compute queue of our own.
//
// On the frame the model would run on (the launch), its evaluate is recorded on our list instead, against
// copies of the guides (the game rewrites its own), and the frame shows the carried edit as a cached frame
// does. The list goes to our queue at the next frame, behind a fence the game's queue signals after everything
// it has been given by then -- the launch frame included -- so the model runs alongside that next frame. The
// frame after it (the landing) makes the game's queue wait for the model, which has normally long finished,
// composes the answer with the launch frame into a texture of ours and hands it to the edit cache, which
// carries it to the frame on screen along the motion since and stores it as on a frame the model runs.
//
// The model's cost leaves the game's queue; the price is two frames of latency on its answer, which the
// cache's reprojection carries. There is one feature, built on our queue while this runs (AsyncBuildMain), and
// rebuilt on the game's list -- the model in step, as without this -- whenever a condition is missing: before
// the upscaler, more than one pass, a model above 100%, an instrument on.
// ---------------------------------------------------------------------------------------------

// Lets everything go. Waits for our queue first (a second at most): nothing of ours may be freed under it.
void AsyncDestroy()
{
    if (g_async.modelDone != nullptr && g_async.doneValue > 0 &&
        g_async.modelDone->GetCompletedValue() < g_async.doneValue)
    {
        if (HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr))
        {
            if (SUCCEEDED(g_async.modelDone->SetEventOnCompletion(g_async.doneValue, done)))
                WaitForSingleObject(done, 1000);

            CloseHandle(done);
        }
    }

    for (ID3D12Resource** r : { &g_async.output, &g_async.depth, &g_async.motion, &g_async.keep, &g_async.composed,
                                &g_async.stampsBack })
    {
        if (*r != nullptr)
        {
            (*r)->Release();
            *r = nullptr;
        }
    }

    if (g_async.stamps != nullptr)
        g_async.stamps->Release();

    if (g_async.list != nullptr)
        g_async.list->Release();

    for (auto*& a : g_async.alloc)
    {
        if (a != nullptr)
            a->Release();

        a = nullptr;
    }

    if (g_async.inputsReady != nullptr)
        g_async.inputsReady->Release();

    if (g_async.modelDone != nullptr)
        g_async.modelDone->Release();

    if (g_async.queue != nullptr)
        g_async.queue->Release();

    g_async.stamps = nullptr;
    g_async.list = nullptr;
    g_async.inputsReady = nullptr;
    g_async.modelDone = nullptr;
    g_async.queue = nullptr;
    g_async.phase = 0;
    g_async.held = false;
}

bool AsyncEnsureQueue(ID3D12Device* device)
{
    if (g_async.queue != nullptr)
        return true;

    if (g_async.broken)
        return false;

    D3D12_COMMAND_QUEUE_DESC qd {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

    bool ok = SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_async.queue)));

    for (auto*& a : g_async.alloc)
        ok = ok && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&a)));

    ok = ok && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, g_async.alloc[0], nullptr,
                                                   IID_PPV_ARGS(&g_async.list)));
    ok = ok && SUCCEEDED(g_async.list->Close());
    ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_async.inputsReady)));
    ok = ok && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_async.modelDone)));

    if (!ok)
    {
        AsyncDestroy();
        g_async.broken = true;
        g_async.why = "no compute queue could be created";
        LOG_ERROR("DLSS-NR async: {}; the model runs in step", g_async.why);
        return false;
    }

    g_async.queue->SetName(L"DLSS-NR model (CacheAsync)");

    // The model's time on our queue, for the overlay. Optional.
    D3D12_QUERY_HEAP_DESC hd {};
    hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    hd.Count = 2 * AsyncState::kStampSlots;

    if (SUCCEEDED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&g_async.stamps))))
    {
        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 2 * AsyncState::kStampSlots * sizeof(UINT64);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&g_async.stampsBack))))
        {
            g_async.stampsBack = nullptr;
            g_async.stamps->Release();
            g_async.stamps = nullptr;
        }
    }
    else
    {
        g_async.stamps = nullptr;
    }

    LOG_INFO("DLSS-NR async: compute queue ready");
    return true;
}

// Whether this frame may run the model in the background, and when it is asked for and may not, why.
bool AsyncWanted(const Config& cfg, float workScale)
{
    if (!cfg.DlssNrCacheAsync.value_or_default() || g_async.broken)
        return false;

    const char* why = nullptr;

    if (!EffCache(cfg))
        why = "needs the edit cache";
    else if (g_preSrDispatch)
        why = "runs after the upscaler only";
    else if (workScale > 1.0f)
        why = "not with a model above 100%";
    else if (EffPasses(cfg) > 1)
        why = "one pass only";
    else if (cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrCompare.value_or_default() != 0 ||
             cfg.DlssNrDebugView.value_or_default() != 0 || cfg.DlssNrUseProxy.value_or_default() ||
             g_capture.isActive() || (g_cache != nullptr && g_cache->DumpActive()))
        why = "stands aside while an instrument is on";

    g_async.why = why != nullptr ? why : "";
    return why == nullptr;
}

// Our queue, the game's, and the frame-sized textures: all a launch or a landing needs but the feature.
bool AsyncReady(ID3D12Device* device, ID3D12CommandQueue* timingQueue, DXGI_FORMAT format, unsigned int width,
                unsigned int height)
{
    if (!AsyncEnsureQueue(device))
        return false;

    // The queue the game submits the upscaler's list on: the one it says, or the one it presents with.
    auto* gameQueue =
        timingQueue != nullptr ? timingQueue : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

    if (gameQueue == nullptr)
    {
        g_async.why = "the game's queue is not known";
        return false;
    }

    // A run in flight keeps the queue it was launched on.
    if (g_async.phase == 0)
        g_async.gameQueue = gameQueue;

    for (ID3D12Resource** r : { &g_async.keep, &g_async.composed })
    {
        if (*r != nullptr)
        {
            const D3D12_RESOURCE_DESC d = (*r)->GetDesc();

            if ((unsigned int) d.Width != width || d.Height != height || d.Format != format)
                ParkNrResource(*r);
        }

        if (*r == nullptr)
            *r = CreateScratch(device, format, width, height);

        if (*r == nullptr)
        {
            g_async.why = "no memory for its textures";
            return false;
        }
    }

    return true;
}

// The launch frame's proxy and frame back to rest, and no run in flight.
void AsyncRelease(ID3D12GraphicsCommandList* cmdList)
{
    if (g_async.held)
    {
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto rest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (g_nr.colorCopy != nullptr)
            Barrier(cmdList, g_nr.colorCopy, read, rest);

        if (g_nr.hdrCopy != nullptr)
            Barrier(cmdList, g_nr.hdrCopy, read, rest);

        if (g_async.reduced && g_nr.colorSmall != nullptr)
            Barrier(cmdList, g_nr.colorSmall, read, rest);

        g_async.held = false;
    }

    g_async.phase = 0;
}

// A run that can no longer land is let go. The game's queue waits for it -- nothing after may touch what it
// reads before it is done -- and the model starts again from nothing the next time it runs here.
void AsyncAbandon(ID3D12GraphicsCommandList* cmdList)
{
    if (g_async.phase == 2 && g_async.gameQueue != nullptr)
        g_async.gameQueue->Wait(g_async.modelDone, g_async.job);

    AsyncRelease(cmdList);
    g_nr.reset = true;
}

// The run recorded on the last frame goes to our queue, behind a fence the game's queue signals after all it has
// been given so far -- the launch frame's list included, since a game submits a frame before it records the
// next. The model then runs alongside the rest of this frame.
void AsyncSubmit()
{
    if (g_async.phase != 1 || g_async.gameQueue == nullptr)
        return;

    g_async.gameQueue->Signal(g_async.inputsReady, ++g_async.inputsValue);
    g_async.queue->Wait(g_async.inputsReady, g_async.inputsValue);

    ID3D12CommandList* lists[] = { g_async.list };
    g_async.queue->ExecuteCommandLists(1, lists);
    g_async.queue->Signal(g_async.modelDone, ++g_async.doneValue);

    g_async.job = g_async.doneValue;
    g_async.allocDone[g_async.allocCur] = g_async.job;
    g_async.stampJob[g_async.stampSlot] = g_async.job;
    g_async.stampSlot = (g_async.stampSlot + 1) % AsyncState::kStampSlots;
    g_async.phase = 2;
}

// The model's time on our queue, from the runs that have finished.
void AsyncReadStamps()
{
    UINT64 frequency = 0;

    if (g_async.stampsBack == nullptr || g_async.modelDone == nullptr ||
        FAILED(g_async.queue->GetTimestampFrequency(&frequency)) || frequency == 0)
        return;

    const UINT64 done = g_async.modelDone->GetCompletedValue();

    for (unsigned int s = 0; s < AsyncState::kStampSlots; ++s)
    {
        if (g_async.stampJob[s] == 0 || done < g_async.stampJob[s])
            continue;

        D3D12_RANGE range { 2 * s * sizeof(UINT64), (2 * s + 2) * sizeof(UINT64) };
        void* mapped = nullptr;

        if (SUCCEEDED(g_async.stampsBack->Map(0, &range, &mapped)) && mapped != nullptr)
        {
            const UINT64* t = (const UINT64*) mapped + 2 * s;

            if (t[1] > t[0])
            {
                const double ms = (double) (t[1] - t[0]) * 1000.0 / (double) frequency;
                g_async.modelMs = g_async.modelMs <= 0.0 ? ms : g_async.modelMs * 0.9 + ms * 0.1;
            }

            D3D12_RANGE nothing { 0, 0 };
            g_async.stampsBack->Unmap(0, &nothing);
        }

        g_async.stampJob[s] = 0;
    }
}

// Off, or standing aside: the textures go (the queue stays; it costs nothing). Memory matters here -- with a second
// instance of the model beside the main one, Control at 1440p went past a 12 GB card's budget and every frame
// slowed down, the stock pass included.
void AsyncTrim()
{
    for (ID3D12Resource** r : { &g_async.output, &g_async.depth, &g_async.motion, &g_async.keep, &g_async.composed })
        ParkNrResource(*r);
}

// The main feature, built on our compute queue: with CacheAsync the model runs only there, so there is one
// feature and not a second beside it. A feature runs on the kind of queue it was built on; this one is never
// evaluated on the game's list (it is rebuilt there when the background stands aside). The build is executed at
// once on our queue; the first evaluate comes on a later frame, in a later list of the same queue.
// busy: our queue is too far behind to take it now (try again next frame).
void* AsyncBuildMain(ID3D12Device* device, const Config& cfg, const wchar_t* snippet, unsigned int workW,
                     unsigned int workH, bool& busy)
{
    busy = false;
    const unsigned int a = (g_async.allocCur + 1) % AsyncState::kAllocators;

    if (g_async.modelDone->GetCompletedValue() < g_async.allocDone[a])
    {
        busy = true;
        return nullptr;
    }

    if (FAILED(g_async.alloc[a]->Reset()) || FAILED(g_async.list->Reset(g_async.alloc[a], nullptr)))
        return nullptr;

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
    void* feature = g_nr.create(snippet, State::Instance().NVNGX_ApplicationDataPath.c_str(), device, g_async.list,
                                g_nr.capabilityParams, workW, workH, (int) cfg.DlssNrPreset.value_or_default(),
                                cfg.DlssNrIntensity.value_or_default(), (int) EffStyle(cfg),
                                cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                                cfg.DlssNrSkinStructure.value_or_default(),
                                cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

    const HRESULT closed = g_async.list->Close();

    if (feature == nullptr || FAILED(closed))
    {
        // Its list never ran, so nothing on the GPU refers to it.
        if (feature != nullptr && g_nr.release != nullptr)
            g_nr.release(feature);

        return nullptr;
    }

    ID3D12CommandList* lists[] = { g_async.list };
    g_async.queue->ExecuteCommandLists(1, lists);
    g_async.queue->Signal(g_async.modelDone, ++g_async.doneValue);
    g_async.allocDone[a] = g_async.doneValue;
    g_async.allocCur = a;
    return feature;
}

// A copy of a guide (NON_PIXEL_SHADER_RESOURCE, left so) the game cannot rewrite before the model reads it. The
// copy rests in NON_PIXEL_SHADER_RESOURCE.
bool AsyncSnapshot(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* src,
                   ID3D12Resource*& snap)
{
    if (src == nullptr)
        return false;

    const D3D12_RESOURCE_DESC want = src->GetDesc();
    bool fresh = false;

    if (snap != nullptr)
    {
        const D3D12_RESOURCE_DESC have = snap->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height || have.Format != TypedGuideFormat(want.Format) ||
            have.MipLevels != want.MipLevels)
            ParkNrResource(snap);
    }

    if (snap == nullptr)
    {
        snap = CreateGuideClone(device, src); // created in COPY_DEST
        fresh = true;

        if (snap == nullptr)
            return false;
    }

    if (!fresh)
        Barrier(cmdList, snap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    Barrier(cmdList, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(snap, src);
    Barrier(cmdList, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, snap, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return true;
}

// The launch: the model's run recorded on our list against copies of this frame's guides, and the proxy and the
// frame held readable for the landing. False holds nothing: the frame is then shown from the cache.
bool AsyncLaunch(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, const Config& cfg,
                 ID3D12Resource* modelInput, ID3D12Resource* depthIn, ID3D12Resource* motionIn, DXGI_FORMAT format,
                 unsigned int width, unsigned int height, unsigned int workW, unsigned int workH, unsigned int guideW,
                 unsigned int guideH, float mvScaleX, float mvScaleY, bool reduced, bool reset)
{
    if (g_nr.feature == nullptr || !g_nr.featureOnCompute)
        return false;

    // Each allocator comes round every third run, many frames on: one still busy means the GPU is that far behind.
    const unsigned int a = (g_async.allocCur + 1) % AsyncState::kAllocators;

    if (g_async.modelDone->GetCompletedValue() < g_async.allocDone[a])
        return false;

    if (g_async.output != nullptr)
    {
        const D3D12_RESOURCE_DESC d = g_async.output->GetDesc();

        if ((unsigned int) d.Width != workW || d.Height != workH || d.Format != format)
            ParkNrResource(g_async.output);
    }

    if (g_async.output == nullptr)
        g_async.output = CreateScratch(device, format, workW, workH);

    if (g_async.output == nullptr || !AsyncSnapshot(cmdList, device, depthIn, g_async.depth) ||
        !AsyncSnapshot(cmdList, device, motionIn, g_async.motion))
        return false;

    if (FAILED(g_async.alloc[a]->Reset()) || FAILED(g_async.list->Reset(g_async.alloc[a], nullptr)))
        return false;

    const unsigned int s = g_async.stampSlot;
    g_async.stampJob[s] = 0;

    if (g_async.stamps != nullptr)
        g_async.list->EndQuery(g_async.stamps, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
    const int result = g_nr.evaluate(
        g_async.list, g_nr.feature, g_nr.capabilityParams, modelInput, g_async.depth, g_async.motion,
        g_async.output, workW, workH, guideW, guideH, g_nr.guideDepthInverted ? 1 : 0, reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(), (int) EffStyle(cfg),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, mvScaleX, mvScaleY);

    if (g_async.stamps != nullptr && g_async.stampsBack != nullptr)
    {
        g_async.list->EndQuery(g_async.stamps, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);
        g_async.list->ResolveQueryData(g_async.stamps, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s, 2, g_async.stampsBack,
                                       2 * s * sizeof(UINT64));
    }

    const HRESULT closed = g_async.list->Close();

    if (result != NVSDK_NGX_Result_Success || FAILED(closed))
    {
        g_async.broken = true;
        g_async.why = "the model refused to run on a compute queue";
        LOG_ERROR("DLSS-NR async: evaluate on our queue returned 0x{:X} ({}), list close 0x{:X}; the model runs "
                  "in step from now on",
                  (uint32_t) result, NgxResultName((unsigned int) result), (uint32_t) closed);
        return false;
    }

    g_async.allocCur = a;
    g_async.phase = 1;
    g_async.launchFrame = g_frames;
    g_async.held = true;
    g_async.heldInput = modelInput;
    g_async.reduced = reduced;
    g_async.format = format;
    g_async.width = width;
    g_async.height = height;
    g_async.workW = workW;
    g_async.workH = workH;
    ++g_async.launches;
    return true;
}

// What the edit cache's GPU budget weighs a model run at in the background: the launch and the landing on the
// game's queue, and the model's own time on ours.
double AsyncRefreshCost() { return g_costRefresh + std::max(0.0, g_costLand - g_costCached) + g_async.modelMs; }

void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer,
                   uiWidth, uiHeight, bbWidth, bbHeight);
}

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == EffStyle(cfg) &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = EffStyle(cfg);
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

// Defined after the pass; shared by its model path and the edit cache's cached frames.
void FinishPassTiming(ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* timingQueue);

} // namespace

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                  ID3D12Resource* InSource, ID3D12Resource* InModel,
                                  ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                                  ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                                  ID3D12Resource* OutKeep)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }
}

void DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                           ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
                           const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (g_nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr ||
        motion == nullptr || output == nullptr)
    {
        ReportSkipOnce(g_nr.failed ? "it already failed this session" : "a resource was missing");
        return;
    }

    ID3D12Resource* target = output;

    // The state the upscaler left the output in. Every upscaler in this tree ends Evaluate by moving
    // the output to OutputResourceBarrier when the user set it (FFXFeature_Dx12.cpp:606 and the FSR2 /
    // XeSS equivalents), and leaves it in UNORDERED_ACCESS -- what its own compute wrote -- when they
    // did not. This pass then reads and writes the output as a UAV, so it normalises to that here and
    // restores the arrival state before every exit. When the config is unset the two states are equal
    // and Barrier() skips the no-op, so the default path is byte-identical.
    //
    // Pre-SR hands the pass its own copy of the colour instead, which arrives as a UAV.
    const D3D12_RESOURCE_STATES outputArrival =
        g_preSrDispatch ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : OutputRestState();

    Barrier(cmdList, target, outputArrival, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The model takes that as a subrect per resource rather than
    // needing them resampled, which is why nothing here rescales anything.
    // The guides are the upscaler's inputs and so are at render resolution, while colour and output
    // are at display resolution. Their sizes come from the resources rather than from the caller:
    // one less thing a call site can get wrong, and the model takes the difference as a subrect per
    // resource rather than needing anything resampled.
    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();
    unsigned int guideWidth = (unsigned int) guideDesc.Width;
    unsigned int guideHeight = guideDesc.Height;

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // What the game rendered wins over how big the texture is.
    //
    // The comment above says the sizes come from the resources so there is one less thing a call site
    // can get wrong, and that was right about call sites and wrong about the game. A dynamic
    // resolution title allocates its depth once at the maximum it will ever need and renders into
    // the corner; the resource then describes the allocation, not the picture, and the model gets
    // handed the stale margin as though it were scene.
    //
    // Bounded by the resource because a subrect larger than the texture is a game bug that would
    // otherwise become a read off the end of it.
    if (frame.RenderSubrectWidth != 0 && frame.RenderSubrectHeight != 0)
    {
        const unsigned int subW = std::min(frame.RenderSubrectWidth, guideWidth);
        const unsigned int subH = std::min(frame.RenderSubrectHeight, guideHeight);

        if (subW != guideWidth || subH != guideHeight)
        {
            static unsigned int saidW = 0, saidH = 0;

            if (saidW != subW || saidH != subH)
            {
                saidW = subW;
                saidH = subH;
                LOG_INFO("DLSS-NR guides: the game renders {}x{} into a {}x{} texture, so the model is "
                         "told the smaller number",
                         subW, subH, guideWidth, guideHeight);
            }
        }

        guideWidth = subW;
        guideHeight = subH;
    }

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    g_nr.guideDepthInverted = frame.DepthInverted;

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        g_nr.reset = true;

        static unsigned long long resets = 0;
        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    // Logged whenever it changes, not once per session.
    //
    // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
    // the output size is what the model is built for -- so a once-only line goes stale the moment the
    // player moves the quality slider, and every later line in the log is then read against numbers
    // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
    // and the log claimed 1920x1080 guides throughout.
    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow { true,       g_nr.guideDepthInverted, g_nr.guideMvScaleX,
                                  g_nr.guideMvScaleY, guideWidth,      guideHeight,
                                  width,      (unsigned int) height };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX,
                 g_nr.guideMvScaleY, guideWidth, guideHeight, width, height);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
    // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
    // working size so the model denoises a super-native input, which the resolve then samples back down.
    // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
    //
    // Pre-SR runs at the render resolution, which the upscaler already made small: the model takes it
    // whole. The comparison key's vanilla mode is the model at full size.
    float workScale = g_preSrDispatch ? 1.0f : EffWorkScale(cfg);
    workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    // CacheAsync: the run recorded on the last frame goes to our queue now (AsyncSubmit); one whose frame no
    // longer matches this one is let go first.
    const bool asyncWanted =
        AsyncWanted(cfg, workScale) && AsyncReady(device, timingQueue, desc.Format, width, height);
    AsyncReadStamps();

    if (g_async.phase != 0 &&
        (!asyncWanted || frame.Reset || desc.Format != g_async.format || width != g_async.width ||
         height != g_async.height || workWidth != g_async.workW || workHeight != g_async.workH ||
         !TuningMatchesFeature(cfg)))
        AsyncAbandon(cmdList);

    if (g_async.phase == 1)
        AsyncSubmit();

    if (!asyncWanted && g_async.phase == 0)
        AsyncTrim();

    ReleaseSurfacesIfFormatChanged(desc.Format);

    const bool resolutionChanged = g_nr.width != width || g_nr.height != height ||
                                   g_nr.workWidth != workWidth || g_nr.workHeight != workHeight;

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = !TuningMatchesFeature(cfg);

    // CacheAsync: the feature lives on our queue while the model runs in the background, on the game's list
    // otherwise; never evaluated on the other one, it is rebuilt when that changes.
    const bool queueChanged = g_nr.feature != nullptr && g_nr.featureOnCompute != asyncWanted;

    if (g_nr.feature != nullptr && (resolutionChanged || tuningChanged || queueChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(g_nr.feature);

        for (void*& f : g_nr.passFeature)
            ParkNrFeature(f);

        // Only a resolution change invalidates the scratch textures. Tuning does not, and throwing
        // them away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged)
        {
            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.passIn);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.outputNative);
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    // The down-leg target is native (the answer is brought back to frame size before the resolve).
    if (workScale > 1.0f && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, desc.Format, width, height);

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr &&
        g_nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return;
        }

        // CacheAsync: on our compute queue, where it will run.
        if (asyncWanted)
        {
            bool busy = false;
            g_nr.feature = AsyncBuildMain(device, cfg, snippet->wstring().c_str(), workWidth, workHeight, busy);

            if (g_nr.feature == nullptr)
            {
                // Not now, or not at all: the background then stands aside and the next frame builds the
                // feature on the game's list, as without it.
                if (!busy)
                {
                    g_async.broken = true;
                    g_async.why = "the model could not be built on a compute queue";
                    LOG_ERROR("DLSS-NR async: {}; the model runs in step", g_async.why);
                }

                device->Release();
                return;
            }

            g_nr.featureOnCompute = true;
            g_nr.width = width;
            g_nr.height = height;
            g_nr.reset = true;
            RecordBuiltTuning(cfg);
            LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {}), the model on its "
                     "own compute queue",
                     width, height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity,
                     g_nr.builtStyle);
            device->Release();
            return;
        }

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        g_nr.featureOnCompute = false;
        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) cfg.DlssNrPreset.value_or_default(),
                        cfg.DlssNrIntensity.value_or_default(), (int) EffStyle(cfg),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        // UI correction at the model's own default: with no UI layer fed to it there
                        // is nothing for it to correct.
                        1);

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})", width,
                 height, guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        device->Release();
        return;
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    // Both have to agree: the caller says what the game intends, the format says what the surface can
    // actually hold. A game that claims HDR while rendering into eight bits gets its frame encoded
    // twice otherwise.
    const bool gameSaysHdr = frame.ColourIsLinearHdr;
    const bool isHdrBuffer = gameSaysHdr && FormatCanHoldLinearHdr(desc.Format);

    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped",
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.

    // On an engine that needs its compute state put back -- the bindless quirks -- the envelope can
    // only restore what was captured. If nothing was captured for this list, the upscaler decided
    // touching state was unsafe this frame, and binding the pass now would leave state the envelope
    // cannot clean up. So on those games, skip the frame rather than corrupt it. Ordinary games do
    // not require restore, so they are unaffected and the pass runs as before.
    const bool restoreRequired = cfg.RestoreComputeSignature.value_or_default() ||
                                 cfg.RestoreGraphicSignature.value_or_default();

    if (restoreRequired && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");

        // The device reference taken at the top of this function is released on every other path out.
        // It was not released here, and this is the one path a bindless game takes every single frame
        // -- so the game that most needed this skip was also leaking a device reference per frame.
        device->Release();
        return;
    }

    // From here on the pass binds its own root signature, heaps and pipeline. Everything below runs
    // inside the envelope so the game's compute state is restored no matter which way this returns.
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_ngxTime == nullptr)
        g_ngxTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->Start(cmdList);
        ++g_timeStarts;
    }

    g_frameKind = 0;

    // Fetch the game's exposure, where the game supplies one and the user asked for it.
    //
    // This used to measure the white point off the frame as well, over a 64x64 grid of tile
    // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
    // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
    // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
    // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
    // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
    // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
    // never dispatched the meter and the white point silently fell back to the slider.
    const bool exposureSettingOn = cfg.DlssNrWhitePointSource.value_or_default() == 1;

    // Nothing held from before the option was switched off may survive switching it back on. See
    // InvalidateExposureMeter for what froze and why it read as a colour cast.
    if (exposureSettingOn && !g_nr.exposureSettingWasOn)
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    const bool wantExposure = exposureSettingOn && frame.ExposureTexture != nullptr;

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;

        // One pixel. Only tile (0,0) is read back, and the tile-mean branch below it in the shader is
        // dead code the dispatch simply never reaches.
        meterParams.Width = 1;
        meterParams.Height = 1;

        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr,
                     (ID3D12Resource*) frame.ExposureTexture, nullptr, g_nr.meter, nullptr);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    // The automatic white point measures the frame as the upscaler wrote it, every frame, on the full
    // 64 x 64 grid. Exclusive with the exposure courier above: they share the meter and its readback.
    if (g_nr.meter != nullptr && cfg.DlssNrWhitePointSource.value_or_default() == 3 && !wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;
        meterParams.Width = kDlssNrMeterGrid;
        meterParams.Height = kDlssNrMeterGrid;
        meterParams.UseLocalMap = 1; // every tile a tile mean, including (0,0)

        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr, nullptr, nullptr, g_nr.meter, nullptr);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        CopyMeterToReadback(cmdList, device, false);

        // A cut or a reset starts from the new scene rather than easing in from the old one.
        ConsumeAutoWhite(frame.Reset);
    }
    else
    {
        g_nr.autoWhiteSnap = true;
    }

    g_nr.gamePreExposure = frame.PreExposure;

    float whitePoint = ResolveWhitePoint(cfg, isHdrBuffer);

    // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
    // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
    // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
    // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && frame.ExposureTexture != nullptr &&
        !g_nr.exposureUnreliable)
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        exposurePreMul = g_nr.gamePreExposure * trim;
    }

    // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
    // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
    // live output before the encode reads it while held, so the encode's own path and barriers below
    // are untouched and the default (hold off) is byte-identical. See design/frame-hold.md.
    //
    // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
    // left in COPY_SOURCE after capture and stays there for every restore.
    {
        const bool hold = cfg.DlssNrHoldFrame.value_or_default();

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();
            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth ||
                                     td.Height != g_nr.heldHeight || td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                // restore -- target already holds the frame to freeze, and the pass runs on it.
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                // Held: restore the frozen frame onto the live output before the encode reads it.
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, g_nr.heldColor);
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            // Suspend white-point measurement while held: use the snapshot so it cannot drift and
            // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
            if (g_nr.heldActive)
                whitePoint = g_nr.heldWhitePoint;
        }
        else if (g_nr.heldActive)
        {
            // Released: let go of the frozen frame and resume live input next frame.
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
        }
    }

    // The temporal edit cache. Off -- the default -- reaches none of this, and the pass below runs
    // exactly as it always has.
    //
    // It stands aside whenever something else wants to see the model's own output on this frame: a
    // held frame, a comparison, a debug view, the proxy path, a capture. Those are instruments, and an
    // instrument reading a carried edit would be measuring the cache rather than the model.
    const bool cacheWanted = EffCache(cfg) && !cfg.DlssNrHoldFrame.value_or_default() &&
                             cfg.DlssNrCompare.value_or_default() == 0 && cfg.DlssNrDebugView.value_or_default() == 0 &&
                             !cfg.DlssNrUseProxy.value_or_default() && !g_capture.isActive();
    bool cacheActive = false;
    bool cacheRefresh = true;
    bool asyncOn = false;

    if (cacheWanted)
    {
        if (g_cache == nullptr)
            g_cache = std::make_unique<DlssNrEditCache_Dx12>(device);

        if (g_cache != nullptr && g_cache->IsInit())
        {
            cacheActive = true;

            // CacheAsync: the model in the background, on the feature built for it.
            asyncOn = asyncWanted && g_nr.featureOnCompute;

            if (!asyncOn && g_async.phase != 0)
                AsyncAbandon(cmdList);

            g_cache->SetFrameCosts(asyncOn ? AsyncRefreshCost() : g_costRefresh, g_costCached);
            cacheRefresh = g_cache->BeginFrame(cfg, device, width, height, desc.Format, frame.Reset || g_nr.reset,
                                               g_preSrDispatch, asyncOn, g_async.phase != 0);
            g_frameKind = cacheRefresh ? 1 : 2;
        }
        else
        {
            ReportSkipOnce("the edit cache could not be created, so the model runs every frame");
        }
    }
    else if (g_cache != nullptr)
    {
        // Whatever it holds is from before it stood aside; switching back on starts from a refresh.
        g_cache->Invalidate();
    }

    if (!cacheActive && g_async.phase != 0)
        AsyncAbandon(cmdList);

    g_async.active = asyncOn;

    // Pre-SR: the frame is the game's jittered render, a different sub-pixel sample of the scene every
    // frame, and the game's motion vectors leave the jitter out. A carried edit is moved by the change
    // of jitter as well, or its fine detail lands up to a pixel off every frame and the upscaler averages
    // it away (measured in Control: detail x1.05 with the cache against x1.12 without it).
    float jitterDeltaX = 0.0f;
    float jitterDeltaY = 0.0f;

    if (g_preSrDispatch)
    {
        if (g_jitterValid && !frame.Reset)
        {
            jitterDeltaX = (float) g_jitterSign * (frame.JitterX - g_lastJitterX) / (float) std::max(width, 1u);
            jitterDeltaY = (float) g_jitterSign * (frame.JitterY - g_lastJitterY) / (float) std::max(height, 1u);
        }

        g_lastJitterX = frame.JitterX;
        g_lastJitterY = frame.JitterY;
        g_jitterValid = true;
    }
    else
    {
        g_jitterValid = false;
    }

    // What the cache needs to know about this frame's guides, once they have been made readable.
    auto cacheInputs = [&](ID3D12Resource* depthReadable, ID3D12Resource* motionReadable)
    {
        DlssNrCacheInputs in {};
        in.depth = depthReadable;
        in.motion = motionReadable;
        in.depthWidth = guideWidth;
        in.depthHeight = guideHeight;

        // The motion texture may be at render or display resolution. Its own size is its valid region,
        // unless it is the same allocation size as depth, in which case the game's subrect covers both.
        const D3D12_RESOURCE_DESC md = motion->GetDesc();
        in.motionWidth = (unsigned int) md.Width;
        in.motionHeight = md.Height;

        if (md.Width == guideDesc.Width && md.Height == guideDesc.Height)
        {
            in.motionWidth = guideWidth;
            in.motionHeight = guideHeight;
        }

        in.mvScaleX = g_nr.guideMvScaleX;
        in.mvScaleY = g_nr.guideMvScaleY;
        in.depthInverted = g_nr.guideDepthInverted;
        in.whitePoint = whitePoint;
        in.passthrough = !isHdrBuffer;
        in.exposure = exposureTex;
        in.useGameExposure = useGameExposure != 0;
        in.exposurePreMul = exposurePreMul;
        in.maxRatio = cfg.DlssNrMaxRatio.value_or_default();
        in.jitterDeltaX = jitterDeltaX;
        in.jitterDeltaY = jitterDeltaY;
        return in;
    };

    // The composition's constants: the model path's, and CacheAsync's landing with the launch frame's white point.
    auto composeParams = [&](float wp)
    {
        DlssNrConstants p {};
        p.Mode = DlssNrMode_Resolve;
        p.WhitePoint = wp;
        p.UseGameExposure = useGameExposure;
        p.ExposurePreMul = exposurePreMul;
        p.Width = width;
        p.Height = height;
        p.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        p.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        p.DebugView = cfg.DlssNrDebugView.value_or_default();
        p.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        p.Transfer = cfg.DlssNrTransfer.value_or_default();
        p.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
        p.Passthrough = isHdrBuffer ? 0u : 1u;
        p.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        p.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        p.CompareMode = cfg.DlssNrCompare.value_or_default();
        p.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        p.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        p.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;
        return p;
    };

    if (cacheActive && !cacheRefresh)
    {
        // A cached frame: no encode, no model, no resolve. The carried edit is laid on the frame the
        // upscaler just wrote, which stays the game's own.
        DlssNr::ExposureScan::Tick(device, cmdList);

        ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
        ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

        if (asyncOn && g_async.phase == 2 && g_frames >= g_async.launchFrame + kAsyncLatency)
        {
            // CacheAsync: the answer of the run launched two frames ago lands here. The game's queue waits for it
            // first; it has normally long finished, having run alongside the frame between.
            g_frameKind = 3;
            g_async.gameQueue->Wait(g_async.modelDone, g_async.job);

            bool landed = false;

            if (depthIn != nullptr && motionIn != nullptr)
            {
                // The launch frame composed with the answer, exactly as on a frame the model runs, into a texture
                // of ours; the edit cache carries it to this frame.
                constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                constexpr auto rest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                ID3D12Resource* proxy = g_async.heldInput;
                ID3D12Resource* answer = g_async.output;
                bool jbuOk = false;

                Barrier(cmdList, g_async.output, rest, read);

                if (g_async.reduced && EffJbu(cfg))
                {
                    if (ID3D12Resource* up = g_cache->UpsampleModel(cmdList, device, g_nr.colorCopy, g_async.heldInput,
                                                                    g_async.output, !isHdrBuffer,
                                                                    cfg.DlssNrJbuSigma.value_or_default()))
                    {
                        proxy = g_nr.colorCopy;
                        answer = up;
                        jbuOk = true;
                    }
                }

                DispatchPass(cmdList, composeParams(g_async.whitePoint), proxy, answer, g_nr.hdrCopy, motionIn,
                             exposureTex, g_async.composed, nullptr);
                Barrier(cmdList, g_async.output, read, rest);

                if (jbuOk)
                    g_cache->FinishUpsample(cmdList);

                Barrier(cmdList, g_async.composed, rest, read);
                landed = g_cache->ConsumeAsync(cmdList, device, target, g_async.keep, g_async.composed, g_nr.hdrCopy,
                                               g_async.depth, cacheInputs(depthIn, motionIn));
                Barrier(cmdList, g_async.composed, read, rest);
            }

            AsyncRelease(cmdList);

            if (landed)
                ++g_async.landed;
        }
        else if (depthIn != nullptr && motionIn != nullptr)
        {
            // CacheAsync holds the launch frame in hdrCopy until its answer lands; the cache keeps its own copy.
            g_cache->RunCached(cmdList, device, target, asyncOn ? g_async.keep : g_nr.hdrCopy,
                               cacheInputs(depthIn, motionIn));
        }

        g_cache->EndFrame(cmdList);
        FinishPassTiming(cmdList, timingQueue);

        // The same hand-back as the end of the model path.
        if (g_nr.depthClone != nullptr)
            Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        if (g_nr.motionClone != nullptr)
            Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputArrival);
        device->Release();
        return;
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.UseGameExposure = useGameExposure;
    encodeParams.ExposurePreMul = exposurePreMul;
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, exposureTex, g_nr.colorCopy, g_nr.hdrCopy);

    Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
    // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
    // of a 4096-thread dispatch, a readback and an nth_element every frame.)

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
            // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
            // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
            // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
            // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
            // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
            // of Output Scaling, so the two can run different filters at once. superDown is built here
            // and used after the model (the down-leg below).
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler)
            {
                if (g_nr.superUp != nullptr)   { delete g_nr.superUp;   g_nr.superUp = nullptr; }
                if (g_nr.superDown != nullptr) { delete g_nr.superDown; g_nr.superDown = nullptr; }
                g_nr.nrScaler = nrScaler;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (g_nr.superUp != nullptr &&
                g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                // Wanted to supersample but the upscaler was not available -- warn once; the box path
                // below can only enlarge blockily, so the user should know the clean path is off.
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr,
                                g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    // Read the exposure scan's candidates on the pass's own command list, once a frame.
    DlssNr::ExposureScan::Tick(device, cmdList);

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputArrival);
        device->Release();
        return;
    }

    // On a refresh with the cache on, the model has not seen the frames since it last ran. Its own
    // history is reprojected by the vectors it is handed, so it is handed the motion accumulated over
    // all of them (same units, same scale) -- or told to reset, as configured. The cache itself keeps
    // the game's own one-frame vectors.
    DlssNrCacheInputs cacheIn {};
    bool resetModel = false;

    if (cacheActive)
    {
        cacheIn = cacheInputs(depthIn, motionIn);

        if (ID3D12Resource* accumulated = g_cache->ModelMotion(cmdList, device, cacheIn, resetModel))
            motionIn = accumulated;

        if (resetModel)
            g_nr.reset = true;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    // The vectors were scaled to full-frame pixels; the image the model reprojects is the
    // working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

    // CacheAsync: this frame launches the model on our queue instead of running it here, and shows the carried
    // edit as a cached frame does; the answer lands two frames on. A feature built for our queue never runs on the
    // game's list: when it cannot launch, the frame is shown from the cache and the model runs at the next.
    if (asyncOn || g_nr.featureOnCompute)
    {
        const bool launched =
            asyncOn && g_async.phase == 0 &&
            AsyncLaunch(cmdList, device, cfg, modelInput, depthIn, motionIn, desc.Format, width, height, workWidth,
                        workHeight, guideWidth, guideHeight, g_nr.guideMvScaleX * mvToWork,
                        g_nr.guideMvScaleY * mvToWork, reduced, g_nr.reset);

        if (launched)
        {
            g_async.whitePoint = whitePoint;
            g_nr.reset = false;
        }
        else
        {
            // Not this frame (our queue is behind, or the cache stood aside): the feature runs only on our queue,
            // so the frame is shown as a cached one, and the model runs at the next.
            ReportSkipOnce("the model's queue could not take a run; the frame was shown from the cache");
            g_nr.reset = true;

            if (cacheActive)
                g_cache->RefreshNext();

            constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            constexpr auto rest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            Barrier(cmdList, g_nr.colorCopy, read, rest);
            Barrier(cmdList, g_nr.hdrCopy, read, rest);

            if (reduced && g_nr.colorSmall != nullptr)
                Barrier(cmdList, g_nr.colorSmall, read, rest);
        }

        // The motion since the model's last run was just taken for it; this frame's is not chained again.
        if (cacheActive && g_async.keep != nullptr)
            g_cache->RunCached(cmdList, device, target, g_async.keep, cacheIn, false);

        if (cacheActive)
            g_cache->EndFrame(cmdList);

        FinishPassTiming(cmdList, timingQueue);

        // The guide clones as at the end of the model path. On a launch the proxy and the frame stay readable until
        // the landing: the model reads one on our queue, the landing reads both.
        if (g_nr.depthClone != nullptr)
            Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        if (g_nr.motionClone != nullptr)
            Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);

        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputArrival);
        device->Release();
        return;
    }

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, g_nr.output, workWidth, workHeight,
            guideWidth, guideHeight, g_nr.guideDepthInverted, g_nr.reset,
            g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session",
                      proxyResult, NgxResultName(proxyResult));
        }

        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputArrival);
        device->Release();
        return;
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->Start(cmdList);

    // Multi-pass (DlssNrPasses, 1 to 3), as RenoDX and the other forks do it: each extra pass runs the
    // model again on the previous pass's answer, on a feature of its own with its own temporal history,
    // and the composition below happens once, against the untouched proxy -- detail builds up from
    // pass to pass while colour and tone are composed a single time.
    //
    // It was removed from this fork because each pass's feature was created and evaluated on the same
    // command list in the same frame, which is the GPU hang the main feature already avoids. Here a pass
    // feature is built on one frame and first evaluated on the next, one at a time, exactly like the
    // main feature; until it is ready the frame simply runs the passes that are.
    const unsigned int passesWanted = EffPasses(cfg);

    for (unsigned int p = passesWanted; p < 4; ++p)
    {
        if (p > 0 && g_nr.passFeature[p] != nullptr)
        {
            LOG_INFO("DLSS-NR: pass {} released", p + 1);
            ParkNrFeature(g_nr.passFeature[p]);
        }
    }

    unsigned int passesReady = 1;

    for (unsigned int p = 1; p < passesWanted; ++p)
    {
        if (g_nr.passFeature[p] != nullptr)
        {
            passesReady = p + 1;
            continue;
        }

        auto snip = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snip.has_value())
            snip = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (snip.has_value())
        {
            // The same tuning as the first pass, so a later pass refines the same look rather than
            // starting another one (RenoDX's default: passes 2+ follow pass 1).
            g_nr.passFeature[p] =
                g_nr.create(snip->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                            cmdList, g_nr.capabilityParams, workWidth, workHeight,
                            (int) cfg.DlssNrPreset.value_or_default(), cfg.DlssNrIntensity.value_or_default(),
                            (int) EffStyle(cfg), cfg.DlssNrLocalStructure.value_or_default(),
                            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);
            g_nr.passReset[p] = true;

            LOG_INFO("DLSS-NR: pass {} {} at {}x{}, first evaluated next frame", p + 1,
                     g_nr.passFeature[p] != nullptr ? "built" : "FAILED to build", workWidth, workHeight);
        }

        // One build per frame, and never evaluated on the frame it was built.
        break;
    }

    if (passesReady > 1 && g_nr.passIn == nullptr)
    {
        g_nr.passIn = CreateScratch(device, desc.Format, workWidth, workHeight);

        if (g_nr.passIn == nullptr)
            passesReady = 1;
    }

    int result = g_nr.evaluate(
        cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, depthIn, motionIn, g_nr.output,
        workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
        g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
        (int) EffStyle(cfg), cfg.DlssNrLocalStructure.value_or_default(),
        cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWork,
        g_nr.guideMvScaleY * mvToWork);

    for (unsigned int p = 1; p < passesReady && result == NVSDK_NGX_Result_Success; ++p)
    {
        // The previous pass's answer becomes this pass's input; the proxy the resolve compares
        // against is left untouched.
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, g_nr.passIn, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(g_nr.passIn, g_nr.output);
        Barrier(cmdList, g_nr.passIn, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        result = g_nr.evaluate(
            cmdList, g_nr.passFeature[p], g_nr.capabilityParams, g_nr.passIn, depthIn, motionIn, g_nr.output,
            workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
            (g_nr.reset || g_nr.passReset[p]) ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
            (int) EffStyle(cfg), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWork,
            g_nr.guideMvScaleY * mvToWork);

        g_nr.passReset[p] = false;
        Barrier(cmdList, g_nr.passIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    g_nr.passesRun = passesReady;

    if (g_ngxTime != nullptr)
        g_ngxTime->End(cmdList);

    g_nr.reset = false;

    if (g_asyncProbeWanted)
        RunAsyncProbe(device, cfg, desc.Format, workWidth, workHeight, guideWidth, guideHeight);

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (workWidth > width || workHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})",
                     workWidth, workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote,
                     (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        const DlssNrConstants resolveParams = composeParams(whitePoint);

        // The numbers the composition actually ran with, logged when any of them changes.
        //
        // A colour report without these cannot be read. Paper white alone decides whether the model
        // was shown a sensible picture or a blown one, and it was absent from every log in the first
        // round of reports -- one tester's "much better at 16" had to be taken on trust because
        // nothing in the file said what the value was. Debug view and compare mode are here for the
        // same reason from the other direction: both change what is on screen, and a screenshot with
        // one left on is indistinguishable from a bug.
        struct ComposeReport
        {
            bool valid;
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
        };

        static ComposeReport loggedCompose {};

        // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
        // Enshrouded session, because a measured white point drifts continuously and every drift was a
        // change. A line per meaningful change is the point; a line per frame is a different problem.
        const ComposeReport composeNow { true,
                                         // 5% steps in log space: the automatic source moves every frame.
                                         std::exp2(std::round(std::log2(std::max(resolveParams.WhitePoint, 1e-4f)) * 14.0f) / 14.0f),
                                         resolveParams.TransferStrength,
                                         resolveParams.ColourStrength,
                                         resolveParams.MaxRatio,
                                         resolveParams.Passthrough,
                                         resolveParams.DebugView,
                                         resolveParams.CompareMode,
                                         resolveParams.Transfer,
                                         g_nr.workWidth,
                                         g_nr.workHeight };

        if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
            loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
            loggedCompose.maxRatio != composeNow.maxRatio ||
            loggedCompose.passthrough != composeNow.passthrough ||
            loggedCompose.debugView != composeNow.debugView ||
            loggedCompose.compareMode != composeNow.compareMode ||
            loggedCompose.residual != composeNow.residual || loggedCompose.workW != composeNow.workW ||
            loggedCompose.workH != composeNow.workH)
        {
            loggedCompose = composeNow;
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, colour transform {}, transfer {}, model {}x{}, debug view {}, compare {}",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW,
                     composeNow.workH, composeNow.debugView, composeNow.compareMode);
        }

        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
        // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
        // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
        // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
        // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
        // falls back to the Nx pair. g_nr.output is NPSR here; outputNative is UAV from last frame.
        bool superDownOk = false;
        if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
            g_nr.superDown->Dispatch(cmdList, g_nr.output, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        ID3D12Resource* resolveProxy = superDownOk ? g_nr.colorCopy : modelInput;
        ID3D12Resource* resolveAnswer = superDownOk ? g_nr.outputNative : g_nr.output;

        // Joint bilateral enlargement, when asked for and the model ran below the frame: the model's
        // residual is brought to full size guided by the full-size proxy, and the resolve then sees
        // two full-size pictures -- its classic path, with nothing left to enlarge. Off leaves the
        // resolve exactly as it was.
        bool jbuOk = false;

        if (!superDownOk && reduced && workScale < 1.0f && EffJbu(cfg))
        {
            if (g_cache == nullptr)
                g_cache = std::make_unique<DlssNrEditCache_Dx12>(device);

            if (g_cache != nullptr && g_cache->IsInit())
            {
                if (ID3D12Resource* up = g_cache->UpsampleModel(cmdList, device, g_nr.colorCopy, modelInput, g_nr.output,
                                                                !isHdrBuffer, cfg.DlssNrJbuSigma.value_or_default()))
                {
                    resolveProxy = g_nr.colorCopy;
                    resolveAnswer = up;
                    jbuOk = true;
                }
            }
        }

        DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, g_nr.hdrCopy, motionIn, exposureTex, target,
                     nullptr);
        Barrier(cmdList, g_nr.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (jbuOk)
            g_cache->FinishUpsample(cmdList);

        if (superDownOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // A refresh: store what the model just did, so the frames until the next one can carry it.
        if (cacheActive)
            g_cache->CaptureRefresh(cmdList, device, target, g_nr.hdrCopy, cacheIn);

        // On-demand capture works in this path too: the staging copy still holds the frame as the
        // upscaler produced it, and the edited frame is the output itself. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            g_capture.record(cmdList, device, g_nr.colorCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (cacheActive)
        g_cache->EndFrame(cmdList);

    FinishPassTiming(cmdList, timingQueue);

    // Put any guide clones back where the next frame's copy expects to find them.
    // A clone left in NON_PIXEL_SHADER_RESOURCE by a frozen frame was never transitioned back to
    // COPY_DEST, because a frozen frame does not copy. Putting it back unconditionally would be a
    // barrier from a state it is not in, so the frozen case is skipped here and picked up by the
    // first live frame after the toggle goes off -- which is a copy, and copies transition it.
    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Leave the staging copy as the next frame expects to find it.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Hand the output back in the state the upscaler and the game expect.
    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputArrival);

    device->Release();
}

namespace
{
// The end of the pass's GPU timing, shared by the model path and the edit cache's cached frames.
void FinishPassTiming(ID3D12GraphicsCommandList* cmdList, ID3D12CommandQueue* timingQueue)
{
    if (g_gpuTime != nullptr)
    {
        g_timeKind[g_timeStarts % 3] = g_frameKind;
        g_gpuTime->End(cmdList);

        // This path records into the game's own list, so there is no queue of ours to read from.
        // A caller that knows which queue the list goes to says so; otherwise the one the upscaler was
        // invoked on serves. The bridges have to say, because they run on a queue of their own that
        // State never learns about -- a Vulkan game creates no D3D12 swapchain, so nothing ever sets
        // currentCommandQueue and the cost went unreported.
        auto* queue = timingQueue != nullptr ? timingQueue
                                             : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

        if (queue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime(queue); ms.has_value())
            {
                g_lastGpuTime = ms;
                g_avgGpuTime = g_avgGpuTime <= 0.0 ? ms.value() : g_avgGpuTime * 0.95 + ms.value() * 0.05;

                // The reading is the frame two starts ago: its kind is in the slot after this one.
                const int kind = g_timeKind[(g_timeStarts + 1) % 3];
                double& cost = kind == 1 ? g_costRefresh : kind == 3 ? g_costLand : g_costCached;

                if (kind != 0)
                    cost = cost <= 0.0 ? ms.value() : cost * 0.9 + ms.value() * 0.1;
            }

            if (g_ngxTime != nullptr)
            {
                if (auto ngx = g_ngxTime->ReadGpuTime(queue); ngx.has_value())
                    g_lastNgxTime = ngx;
            }

            // The split, once every few hundred frames. What is worth reading is not the total but the
            // remainder: the model's cost is NVIDIA's to set, and everything else is ours.
            static unsigned long long lastSplitLog = 0;

            // Skipped with the edit cache on: the model's timer then belongs to some earlier frame
            // while the total belongs to this one, and the difference means nothing.
            if (g_lastGpuTime.has_value() && g_lastNgxTime.has_value() && g_frames - lastSplitLog > 600 &&
                !EffCache(*Config::Instance()))
            {
                lastSplitLog = g_frames;
                const double total = g_lastGpuTime.value();
                const double ngx = g_lastNgxTime.value();
                LOG_INFO("DLSS-NR cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms ours ({:.0f}% ours)",
                         total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
            }
        }
    }
}
} // namespace

// ---------------------------------------------------------------------------------------------
// The A/B benchmark: the same scene with Neural Rendering off, as OptiScaler ships it (the model every
// frame, full size), with the user's settings and -- when asked -- with those settings run before the
// upscaler (pre-SR), one after the other and measured the same way. Each phase ends with a short
// capture: a picture of the frame and how much it flickers from one frame to the next. The result is a
// page beside OptiScaler, pictures included.
//
// Frame time is the interval between upscaler evaluates -- the frames the game actually renders. With
// frame generation the screen shows more than that, but generated frames cost nothing here and would
// only hide the difference being measured.
//
// No setting is written: each phase is a comparison override (see ActiveCompare), so the "your
// settings" phases run exactly what the user has, and cancelling leaves nothing behind.
// ---------------------------------------------------------------------------------------------
namespace
{
struct BenchState
{
    bool active = false;
    bool captures = true;
    std::vector<int> plan; // the phases to run, in order
    size_t step = 0;
    int phase = 0;
    bool capturing = false;
    LARGE_INTEGER phaseStart {};
    LARGE_INTEGER last {};
    std::vector<float> frames;
    double gpuSum = 0.0;
    unsigned int gpuCount = 0;

    DlssNr::BenchmarkResult results[DlssNr::kBenchmarkPhases];
    std::vector<float> series[DlssNr::kBenchmarkPhases];

    std::filesystem::path folder;
    std::string report;
    std::string settings;
    unsigned int renderWidth = 0;
    unsigned int renderHeight = 0;

    // Counters at the start of the phase: where the pass ran, and the edit cache's runs.
    unsigned long long passBefore0 = 0;
    unsigned long long passAfter0 = 0;
    unsigned long long refreshes0 = 0;
    unsigned long long cached0 = 0;
};

BenchState g_bench;

constexpr double kBenchWarmup = 3.0;  // seconds: model rebuilds, history settles
constexpr double kBenchMeasure = 8.0; // seconds measured per phase
constexpr unsigned int kBenchShotFrames = 10;

double Seconds(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double) (b.QuadPart - a.QuadPart) / (double) f.QuadPart;
}

int ComparisonFor(int phase)
{
    switch (phase)
    {
    case 0: return (int) DlssNr::CompareMode::Off;
    case 1: return (int) DlssNr::CompareMode::Vanilla;
    case 3: return kCompareOtherPlacement;
    default: return (int) DlssNr::CompareMode::Yours; // 2, and the style and pass phases 4 to 7
    }
}

// The other pass count the pass phase measures: 2 if yours run one, else 1.
int OtherPasses() { return Config::Instance()->DlssNrPasses.value_or_default() > 1 ? 1 : 2; }

std::string PictureName(int phase) { return std::format("mode{}.png", phase); }

// The names on the page, in French: the page is read by the person who plays, not by the code.
const char* PageName(int phase)
{
    switch (phase)
    {
    case 0: return "DLSS 5 d&eacute;sactiv&eacute;";
    case 1: return "DLSS 5 d'origine (OptiScaler)";
    case 2: return "DLSS 5 optimis&eacute; (tes r&eacute;glages)";
    case 3:
        return Config::Instance()->DlssNrPreSr.value_or_default() ? "DLSS 5 optimis&eacute;, apr&egrave;s l'upscaler"
                                                                   : "DLSS 5 optimis&eacute; + pre-SR";
    case 4: return "Style Default";
    case 5: return "Style Natural";
    case 6: return "Style Cin&eacute;matique";
    case 7: return OtherPasses() > 1 ? "Multi-pass : 2 passes" : "Multi-pass : 1 passe";
    default: return "?";
    }
}

const char* PageColour(int phase)
{
    switch (phase)
    {
    case 0: return "#8b949e";
    case 1: return "#f0883e";
    case 2: return "#3fb950";
    case 3: return "#58a6ff";
    case 4: return "#d2a8ff";
    case 5: return "#79c0ff";
    case 6: return "#ffa657";
    default: return "#f778ba";
    }
}

void BenchApplyPhase()
{
    g_bench.phase = g_bench.plan[g_bench.step];
    g_bench.capturing = false;
    g_benchCompare = ComparisonFor(g_bench.phase);
    g_benchStyle = (g_bench.phase >= 4 && g_bench.phase <= 6) ? g_bench.phase - 4 : -1;
    g_benchPasses = g_bench.phase == 7 ? OtherPasses() : -1;
    g_bench.frames.clear();
    g_bench.gpuSum = 0.0;
    g_bench.gpuCount = 0;
    g_bench.passBefore0 = g_passBefore;
    g_bench.passAfter0 = g_passAfter;
    g_bench.refreshes0 = g_cache != nullptr ? g_cache->GetStatus().refreshes : 0;
    g_bench.cached0 = g_cache != nullptr ? g_cache->GetStatus().cached : 0;
    QueryPerformanceCounter(&g_bench.phaseStart);
    g_bench.last = g_bench.phaseStart;
}

void BenchFinishPhase()
{
    auto& r = g_bench.results[g_bench.phase];
    r = {};

    if (g_bench.frames.size() >= 10)
    {
        std::vector<float> sorted = g_bench.frames;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0.0;

        for (float f : sorted)
            sum += f;

        const double avg = sum / sorted.size();

        // 1% low: the frame rate of the slowest 1% of frames, the number that says whether it stutters.
        const size_t from = (size_t) (sorted.size() * 0.99);
        double slow = 0.0;

        for (size_t i = from; i < sorted.size(); ++i)
            slow += sorted[i];

        slow /= std::max<size_t>(1, sorted.size() - from);

        r.valid = true;
        r.fps = avg > 0.0 ? 1000.0 / avg : 0.0;
        r.low1 = slow > 0.0 ? 1000.0 / slow : 0.0;
        r.frameMs = avg;
        r.nrMs = g_bench.gpuCount > 0 ? g_bench.gpuSum / g_bench.gpuCount : 0.0;
        r.frames = (unsigned int) sorted.size();

        // Pacing, in the order the frames came: the step from each frame time to the next.
        std::vector<float> steps;
        steps.reserve(g_bench.frames.size());

        for (size_t i = 1; i < g_bench.frames.size(); ++i)
            steps.push_back(std::abs(g_bench.frames[i] - g_bench.frames[i - 1]));

        if (!steps.empty())
        {
            double stepSum = 0.0;

            for (float v : steps)
                stepSum += v;

            r.pacingMs = stepSum / steps.size();
            std::sort(steps.begin(), steps.end());
            r.pacingP99 = steps[std::min(steps.size() - 1, (size_t) (steps.size() * 0.99))];
        }
    }

    // Where the pass really ran, and whether pre-SR, asked for, fell back after the upscaler (the game
    // calls Ray Reconstruction, which has no image before its upscale to work on).
    {
        const unsigned long long before = g_passBefore - g_bench.passBefore0;
        const unsigned long long after = g_passAfter - g_bench.passAfter0;
        const Config& cfg = *Config::Instance();
        r.placement = (before == 0 && after == 0) || g_bench.phase == 0 ? 0 : (before >= after ? 2 : 1);
        r.preSrFellBack = r.placement == 1 && EffEnabled(cfg) && EffPreSr(cfg);

        if (g_cache != nullptr && EffCache(cfg) && g_bench.phase != 0)
        {
            const auto cs = g_cache->GetStatus();
            const unsigned long long runs = cs.refreshes - g_bench.refreshes0;
            const unsigned long long carried = cs.cached - g_bench.cached0;

            if (runs + carried > 0)
                r.modelShare = (float) runs / (float) (runs + carried);

            r.rejected = cs.lastRejected + cs.printRejected;
        }
    }

    g_bench.series[g_bench.phase] = g_bench.frames;

    if (g_bench.phase != 0 && g_nr.guideWidth != 0)
    {
        g_bench.renderWidth = g_nr.guideWidth;
        g_bench.renderHeight = g_nr.guideHeight;
    }

    LOG_INFO("DLSS-NR benchmark: {} -> {:.1f} fps, 1% low {:.1f}, frame {:.2f} ms, NR pass {:.2f} ms ({} frames), "
             "pacing {:.2f} ms (p99 {:.2f}), placement {}{}, model on {:.0f}% of frames{}",
             DlssNr::BenchmarkPhaseName(g_bench.phase), r.fps, r.low1, r.frameMs, r.nrMs, r.frames, r.pacingMs,
             r.pacingP99, r.placement == 2 ? "before the upscaler" : r.placement == 1 ? "after the upscaler" : "none",
             r.preSrFellBack ? " (pre-SR fell back)" : "", r.modelShare < 0.0f ? 100.0f : 100.0f * r.modelShare,
             g_async.active ? std::format(", in the background ({:.2f} ms on our queue)", g_async.modelMs)
                            : std::string());
}

// The frame times of one phase as an SVG polyline, averaged down to at most `points` points.
std::string Polyline(const std::vector<float>& series, double maxMs, double width, double height, size_t points)
{
    if (series.empty() || maxMs <= 0.0)
        return {};

    const size_t n = std::min(points, series.size());
    std::string out;

    for (size_t i = 0; i < n; ++i)
    {
        const size_t a = i * series.size() / n;
        const size_t b = std::max(a + 1, (i + 1) * series.size() / n);
        double sum = 0.0;

        for (size_t k = a; k < b; ++k)
            sum += series[k];

        const double v = sum / (double) (b - a);
        const double x = n > 1 ? width * (double) i / (double) (n - 1) : 0.0;
        const double y = height - std::min(v / maxMs, 1.0) * height;
        out += std::format("{:.1f},{:.1f} ", x, y);
    }

    return out;
}

void WriteReport()
{
    const auto& vanilla = g_bench.results[1];
    const auto& yours = g_bench.results[2];

    std::time_t t = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &t);
    char when[64];
    std::strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &local);

    unsigned int outW = 0, outH = 0;

    for (int p : g_bench.plan)
    {
        if (g_bench.results[p].shotWidth != 0)
        {
            outW = g_bench.results[p].shotWidth;
            outH = g_bench.results[p].shotHeight;
            break;
        }
    }

    std::string game = Util::ExePath().filename().string();
    std::string h;
    h.reserve(64 * 1024);

    h += "<!doctype html><html lang=\"fr\"><head><meta charset=\"utf-8\">"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
         "<title>Benchmark DLSS 5</title><style>"
         ":root{--bg:#0d1117;--card:#161b22;--line:#30363d;--text:#e6edf3;--dim:#8b949e;--good:#3fb950;--bad:#f85149}"
         "@media (prefers-color-scheme: light){:root{--bg:#f6f8fa;--card:#fff;--line:#d0d7de;--text:#1f2328;--dim:#59636e}}"
         "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);"
         "font:15px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif}"
         "main{max-width:1180px;margin:0 auto;padding:28px 16px 60px}"
         "h1{font-size:26px;margin:0 0 4px}h2{font-size:18px;margin:34px 0 12px}"
         ".dim{color:var(--dim)}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px}"
         ".hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;margin-top:20px}"
         ".big{font-size:34px;font-weight:700;line-height:1.1}.good{color:var(--good)}.bad{color:var(--bad)}"
         "table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}"
         "th,td{padding:9px 10px;border-bottom:1px solid var(--line);text-align:right;white-space:nowrap}"
         "th:first-child,td:first-child{text-align:left}th{color:var(--dim);font-weight:600;font-size:13px}"
         ".scroll{overflow-x:auto}.bar{height:22px;border-radius:5px;min-width:2px}"
         ".bars div.row{display:grid;grid-template-columns:260px 1fr 70px;gap:10px;align-items:center;margin:8px 0}"
         ".shots{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:14px}"
         ".shots img{width:100%;border-radius:8px;display:block;border:1px solid var(--line)}"
         ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:7px}"
         "svg{width:100%;height:auto;display:block}.note{border-left:3px solid #58a6ff;padding:10px 14px;margin-top:16px}"
         "@media (max-width:640px){.bars div.row{grid-template-columns:1fr 60px}.bars div.row>span:first-child{grid-column:1/-1}}"
         "</style></head><body><main>";

    h += std::format("<h1>Benchmark DLSS 5</h1><div class=\"dim\">{} &middot; {}", game, when);

    if (outW != 0)
        h += std::format(" &middot; sortie {}x{}", outW, outH);

    if (g_bench.renderWidth != 0)
        h += std::format(" &middot; rendu {}x{}", g_bench.renderWidth, g_bench.renderHeight);

    h += "</div>";

    // The answer first.
    h += "<div class=\"hero\">";

    for (int p : g_bench.plan)
    {
        const auto& r = g_bench.results[p];

        if (!r.valid)
            continue;

        std::string delta;

        if (p != 1 && vanilla.valid && vanilla.fps > 0.0)
        {
            const double pct = 100.0 * (r.fps / vanilla.fps - 1.0);
            delta = std::format("<div class=\"{}\">{:+.0f}% vs d'origine</div>", pct >= 0.0 ? "good" : "bad", pct);
        }
        else if (p == 1)
        {
            delta = "<div class=\"dim\">r&eacute;f&eacute;rence</div>";
        }

        h += std::format("<div class=\"card\"><div class=\"dim\"><span class=\"dot\" style=\"background:{}\"></span>{}</div>"
                         "<div class=\"big\">{:.1f} <span class=\"dim\" style=\"font-size:16px\">FPS</span></div>{}</div>",
                         PageColour(p), PageName(p), r.fps, delta);
    }

    h += "</div>";

    h += "<div class=\"card note\"><b>Pourquoi le compteur du jeu ne montre pas toujours la diff&eacute;rence&nbsp;:</b> "
         "ces chiffres sont les images que le jeu <i>calcule</i>. Avec la g&eacute;n&eacute;ration d'images (MFG x2 &agrave; x6), "
         "le compteur affich&eacute; multiplie ce nombre puis plafonne &agrave; la fr&eacute;quence de l'&eacute;cran "
         "(Reflex / V-Sync). Exemple en x6 sur un &eacute;cran 240&nbsp;Hz&nbsp;: 31&nbsp;FPS de base donnent 186, 48 en "
         "donneraient 288 mais l'&eacute;cran coupe vers 225&nbsp;: les deux semblent proches alors que l'un calcule 55% "
         "d'images r&eacute;elles en plus. Le gain se voit dans la latence, la fluidit&eacute; de base et le ghosting "
         "du MFG (moins d'images invent&eacute;es entre deux vraies). Pour le voir en jeu&nbsp;: la touche de comparaison "
         "(F6 par d&eacute;faut) bascule optimis&eacute; / d'origine / d&eacute;sactiv&eacute; et affiche ces FPS "
         "r&eacute;els.</div>";

    // The numbers.
    h += "<h2>Mesures</h2><div class=\"card scroll\"><table><tr><th>Mode</th><th>FPS</th><th>1% low</th>"
         "<th>Temps d'image</th><th>Co&ucirc;t DLSS 5</th><th>vs d'origine</th><th>Scintillement moyen</th>"
         "<th>Scintillement p90</th><th>R&eacute;gularit&eacute;</th><th>Mod&egrave;le</th><th>Placement</th></tr>";

    for (int p : g_bench.plan)
    {
        const auto& r = g_bench.results[p];

        if (!r.valid)
            continue;

        std::string versus = "&mdash;";

        if (p != 1 && vanilla.valid && vanilla.fps > 0.0)
        {
            const double pct = 100.0 * (r.fps / vanilla.fps - 1.0);
            versus = std::format("<span class=\"{}\">{:+.0f}%</span>", pct >= 0.0 ? "good" : "bad", pct);
        }

        const std::string model = p == 0 ? std::string("&mdash;")
                                  : r.modelShare < 0.0f ? std::string("100%")
                                                        : std::format("{:.0f}%", 100.0f * r.modelShare);
        const std::string placement = r.placement == 2   ? std::string("avant l'upscaler")
                                      : r.placement == 1 ? std::string(r.preSrFellBack ? "apr&egrave;s (repli)"
                                                                                       : "apr&egrave;s l'upscaler")
                                                         : std::string("&mdash;");

        h += std::format("<tr><td><span class=\"dot\" style=\"background:{}\"></span>{}</td><td><b>{:.1f}</b></td>"
                         "<td>{:.1f}</td><td>{:.2f} ms</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td>"
                         "<td>{:.2f} ms</td><td>{}</td><td>{}</td></tr>",
                         PageColour(p), PageName(p), r.fps, r.low1, r.frameMs,
                         p == 0 ? std::string("&mdash;") : std::format("{:.2f} ms", r.nrMs), versus,
                         r.flickerValid ? std::format("{:.2f}%", r.flickerMean) : std::string("&mdash;"),
                         r.flickerValid ? std::format("{:.2f}%", r.flickerP95) : std::string("&mdash;"), r.pacingMs,
                         model, placement);
    }

    bool fellBack = false;

    for (int p : g_bench.plan)
        fellBack = fellBack || (g_bench.results[p].valid && g_bench.results[p].preSrFellBack);

    h += "</table></div><p class=\"dim\">FPS et 1% low&nbsp;: images calcul&eacute;es par le jeu pendant 8&nbsp;s apr&egrave;s "
         "3&nbsp;s de stabilisation, sans la g&eacute;n&eacute;ration d'images. Co&ucirc;t DLSS 5&nbsp;: la passe enti&egrave;re "
         "sur le GPU, en moyenne. Scintillement&nbsp;: variation de luminosit&eacute; d'une image &agrave; la suivante sur 10 "
         "images cons&eacute;cutives, cam&eacute;ra immobile, sans les 5% de pixels qui bougent le plus (objets anim&eacute;s, "
         "particules)&nbsp;; plus bas = plus stable, le mode d&eacute;sactiv&eacute; donne le bruit propre au jeu. "
         "R&eacute;gularit&eacute;&nbsp;: de combien le temps d'image change en moyenne d'une image &agrave; la suivante "
         "(un mod&egrave;le lanc&eacute; une image sur deux se voit ici, pas dans la moyenne)&nbsp;; plus bas = plus "
         "fluide. Mod&egrave;le&nbsp;: part des images o&ugrave; le mod&egrave;le a vraiment tourn&eacute;. "
         "Placement&nbsp;: o&ugrave; la passe a r&eacute;ellement tourn&eacute;.</p>";

    if (fellBack)
        h += "<div class=\"card note\"><b>Pre-SR en repli&nbsp;:</b> le pre-SR &eacute;tait demand&eacute; mais le jeu "
             "n'appelle pas DLSS Super Resolution (Ray Reconstruction activ&eacute;, le plus souvent)&nbsp;: la passe a "
             "tourn&eacute; apr&egrave;s l'upscaler. Pour mesurer le pre-SR, d&eacute;sactive Ray Reconstruction dans le "
             "jeu.</div>";

    // Bars.
    double best = 0.0;

    for (int p : g_bench.plan)
        best = std::max(best, g_bench.results[p].fps);

    h += "<h2>Images calcul&eacute;es par seconde</h2><div class=\"card bars\">";

    for (int p : g_bench.plan)
    {
        const auto& r = g_bench.results[p];

        if (!r.valid || best <= 0.0)
            continue;

        h += std::format("<div class=\"row\"><span>{}</span><div class=\"bar\" style=\"width:{:.1f}%;background:{}\"></div>"
                         "<span><b>{:.1f}</b></span></div>",
                         PageName(p), 100.0 * r.fps / best, PageColour(p), r.fps);
    }

    h += "</div>";

    // Frame times.
    std::vector<float> all;

    for (int p : g_bench.plan)
        all.insert(all.end(), g_bench.series[p].begin(), g_bench.series[p].end());

    if (!all.empty())
    {
        std::sort(all.begin(), all.end());
        const double top = std::max(10.0, std::ceil(all[(size_t) (all.size() * 0.995)] * 1.15 / 5.0) * 5.0);
        const double W = 1000.0, H = 260.0;

        h += "<h2>Temps de chaque image (plus bas = plus rapide, plus plat = plus r&eacute;gulier)</h2><div class=\"card\">";
        h += std::format("<svg viewBox=\"-40 -10 {} {}\" role=\"img\" aria-label=\"temps d'image\">", W + 50, H + 34);

        for (int g = 0; g <= 4; ++g)
        {
            const double y = H * g / 4.0;
            h += std::format("<line x1=\"0\" x2=\"{}\" y1=\"{:.1f}\" y2=\"{:.1f}\" stroke=\"currentColor\" opacity=\"0.12\"/>"
                             "<text x=\"-6\" y=\"{:.1f}\" font-size=\"12\" text-anchor=\"end\" fill=\"currentColor\" "
                             "opacity=\"0.6\">{:.0f} ms</text>",
                             W, y, y, y + 4, top * (1.0 - g / 4.0));
        }

        for (int p : g_bench.plan)
        {
            const std::string pts = Polyline(g_bench.series[p], top, W, H, 400);

            if (!pts.empty())
                h += std::format("<polyline fill=\"none\" stroke=\"{}\" stroke-width=\"2\" points=\"{}\"/>", PageColour(p), pts);
        }

        h += std::format("<text x=\"{}\" y=\"{}\" font-size=\"12\" text-anchor=\"end\" fill=\"currentColor\" opacity=\"0.6\">"
                         "8 secondes de mesure par mode</text></svg></div>",
                         W, H + 26);
    }

    // Pictures.
    bool anyPicture = false;

    for (int p : g_bench.plan)
        anyPicture = anyPicture || g_bench.results[p].picture;

    if (anyPicture)
    {
        h += "<h2>Captures (m&ecirc;me sc&egrave;ne, apr&egrave;s chaque mesure)</h2><div class=\"shots\">";

        for (int p : g_bench.plan)
        {
            const auto& r = g_bench.results[p];

            if (!r.picture)
                continue;

            const std::string file = PictureName(p);
            h += std::format("<figure class=\"card\" style=\"margin:0\"><a href=\"{0}\" target=\"_blank\"><img src=\"{0}\" "
                             "alt=\"{1}\" loading=\"lazy\"></a><figcaption style=\"margin-top:8px\"><span class=\"dot\" "
                             "style=\"background:{2}\"></span><b>{1}</b> &middot; {3:.1f} FPS</figcaption></figure>",
                             file, PageName(p), PageColour(p), r.fps);
        }

        h += "</div><p class=\"dim\">Image de sortie de l'upscaler apr&egrave;s DLSS&nbsp;5, avant l'interface, "
             "d&eacute;velopp&eacute;e avec la m&ecirc;me exposition pour tous les modes (les couleurs peuvent diff&eacute;rer "
             "un peu du rendu final du jeu). Clique pour la taille r&eacute;elle.</p>";
    }

    if (!g_bench.settings.empty())
        h += std::format("<h2>R&eacute;glages test&eacute;s</h2><div class=\"card dim\">{}</div>", g_bench.settings);

    if (vanilla.valid && yours.valid)
        h += std::format("<p class=\"dim\" style=\"margin-top:30px\">En r&eacute;sum&eacute;&nbsp;: {:.1f} &rarr; {:.1f} FPS "
                         "calcul&eacute;s ({:+.0f}%), co&ucirc;t de DLSS&nbsp;5 {:.1f} &rarr; {:.1f} ms par image.</p>",
                         vanilla.fps, yours.fps, 100.0 * (yours.fps / vanilla.fps - 1.0), vanilla.nrMs, yours.nrMs);

    h += "</main></body></html>";

    std::error_code ec;
    std::filesystem::create_directories(g_bench.folder, ec);
    const auto page = g_bench.folder / "rapport.html";
    FILE* f = _wfopen(page.wstring().c_str(), L"wb");

    if (f != nullptr)
    {
        fwrite(h.data(), 1, h.size(), f);
        fclose(f);
        g_bench.report = page.string();

        // And a fixed name for the latest one, beside OptiScaler.
        const auto latest = Util::DllPath().remove_filename() / "dlssnr-benchmark.html";
        const std::string rel = std::filesystem::relative(page, latest.parent_path(), ec).generic_string();
        FILE* l = _wfopen(latest.wstring().c_str(), L"wb");

        if (l != nullptr)
        {
            const std::string redirect = std::format(
                "<!doctype html><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"0; url={0}\">"
                "<title>Benchmark DLSS 5</title><a href=\"{0}\">Dernier rapport</a>",
                rel.empty() ? page.generic_string() : rel);
            fwrite(redirect.data(), 1, redirect.size(), l);
            fclose(l);
        }

        LOG_INFO("DLSS-NR benchmark report written to {}", g_bench.report);
    }
}

void BenchEnd()
{
    g_benchCompare = -1;
    g_benchStyle = -1;
    g_benchPasses = -1;
    g_bench.active = false;
    g_bench.capturing = false;

    // A copy on disk, so a result can be compared with the next build's.
    const auto path = Util::DllPath().remove_filename() / "dlssnr-benchmark.txt";
    FILE* f = _wfopen(path.wstring().c_str(), L"a");

    if (f != nullptr)
    {
        fprintf(f, "--- DLSS-NR benchmark (rendered frames, frame generation excluded)\n");

        for (int p : g_bench.plan)
        {
            const auto& r = g_bench.results[p];

            if (r.valid)
                fprintf(f, "%-44s %7.1f fps  1%% low %7.1f  frame %6.2f ms  NR pass %6.2f ms  flicker %5.2f%%\n",
                        DlssNr::BenchmarkPhaseName(p), r.fps, r.low1, r.frameMs, r.nrMs,
                        r.flickerValid ? r.flickerMean : 0.0f);
        }

        fclose(f);
    }

    WriteReport();
}

void BenchAdvance()
{
    if (++g_bench.step >= g_bench.plan.size())
    {
        BenchEnd();
        return;
    }

    BenchApplyPhase();
}

void BenchTick()
{
    if (!g_bench.active)
        return;

    // The capture after a phase: wait for it, picture written and all, before the next phase starts.
    if (g_bench.capturing)
    {
        if (DlssNrShot::Busy())
            return;

        const auto shot = DlssNrShot::Get(g_bench.phase);
        auto& r = g_bench.results[g_bench.phase];
        r.flickerValid = shot.valid;
        r.flickerMean = shot.flickerMean;
        r.flickerP95 = shot.flickerP95;
        r.picture = shot.picture;
        r.shotWidth = shot.width;
        r.shotHeight = shot.height;
        BenchAdvance();
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const double sincePhase = Seconds(g_bench.phaseStart, now);
    const double frameMs = Seconds(g_bench.last, now) * 1000.0;
    g_bench.last = now;

    if (sincePhase > kBenchWarmup && frameMs > 0.0 && frameMs < 1000.0)
    {
        g_bench.frames.push_back((float) frameMs);

        if (g_bench.phase != 0 && g_lastGpuTime.has_value())
        {
            g_bench.gpuSum += g_lastGpuTime.value();
            g_bench.gpuCount++;
        }
    }

    if (sincePhase >= kBenchWarmup + kBenchMeasure)
    {
        BenchFinishPhase();

        if (g_bench.captures)
        {
            DlssNrShot::Request(g_bench.phase, kBenchShotFrames, g_bench.folder / PictureName(g_bench.phase));
            g_bench.capturing = true;
            return;
        }

        BenchAdvance();
    }
}

// The frames the game renders per second, for the line on screen: a ring of recent frame times.
struct LiveState
{
    LARGE_INTEGER last {};
    float ring[120] = {};
    unsigned int count = 0;
    unsigned int head = 0;
    LARGE_INTEGER lastSwitch {};
};

LiveState g_live;

void LiveTick()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_live.last.QuadPart != 0)
    {
        const double ms = Seconds(g_live.last, now) * 1000.0;

        if (ms > 0.0 && ms < 500.0)
        {
            g_live.ring[g_live.head] = (float) ms;
            g_live.head = (g_live.head + 1) % 120;
            g_live.count = std::min(g_live.count + 1, 120u);
        }
    }

    g_live.last = now;
}
} // namespace

namespace DlssNr
{
const char* BenchmarkPhaseName(int phase)
{
    switch (phase)
    {
    case 0: return "Neural Rendering off";
    case 1: return "DLSS 5 as OptiScaler ships it";
    case 2: return "DLSS 5, your settings";
    case 3:
        return Config::Instance()->DlssNrPreSr.value_or_default() ? "DLSS 5, your settings, after the upscaler"
                                                                   : "DLSS 5, your settings, pre-SR";
    case 4: return "DLSS 5, your settings, style Default";
    case 5: return "DLSS 5, your settings, style Natural";
    case 6: return "DLSS 5, your settings, style Cinematic";
    case 7: return OtherPasses() > 1 ? "DLSS 5, your settings, 2 passes" : "DLSS 5, your settings, 1 pass";
    default: return "?";
    }
}

void StartBenchmark(bool includeOff, bool includeOtherPlacement, bool captures, bool includeStyles,
                    bool includePasses)
{
    if (g_bench.active)
        return;

    const Config& cfg = *Config::Instance();

    for (auto& r : g_bench.results)
        r = {};

    for (auto& s : g_bench.series)
        s.clear();

    g_bench.plan.clear();

    if (includeOff)
        g_bench.plan.push_back(0);

    g_bench.plan.push_back(1);
    g_bench.plan.push_back(2);

    // The same settings at the other placement: before the upscaler if yours run after it, and after
    // it if yours run before -- so the page always shows what the placement is worth.
    if (includeOtherPlacement)
        g_bench.plan.push_back(3);

    // The three looks of the model, on your settings: each rebuilds the model, which the warm-up covers.
    if (includeStyles)
    {
        g_bench.plan.push_back(4);
        g_bench.plan.push_back(5);
        g_bench.plan.push_back(6);
    }

    if (includePasses)
        g_bench.plan.push_back(7);

    std::time_t t = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);

    g_bench.folder = Util::DllPath().remove_filename() / "dlssnr-benchmark" / stamp;
    g_bench.report.clear();
    g_bench.captures = captures;
    g_bench.renderWidth = g_bench.renderHeight = 0;

    // What "your settings" is, written on the page so a result says what it measured.
    static const char* kStyleNames[] = { "Default", "Natural", "Cin&eacute;matique" };
    g_bench.settings = std::format(
        "Style {} &middot; {} passe(s) &middot; mod&egrave;le {:.0f}%{} &middot; {} &middot; cache {} &middot; point blanc {}",
        kStyleNames[std::min(cfg.DlssNrStyle.value_or_default(), 2u)], std::clamp(cfg.DlssNrPasses.value_or_default(), 1u, 3u),
        cfg.DlssNrWorkingScale.value_or_default() * 100.0f,
        cfg.DlssNrJbuUpsample.value_or_default() ? " + agrandissement guid&eacute;" : "",
        cfg.DlssNrPreSr.value_or_default() ? "avant l'upscaler (pre-SR)" : "apr&egrave;s l'upscaler",
        cfg.DlssNrCacheEnabled.value_or_default()
            ? std::format("1 image sur {}{}{}", cfg.DlssNrCacheInterval.value_or_default(),
                          cfg.DlssNrCacheAdaptive.value_or_default() ? ", adaptatif" : "",
                          cfg.DlssNrCacheCrossfade.value_or_default() ? ", transitions douces" : "")
            : std::string("d&eacute;sactiv&eacute; (mod&egrave;le &agrave; chaque image)"),
        cfg.DlssNrWhitePointSource.value_or_default() == 3 ? "automatique" : "manuel");

    DlssNrShot::Reset();

    g_bench.step = 0;
    g_bench.active = true;
    BenchApplyPhase();
    LOG_INFO("DLSS-NR benchmark started ({} phases, captures {})", g_bench.plan.size(), captures ? "on" : "off");
}

void CancelBenchmark()
{
    g_benchCompare = -1;
    g_benchStyle = -1;
    g_benchPasses = -1;
    g_bench.active = false;
    g_bench.capturing = false;
}

BenchmarkStatus GetBenchmarkStatus()
{
    BenchmarkStatus s {};
    s.active = g_bench.active;
    s.phase = g_bench.phase;
    s.capturing = g_bench.capturing;
    s.step = (int) g_bench.step;
    s.steps = (int) g_bench.plan.size();

    if (g_bench.active && !g_bench.capturing)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double total = kBenchWarmup + kBenchMeasure;
        s.phaseProgress = (float) std::clamp(Seconds(g_bench.phaseStart, now) / total, 0.0, 1.0);
        s.warmingUp = Seconds(g_bench.phaseStart, now) < kBenchWarmup;
    }

    for (int p = 0; p < kBenchmarkPhases; ++p)
        s.results[p] = g_bench.results[p];

    s.report = g_bench.report;
    return s;
}

CompareMode GetCompareMode() { return (CompareMode) g_userCompare; }

void SetCompareMode(CompareMode m)
{
    g_userCompare = std::clamp((int) m, 0, 2);
    QueryPerformanceCounter(&g_live.lastSwitch);
    LOG_INFO("DLSS-NR comparison: {}", CompareModeName((CompareMode) g_userCompare));
}

void CycleCompareMode() { SetCompareMode((CompareMode) ((g_userCompare + 1) % 3)); }

const char* CompareModeName(CompareMode m)
{
    switch (m)
    {
    case CompareMode::Vanilla: return "Vanilla (as OptiScaler ships it)";
    case CompareMode::Off: return "Off";
    default: return "Optimised (your settings)";
    }
}

double SecondsSinceCompareSwitch()
{
    if (g_live.lastSwitch.QuadPart == 0)
        return 1e9;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return Seconds(g_live.lastSwitch, now);
}

LiveStats GetLiveStats()
{
    const Config& cfg = *Config::Instance();
    LiveStats s {};

    if (g_live.count > 0)
    {
        double sum = 0.0;

        for (unsigned int i = 0; i < g_live.count; ++i)
            sum += g_live.ring[i];

        s.renderedFps = sum > 0.0 ? 1000.0 * g_live.count / sum : 0.0;
    }

    s.valid = g_live.count > 0;
    s.enabled = EffEnabled(cfg);
    s.preSr = s.enabled && EffPreSr(cfg);
    s.cache = s.enabled && EffCache(cfg);
    s.nrMs = s.enabled && g_nr.feature != nullptr ? g_avgGpuTime : 0.0;
    s.modelWidth = g_nr.workWidth;
    s.passes = s.enabled ? g_nr.passesRun : 0;
    s.style = EffStyle(cfg);
    s.modelHeight = g_nr.workHeight;
    s.benchmark = g_bench.active;
    s.mode = g_bench.active ? BenchmarkPhaseName(g_bench.phase) : CompareModeName((CompareMode) g_userCompare);
    s.compare = (CompareMode) g_userCompare;
    return s;
}
} // namespace DlssNr

namespace DlssNr
{
void RetryAfterFailure()
{
    g_nr.failed = false;
    g_nr.reason = "";
    g_nr.reset = true;

}

// What the game says about this frame, from its parameter block: the depth convention, HDR, a reset,
// the real render size, the motion vector scale and its exposure. The same for either placement.
static void ReadFrameInfo(NVSDK_NGX_Parameter* params, DlssNrFrameInfo& frame)
{
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &frame.JitterX) != NVSDK_NGX_Result_Success)
        frame.JitterX = 0.0f;

    if (params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &frame.JitterY) != NVSDK_NGX_Result_Success)
        frame.JitterY = 0.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        struct ExposureReport
        {
            bool valid;
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;
        };

        static ExposureReport logged {};
        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.
        static float loggedExposure = -1.0f;

        if (g_nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - g_nr.gameExposure) > std::max(0.02f * g_nr.gameExposure, 1e-5f))
        {
            loggedExposure = g_nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            static float loggedScan = -1.0f;

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                if (g_nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, g_nr.gameExposure, scanned / g_nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }
}

// Reads the game's parameter block and runs the pass on what it finds.
//
// This is the call site's job, not the pass's. A caller that has the resources in hand -- a
// reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
// RunPass directly and never touches an NGX parameter block.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue)
{
    // Every rendered frame reaches here, Neural Rendering on or off, which is what lets the benchmark
    // time the off phase the same way as the others -- and what the line on screen counts.
    BenchTick();
    LiveTick();
    CheckCaptureTrigger();

    if (cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");

    // The benchmark's capture looks at the finished frame, whatever made it.
    auto lookAtResult = [&]()
    {
        if (target != nullptr)
            DlssNrShot::Tick(cmdList, target, OutputRestState());
    };

    // Pre-SR already ran on this frame, before the upscaler, which then upscaled its result.
    const bool preSrRan = g_preSr.ranThisFrame;
    g_preSr.ranThisFrame = false;

    if (preSrRan)
    {
        ++g_passBefore;
        lookAtResult();
        return;
    }

    if (!EffEnabled(*Config::Instance()))
    {
        ReportSkipOnce("it is switched off");
        lookAtResult();
        return;
    }

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr    ? "the parameters carried no output texture"
                       : depth == nullptr   ? "the parameters carried no depth"
                                            : "the parameters carried no motion vectors");
        return;
    }

    DlssNrFrameInfo frame {};
    ReadFrameInfo(params, frame);

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.
    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    device->Release();

    if (g_compose == nullptr)
    {
        ReportSkipOnce("the pass could not be created");
        return;
    }

    ++g_passAfter;
    g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue);
    lookAtResult();
}

// Pre-SR. The game's render-resolution colour is copied into a texture of ours, the whole pass runs on
// that copy -- cache, stabiliser, white point and all -- and the game's upscaler is handed the copy in
// place of its own colour, so it upscales the enhanced frame. The model works on the render resolution
// instead of the output's: 1484x835 rather than 2560x1440 for DLSS Balanced at 1440p, 2.9 times fewer
// pixels, and the upscaler's own temporal accumulation then steadies what it adds.
//
// The game's colour is read where the upscaler would read it and never written or transitioned; the
// parameter block gets its own pointer back after the upscaler (EndBeforeUpscale).
bool EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue)
{
    const Config& cfg = *Config::Instance();
    g_preSr.swapped = false;
    g_preSr.ranThisFrame = false;

    if (cmdList == nullptr || params == nullptr || !EffEnabled(cfg) || !EffPreSr(cfg))
    {
        if (g_preSr.wasOn)
        {
            g_preSr.wasOn = false;
            LOG_INFO("DLSS-NR: running after the upscaler again");
        }

        return false;
    }

    // Through whichever slot the game used: typed by a D3D12 game, untyped by OptiScaler's bridges.
    ID3D12Resource* colour = nullptr;
    bool typed = true;

    if (params->Get(NVSDK_NGX_Parameter_Color, &colour) != NVSDK_NGX_Result_Success || colour == nullptr)
    {
        void* untyped = nullptr;
        colour = nullptr;
        typed = false;

        if (params->Get(NVSDK_NGX_Parameter_Color, &untyped) == NVSDK_NGX_Result_Success)
            colour = (ID3D12Resource*) untyped;
    }

    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce("pre-SR: the parameters carried no colour, depth or motion vectors");
        return false;
    }

    const D3D12_RESOURCE_DESC cd = colour->GetDesc();

    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cd.SampleDesc.Count != 1)
    {
        ReportSkipOnce("pre-SR: the game's colour is not a plain 2D texture");
        return false;
    }

    DlssNrFrameInfo frame {};
    ReadFrameInfo(params, frame);

    ID3D12Device* device = nullptr;

    if (FAILED(colour->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    bool ready = false;

    {
        std::lock_guard<std::mutex> nrLock(g_nrMutex);

        // The edit cache's shader does the copy.
        if (g_cache == nullptr)
            g_cache = std::make_unique<DlssNrEditCache_Dx12>(device);

        ready = g_compose != nullptr && g_cache != nullptr && g_cache->IsInit() && !g_nr.failed;

        // Our copy: the colour's size, a typed member of its format family, with unordered access.
        const DXGI_FORMAT format = TypedGuideFormat(cd.Format);

        if (ready && g_preSr.tex != nullptr)
        {
            const D3D12_RESOURCE_DESC td = g_preSr.tex->GetDesc();

            if (td.Width != cd.Width || td.Height != cd.Height || td.Format != format)
                ParkNrResource(g_preSr.tex);
        }

        if (ready && g_preSr.tex == nullptr)
        {
            g_preSr.tex = CreateScratch(device, format, (unsigned int) cd.Width, cd.Height);
            g_preSr.texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            if (g_preSr.tex != nullptr)
                LOG_INFO("DLSS-NR pre-SR: running before the upscaler on its {}x{} colour (format {})",
                         (unsigned int) cd.Width, cd.Height, (int) format);
        }

        ready = ready && g_preSr.tex != nullptr;

        if (ready)
        {
            ScopedNrStateEnvelope envelope(cmdList);
            Barrier(cmdList, g_preSr.tex, g_preSr.texState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g_preSr.texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            ready = g_cache->CopyIn(cmdList, colour, g_preSr.tex);
        }
    }

    if (!ready)
    {
        ReportSkipOnce("pre-SR: could not prepare its copy of the colour");
        device->Release();
        return false;
    }

    // The pass, on the copy. Whatever it does or declines to do this frame, the copy holds at least the
    // game's own colour, so handing it over is always safe.
    g_preSrDispatch = true;
    g_compose->Dispatch(cmdList, g_preSr.tex, depth, motion, g_preSr.tex, frame, timingQueue);
    g_preSrDispatch = false;

    // Readable the way the upscaler reads its inputs: as a shader resource, or the state the user told
    // OptiScaler the game leaves its colour in.
    const D3D12_RESOURCE_STATES readState =
        cfg.ColorResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
            : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Barrier(cmdList, g_preSr.tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readState);
    g_preSr.texState = readState;

    g_preSr.original = colour;
    g_preSr.typed = typed;

    if (typed)
        params->Set(NVSDK_NGX_Parameter_Color, g_preSr.tex);
    else
        params->Set(NVSDK_NGX_Parameter_Color, (void*) g_preSr.tex);

    g_preSr.swapped = true;
    g_preSr.ranThisFrame = true;
    g_preSr.wasOn = true;
    device->Release();
    return true;
}

void EndBeforeUpscale(NVSDK_NGX_Parameter* params)
{
    if (!g_preSr.swapped || params == nullptr)
        return;

    if (g_preSr.typed)
        params->Set(NVSDK_NGX_Parameter_Color, g_preSr.original);
    else
        params->Set(NVSDK_NGX_Parameter_Color, (void*) g_preSr.original);

    g_preSr.swapped = false;
    g_preSr.original = nullptr;
}

// The pass. Resources in, nothing read from anywhere the caller cannot see.

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    // Every other entry point in this file takes the lock before touching g_nr; this one was reaching
    // EnsureForwarder without it.
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
    // into a subsystem on the game's own device rather than reading something we already hold.
    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
    const int bits = probe(snippet->wstring().c_str());

    // And the question NGX has an API for. Asked first because it creates nothing: if the feature
    // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*))
        GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        // The adapter the game is actually running on. Without it the query answers
        // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
        // the question -- that is what the first attempt got, on a 5080.
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            factory->EnumAdapters(0, &adapter);

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0        ? "SUPPORTED"
                              : (supported & 16)    ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)     ? "AdapterUnsupported"
                              : (supported & 2)     ? "DriverVersionUnsupported"
                              : (supported & 8)     ? "OSVersionBelowMinimum"
                              : (supported & 1)     ? "CheckNotPresent"
                                                    : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})",
             bits, (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    // Four ways of asking, since the feature has already said it supports this platform.
    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
        // with a minimum architecture this card meets, so the platform is not the obstacle and saying
        // otherwise here would be printing a conclusion the evidence does not carry.
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

// What the game offers by way of exposure, and what has been read from it. For the menu, so a user
// can see whether this game supplies one at all without having to read a log.
ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    s.unreliable = g_nr.exposureUnreliable;
    return s;
}

std::optional<double> LastGpuTime() { return g_lastGpuTime; }

float AutoWhitePoint() { return g_nr.autoWhiteValid ? std::exp2(g_nr.autoWhiteLog) : 0.0f; }

float AutoWhiteMeasured() { return g_nr.autoWhiteMeasured; }



void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

CacheStatus GetCacheStatus()
{
    CacheStatus s {};
    s.averageMs = g_avgGpuTime;
    s.asyncOn = g_async.active;
    s.asyncModelMs = g_async.modelMs;
    s.asyncLanded = g_async.landed;
    s.asyncWhy = g_async.why;

    if (g_cache == nullptr)
        return s;

    const auto c = g_cache->GetStatus();
    s.exists = true;
    s.historyValid = c.active;
    s.refreshes = c.refreshes;
    s.cached = c.cached;
    s.framesSinceRefresh = c.framesSinceRefresh;
    s.lastRejected = c.lastRejected;
    s.cumulativeRejected = c.cumulativeRejected;
    s.lastRefreshReason = c.lastRefreshReason;
    s.regime = c.regime;
    s.dumpWritten = c.dumpWritten;
    s.dumpActive = c.dumpActive;
    s.ghostFlags = c.ghostFlags;
    s.intervalNow = c.intervalNow;
    s.printRejected = c.printRejected;
    s.speed = c.speed;
    s.budgetFloor = c.budgetFloor;
    s.costRefresh = c.costRefresh;
    s.costCached = c.costCached;

    static_assert(CacheStatus::kStages == DlssNrEditCache_Dx12::Status::kStages);
    static_assert(CacheStatus::kPulsePhases == DlssNrEditCache_Dx12::Status::kPulsePhases);

    s.pulsePhases = c.pulsePhases;

    for (int i = 0; i < CacheStatus::kPulsePhases; ++i)
        s.pulseStep[i] = c.pulseStep[i];

    for (int i = 0; i < CacheStatus::kStages; ++i)
    {
        s.stageMs[i] = c.stageMs[i];
        s.stageName[i] = DlssNrEditCache_Dx12::StageName(i);
    }

    return s;
}

void RequestCacheDump()
{
    // The dump lives in the cache, and the cache only exists once it has been on. Asking before then
    // is answered in the log rather than silently dropped.
    if (g_cache == nullptr || !Config::Instance()->DlssNrCacheEnabled.value_or_default())
    {
        LOG_WARN("DLSS-NR edit cache dump: turn the edit cache on first");
        return;
    }

    g_cache->RequestDump(Config::Instance()->DlssNrCacheDumpFrames.value_or_default());
}

void Shutdown()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    AsyncDestroy();
    g_async = AsyncState {};

    for (auto& r : g_nrRetired)
    {
        if (r.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();
    }

    g_nrRetired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    for (void*& f : g_nr.passFeature)
    {
        if (f != nullptr && g_nr.release != nullptr)
            g_nr.release(f);

        f = nullptr;
    }

    if (g_preSr.tex != nullptr)
    {
        g_preSr.tex->Release();
        g_preSr.tex = nullptr;
    }

    if (g_nr.passIn != nullptr)
    {
        g_nr.passIn->Release();
        g_nr.passIn = nullptr;
    }

    g_preSr = {};
    DlssNrShot::Shutdown();

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    if (g_nr.outputNative != nullptr)
    {
        g_nr.outputNative->Release();
        g_nr.outputNative = nullptr;
    }

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    // The slots these flags describe have just been released, so nothing may vouch for what the next
    // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
    // transition within the same scene, and dropping to the slider for a few frames would be the
    // flicker the held value exists to prevent. The user switching the option off is the case where
    // the held value has to go, and that is handled at the edge in Dispatch.
    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    g_nr.meterFrames = 0;


    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }



    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    g_cache.reset();
    g_avgGpuTime = 0.0;
    g_gpuTime.reset();
    g_ngxTime.reset();
    g_lastNgxTime.reset();
    g_lastGpuTime.reset();

    g_compose.reset();
}
} // namespace DlssNr
