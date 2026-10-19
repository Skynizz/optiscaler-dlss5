#include "pch.h"
#include "FGMeter.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <hooks/D3D12_Hooks.h>
#include <hooks/Streamline_Hooks.h>
#include <hudfix/Hudfix_Dx12.h>
#include <shaders/fg_meter/FGM_Dx12.h>

#include <dxgi1_4.h>

#include <algorithm>
#include <deque>
#include <format>
#include <memory>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr UINT kThumbW = 64;
constexpr UINT kThumbH = 36;
constexpr UINT kRowPitch = 256; // 64 floats: exactly the copy alignment
constexpr UINT kSlots = 8;

std::mutex g_mutex;
LONGLONG g_frequency = 0;

LONGLONG Now()
{
    if (g_frequency == 0)
    {
        LARGE_INTEGER f {};
        QueryPerformanceFrequency(&f);
        g_frequency = f.QuadPart;
    }

    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double Seconds(LONGLONG ticks) { return (double) ticks / (double) g_frequency; }

bool MeterOn() { return Config::Instance()->FGMeter.value_or_default(); }
bool AutoOn() { return Config::Instance()->FGAutoMultiplier.value_or_default(); }

// What happened in the last two seconds.
std::deque<LONGLONG> g_presents;   // frames handed to the display
std::deque<bool> g_presentTearing; // whether each was presented with tearing allowed (same order)
int g_lastSyncInterval = -1;
std::deque<LONGLONG> g_realFrames; // frames the upscaler ran for
LONGLONG g_lastReal = 0;

struct Diff
{
    LONGLONG at;
    double value;
};

std::deque<Diff> g_diffs; // between consecutive thumbnails

int g_queued = -1;
double g_refresh = 0.0;
LONGLONG g_refreshAt = 0;

FGMeter::Snapshot g_snapshot;
LONGLONG g_snapshotAt = 0;
double g_diffMedian = 0.0;
// Thumbnail bookkeeping, for the log.
std::atomic<unsigned> g_thumbCalls { 0 };
std::atomic<unsigned> g_thumbSubmitted { 0 };
std::atomic<unsigned> g_thumbRead { 0 };
std::atomic<unsigned> g_thumbFull { 0 };
std::atomic<UINT64> g_thumbFenceDone { 0 };
// The median difference between consecutive thumbnails below which the scene counts as still.
constexpr double kStillBelow = 0.003;
LONGLONG g_logAt = 0;

// The auto multiplier.
int g_autoMultiplier = 0;
double g_autoTarget = 0.0;
LONGLONG g_autoChangedAt = 0;
LONGLONG g_upSince = 0;
LONGLONG g_downSince = 0;
bool g_autoNative = false;

template <class T> void Trim(std::deque<T>& q, LONGLONG before)
{
    while (!q.empty() && q.front() < before)
        q.pop_front();
}

void TrimDiffs(LONGLONG before)
{
    while (!g_diffs.empty() && g_diffs.front().at < before)
        g_diffs.pop_front();
}

double RateOverLastSecond(const std::deque<LONGLONG>& q, LONGLONG now)
{
    const LONGLONG from = now - g_frequency;
    LONGLONG first = 0;
    LONGLONG last = 0;
    size_t n = 0;

    for (const LONGLONG t : q)
    {
        if (t < from)
            continue;

        if (n == 0)
            first = t;

        last = t;
        n++;
    }

    if (n < 2 || last <= first)
        return 0.0;

    return (double) (n - 1) / Seconds(last - first);
}

// Called with the lock held.
void ComputeSnapshot(LONGLONG now)
{
    FGMeter::Snapshot s {};
    const LONGLONG from = now - g_frequency;

    std::vector<double> intervals;
    LONGLONG previous = 0;
    const double scan = g_refresh > 0.0 ? 1.0 / g_refresh : 0.0;
    size_t presented = 0;
    size_t tearing = 0;
    size_t fast = 0;
    size_t risky = 0;

    for (size_t i = 0; i < g_presents.size(); i++)
    {
        const LONGLONG t = g_presents[i];

        if (t < from)
            continue;

        presented++;
        const bool tears = i < g_presentTearing.size() && g_presentTearing[i];

        if (tears)
            tearing++;

        if (previous != 0)
        {
            const double dt = Seconds(t - previous);
            intervals.push_back(dt);

            // Less than one refresh after the frame before: the display is still drawing it.
            if (scan > 0.0 && dt < scan)
            {
                fast++;

                if (tears)
                    risky++;
            }
        }

        previous = t;
    }

    if (presented > 0)
    {
        s.tearingAllowed = (double) tearing / (double) presented;
        s.fasterThanScan = (double) fast / (double) presented;
        s.tearRisk = (double) risky / (double) presented;
    }

    s.syncInterval = g_lastSyncInterval;

    if (intervals.size() >= 2)
    {
        double sum = 0.0;

        for (const double dt : intervals)
            sum += dt;

        const double ideal = sum / (double) intervals.size();
        size_t onTime = 0;
        size_t stacked = 0;
        size_t late = 0;

        for (const double dt : intervals)
        {
            if (dt < ideal * 0.25)
                stacked++;
            else if (dt > ideal * 1.5)
                late++;
            else if (dt >= ideal * 0.5)
                onTime++;
        }

        s.valid = true;
        s.shownFps = 1.0 / ideal;
        s.onTime = (double) onTime / (double) intervals.size();
        s.stacked = (double) stacked / (double) intervals.size();
        s.late = (double) late / (double) intervals.size();
    }

    s.renderedFps = RateOverLastSecond(g_realFrames, now);
    s.multiplier = s.renderedFps > 0.0 ? s.shownFps / s.renderedFps : 0.0;

    // Duplicates: a frame barely different from the one before while the scene as a whole moves.
    std::vector<double> diffs;

    for (const Diff& d : g_diffs)
    {
        if (d.at >= from)
            diffs.push_back(d.value);
    }

    if (diffs.size() >= 6)
    {
        std::vector<double> sorted = diffs;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        g_diffMedian = median;

        if (median > kStillBelow)
        {
            size_t dup = 0;

            for (const double d : diffs)
            {
                if (d < median * 0.2)
                    dup++;
            }

            s.duplicatesKnown = true;
            s.duplicates = (double) dup / (double) diffs.size();
        }
    }

    s.queued = g_queued;
    s.refresh = g_refresh;
    s.target = g_autoTarget;
    s.autoMultiplier = g_autoMultiplier;
    g_snapshot = s;

    if (MeterOn() && (g_logAt == 0 || Seconds(now - g_logAt) >= 5.0))
    {
        g_logAt = now;
        LOG_INFO("FG meter: shown {:.0f} fps, rendered {:.1f} fps (x{:.2f}), on time {:.0f}%, stacked {:.0f}%, "
                 "late {:.0f}%, duplicates {} (frame diff {:.4f}), queued {}, refresh {:.0f} Hz, auto {}",
                 s.shownFps, s.renderedFps, s.multiplier, s.onTime * 100.0, s.stacked * 100.0, s.late * 100.0,
                 s.duplicatesKnown ? std::format("{:.0f}%", s.duplicates * 100.0) : std::string("n/a (still)"),
                 g_diffMedian, s.queued, s.refresh, g_autoMultiplier);
        LOG_INFO("FG meter tearing: sync interval {}, tearing allowed {:.0f}%, faster than one refresh {:.0f}%, "
                 "tear risk {:.0f}%",
                 s.syncInterval, s.tearingAllowed * 100.0, s.fasterThanScan * 100.0, s.tearRisk * 100.0);

    }
}

// --- The auto multiplier -------------------------------------------------------------------------------------

void ApplyMultiplier(int multiplier, bool native, double rendered)
{
    auto* cfg = Config::Instance();

    if (native)
    {
        cfg->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
        cfg->FGDLSSGOverrideInterpolationCount.set_volatile_value(multiplier - 1);
        StreamlineHooks::updateDlssgOptions();
    }
    else
    {
        cfg->FGDLSSGForceDMFG.set_volatile_value(false);
        cfg->FGDLSSGInterpolationCount.set_volatile_value(multiplier - 1);
    }

    LOG_INFO("FG auto multiplier: {}x (rendered {:.1f} fps, target {:.0f} fps)", multiplier, rendered, g_autoTarget);
}

// Auto off: the multiplier saved in the ini comes back.
void RestoreMultiplier()
{
    auto* cfg = Config::Instance();

    if (g_autoNative)
    {
        cfg->FGDLSSGOverrideInterpolationCount = cfg->FGDLSSGOverrideInterpolationCount.value_for_config();
        cfg->FGDLSSGOverrideForceDMFG = cfg->FGDLSSGOverrideForceDMFG.value_for_config();
        StreamlineHooks::updateDlssgOptions();
    }
    else
    {
        cfg->FGDLSSGInterpolationCount = cfg->FGDLSSGInterpolationCount.value_for_config();
        cfg->FGDLSSGForceDMFG = cfg->FGDLSSGForceDMFG.value_for_config();
    }

    LOG_INFO("FG auto multiplier off: the saved multiplier is back");
}

void RunAuto(LONGLONG now)
{
    const auto& st = State::Instance();

    if (!AutoOn())
    {
        if (g_autoMultiplier != 0)
        {
            RestoreMultiplier();
            g_autoMultiplier = 0;
            g_autoTarget = 0.0;
        }

        return;
    }

    // NVIDIA's frame generation, running: the game's own (its multiplier overridden) or OptiScaler's injected one.
    const bool native = st.dlssgGameNative && st.activeFgOutput != FGOutput::DLSSG;
    const bool injected = st.activeFgOutput == FGOutput::DLSSG;

    if ((!native && !injected) || !st.dlssgMfgMax.has_value())
        return;

    double rendered = 0.0;
    double refresh = 0.0;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        rendered = RateOverLastSecond(g_realFrames, now);
        refresh = g_refresh;
    }

    const float wanted = Config::Instance()->FGAutoMultiplierTarget.value_or_default();
    const double target = wanted > 0.0f ? (double) wanted : refresh * 0.95;

    if (rendered < 5.0 || target <= 0.0)
        return;

    g_autoTarget = target;
    g_autoNative = native;

    const int maxMultiplier = std::clamp(st.dlssgMfgMax.value() + 1, 2, 6);

    if (g_autoMultiplier == 0)
    {
        g_autoMultiplier = std::clamp((int) (target / rendered), 2, maxMultiplier);
        g_autoChangedAt = now;
        g_upSince = g_downSince = 0;
        ApplyMultiplier(g_autoMultiplier, native, rendered);
        return;
    }

    g_autoMultiplier = std::min(g_autoMultiplier, maxMultiplier);

    // Down at once when the frames shown pass the target (more than the display shows, or than asked for).
    if (g_autoMultiplier > 2 && rendered * g_autoMultiplier > target * 1.02)
    {
        if (g_downSince == 0)
            g_downSince = now;

        if (Seconds(now - g_downSince) >= 0.3 && Seconds(now - g_autoChangedAt) >= 0.5)
        {
            g_autoMultiplier--;
            g_autoChangedAt = now;
            g_downSince = 0;
            ApplyMultiplier(g_autoMultiplier, native, rendered);
        }

        return;
    }

    g_downSince = 0;

    // Up only when one more still fits under the target, steadily: generated frames cost a little, so the rendered
    // rate drops once it goes up, and the margin keeps it from going back and forth.
    if (g_autoMultiplier < maxMultiplier && rendered * (g_autoMultiplier + 1) < target * 0.97)
    {
        if (g_upSince == 0)
            g_upSince = now;

        if (Seconds(now - g_upSince) >= 1.5 && Seconds(now - g_autoChangedAt) >= 1.0)
        {
            g_autoMultiplier++;
            g_autoChangedAt = now;
            g_upSince = 0;
            ApplyMultiplier(g_autoMultiplier, native, rendered);
        }
    }
    else
    {
        g_upSince = 0;
    }
}

// --- Thumbnails ----------------------------------------------------------------------------------------------

struct Slot
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT64 fence = 0;
    bool pending = false;
    bool afterGap = false; // a frame before this one was not captured: no difference against the last
};

std::unique_ptr<FGM_Dx12> g_shader;
ID3D12Device* g_thumbDevice = nullptr;
ID3D12Resource* g_thumb = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
D3D12_COMMAND_LIST_TYPE g_listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
Slot g_slots[kSlots];
UINT g_next = 0;
UINT g_harvest = 0;
bool g_thumbFailed = false;
bool g_gap = false;
std::vector<float> g_previous;
bool g_havePrevious = false;

// A dump of consecutive presented frames at full resolution (fgdump.trigger beside OptiScaler): every frame the
// display was handed, generated ones included, to look at exactly what frame generation made.
struct DumpFrame
{
    ID3D12Resource* readback = nullptr;
    UINT64 fence = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bytes = 0;
    LONGLONG at = 0;
};

constexpr int kDumpFrames = 14;
std::vector<DumpFrame> g_dump;
int g_dumpWanted = 0;
LONGLONG g_dumpCheckedAt = 0;

void WriteDump()
{
    struct Out
    {
        std::vector<unsigned char> data;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        LONGLONG at;
    };

    auto frames = std::make_shared<std::vector<Out>>();

    for (DumpFrame& d : g_dump)
    {
        void* mapped = nullptr;

        if (d.readback->Map(0, nullptr, &mapped) == S_OK && mapped != nullptr)
        {
            Out o;
            o.data.assign((const unsigned char*) mapped, (const unsigned char*) mapped + d.bytes);
            o.footprint = d.footprint;
            o.at = d.at;
            frames->push_back(std::move(o));

            const D3D12_RANGE nothing { 0, 0 };
            d.readback->Unmap(0, &nothing);
        }

        d.readback->Release();
    }

    g_dump.clear();
    const LONGLONG frequency = g_frequency;
    const auto dir = Util::DllPath().remove_filename() / "fgdump";

    // Written away from the presenting thread: 14 full frames would stall it.
    std::thread([frames, dir, frequency]() {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream manifest(dir / "manifest.txt");
        const LONGLONG first = frames->empty() ? 0 : frames->front().at;

        for (size_t i = 0; i < frames->size(); i++)
        {
            const Out& o = (*frames)[i];
            const auto name = std::format("frame_{:02}.raw", i);
            std::ofstream f(dir / name, std::ios::binary);
            f.write((const char*) o.data.data(), (std::streamsize) o.data.size());

            manifest << name << " width=" << o.footprint.Footprint.Width << " height=" << o.footprint.Footprint.Height
                     << " format=" << (int) o.footprint.Footprint.Format
                     << " rowpitch=" << o.footprint.Footprint.RowPitch
                     << " ms=" << std::format("{:.3f}", (double) (o.at - first) * 1000.0 / (double) frequency) << "\n";
        }

        LOG_INFO("FG meter: dumped {} presented frames to {}", frames->size(), dir.string());
    }).detach();
}

bool InitThumbnails(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
{
    g_thumbDevice = device;
    g_listType = type;
    g_shader = std::make_unique<FGM_Dx12>("FG meter thumbnail", device);

    if (!g_shader->IsInit())
        return false;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = kThumbW;
    tex.Height = kThumbH;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R32_FLOAT;
    tex.SampleDesc.Count = 1;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        nullptr, IID_PPV_ARGS(&g_thumb)) != S_OK)
        return false;

    D3D12_HEAP_PROPERTIES readHeap {};
    readHeap.Type = D3D12_HEAP_TYPE_READBACK;
    const D3D12_RESOURCE_DESC buffer = CD3DX12_RESOURCE_DESC::Buffer((UINT64) kRowPitch * kThumbH);

    for (Slot& s : g_slots)
    {
        if (device->CreateCommandAllocator(type, IID_PPV_ARGS(&s.allocator)) != S_OK ||
            device->CreateCommandList(0, type, s.allocator, nullptr, IID_PPV_ARGS(&s.list)) != S_OK ||
            device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&s.readback)) != S_OK)
            return false;

        s.list->Close();
    }

    if (device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
        return false;

    g_previous.assign(kThumbW * kThumbH, 0.0f);
    LOG_INFO("FG meter: thumbnails of every presented frame, on a {} queue",
             type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute");
    return true;
}

// Reads every finished thumbnail, in order, and records its difference from the one before.
void Harvest(LONGLONG now)
{
    std::vector<float> current(kThumbW * kThumbH);

    // The slots in flight are a run starting at g_harvest, in the order they were submitted. (Comparing g_harvest
    // with g_next cannot tell a full ring from an empty one: once all eight were in flight, nothing was read again.)
    for (UINT i = 0; i < kSlots; i++)
    {
        Slot& s = g_slots[g_harvest];

        if (!s.pending || g_fence->GetCompletedValue() < s.fence)
            break;

        {
            void* mapped = nullptr;
            const D3D12_RANGE range { 0, (SIZE_T) kRowPitch * kThumbH };

            if (s.readback->Map(0, &range, &mapped) == S_OK && mapped != nullptr)
            {
                for (UINT y = 0; y < kThumbH; y++)
                    memcpy(&current[y * kThumbW], (const unsigned char*) mapped + y * kRowPitch, kThumbW * sizeof(float));

                const D3D12_RANGE nothing { 0, 0 };
                s.readback->Unmap(0, &nothing);

                if (g_havePrevious && !s.afterGap)
                {
                    double diff = 0.0;
                    double level = 0.0;

                    for (size_t i = 0; i < current.size(); i++)
                    {
                        diff += std::abs((double) current[i] - (double) g_previous[i]);
                        level += 0.5 * ((double) current[i] + (double) g_previous[i]);
                    }

                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_diffs.push_back({ now, diff / (level + 1e-3) });
                }

                g_previous.swap(current);
                g_havePrevious = true;
                g_thumbRead++;
            }

            s.pending = false;
        }

        g_harvest = (g_harvest + 1) % kSlots;
    }
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}
} // namespace

void FGMeter::OnRealFrame()
{
    if (!MeterOn() && !AutoOn() && g_autoMultiplier == 0)
        return;

    const LONGLONG now = Now();

    // Some games run the upscaler twice for one frame: a second run within 2 ms is the same frame.
    if (g_lastReal != 0 && Seconds(now - g_lastReal) < 0.002)
        return;

    g_lastReal = now;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_realFrames.push_back(now);
        Trim(g_realFrames, now - 2 * g_frequency);
    }

    RunAuto(now);
}

void FGMeter::BeforePresent(UINT syncInterval, UINT flags)
{
    if (!MeterOn() && !AutoOn())
        return;

    const LONGLONG now = Now();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_presents.push_back(now);
    g_presentTearing.push_back(syncInterval == 0 || (flags & DXGI_PRESENT_ALLOW_TEARING) != 0);
    g_lastSyncInterval = (int) syncInterval;

    while (!g_presents.empty() && g_presents.front() < now - 2 * g_frequency)
    {
        g_presents.pop_front();
        g_presentTearing.pop_front();
    }

    TrimDiffs(now - 2 * g_frequency);

    if (g_snapshotAt == 0 || Seconds(now - g_snapshotAt) >= 0.5)
    {
        g_snapshotAt = now;
        ComputeSnapshot(now);
    }
}

void FGMeter::AfterPresent(IDXGISwapChain* swapChain)
{
    if ((!MeterOn() && !AutoOn()) || swapChain == nullptr)
        return;

    const LONGLONG now = Now();

    // Frames waiting for the display: presented, not yet shown.
    int queued = -1;

    if (MeterOn())
    {
        DXGI_FRAME_STATISTICS stats {};
        UINT last = 0;

        if (swapChain->GetFrameStatistics(&stats) == S_OK && swapChain->GetLastPresentCount(&last) == S_OK &&
            last >= stats.PresentCount)
        {
            queued = (int) (last - stats.PresentCount);
        }
    }

    // The display's refresh rate, every few seconds (the window can move to another monitor).
    double refresh = 0.0;
    bool readRefresh = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queued = queued;
        readRefresh = g_refreshAt == 0 || Seconds(now - g_refreshAt) >= 3.0;

        if (readRefresh)
            g_refreshAt = now;
    }

    if (readRefresh)
    {
        DXGI_SWAP_CHAIN_DESC desc {};

        if (swapChain->GetDesc(&desc) == S_OK && desc.OutputWindow != nullptr)
        {
            HMONITOR monitor = MonitorFromWindow(desc.OutputWindow, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW info {};
            info.cbSize = sizeof(info);
            DEVMODEW mode {};
            mode.dmSize = sizeof(mode);

            if (monitor != nullptr && GetMonitorInfoW(monitor, &info) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            {
                refresh = (double) mode.dmDisplayFrequency;
            }
        }

        if (refresh > 0.0)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_refresh = refresh;
        }
    }
}

void FGMeter::Thumbnail(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    if (!MeterOn() || swapChain == nullptr || queue == nullptr || g_thumbFailed ||
        State::Instance().activeFgOutput == FGOutput::Synced)
        return;

    g_thumbCalls++;

    const D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();

    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && qd.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        return;

    IDXGISwapChain3* sc3 = nullptr;

    if (swapChain->QueryInterface(IID_PPV_ARGS(&sc3)) != S_OK || sc3 == nullptr)
        return;

    ID3D12Resource* backBuffer = nullptr;
    const UINT index = sc3->GetCurrentBackBufferIndex();
    const HRESULT got = sc3->GetBuffer(index, IID_PPV_ARGS(&backBuffer));
    sc3->Release();

    if (got != S_OK || backBuffer == nullptr)
        return;

    ID3D12Device* device = nullptr;

    if (backBuffer->GetDevice(IID_PPV_ARGS(&device)) != S_OK || device == nullptr)
    {
        backBuffer->Release();
        return;
    }

    const LONGLONG now = Now();

    if (g_thumbDevice == nullptr && !InitThumbnails(device, qd.Type))
    {
        LOG_ERROR("FG meter: thumbnails could not be set up; duplicates are not measured this session");
        g_thumbFailed = true;
    }

    // Our lists are of one type; a queue of the other is skipped.
    if (g_thumbFailed || device != g_thumbDevice || qd.Type != g_listType)
    {
        device->Release();
        backBuffer->Release();
        return;
    }

    Harvest(now);

    // A dump asked for? Checked twice a second.
    if (g_dumpWanted == 0 && g_dump.empty() && (g_dumpCheckedAt == 0 || Seconds(now - g_dumpCheckedAt) >= 0.5))
    {
        g_dumpCheckedAt = now;
        const auto trigger = Util::DllPath().remove_filename() / "fgdump.trigger";
        std::error_code ec;

        if (std::filesystem::exists(trigger, ec))
        {
            std::filesystem::remove(trigger, ec);
            g_dumpWanted = kDumpFrames;
            LOG_INFO("FG meter: dumping the next {} presented frames", kDumpFrames);
        }
    }

    // A finished dump is read back and written out.
    if (g_dumpWanted == 0 && !g_dump.empty() && g_fence->GetCompletedValue() >= g_dump.back().fence)
        WriteDump();

    Slot& s = g_slots[g_next];

    if (s.pending)
    {
        // All slots in flight: this frame goes uncaptured, and the next one is not compared against the last.
        g_gap = true;
        g_thumbFull++;
        g_thumbFenceDone = g_fence->GetCompletedValue();
        device->Release();
        backBuffer->Release();
        return;
    }

    const D3D12_RESOURCE_DESC bd = backBuffer->GetDesc();

    if (s.allocator->Reset() != S_OK || s.list->Reset(s.allocator, nullptr) != S_OK)
    {
        device->Release();
        backBuffer->Release();
        return;
    }

    {
        // Our own list: our binds are not recorded as anyone else's, and HUDFix does not look at them.
        ScopedSkipHeapCapture skipHeap;
        D3D12Hooks::SetRootSignatureTracking(false);
        const bool hudfixWas = Hudfix_Dx12::GetSkipStatus();
        Hudfix_Dx12::SetSkipStatus(true);

        FGMConstants c {};
        c.srcWidth = (UINT) bd.Width;
        c.srcHeight = bd.Height;
        c.thumbWidth = kThumbW;
        c.thumbHeight = kThumbH;

        Barrier(s.list, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_shader->Dispatch(s.list, backBuffer, g_thumb, c);

        // The dump: the whole frame, copied out as it goes to the display.
        if (g_dumpWanted > 0)
        {
            DumpFrame d;
            const D3D12_RESOURCE_DESC rd = backBuffer->GetDesc();
            device->GetCopyableFootprints(&rd, 0, 1, 0, &d.footprint, nullptr, nullptr, &d.bytes);

            D3D12_HEAP_PROPERTIES readHeap {};
            readHeap.Type = D3D12_HEAP_TYPE_READBACK;
            const D3D12_RESOURCE_DESC buffer = CD3DX12_RESOURCE_DESC::Buffer(d.bytes);

            if (device->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&d.readback)) == S_OK)
            {
                Barrier(s.list, backBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);

                D3D12_TEXTURE_COPY_LOCATION ddst {};
                ddst.pResource = d.readback;
                ddst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                ddst.PlacedFootprint = d.footprint;

                D3D12_TEXTURE_COPY_LOCATION dsrc {};
                dsrc.pResource = backBuffer;
                dsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dsrc.SubresourceIndex = 0;

                s.list->CopyTextureRegion(&ddst, 0, 0, 0, &dsrc, nullptr);
                Barrier(s.list, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                d.fence = g_fenceValue + 1; // the value this list signals
                d.at = now;
                g_dump.push_back(d);
            }

            g_dumpWanted--;
        }

        Barrier(s.list, backBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        Barrier(s.list, g_thumb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = s.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        dst.PlacedFootprint.Footprint.Width = kThumbW;
        dst.PlacedFootprint.Footprint.Height = kThumbH;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;

        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = g_thumb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        s.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(s.list, g_thumb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        Hudfix_Dx12::SetSkipStatus(hudfixWas);
        D3D12Hooks::SetRootSignatureTracking(true);
    }

    if (s.list->Close() == S_OK)
    {
        ID3D12CommandList* lists[] = { s.list };
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(g_fence, ++g_fenceValue);

        s.fence = g_fenceValue;
        s.pending = true;
        g_thumbSubmitted++;
        s.afterGap = g_gap;
        g_gap = false;
        g_next = (g_next + 1) % kSlots;
    }

    device->Release();
    backBuffer->Release();
}

bool FGMeter::WantsPresentHook() { return MeterOn() || AutoOn(); }

FGMeter::Snapshot FGMeter::Get()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FGMeter::Snapshot s = g_snapshot;
    s.autoMultiplier = g_autoMultiplier;
    s.target = g_autoTarget;
    return s;
}
