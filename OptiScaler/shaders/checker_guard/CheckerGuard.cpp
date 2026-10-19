#include "pch.h"
#include "CheckerGuard.h"
#include "CG_Dx12.h"

#include <Config.h>
#include <State.h>
#include <hooks/D3D12_Hooks.h>
#include <hudfix/Hudfix_Dx12.h>
#include <Util.h>

#include <filesystem>
#include <fstream>

#include <memory>
#include <set>

namespace
{
std::unique_ptr<CG_Dx12> g_shader;
ID3D12Resource* g_tex = nullptr;
D3D12_RESOURCE_STATES g_texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
ID3D12Resource* g_original = nullptr;
bool g_typed = true;
bool g_swapped = false;
bool g_failed = false;

void ReportOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("Speckle cleanup did not run: {}", reason);
}

// The typed member of a typeless family, so the copy can be viewed and written.
DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
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

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* name, bool& typed)
{
    ID3D12Resource* res = nullptr;
    typed = true;

    if (params->Get(name, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // OptiScaler's D3D11 and Vulkan bridges set their resources untyped.
    void* untyped = nullptr;
    typed = false;

    if (params->Get(name, &untyped) == NVSDK_NGX_Result_Success)
        return (ID3D12Resource*) untyped;

    return nullptr;
}

// The same envelope OptiScaler's own compute passes run in: our binds are not recorded as the game's, and the
// game's compute state is put back afterwards where a game needs it (bindless engines).
struct ScopedStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;
    bool hudfixWas;

    explicit ScopedStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
        hudfixWas = Hudfix_Dx12::GetSkipStatus();
        Hudfix_Dx12::SetSkipStatus(true);
    }

    ~ScopedStateEnvelope()
    {
        Hudfix_Dx12::SetSkipStatus(hudfixWas);
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};
} // namespace

bool CheckerGuard::BeginBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
{
    g_swapped = false;

    const Config& cfg = *Config::Instance();

    if (cmdList == nullptr || params == nullptr || g_failed || !cfg.FGSpeckleCleanup.value_or_default())
        return false;

    bool typed = true;
    bool motionTyped = true;
    ID3D12Resource* colour = GetResource(params, NVSDK_NGX_Parameter_Color, typed);
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, motionTyped);
    bool depthTyped = true;
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, depthTyped);

    if (depth != nullptr && depth->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        depth = nullptr;

    if (colour == nullptr || motion == nullptr)
    {
        ReportOnce("the upscaler was given no colour or motion vectors");
        return false;
    }

    const D3D12_RESOURCE_DESC cd = colour->GetDesc();
    const D3D12_RESOURCE_DESC md = motion->GetDesc();

    if (cd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || cd.SampleDesc.Count != 1 ||
        md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        ReportOnce("the colour or motion vectors are not plain 2D textures");
        return false;
    }

    // An sRGB colour would be read back as linear from a copy we can write; left alone.
    if (cd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || cd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
    {
        ReportOnce("the colour is sRGB");
        return false;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(colour->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    if (g_shader == nullptr)
    {
        g_shader = std::make_unique<CG_Dx12>("Speckle cleanup", device);

        if (!g_shader->IsInit())
        {
            LOG_ERROR("Speckle cleanup: the shader could not be created, off for this session");
            g_failed = true;
            device->Release();
            return false;
        }
    }

    // Our copy: the colour's size, a typed member of its format family, with unordered access.
    const DXGI_FORMAT format = TypedFormat(cd.Format);

    if (g_tex != nullptr)
    {
        const D3D12_RESOURCE_DESC td = g_tex->GetDesc();

        if (td.Width != cd.Width || td.Height != cd.Height || td.Format != format)
        {
            g_tex->Release();
            g_tex = nullptr;
        }
    }

    if (g_tex == nullptr)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = cd.Width;
        desc.Height = cd.Height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if (device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                            nullptr, IID_PPV_ARGS(&g_tex)) != S_OK)
        {
            g_tex = nullptr;
            ReportOnce("its copy of the colour could not be created (format not writable?)");
            device->Release();
            return false;
        }

        g_texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        g_tex->SetName(L"Speckle cleanup colour");
        LOG_INFO("Speckle cleanup: running before the upscaler on its {}x{} colour (format {})",
                 (unsigned int) cd.Width, cd.Height, (int) format);
    }

    device->Release();

    CGConstants c {};
    c.width = (UINT) cd.Width;
    c.height = cd.Height;
    c.rectWidth = c.width;
    c.rectHeight = c.height;

    unsigned int rectW = 0;
    unsigned int rectH = 0;

    if (params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &rectW) == NVSDK_NGX_Result_Success &&
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &rectH) == NVSDK_NGX_Result_Success &&
        rectW > 0 && rectH > 0)
    {
        c.rectWidth = std::min<UINT>(rectW, c.width);
        c.rectHeight = std::min<UINT>(rectH, c.height);
    }

    c.mvWidth = (UINT) std::max<UINT64>(md.Width, 1);
    c.mvHeight = std::max<UINT>(md.Height, 1);

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &c.mvScaleX) != NVSDK_NGX_Result_Success)
        c.mvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &c.mvScaleY) != NVSDK_NGX_Result_Success)
        c.mvScaleY = 1.0f;

    if (depth != nullptr)
    {
        const D3D12_RESOURCE_DESC dd = depth->GetDesc();
        c.depthWidth = (UINT) std::min<UINT64>(dd.Width, c.rectWidth);
        c.depthHeight = std::min<UINT>(dd.Height, c.rectHeight);

        unsigned int createFlags = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);
        c.depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0 ? 1u : 0u;
    }
    else
    {
        ReportOnce("the upscaler was given no depth: the surface test is skipped (foliage is not protected)");
    }

    c.planarTolerance = 0.01f;
    c.strength = std::clamp(cfg.FGSpeckleCleanupStrength.value_or_default(), 0.0f, 1.0f);
    c.threshold = 0.06f;
    c.minMotion = 2.0f;
    c.debugView = cfg.FGSpeckleCleanupDebug.value_or_default() ? 2u : 0u;

    // Diagnostics: a file "speckle-debug.txt" beside OptiScaler holding a debug view number (3: raw depth).
    {
        static LONGLONG checkedAt = 0;
        static UINT forced = 0;
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);

        if (now.QuadPart - checkedAt > 10000000)
        {
            checkedAt = now.QuadPart;
            std::error_code ec;
            const auto file = Util::DllPath().remove_filename() / "speckle-debug.txt";
            forced = 0;

            if (std::filesystem::exists(file, ec))
            {
                std::ifstream f(file);
                f >> forced;
            }
        }

        if (forced != 0)
            c.debugView = forced;
    }

    {
        ScopedStateEnvelope envelope(cmdList);
        Barrier(cmdList, g_tex, g_texState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_texState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (!g_shader->Dispatch(cmdList, colour, motion, depth, g_tex, c))
        {
            ReportOnce("the dispatch failed");
            return false;
        }
    }

    // Readable the way the upscaler reads its colour.
    const D3D12_RESOURCE_STATES readState =
        cfg.ColorResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
            : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    Barrier(cmdList, g_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, readState);
    g_texState = readState;

    g_original = colour;
    g_typed = typed;

    if (typed)
        params->Set(NVSDK_NGX_Parameter_Color, g_tex);
    else
        params->Set(NVSDK_NGX_Parameter_Color, (void*) g_tex);

    g_swapped = true;
    return true;
}

void CheckerGuard::EndBeforeUpscale(NVSDK_NGX_Parameter* params)
{
    if (!g_swapped || params == nullptr)
        return;

    if (g_typed)
        params->Set(NVSDK_NGX_Parameter_Color, g_original);
    else
        params->Set(NVSDK_NGX_Parameter_Color, (void*) g_original);

    g_swapped = false;
    g_original = nullptr;
}
