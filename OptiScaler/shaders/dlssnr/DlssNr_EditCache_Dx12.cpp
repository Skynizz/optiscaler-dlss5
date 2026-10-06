#include "pch.h"

#include "DlssNr_EditCache_Dx12.h"

#include <Config.h>
#include <Util.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "precompile/DlssNr_Cache_Shader.h"

namespace
{
void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (res == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmd->ResourceBarrier(1, &b);
}

constexpr D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr D3D12_RESOURCE_STATES kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ID3D12Resource* CreateTexture(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height,
                              bool uav, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max(width, 1u);
    desc.Height = std::max(height, 1u);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* res = nullptr;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&res))))
        return nullptr;

    return res;
}

ID3D12Resource* CreateReadback(ID3D12Device* device, unsigned long long bytes)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* res = nullptr;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    return res;
}

// The format a shader may read a resource as. Depth formats and the typeless families the guides
// arrive in need naming explicitly: a view of D32_FLOAT is not a thing, R32_FLOAT is.
DXGI_FORMAT ReadableFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return f;
    }
}

void MakeSrv(ID3D12Device* device, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE handle,
             DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, UINT plane = 0)
{
    const D3D12_RESOURCE_DESC desc = res->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC v {};
    v.Format = format != DXGI_FORMAT_UNKNOWN ? format : ReadableFormat(desc.Format);
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    v.Texture2D.PlaneSlice = plane;
    device->CreateShaderResourceView(res, &v, handle);
}

void MakeUav(ID3D12Device* device, ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    const D3D12_RESOURCE_DESC desc = res->GetDesc();

    D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
    v.Format = ReadableFormat(desc.Format);
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipSlice = 0;
    device->CreateUnorderedAccessView(res, nullptr, &v, handle);
}

unsigned int Groups(unsigned int n) { return (n + 7u) / 8u; }

unsigned int LevelDim(unsigned int full, unsigned int level)
{
    unsigned int s = full;

    for (unsigned int i = 0; i <= level; ++i)
        s = (s + kDlssNrCachePyramidStep - 1) / kDlssNrCachePyramidStep;

    return std::max(s, 1u);
}

// A .npy file: the format numpy reads natively, so the measurement script needs nothing else.
bool WriteNpy(const std::filesystem::path& path, const char* descr, unsigned int height, unsigned int width,
              unsigned int channels, const unsigned char* data, size_t bytesPerRow, size_t rowPitch)
{
    std::ofstream f(path, std::ios::binary);

    if (!f)
        return false;

    char dict[256];
    snprintf(dict, sizeof(dict), "{'descr': '%s', 'fortran_order': False, 'shape': (%u, %u, %u), }", descr,
             height, width, channels);

    std::string header(dict);
    const size_t preamble = 10; // magic (6) + version (2) + header length (2)
    size_t total = preamble + header.size() + 1;
    const size_t pad = (64 - (total % 64)) % 64;
    header.append(pad, ' ');
    header.push_back('\n');

    const unsigned char magic[8] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0 };
    f.write((const char*) magic, 8);
    const uint16_t len = (uint16_t) header.size();
    f.write((const char*) &len, 2);
    f.write(header.data(), header.size());

    for (unsigned int y = 0; y < height; ++y)
        f.write((const char*) data + y * rowPitch, bytesPerRow);

    return (bool) f;
}
} // namespace

DlssNrEditCache_Dx12::DlssNrEditCache_Dx12(ID3D12Device* device) : Shader_Dx12("DLSS-NR edit cache", device)
{
    if (device == nullptr)
        return;

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(device, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to set up the root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrCacheConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (auto& cb : _constantBuffers)
    {
        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cb))))
        {
            LOG_ERROR("[{0}] Could not create a constant buffer", _name);
            return;
        }
    }

    if (!CreateComputePipeline(device, &_pipelineState, DlssNrCache_cso, sizeof(DlssNrCache_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    // Stand-ins for the slots a mode does not use. An unbound descriptor is a read from nothing.
    _dummySrv = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, false, kSrv);
    _dummyUav = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, true, kUav);

    if (_dummySrv == nullptr || _dummyUav == nullptr)
    {
        LOG_ERROR("[{0}] Could not create the stand-in textures", _name);
        return;
    }

    _init = InitHeaps(device, _frameHeaps, DLSSNR_CACHE_NUM_OF_HEAPS);

    if (_init)
        LOG_INFO("DLSS-NR edit cache: shader ready");
}

DlssNrEditCache_Dx12::~DlssNrEditCache_Dx12()
{
    ReleaseAll(true);
    DumpRelease();

    for (auto& cb : _constantBuffers)
        SAFE_RELEASE(cb);

    SAFE_RELEASE(_dummySrv);
    SAFE_RELEASE(_dummyUav);
}

void DlssNrEditCache_Dx12::Park(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    // Retired, not released: with frame generation the GPU may be several frames behind this.
    _retired.push_back({ res, 32 });
    res = nullptr;
}

void DlssNrEditCache_Dx12::TickRetired()
{
    for (size_t i = 0; i < _retired.size();)
    {
        if (--_retired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        _retired[i].resource->Release();
        _retired.erase(_retired.begin() + i);
    }
}

void DlssNrEditCache_Dx12::ReleaseAll(bool immediately)
{
    _finalValid = false;

    // The pre-SR copy target belongs to the caller and is not here.
    ID3D12Resource** all[] = { &_finalRaw,  &_finalHist[0], &_finalHist[1], &_histTarget[0], &_histTarget[1],
                               &_histEdit[0], &_histEdit[1], &_histGuide[0], &_histGuide[1], &_level[0],
                               &_level[1],  &_level[2],     &_levelGuide,   &_stats,         &_modelUp,
                               &_accMv[0],  &_accMv[1] };

    for (ID3D12Resource** r : all)
    {
        if (immediately)
        {
            SAFE_RELEASE(*r);
        }
        else
        {
            Park(*r);
        }
    }

    for (unsigned int i = 0; i < kDlssNrCacheStatSlots; ++i)
    {
        if (immediately)
        {
            SAFE_RELEASE(_statsReadback[i]);
        }
        else
        {
            Park(_statsReadback[i]);
        }

        _statsPending[i] = false;
    }

    if (immediately)
    {
        for (auto& r : _retired)
            r.resource->Release();

        _retired.clear();
    }

    _width = _height = 0;
    _accWidth = _accHeight = 0;
    _accFormat = DXGI_FORMAT_UNKNOWN;
    _accInModelState = false;
    _historyValid = false;
}

bool DlssNrEditCache_Dx12::EnsureResources(ID3D12Device* device, unsigned int width, unsigned int height,
                                           DXGI_FORMAT format)
{
    if (_width == width && _height == height && _format == format && _histEdit[0] != nullptr)
        return true;

    if (_width != 0)
        LOG_INFO("DLSS-NR edit cache: {}x{} -> {}x{}, rebuilding", _width, _height, width, height);

    ReleaseAll(false);

    for (unsigned int i = 0; i < 2; ++i)
    {
        _histEdit[i] = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, true, kUav);
        // 32-bit depth: the tolerance is relative and a few percent, which half floats only just carry.
        _histGuide[i] = CreateTexture(device, DXGI_FORMAT_R32G32_FLOAT, width, height, true, kUav);
        _histTarget[i] = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, true, kUav);
    }

    for (unsigned int l = 0; l < kDlssNrCachePyramidLevels; ++l)
        _level[l] = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, LevelDim(width, l), LevelDim(height, l),
                                  true, kUav);

    _levelGuide = CreateTexture(device, DXGI_FORMAT_R32G32_FLOAT, LevelDim(width, 0), LevelDim(height, 0), true, kUav);
    _finalRaw = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, true, kUav);

    for (auto& h : _finalHist)
        h = CreateTexture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, true, kUav);
    _stats = CreateTexture(device, DXGI_FORMAT_R32_UINT, kDlssNrCacheStatsWidth, 1, true, kUav);

    for (auto& rb : _statsReadback)
        rb = CreateReadback(device, 256);

    bool ok = _stats != nullptr && _levelGuide != nullptr && _finalRaw != nullptr && _finalHist[0] != nullptr &&
              _finalHist[1] != nullptr;

    for (unsigned int i = 0; i < 2; ++i)
        ok = ok && _histEdit[i] != nullptr && _histGuide[i] != nullptr && _histTarget[i] != nullptr;

    for (auto* l : _level)
        ok = ok && l != nullptr;

    for (auto* rb : _statsReadback)
        ok = ok && rb != nullptr;

    if (!ok)
    {
        LOG_ERROR("DLSS-NR edit cache: could not allocate its textures at {}x{}", width, height);
        ReleaseAll(false);
        return false;
    }

    _width = width;
    _height = height;
    _format = format;
    _historyValid = false;
    _accReset = true;
    return true;
}

bool DlssNrEditCache_Dx12::EnsureAccumulator(ID3D12Device* device, ID3D12Resource* motion)
{
    const D3D12_RESOURCE_DESC md = motion->GetDesc();
    const DXGI_FORMAT readable = ReadableFormat(md.Format);

    // The game's own precision where it is a two-channel float, so the model reads what it would have.
    const DXGI_FORMAT want = (readable == DXGI_FORMAT_R32G32_FLOAT) ? DXGI_FORMAT_R32G32_FLOAT
                                                                    : DXGI_FORMAT_R16G16_FLOAT;

    // A change of the motion texture's shape retires the accumulator.
    if (_accWidth != (unsigned int) md.Width || _accHeight != md.Height || _accFormat != want)
    {
        for (auto*& a : _accMv)
            Park(a);

        _accInModelState = false;
        _accWidth = (unsigned int) md.Width;
        _accHeight = md.Height;
        _accFormat = want;
        _accReset = true;
    }

    if (_accMv[0] != nullptr && _accMv[1] != nullptr)
        return true;

    for (auto*& a : _accMv)
    {
        Park(a);
        a = CreateTexture(device, want, (unsigned int) md.Width, md.Height, true, kUav);
    }

    if (_accMv[0] == nullptr || _accMv[1] == nullptr)
    {
        Park(_accMv[0]);
        Park(_accMv[1]);
        return false;
    }

    _accReset = true;
    return true;
}

void DlssNrEditCache_Dx12::Invalidate()
{
    _finalValid = false;
    _historyValid = false;
    _accReset = true;
}

bool DlssNrEditCache_Dx12::BeginFrame(const Config& cfg, ID3D12Device* device, unsigned int width,
                                      unsigned int height, DXGI_FORMAT format, bool reset, bool preSr)
{
    ++_frame;
    TickRetired();

    if (!EnsureResources(device, width, height, format))
        return true; // cannot cache: run the model, which is what off would do

    ConsumeStats();

    // Latched once so every pass of this frame agrees, however the menu moves mid-frame.
    _depthTol = std::clamp(cfg.DlssNrCacheDepthTolerance.value_or_default(), 0.005f, 1.0f);
    _colourTol = std::clamp(cfg.DlssNrCacheColourTolerance.value_or_default(), 0.05f, 8.0f);
    _highDecay = std::clamp(cfg.DlssNrCacheHighDecay.value_or_default(), 0.0f, 1.0f);
    _refreshBlend = std::clamp(cfg.DlssNrCacheRefreshBlend.value_or_default(), 0.05f, 1.0f);
    _lowGain = std::clamp(cfg.DlssNrCacheLowGain.value_or_default(), 0.0f, 4.0f);
    _highGain = std::clamp(cfg.DlssNrCacheHighGain.value_or_default(), 0.0f, 4.0f);
    _bilateral = cfg.DlssNrCacheBilateral.value_or_default();
    _stabilize = std::clamp(cfg.DlssNrCacheStabilize.value_or_default(), 0.0f, 4.0f);
    _despeckle = cfg.DlssNrCacheDespeckle.value_or_default();
    // Before the upscaler, the high band's temporal blend and the keyframe crossfade (below) stand aside.
    // Each model answer there belongs to one jitter -- the scene sampled a fraction of a pixel elsewhere
    // -- so blending two answers lays two copies of the detail a fraction of a pixel apart, and the
    // upscaler, whose own accumulation already does this job properly, averages them into blur
    // (measured in Control: detail x1.05 with them, x1.15 without, against x1.20 for vanilla).
    // The regional light (luminance stability) is broad enough not to care, and stays.
    _temporal = preSr ? 0.0f : std::clamp(cfg.DlssNrCacheTemporal.value_or_default(), 0.0f, 0.9f);
    _lowTemporal = std::clamp(cfg.DlssNrCacheLowTemporal.value_or_default(), 0.0f, 0.95f);
    _debugView = cfg.DlssNrCacheDebugView.value_or_default();
    _modelHistory = cfg.DlssNrCacheModelHistory.value_or_default();

    const unsigned int interval = std::clamp(cfg.DlssNrCacheInterval.value_or_default(), 1u, 16u);
    const bool adaptive = cfg.DlssNrCacheAdaptive.value_or_default();
    const float threshold = std::clamp(cfg.DlssNrCacheAdaptiveThreshold.value_or_default(), 0.001f, 1.0f);

    const char* why = nullptr;

    if (!_historyValid)
        why = "no history";
    else if (reset)
        why = "the game or the model reset";
    else if (_dumpWanted > 0 && !_dumpObserve)
        why = "measurement dump";
    else if (_frame - _lastRefresh >= (_intervalNow = EffectiveInterval(interval, adaptive, threshold)))
        why = _regime == 0 ? "interval (still: slower)" : _regime == 2 ? "interval (fast motion: faster)" : "interval";

    // Keyframe crossfade step for this frame: the share of the remaining way to the model's latest answer,
    // so the shown edit reaches it exactly when the model runs next.
    _crossfadeOn = cfg.DlssNrCacheCrossfade.value_or_default() && !preSr;
    const unsigned int intervalNow = std::max(1u, _intervalNow);
    const unsigned int since = (unsigned int) (_frame - _lastRefresh);

    if (why == nullptr)
    {
        _crossfade = since >= intervalNow ? 1.0f : 1.0f / (float) (intervalNow - since);
        ++_cachedFrames;
        return false;
    }

    _crossfade = 1.0f / (float) intervalNow;

    _refreshReason = why;
    _lastRefresh = _frame;
    _cumulativeRejected = 0.0f;
    ++_refreshes;
    return true;
}

// The interval actually used. With adaptation on, a regular cadence per regime instead of runs fired
// early whenever enough was revealed: the early runs made the cadence irregular (2, 3, 2, 2, 3...) and an
// irregular refresh is visible as flicker in its own right. Each regime holds until the motion has
// clearly left it, so the cadence does not hop either.
//
//   still   (almost nothing revealed)  twice the interval, up to 8 -- standing still costs least
//   moving                              the interval as set
//   fast    (more than the threshold)   half the interval -- more of the frame is new each frame
unsigned int DlssNrEditCache_Dx12::EffectiveInterval(unsigned int interval, bool adaptive, float threshold)
{
    if (!adaptive || interval <= 1)
    {
        _regime = 1;
        return interval;
    }

    // Standing still is not zero: TAA jitter on thin things alone rejects about 0.6% of the frame in
    // Control. The floor for "still" sits above that, or the regime never engages.
    const int wanted = _motion > threshold ? 2 : (_motion < std::max(0.015f, threshold * 0.15f) ? 0 : 1);

    if (wanted == _regime)
    {
        _regimeCandidate = wanted;
        _regimeFrames = 0;
    }
    else
    {
        if (wanted != _regimeCandidate)
        {
            _regimeCandidate = wanted;
            _regimeFrames = 0;
        }

        // Into fast motion quickly, out of it and into stillness slowly: a lag there costs a little
        // performance, a lag the other way costs flicker.
        const unsigned int needed = wanted == 2 ? 4u : 30u;

        if (++_regimeFrames >= needed)
        {
            _regime = wanted;
            _regimeFrames = 0;
        }
    }

    switch (_regime)
    {
    case 0: return std::min(interval * 2u, 8u);
    case 2: return std::max(1u, (interval + 1u) / 2u);
    default: return interval;
    }
}

void DlssNrEditCache_Dx12::ConsumeStats()
{
    for (unsigned int s = 0; s < kDlssNrCacheStatSlots; ++s)
    {
        // Three frames of distance, like the white point meter: there is no fence here, and the frame
        // that wrote the slot has to be long retired before it is mapped.
        if (!_statsPending[s] || _frame - _statsFrame[s] < 3 || _statsReadback[s] == nullptr)
            continue;

        _statsPending[s] = false;

        void* mapped = nullptr;
        D3D12_RANGE range { 0, kDlssNrCacheStatsWidth * sizeof(uint32_t) };

        if (FAILED(_statsReadback[s]->Map(0, &range, &mapped)) || mapped == nullptr)
            continue;

        const uint32_t rejected = ((const uint32_t*) mapped)[s];

        D3D12_RANGE nothing { 0, 0 };
        _statsReadback[s]->Unmap(0, &nothing);

        const float total = (float) std::max(1u, _width * _height);

        const float fraction = std::min(1.0f, rejected / total);
        _lastRejected = fraction;

        // How much is being revealed, smoothed over a few readings: the motion the regimes follow.
        _motion = _motion * 0.75f + fraction * 0.25f;

        // Only what happened since the last refresh decides the next one.
        if (_statsFrame[s] > _lastRefresh)
            _cumulativeRejected += fraction;
    }
}

DlssNrCacheConstants DlssNrEditCache_Dx12::BaseConstants(const DlssNrCacheInputs& in) const
{
    DlssNrCacheConstants c {};
    c.Width = _width;
    c.Height = _height;
    c.DepthWidth = std::max(in.depthWidth, 1u);
    c.DepthHeight = std::max(in.depthHeight, 1u);
    c.MotionWidth = std::max(in.motionWidth, 1u);
    c.MotionHeight = std::max(in.motionHeight, 1u);
    c.MvScaleX = in.mvScaleX;
    c.MvScaleY = in.mvScaleY;
    c.DepthInverted = in.depthInverted ? 1u : 0u;
    c.Epsilon = std::max(in.passthrough ? 1.0f : in.whitePoint, 1e-4f) / 512.0f;
    c.DepthTolerance = _depthTol;
    c.ColourTolerance = _colourTol;
    c.HighDecay = _highDecay;
    c.RefreshBlend = _refreshBlend;
    c.HistoryValid = _historyValid ? 1u : 0u;
    c.LowGain = _lowGain;
    c.HighGain = _highGain;
    c.Bilateral = _bilateral ? 1u : 0u;
    c.DebugView = _debugView;
    c.StatsSlot = _statsSlot;
    c.Passthrough = in.passthrough ? 1u : 0u;
    c.FrameIndex = (uint32_t) _frame;
    c.UseGameExposure = (in.useGameExposure && in.exposure != nullptr && !in.passthrough) ? 1u : 0u;
    c.ExposurePreMul = in.exposurePreMul;
    c.MaxLumaEdit = std::log2(std::max(in.maxRatio, 1.0f)) + 0.5f;
    c.Stabilize = _stabilize;
    c.Despeckle = _despeckle ? 1u : 0u;
    c.CrossfadeOn = _crossfadeOn ? 1u : 0u;
    c.Crossfade = _crossfade;
    c.Temporal = _debugView == 0 ? _temporal : 0.0f;
    c.LowTemporal = _lowTemporal;
    c.TemporalValid = _finalValid ? 1u : 0u;
    c.JitterDeltaX = in.jitterDeltaX;
    c.JitterDeltaY = in.jitterDeltaY;
    return c;
}

bool DlssNrEditCache_Dx12::Pass(ID3D12GraphicsCommandList* cmd, const DlssNrCacheConstants& constants,
                                ID3D12Resource* const (&srv)[kSrvCount], ID3D12Resource* const (&uav)[kUavCount],
                                unsigned int groupsX, unsigned int groupsY)
{
    if (!_init || cmd == nullptr || _device == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_CACHE_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    for (uint32_t i = 0; i < kSrvCount; ++i)
    {
        if (i == 10)
        {
            MakeSrv(_device, srv[10] != nullptr ? srv[10] : (_exposure != nullptr ? _exposure : _dummySrv),
                    heap.GetSrvCPU(10));
            continue;
        }

        MakeSrv(_device, srv[i] != nullptr ? srv[i] : _dummySrv, heap.GetSrvCPU(i));
    }

    for (uint32_t i = 0; i < kUavCount; ++i)
    {
        ID3D12Resource* r = uav[i];

        if (r == nullptr)
            r = (i == 5) ? _stats : _dummyUav;

        MakeUav(_device, r != nullptr ? r : _dummyUav, heap.GetUavCPU(i));
    }

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], constants, heap.GetCbvCPU(0)))
        return false;

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmd->SetDescriptorHeaps(_countof(heaps), heaps);
    cmd->SetComputeRootSignature(_rootSignature);
    cmd->SetPipelineState(_pipelineState);
    cmd->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmd->Dispatch(std::max(groupsX, 1u), std::max(groupsY, 1u), 1);
    return true;
}

void DlssNrEditCache_Dx12::Accumulate(ID3D12GraphicsCommandList* cmd, const DlssNrCacheInputs& in)
{
    if (_modelHistory != 1 || _accMv[0] == nullptr || in.motion == nullptr)
        return;

    ID3D12Resource* prev = _accMv[_accCur];
    ID3D12Resource* next = _accMv[1 - _accCur];

    DlssNrCacheConstants c = BaseConstants(in);
    c.Mode = DlssNrCacheMode_AccumulateMv;
    c.Width = std::min(in.motionWidth, _accWidth);
    c.Height = std::min(in.motionHeight, _accHeight);
    c.SourceWidth = _accWidth;
    c.SourceHeight = _accHeight;
    c.AccumulateReset = _accReset ? 1u : 0u;

    Barrier(cmd, prev, kUav, kSrv);
    ID3D12Resource* srv[kSrvCount] = { nullptr, nullptr, nullptr, nullptr, in.motion, prev };
    ID3D12Resource* uav[kUavCount] = { next };
    Pass(cmd, c, srv, uav, Groups(c.Width), Groups(c.Height));
    Barrier(cmd, prev, kSrv, kUav);

    _accCur = 1 - _accCur;
    _accReset = false;
}

void DlssNrEditCache_Dx12::BuildCoarseLevels(ID3D12GraphicsCommandList* cmd)
{
    DlssNrCacheInputs none {};
    DlssNrCacheConstants c = BaseConstants(none);
    c.Mode = DlssNrCacheMode_Downsample;

    for (unsigned int l = 1; l < kDlssNrCachePyramidLevels; ++l)
    {
        c.SourceWidth = LevelDim(_width, l - 1);
        c.SourceHeight = LevelDim(_height, l - 1);
        c.Width = LevelDim(_width, l);
        c.Height = LevelDim(_height, l);

        Barrier(cmd, _level[l - 1], kUav, kSrv);
        ID3D12Resource* srv[kSrvCount] = { nullptr, nullptr, nullptr, nullptr, nullptr, _level[l - 1] };
        ID3D12Resource* uav[kUavCount] = { _level[l] };
        Pass(cmd, c, srv, uav, Groups(c.Width), Groups(c.Height));
    }

    // Every level is read by the apply; the last one written goes over with the rest.
    Barrier(cmd, _level[kDlssNrCachePyramidLevels - 1], kUav, kSrv);
    Barrier(cmd, _levelGuide, kUav, kSrv);
}

void DlssNrEditCache_Dx12::ApplyPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target,
                                     ID3D12Resource* original, const DlssNrCacheInputs& in)
{
    // _histEdit/_histGuide[_cur] were just written; the levels are all SRV from BuildCoarseLevels.
    Barrier(cmd, _histEdit[_cur], kUav, kSrv);
    Barrier(cmd, _histGuide[_cur], kUav, kSrv);

    DlssNrCacheConstants c = BaseConstants(in);
    c.Mode = DlssNrCacheMode_Apply;

    const bool stabilise = c.Temporal > 0.0f;

    ID3D12Resource* srv[kSrvCount] = { _histEdit[_cur], _histGuide[_cur], original, in.depth, nullptr,
                                       _level[0],       _levelGuide,      _level[1], _level[2] };
    ID3D12Resource* uav[kUavCount] = { target, nullptr, nullptr, stabilise ? _finalRaw : nullptr };
    Pass(cmd, c, srv, uav, Groups(_width), Groups(_height));

    Barrier(cmd, _histEdit[_cur], kSrv, kUav);
    Barrier(cmd, _histGuide[_cur], kSrv, kUav);

    for (auto* l : _level)
        Barrier(cmd, l, kSrv, kUav);

    Barrier(cmd, _levelGuide, kSrv, kUav);

    if (stabilise)
        TemporalPass(cmd, target, original, in);
}

void DlssNrEditCache_Dx12::TemporalPass(ID3D12GraphicsCommandList* cmd, ID3D12Resource* target,
                                        ID3D12Resource* original, const DlssNrCacheInputs& in)
{
    // _histGuide[_cur] is this frame's guide, _histGuide[1 - _cur] last frame's: what the history was
    // shot on, for the depth test of its reprojection.
    ID3D12Resource* prev = _finalHist[_finalCur];
    ID3D12Resource* next = _finalHist[1 - _finalCur];

    Barrier(cmd, _finalRaw, kUav, kSrv);
    Barrier(cmd, prev, kUav, kSrv);
    Barrier(cmd, _histGuide[_cur], kUav, kSrv);
    Barrier(cmd, _histGuide[1 - _cur], kUav, kSrv);

    DlssNrCacheConstants c = BaseConstants(in);
    c.Mode = DlssNrCacheMode_Temporal;

    ID3D12Resource* srv[kSrvCount] = { nullptr, _histGuide[_cur], original, in.depth, in.motion,
                                       _finalRaw, prev, _histGuide[1 - _cur] };
    ID3D12Resource* uav[kUavCount] = { target, nullptr, nullptr, next };
    Pass(cmd, c, srv, uav, Groups(_width), Groups(_height));

    Barrier(cmd, _finalRaw, kSrv, kUav);
    Barrier(cmd, prev, kSrv, kUav);
    Barrier(cmd, _histGuide[_cur], kSrv, kUav);
    Barrier(cmd, _histGuide[1 - _cur], kSrv, kUav);

    _finalCur = 1 - _finalCur;
    _finalValid = true;
}

bool DlssNrEditCache_Dx12::RunCached(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                                     ID3D12Resource* keep, const DlssNrCacheInputs& in)
{
    if (!_init || target == nullptr || keep == nullptr || in.depth == nullptr || in.motion == nullptr)
        return false;

    _exposure = in.useGameExposure ? in.exposure : nullptr;

    if (_modelHistory == 1 && EnsureAccumulator(device, in.motion))
        Accumulate(cmd, in);

    const unsigned int prev = _cur;
    const unsigned int next = 1 - _cur;

    DlssNrCacheConstants c = BaseConstants(in);

    // This frame's counters start from zero.
    c.Mode = DlssNrCacheMode_ClearStats;
    {
        ID3D12Resource* srv[kSrvCount] = {};
        ID3D12Resource* uav[kUavCount] = {};
        Pass(cmd, c, srv, uav, 1, 1);
    }

    {
        D3D12_RESOURCE_BARRIER uavBarrier {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = _stats;
        cmd->ResourceBarrier(1, &uavBarrier);
    }

    // Reproject: last frame's history onto this one, validated, plus the first pyramid level.
    Barrier(cmd, target, kUav, kSrv);
    Barrier(cmd, _histEdit[prev], kUav, kSrv);
    Barrier(cmd, _histGuide[prev], kUav, kSrv);
    Barrier(cmd, _histTarget[prev], kUav, kSrv);

    c.Mode = DlssNrCacheMode_Reproject;

    {
        ID3D12Resource* srv[kSrvCount] = { _histEdit[prev], _histGuide[prev], target,  in.depth,
                                           in.motion,       nullptr,          nullptr, nullptr,
                                           nullptr,         nullptr,          nullptr, _histTarget[prev] };
        ID3D12Resource* uav[kUavCount] = { _histEdit[next], _histGuide[next], keep, _level[0], _levelGuide, _stats,
                                           _histTarget[next] };
        Pass(cmd, c, srv, uav, Groups(_width), Groups(_height));
    }

    Barrier(cmd, _histEdit[prev], kSrv, kUav);
    Barrier(cmd, _histGuide[prev], kSrv, kUav);
    Barrier(cmd, _histTarget[prev], kSrv, kUav);
    Barrier(cmd, target, kSrv, kUav);

    // The counters go home on a readback looked at three frames from now.
    {
        const unsigned int slot = _statsSlot;

        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = _stats;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = _statsReadback[slot];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
        dst.PlacedFootprint.Footprint.Width = kDlssNrCacheStatsWidth;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = 256;

        Barrier(cmd, _stats, kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd, _stats, D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);

        _statsFrame[slot] = _frame;
        _statsPending[slot] = true;
        _statsSlot = (slot + 1) % kDlssNrCacheStatSlots;
    }

    _cur = next;

    // keep now holds the untouched frame; the apply reads it and writes target.
    Barrier(cmd, keep, kUav, kSrv);
    BuildCoarseLevels(cmd);
    ApplyPass(cmd, target, keep, in);

    if (_dumpWanted > 0 && _dumpObserve)
    {
        Barrier(cmd, target, kUav, kSrv);
        DumpRecord(cmd, device, target, keep, in);
        Barrier(cmd, target, kSrv, kUav);
    }

    Barrier(cmd, keep, kSrv, kUav);
    return true;
}

ID3D12Resource* DlssNrEditCache_Dx12::ModelMotion(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                                  const DlssNrCacheInputs& in, bool& resetModel)
{
    resetModel = false;

    if (_modelHistory == 2)
    {
        resetModel = true;
        return nullptr;
    }

    if (_modelHistory != 1 || in.motion == nullptr || !EnsureAccumulator(device, in.motion))
        return nullptr;

    Accumulate(cmd, in);

    // Read by the model now; EndFrame puts it back.
    Barrier(cmd, _accMv[_accCur], kUav, kSrv);
    _accInModelState = true;

    // The model has seen everything up to this frame; the next accumulation starts again from zero.
    _accReset = true;
    return _accMv[_accCur];
}

bool DlssNrEditCache_Dx12::CaptureRefresh(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                          ID3D12Resource* target, ID3D12Resource* original,
                                          const DlssNrCacheInputs& in)
{
    if (!_init || target == nullptr || original == nullptr || in.depth == nullptr || in.motion == nullptr)
        return false;

    _exposure = in.useGameExposure ? in.exposure : nullptr;

    const unsigned int prev = _cur;
    const unsigned int next = 1 - _cur;

    Barrier(cmd, target, kUav, kSrv);
    Barrier(cmd, _histEdit[prev], kUav, kSrv);
    Barrier(cmd, _histGuide[prev], kUav, kSrv);
    Barrier(cmd, _histTarget[prev], kUav, kSrv);

    DlssNrCacheConstants c = BaseConstants(in);
    c.Mode = DlssNrCacheMode_Capture;
    {
        ID3D12Resource* srv[kSrvCount] = { _histEdit[prev], _histGuide[prev], original, in.depth,
                                           in.motion,       target,           nullptr,  nullptr,
                                           nullptr,         nullptr,          nullptr,  _histTarget[prev] };
        ID3D12Resource* uav[kUavCount] = { _histEdit[next], _histGuide[next], nullptr, _level[0], _levelGuide, nullptr,
                                           _histTarget[next] };
        Pass(cmd, c, srv, uav, Groups(_width), Groups(_height));
    }

    Barrier(cmd, _histEdit[prev], kSrv, kUav);
    Barrier(cmd, _histGuide[prev], kSrv, kUav);
    Barrier(cmd, _histTarget[prev], kSrv, kUav);

    const bool wasValid = _historyValid;
    _cur = next;
    _historyValid = true;

    if (_dumpWanted > 0 && !_dumpObserve)
        DumpRecord(cmd, device, target, original, in);

    // The stored edit is the model's own unless something changed it -- the blend, the gains, the
    // anti-flicker or the despeckle -- in which case the frame on screen has to be the stored one too.
    // Without this the refresh frames showed the model's raw answer: exactly the frames where a speck
    // pops, unfiltered, while the frames between were steady.
    const bool rewrite = (_refreshBlend < 0.999f && wasValid) || std::abs(_lowGain - 1.0f) > 1e-3f ||
                         std::abs(_highGain - 1.0f) > 1e-3f || _debugView != 0 || (_stabilize > 0.0f && wasValid) ||
                         _despeckle || (_crossfadeOn && wasValid && _crossfade < 0.999f) || _temporal > 0.0f;

    Barrier(cmd, target, kSrv, kUav);

    if (rewrite)
    {
        BuildCoarseLevels(cmd);
        ApplyPass(cmd, target, original, in);
    }

    if (_dumpWanted > 0 && _dumpObserve)
    {
        Barrier(cmd, target, kUav, kSrv);
        DumpRecord(cmd, device, target, original, in);
        Barrier(cmd, target, kSrv, kUav);
    }

    return true;
}

void DlssNrEditCache_Dx12::EndFrame(ID3D12GraphicsCommandList* cmd)
{
    if (_accInModelState && _accMv[_accCur] != nullptr)
        Barrier(cmd, _accMv[_accCur], kSrv, kUav);

    _accInModelState = false;

    if (_dumpWanted > 0 && _dumpCaptured >= _dumpWanted && _dumpWriteAt != 0 && _frame >= _dumpWriteAt)
        DumpWrite();
}

ID3D12Resource* DlssNrEditCache_Dx12::UpsampleModel(ID3D12GraphicsCommandList* cmd, ID3D12Device* device,
                                                    ID3D12Resource* fullProxy, ID3D12Resource* smallProxy,
                                                    ID3D12Resource* smallModel, bool passthrough, float sigma)
{
    if (!_init || fullProxy == nullptr || smallProxy == nullptr || smallModel == nullptr)
        return nullptr;

    const D3D12_RESOURCE_DESC fd = fullProxy->GetDesc();
    const D3D12_RESOURCE_DESC sd = smallProxy->GetDesc();

    if (_modelUp != nullptr)
    {
        const D3D12_RESOURCE_DESC have = _modelUp->GetDesc();

        if (have.Width != fd.Width || have.Height != fd.Height || have.Format != fd.Format)
            Park(_modelUp);
    }

    if (_modelUp == nullptr)
    {
        _modelUp = CreateTexture(device, fd.Format, (unsigned int) fd.Width, fd.Height, true, kUav);

        if (_modelUp == nullptr)
            return nullptr;
    }

    DlssNrCacheConstants c {};
    c.Mode = DlssNrCacheMode_JbuUpsample;
    c.Width = (unsigned int) fd.Width;
    c.Height = fd.Height;
    c.SourceWidth = (unsigned int) sd.Width;
    c.SourceHeight = sd.Height;
    c.Passthrough = passthrough ? 1u : 0u;
    c.JbuSigma = std::clamp(sigma, 0.005f, 1.0f);
    c.MaxLumaEdit = 1.5f;

    ID3D12Resource* srv[kSrvCount] = { nullptr, nullptr, fullProxy, nullptr, nullptr, smallProxy, smallModel };
    ID3D12Resource* uav[kUavCount] = { _modelUp };

    if (!Pass(cmd, c, srv, uav, Groups(c.Width), Groups(c.Height)))
        return nullptr;

    Barrier(cmd, _modelUp, kUav, kSrv);
    return _modelUp;
}

bool DlssNrEditCache_Dx12::CopyIn(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, ID3D12Resource* dst)
{
    if (!_init || cmd == nullptr || src == nullptr || dst == nullptr)
        return false;

    const D3D12_RESOURCE_DESC dd = dst->GetDesc();

    DlssNrCacheConstants c {};
    c.Mode = DlssNrCacheMode_Copy;
    c.Width = (unsigned int) dd.Width;
    c.Height = dd.Height;

    ID3D12Resource* srv[kSrvCount] = { nullptr, nullptr, src };
    ID3D12Resource* uav[kUavCount] = { dst };

    if (!Pass(cmd, c, srv, uav, Groups(c.Width), Groups(c.Height)))
        return false;

    // What follows reads it, as a UAV or after a transition.
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = dst;
    cmd->ResourceBarrier(1, &b);
    return true;
}

void DlssNrEditCache_Dx12::FinishUpsample(ID3D12GraphicsCommandList* cmd)
{
    if (_modelUp != nullptr)
        Barrier(cmd, _modelUp, kSrv, kUav);
}

void DlssNrEditCache_Dx12::RequestDump(unsigned int frames, bool observe)
{
    if (_dumpWanted > 0)
        return;

    DumpRelease();
    _dumpObserve = observe;
    _dumpWanted = std::clamp(frames, 2u, 32u);
    _dumpCaptured = 0;
    _dumpWriteAt = 0;
    LOG_INFO("DLSS-NR edit cache: measurement dump of {} frames requested", _dumpWanted);
}

void DlssNrEditCache_Dx12::DumpRecord(ID3D12GraphicsCommandList* cmd, ID3D12Device* device, ID3D12Resource* target,
                                      ID3D12Resource* original, const DlssNrCacheInputs& in)
{
    if (_dumpCaptured >= _dumpWanted)
        return;

    static const DXGI_FORMAT kFormats[3] = { DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             DXGI_FORMAT_R32G32B32A32_FLOAT };

    if (_dumpTex[0] == nullptr)
    {
        for (int i = 0; i < 3; ++i)
            _dumpTex[i] = CreateTexture(device, kFormats[i], _width, _height, true, kUav);

        _dumpFrames.resize(_dumpWanted);

        for (auto& f : _dumpFrames)
        {
            for (int i = 0; i < 3; ++i)
            {
                D3D12_RESOURCE_DESC desc = _dumpTex[i] != nullptr ? _dumpTex[i]->GetDesc() : D3D12_RESOURCE_DESC {};
                UINT64 bytes = 0;
                device->GetCopyableFootprints(&desc, 0, 1, 0, &f.layout[i], nullptr, nullptr, &bytes);
                f.readback[i] = CreateReadback(device, bytes);
            }
        }

        bool ok = true;

        for (auto* t : _dumpTex)
            ok = ok && t != nullptr;

        for (auto& f : _dumpFrames)
            for (auto* r : f.readback)
                ok = ok && r != nullptr;

        if (!ok)
        {
            LOG_ERROR("DLSS-NR edit cache: not enough memory for a {}-frame dump at {}x{}", _dumpWanted, _width,
                      _height);
            DumpRelease();
            _dumpWanted = 0;
            return;
        }
    }

    // target is SRV here (CaptureRefresh), original is SRV from the main pass.
    DlssNrCacheConstants c = BaseConstants(in);
    c.Mode = DlssNrCacheMode_DumpPack;

    ID3D12Resource* srv[kSrvCount] = { nullptr, nullptr, original, in.depth, in.motion, target };
    ID3D12Resource* uav[kUavCount] = { _dumpTex[0], _dumpTex[2], nullptr, _dumpTex[1] };
    Pass(cmd, c, srv, uav, Groups(_width), Groups(_height));

    DumpFrame& f = _dumpFrames[_dumpCaptured];
    f.whitePoint = in.passthrough ? 1.0f : in.whitePoint;

    for (int i = 0; i < 3; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = _dumpTex[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = f.readback[i];
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = f.layout[i];

        Barrier(cmd, _dumpTex[i], kUav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cmd, _dumpTex[i], D3D12_RESOURCE_STATE_COPY_SOURCE, kUav);
    }

    ++_dumpCaptured;

    // Written well after the last copy, once the GPU is certainly past it: this path has no fence.
    if (_dumpCaptured >= _dumpWanted)
        _dumpWriteAt = _frame + 8;
}

void DlssNrEditCache_Dx12::DumpWrite()
{
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-cachedump";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    static const char* kNames[3] = { "orig", "nr", "geo" };
    static const char* kDescr[3] = { "<f2", "<f2", "<f4" };
    static const unsigned int kBpp[3] = { 8, 8, 16 };

    std::string whitePoints;

    for (unsigned int k = 0; k < _dumpCaptured; ++k)
    {
        DumpFrame& f = _dumpFrames[k];

        for (int i = 0; i < 3; ++i)
        {
            void* mapped = nullptr;

            if (FAILED(f.readback[i]->Map(0, nullptr, &mapped)) || mapped == nullptr)
                continue;

            char name[64];
            snprintf(name, sizeof(name), "frame_%03u_%s.npy", k, kNames[i]);
            WriteNpy(dir / name, kDescr[i], _height, _width, 4, (const unsigned char*) mapped + f.layout[i].Offset,
                     (size_t) _width * kBpp[i], f.layout[i].Footprint.RowPitch);

            D3D12_RANGE nothing { 0, 0 };
            f.readback[i]->Unmap(0, &nothing);
        }

        char wp[32];
        snprintf(wp, sizeof(wp), "%s%.6f", k == 0 ? "" : ", ", f.whitePoint);
        whitePoints += wp;
    }

    std::ofstream manifest(dir / "manifest.json");
    manifest << "{\n"
             << "  \"frames\": " << _dumpCaptured << ",\n"
             << "  \"width\": " << _width << ",\n"
             << "  \"height\": " << _height << ",\n"
             << "  \"white_points\": [" << whitePoints << "],\n"
             << "  \"epsilon_rule\": \"white_point / 512\",\n"
             << "  \"observe\": " << (_dumpObserve ? "true" : "false") << ",\n"
             << "  \"orig\": \"the frame as the upscaler wrote it, linear, RGBA float16\",\n"
             << "  \"nr\": \"the same frame after Neural Rendering (the model ran on every dumped frame)\",\n"
             << "  \"geo\": \"float32: [0:2] uv offset to the PREVIOUS frame (prev_uv = uv + geo.xy), "
                "[2] pseudo-linear depth (proportional to view depth), [3] log2 luma of orig\"\n"
             << "}\n";

    _dumpWritten = _dumpCaptured;
    LOG_INFO("DLSS-NR edit cache: wrote {} dumped frames to {}", _dumpCaptured, dir.string());

    DumpRelease();
    _dumpWanted = 0;
}

void DlssNrEditCache_Dx12::DumpRelease()
{
    for (auto& t : _dumpTex)
    {
        if (t != nullptr)
            Park(t);
    }

    for (auto& f : _dumpFrames)
        for (auto& r : f.readback)
            SAFE_RELEASE(r); // readback memory: nothing on the GPU references it once written

    _dumpFrames.clear();
    _dumpCaptured = 0;
    _dumpWriteAt = 0;
}

DlssNrEditCache_Dx12::Status DlssNrEditCache_Dx12::GetStatus() const
{
    Status s {};
    s.active = _historyValid;
    s.refreshes = _refreshes;
    s.cached = _cachedFrames;
    s.framesSinceRefresh = (unsigned int) (_frame - _lastRefresh);
    s.lastRejected = _lastRejected;
    s.cumulativeRejected = _cumulativeRejected;
    s.lastRefreshReason = _refreshReason;
    s.regime = _regime;
    s.dumpWritten = _dumpWritten;
    s.dumpActive = _dumpWanted > 0;
    return s;
}
