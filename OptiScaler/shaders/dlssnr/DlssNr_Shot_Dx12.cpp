#include "pch.h"

#include "DlssNr_Shot_Dx12.h"

#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
// Frames between recording a copy and reading it back. There is no fence on the game's queue to wait
// on; with Reflex and frame generation the GPU runs at most a few frames behind, so this is ample.
constexpr unsigned long long kLag = 8;

// Every other pixel each way: a quarter of the frame is plenty for a mean and a percentile, and keeps
// the per-frame work on the render thread to a few milliseconds.
constexpr unsigned int kStride = 2;

// Histogram of the per-pixel change, in 1/1024 stop bins up to two stops.
constexpr unsigned int kBinsPerStop = 1024;
constexpr unsigned int kBins = 2 * kBinsPerStop;

enum class Kind
{
    Unsupported,
    Half4,   // R16G16B16A16_FLOAT, linear
    Float4,  // R32G32B32A32_FLOAT, linear
    Packed,  // R11G11B10_FLOAT, linear
    Rgb10,   // R10G10B10A2_UNORM, display-referred
    Rgba8,   // R8G8B8A8_UNORM(_SRGB), display-referred
    Bgra8    // B8G8R8A8_UNORM(_SRGB), display-referred
};

Kind KindOf(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return Kind::Half4;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return Kind::Float4;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return Kind::Packed;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return Kind::Rgb10;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return Kind::Rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return Kind::Bgra8;
    default:
        return Kind::Unsupported;
    }
}

bool IsLinear(Kind k) { return k == Kind::Half4 || k == Kind::Float4 || k == Kind::Packed; }

unsigned int BytesPerPixel(Kind k) { return k == Kind::Half4 ? 8u : (k == Kind::Float4 ? 16u : 4u); }

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t) (h >> 15) << 31;
    const uint32_t e = (h >> 10) & 31u;
    const uint32_t m = h & 1023u;

    if (e == 0)
    {
        const float v = std::ldexp((float) m, -24);
        return sign ? -v : v;
    }

    const uint32_t bits = e == 31 ? (sign | 0x7F800000u | (m << 13)) : (sign | ((e + 112u) << 23) | (m << 13));
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// One channel of R11G11B10_FLOAT: five exponent bits, no sign, mBits of mantissa.
float SmallFloat(uint32_t v, int mBits)
{
    const uint32_t e = v >> mBits;
    const uint32_t m = v & ((1u << mBits) - 1u);

    if (e == 0)
        return std::ldexp((float) m, -14 - mBits);

    if (e == 31)
        return 65504.0f;

    return std::ldexp(1.0f + (float) m / (float) (1u << mBits), (int) e - 15);
}

void ReadPixel(const uint8_t* p, Kind k, float rgb[3])
{
    switch (k)
    {
    case Kind::Half4:
    {
        const uint16_t* h = (const uint16_t*) p;
        rgb[0] = HalfToFloat(h[0]);
        rgb[1] = HalfToFloat(h[1]);
        rgb[2] = HalfToFloat(h[2]);
        break;
    }
    case Kind::Float4:
        std::memcpy(rgb, p, 12);
        break;
    case Kind::Packed:
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        rgb[0] = SmallFloat(v & 0x7FFu, 6);
        rgb[1] = SmallFloat((v >> 11) & 0x7FFu, 6);
        rgb[2] = SmallFloat((v >> 22) & 0x3FFu, 5);
        break;
    }
    case Kind::Rgb10:
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        rgb[0] = (float) (v & 1023u) / 1023.0f;
        rgb[1] = (float) ((v >> 10) & 1023u) / 1023.0f;
        rgb[2] = (float) ((v >> 20) & 1023u) / 1023.0f;
        break;
    }
    case Kind::Rgba8:
        rgb[0] = p[0] / 255.0f;
        rgb[1] = p[1] / 255.0f;
        rgb[2] = p[2] / 255.0f;
        break;
    case Kind::Bgra8:
        rgb[0] = p[2] / 255.0f;
        rgb[1] = p[1] / 255.0f;
        rgb[2] = p[0] / 255.0f;
        break;
    default:
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        break;
    }

    for (int c = 0; c < 3; ++c)
        rgb[c] = std::isfinite(rgb[c]) ? std::max(rgb[c], 0.0f) : 0.0f;
}

float Luma(const float rgb[3]) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }

// A filmic curve (Narkowicz's fit of ACES) and the sRGB encoding: the picture as a screen would show
// it, the same for every mode of one benchmark.
uint8_t Develop(float linear)
{
    const float x = std::max(linear, 0.0f);
    float t = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
    t = std::clamp(t, 0.0f, 1.0f);
    const float e = t <= 0.0031308f ? 12.92f * t : 1.055f * std::pow(t, 1.0f / 2.4f) - 0.055f;
    return (uint8_t) std::lround(std::clamp(e, 0.0f, 1.0f) * 255.0f);
}

bool WritePng(const std::filesystem::path& path, unsigned int w, unsigned int h, const std::vector<uint8_t>& bgr)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    do
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            break;

        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)))
            break;

        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
            break;

        if (FAILED(encoder->CreateNewFrame(&frame, &props)) || FAILED(frame->Initialize(props)))
            break;

        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;

        if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&format)) ||
            !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
            break;

        if (FAILED(frame->WritePixels(h, w * 3, (UINT) bgr.size(), (BYTE*) bgr.data())))
            break;

        ok = SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    } while (false);

    if (props != nullptr)
        props->Release();
    if (frame != nullptr)
        frame->Release();
    if (encoder != nullptr)
        encoder->Release();
    if (stream != nullptr)
        stream->Release();
    if (factory != nullptr)
        factory->Release();

    if (SUCCEEDED(init))
        CoUninitialize();

    return ok;
}

struct Slot
{
    ID3D12Resource* buffer = nullptr;
    unsigned long long frame = 0;
    bool pending = false;
};

struct Job
{
    bool active = false;
    int tag = 0;
    unsigned int wanted = 0;
    unsigned int recorded = 0;
    unsigned int collected = 0;
    std::filesystem::path png;

    Kind kind = Kind::Unsupported;
    unsigned int width = 0;
    unsigned int height = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    std::vector<Slot> slots;

    // The run's analysis, built as frames come back.
    std::vector<float> previous;
    bool havePrevious = false;
    float floor = 0.0f;
    double sum = 0.0;
    unsigned long long count = 0;
    std::vector<unsigned long long> histogram;

    // The first frame, rows packed tight, for the picture.
    std::vector<uint8_t> first;
};

Job g_job;
unsigned long long g_frame = 0;

std::mutex g_resultsMutex;
std::map<int, DlssNrShot::Result> g_results;

std::thread g_worker;
std::atomic<bool> g_writing { false };

// The exposure every picture of one benchmark is developed with, set by the first: 0 until then.
float g_exposure = 0.0f;

void ReleaseSlots()
{
    for (auto& s : g_job.slots)
    {
        if (s.buffer != nullptr)
            s.buffer->Release();
    }

    g_job.slots.clear();
}

void Abandon(const char* why)
{
    LOG_WARN("DLSS-NR benchmark capture: {}", why);

    {
        std::lock_guard<std::mutex> lock(g_resultsMutex);
        g_results[g_job.tag] = {};
    }

    ReleaseSlots();
    g_job = {};
}

void Collect(Slot& slot)
{
    void* mapped = nullptr;
    D3D12_RANGE range { 0, (SIZE_T) g_job.bytes };

    if (FAILED(slot.buffer->Map(0, &range, &mapped)) || mapped == nullptr)
    {
        ++g_job.collected;
        return;
    }

    const uint8_t* base = (const uint8_t*) mapped + g_job.footprint.Offset;
    const UINT pitch = g_job.footprint.Footprint.RowPitch;
    const unsigned int bpp = BytesPerPixel(g_job.kind);
    const unsigned int w = g_job.width;
    const unsigned int h = g_job.height;
    const unsigned int sw = (w + kStride - 1) / kStride;
    const unsigned int sh = (h + kStride - 1) / kStride;

    std::vector<float> luma((size_t) sw * sh);
    double logSum = 0.0;

    for (unsigned int y = 0, j = 0; y < h; y += kStride, ++j)
    {
        const uint8_t* row = base + (size_t) y * pitch;

        for (unsigned int x = 0, i = 0; x < w; x += kStride, ++i)
        {
            float rgb[3];
            ReadPixel(row + (size_t) x * bpp, g_job.kind, rgb);
            const float l = Luma(rgb);
            luma[(size_t) j * sw + i] = l;
            logSum += std::log2(std::max(l, 1e-8f));
        }
    }

    if (g_job.collected == 0)
    {
        // The scene's own level decides what counts as visible: a twentieth of its geometric mean. Below
        // that the change of a few counts is not something anyone sees, and would dominate a ratio.
        const float geoMean = std::exp2((float) (logSum / std::max<size_t>(1, luma.size())));
        g_job.floor = std::max(geoMean * 0.05f, 1e-7f);

        if (g_exposure <= 0.0f && IsLinear(g_job.kind))
            g_exposure = 0.18f / std::max(geoMean, 1e-8f);

        const size_t rowBytes = (size_t) w * bpp;
        g_job.first.resize(rowBytes * h);

        for (unsigned int y = 0; y < h; ++y)
            std::memcpy(g_job.first.data() + (size_t) y * rowBytes, base + (size_t) y * pitch, rowBytes);
    }

    D3D12_RANGE nothing { 0, 0 };
    slot.buffer->Unmap(0, &nothing);

    for (float& l : luma)
        l = std::log2(l + g_job.floor);

    if (g_job.havePrevious)
    {
        for (size_t k = 0; k < luma.size(); ++k)
        {
            const float d = std::abs(luma[k] - g_job.previous[k]);
            g_job.sum += d;
            ++g_job.count;
            g_job.histogram[std::min<size_t>((size_t) (d * (float) kBinsPerStop), kBins - 1)]++;
        }
    }

    g_job.previous.swap(luma);
    g_job.havePrevious = true;
    ++g_job.collected;
}

void Finish()
{
    DlssNrShot::Result r {};
    r.valid = g_job.count > 0;
    r.width = g_job.width;
    r.height = g_job.height;

    if (r.valid)
    {
        // The mean leaves out the largest 5% of changes: an animated object or particles in the still
        // scene move a few percent of the pixels a lot, and are not flicker (Control's floating debris
        // alone swung the 1%-trimmed figure by a third between runs). Stops become a percentage of
        // brightness as 2^d - 1.
        const auto binStops = [](unsigned int bin) { return (bin + 0.5) / (double) kBinsPerStop; };
        const unsigned long long keep = (unsigned long long) (0.95 * (double) g_job.count);
        const unsigned long long p95 = (unsigned long long) (0.90 * (double) g_job.count);
        unsigned long long seen = 0;
        double sum = 0.0;
        bool havePercentile = false;

        for (unsigned int bin = 0; bin < kBins && seen < keep; ++bin)
        {
            const unsigned long long take = std::min(g_job.histogram[bin], keep - seen);
            sum += take * binStops(bin);
            seen += take;

            if (!havePercentile && seen >= p95)
            {
                havePercentile = true;
                r.flickerP95 = (float) (100.0 * (std::exp2(binStops(bin)) - 1.0));
            }
        }

        r.flickerMean = (float) (100.0 * (std::exp2(sum / (double) std::max(1ull, seen)) - 1.0));
    }

    {
        std::lock_guard<std::mutex> lock(g_resultsMutex);
        g_results[g_job.tag] = r;
    }

    LOG_INFO("DLSS-NR benchmark capture {}: {} frames, flicker {:.3f}% mean, {:.3f}% p95", g_job.tag,
             g_job.collected, r.flickerMean, r.flickerP95);

    // The picture is developed and written off the render thread.
    if (!g_job.first.empty())
    {
        if (g_worker.joinable())
            g_worker.join();

        g_writing = true;

        g_worker = std::thread(
            [tag = g_job.tag, path = g_job.png, data = std::move(g_job.first), kind = g_job.kind, w = g_job.width,
             h = g_job.height, exposure = g_exposure > 0.0f ? g_exposure : 1.0f]()
            {
                const unsigned int bpp = BytesPerPixel(kind);
                std::vector<uint8_t> bgr((size_t) w * h * 3);

                for (unsigned int y = 0; y < h; ++y)
                {
                    const uint8_t* row = data.data() + (size_t) y * w * bpp;
                    uint8_t* out = bgr.data() + (size_t) y * w * 3;

                    for (unsigned int x = 0; x < w; ++x)
                    {
                        float rgb[3];
                        ReadPixel(row + (size_t) x * bpp, kind, rgb);

                        for (int c = 0; c < 3; ++c)
                        {
                            const uint8_t v = IsLinear(kind)
                                                  ? Develop(rgb[c] * exposure)
                                                  : (uint8_t) std::lround(std::clamp(rgb[c], 0.0f, 1.0f) * 255.0f);
                            out[x * 3 + (2 - c)] = v;
                        }
                    }
                }

                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
                const bool ok = WritePng(path, w, h, bgr);

                {
                    std::lock_guard<std::mutex> lock(g_resultsMutex);
                    g_results[tag].picture = ok;
                }

                if (!ok)
                    LOG_WARN("DLSS-NR benchmark capture: could not write {}", path.string());

                g_writing = false;
            });
    }

    ReleaseSlots();
    g_job = {};
}

void Record(ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_DESC desc = output->GetDesc();

    if (g_job.recorded == 0)
    {
        g_job.kind = KindOf(desc.Format);

        if (g_job.kind == Kind::Unsupported || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            Abandon("the frame's format is not one it can read");
            return;
        }

        ID3D12Device* device = nullptr;

        if (FAILED(output->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        {
            Abandon("no device");
            return;
        }

        device->GetCopyableFootprints(&desc, 0, 1, 0, &g_job.footprint, nullptr, nullptr, &g_job.bytes);
        g_job.width = (unsigned int) desc.Width;
        g_job.height = desc.Height;
        g_job.histogram.assign(kBins, 0);

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = g_job.bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        g_job.slots.resize(g_job.wanted);

        for (auto& s : g_job.slots)
        {
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&s.buffer))))
                s.buffer = nullptr;
        }

        device->Release();

        for (auto& s : g_job.slots)
        {
            if (s.buffer == nullptr)
            {
                Abandon("not enough memory for the readback");
                return;
            }
        }
    }
    else if ((unsigned int) desc.Width != g_job.width || desc.Height != g_job.height)
    {
        Abandon("the frame changed size while recording");
        return;
    }

    Slot& slot = g_job.slots[g_job.recorded];

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = output;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = slot.buffer;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = g_job.footprint;

    auto barrier = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = output;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
    };

    barrier(state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, state);

    slot.frame = g_frame;
    slot.pending = true;
    ++g_job.recorded;
}
} // namespace

namespace DlssNrShot
{
void Request(int tag, unsigned int frames, const std::filesystem::path& png)
{
    if (Busy())
    {
        LOG_WARN("DLSS-NR benchmark capture: still busy with the previous one");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_resultsMutex);
        g_results.erase(tag);
    }

    g_job = {};
    g_job.active = true;
    g_job.tag = tag;
    g_job.wanted = std::clamp(frames, 2u, 16u);
    g_job.png = png;
}

bool Busy() { return g_job.active || g_writing; }

Result Get(int tag)
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    const auto it = g_results.find(tag);
    return it != g_results.end() ? it->second : Result {};
}

void Reset()
{
    std::lock_guard<std::mutex> lock(g_resultsMutex);
    g_results.clear();
    g_exposure = 0.0f;
}

void Tick(ID3D12GraphicsCommandList* cmd, ID3D12Resource* output, D3D12_RESOURCE_STATES state)
{
    ++g_frame;

    if (!g_job.active)
        return;

    for (auto& s : g_job.slots)
    {
        if (s.pending && g_frame - s.frame >= kLag)
        {
            s.pending = false;
            Collect(s);
        }
    }

    if (g_job.recorded < g_job.wanted && cmd != nullptr && output != nullptr)
    {
        Record(cmd, output, state);

        if (!g_job.active)
            return;
    }

    if (g_job.collected >= g_job.wanted)
        Finish();
}

void Shutdown()
{
    if (g_worker.joinable())
        g_worker.join();

    ReleaseSlots();
    g_job = {};
}
} // namespace DlssNrShot
