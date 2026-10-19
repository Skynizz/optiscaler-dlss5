#include "pch.h"
#include "SyncedSwapchain.h"

#include <Util.h>

SyncedDXGISwapChain::SyncedDXGISwapChain(IDXGISwapChain* real, ID3D12CommandQueue* gameQueue,
                                         ID3D12CommandQueue* presentQueue, HWND hwnd)
    : _real(real), _queue(presentQueue), _gameQueue(gameQueue), _hwnd(hwnd)
{
    // The real swapchain's own reference is the one we hold; the wider interfaces are borrowed from it.
    if (_real->QueryInterface(IID_PPV_ARGS(&_real1)) == S_OK)
        _real1->Release();

    if (_real->QueryInterface(IID_PPV_ARGS(&_real2)) == S_OK)
        _real2->Release();

    if (_real->QueryInterface(IID_PPV_ARGS(&_real3)) == S_OK)
        _real3->Release();

    if (_real->QueryInterface(IID_PPV_ARGS(&_real4)) == S_OK)
        _real4->Release();

    _queue->AddRef();
    _gameQueue->AddRef();
    _queue->GetDevice(IID_PPV_ARGS(&_device));

    // Under Streamline (a game shipping DLSS Frame Generation) the swapchain arrives on Streamline's own pacer queue,
    // not the game's: frame generation then works among NVIDIA's pacing, unsynchronised with the game's rendering,
    // and the GPU hung (The Witcher 3, DRED). Said plainly; removing sl.dlss_g.dll from the game avoids it.
    {
        wchar_t name[128] = {};
        UINT size = sizeof(name) - sizeof(wchar_t);

        if (_gameQueue->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name) == S_OK)
        {
            const std::wstring queueName(name, size / sizeof(wchar_t));

            if (queueName.find(L"sl.dlss_g") != std::wstring::npos)
                LOG_ERROR("Synced FG: the swapchain is on Streamline's DLSS-G queue ({}), not the game's -- expect hangs. "
                          "Remove or rename sl.dlss_g.dll in the game folder to use Synced FG",
                          wstring_to_string(queueName));
            else
                LOG_INFO("Synced FG: game queue {}", wstring_to_string(queueName));
        }
    }

    LARGE_INTEGER frequency {};
    QueryPerformanceFrequency(&frequency);
    _qpcFrequency = std::max<LONGLONG>(frequency.QuadPart, 1);

    if (_device != nullptr)
    {
        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_readyFence));
        _readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        _fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        for (auto& slot : _slots)
        {
            _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));

            if (slot.allocator != nullptr &&
                _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
                                           IID_PPV_ARGS(&slot.list)) == S_OK)
            {
                slot.list->SetName(L"Synced FG present");
                slot.list->Close();
            }
        }
    }

    CreateProxies();
    ReadRefreshRate();

    _timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (_timer == nullptr)
        LOG_WARN("Synced FG: no high-resolution timer, pacing falls back to Sleep");

    _thread = std::thread(&SyncedDXGISwapChain::PresenterLoop, this);
    _watchdog = std::thread(&SyncedDXGISwapChain::WatchdogLoop, this);

    LOG_INFO("Synced FG swapchain created, {} back buffers", _proxies.size());
}

SyncedDXGISwapChain::~SyncedDXGISwapChain() {}

bool SyncedDXGISwapChain::CreateProxies()
{
    if (_device == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};

    if (_real->GetDesc(&desc) != S_OK)
        return false;

    for (UINT i = 0; i < desc.BufferCount; i++)
    {
        ID3D12Resource* realBuffer = nullptr;

        if (_real->GetBuffer(i, IID_PPV_ARGS(&realBuffer)) != S_OK || realBuffer == nullptr)
        {
            LOG_ERROR("Synced FG: could not get real back buffer {}", i);
            ReleaseBuffers();
            return false;
        }

        _realBuffers.push_back(realBuffer);

        const D3D12_RESOURCE_DESC bufferDesc = realBuffer->GetDesc();
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* proxy = nullptr;

        if (_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&proxy)) != S_OK ||
            proxy == nullptr)
        {
            LOG_ERROR("Synced FG: could not create back buffer {}", i);
            ReleaseBuffers();
            return false;
        }

        proxy->SetName(std::format(L"Synced FG back buffer {}", i).c_str());
        _proxies.push_back(proxy);
    }

    _proxyIndex = 0;
    _generation++;
    return true;
}

void SyncedDXGISwapChain::ReleaseBuffers()
{
    for (auto* proxy : _proxies)
        proxy->Release();

    for (auto* buffer : _realBuffers)
        buffer->Release();

    _proxies.clear();
    _realBuffers.clear();
}

void SyncedDXGISwapChain::WaitIdle(std::unique_lock<std::mutex>& lock)
{
    _drain.wait(lock, [this] { return (_jobs.empty() && !_busy) || _stop; });
}

void SyncedDXGISwapChain::WaitGpu()
{
    if (_fence == nullptr || _fenceEvent == nullptr || _readyFence == nullptr)
        return;

    // Both queues: the game's (the frames being recorded) and ours (the copies and presents).
    const UINT64 ready = ++_readyValue;
    _gameQueue->Signal(_readyFence, ready);

    if (_readyFence->GetCompletedValue() < ready)
    {
        _readyFence->SetEventOnCompletion(ready, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 5000);
    }

    const UINT64 value = ++_fenceValue;
    _queue->Signal(_fence, value);

    if (_fence->GetCompletedValue() < value)
    {
        _fence->SetEventOnCompletion(value, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 5000);
    }
}

// The refresh rate of the monitor the window is on.
void SyncedDXGISwapChain::ReadRefreshRate()
{
    HMONITOR monitor = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);

    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        return;

    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);

    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
    {
        _refreshHz = (double) mode.dmDisplayFrequency;
        LOG_INFO("Synced FG: display refreshes at {} Hz", mode.dmDisplayFrequency);
    }
}

// Sleeps on the high-resolution timer until half a millisecond before `due`, then spins.
void SyncedDXGISwapChain::WaitUntil(LONGLONG due)
{
    while (true)
    {
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        const double remainingMs = (double) (due - now.QuadPart) * 1000.0 / (double) _qpcFrequency;

        if (remainingMs <= 0.0)
            return;

        if (remainingMs > 0.6)
        {
            if (_timer != nullptr)
            {
                LARGE_INTEGER relative {};
                relative.QuadPart = -(LONGLONG) ((remainingMs - 0.5) * 10000.0); // 100 ns units, relative

                if (SetWaitableTimerEx(_timer, &relative, 0, nullptr, nullptr, nullptr, 0))
                {
                    WaitForSingleObject(_timer, 100);
                    continue;
                }
            }

            Sleep(remainingMs > 2.0 ? 1 : 0);
        }
        else
        {
            YieldProcessor();
        }
    }
}

// Every present (or drop), and every few seconds a line: the presents per second, the mean interval between them
// and the 99th percentile, the largest, and how many generated frames were dropped.
void SyncedDXGISwapChain::RecordPresent(bool presented)
{
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);

    if (!presented)
    {
        _dropped++;
        return;
    }

    if (_lastPresent != 0)
        _presentIntervals.push_back((double) (now.QuadPart - _lastPresent) * 1000.0 / (double) _qpcFrequency);

    _lastPresent = now.QuadPart;

    if (_statsStart == 0)
        _statsStart = now.QuadPart;

    const double elapsed = (double) (now.QuadPart - _statsStart) / (double) _qpcFrequency;

    if (elapsed < 5.0 || _presentIntervals.size() < 10)
        return;

    std::vector<double> sorted = _presentIntervals;
    std::sort(sorted.begin(), sorted.end());
    double mean = 0.0;

    for (double v : sorted)
        mean += v;

    mean /= (double) sorted.size();
    const double p99 = sorted[std::min(sorted.size() - 1, (size_t) ((double) sorted.size() * 0.99))];

    LOG_INFO("Synced FG pacing: {:.0f} presents/s, interval {:.2f} ms mean, {:.2f} ms p99, {:.2f} ms max, {} dropped"
             " | batches {}, late {}, no generated frame {}, game held {:.0f} ms",
             (double) _presentIntervals.size() / elapsed, mean, p99, sorted.back(), _dropped, _batches.exchange(0),
             _batchesLate.exchange(0), _batchesBare.exchange(0), _heldMs.exchange(0.0));

    _presentIntervals.clear();
    _dropped = 0;
    _statsStart = now.QuadPart;
}

double SyncedDXGISwapChain::GameFps() const
{
    return _interval > 0.0 ? (double) _qpcFrequency / _interval : 0.0;
}

ID3D12Resource* SyncedDXGISwapChain::CurrentProxy()
{
    return _proxyIndex < _proxies.size() ? _proxies[_proxyIndex] : nullptr;
}

void SyncedDXGISwapChain::ThrottleGpu(UINT framesAgo)
{
    if (_fence == nullptr || _fenceEvent == nullptr || framesAgo == 0 || framesAgo >= kRealFenceRing)
        return;

    UINT64 value = 0;

    {
        std::unique_lock<std::mutex> lock(_mutex);

        if (_handedOver <= framesAgo)
            return;

        const UINT64 index = _handedOver - framesAgo + 1;

        // Its job may still be waiting for its time: wait for the presenter to submit it first.
        _drain.wait_for(lock, std::chrono::milliseconds(500),
                        [&] { return _realFenceValues[index % kRealFenceRing] != 0 || _stop; });
        value = _realFenceValues[index % kRealFenceRing];
    }

    if (value == 0 || _fence->GetCompletedValue() >= value)
        return;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (event == nullptr)
        return;

    _fence->SetEventOnCompletion(value, event);

    if (WaitForSingleObject(event, 2000) != WAIT_OBJECT_0)
        LOG_WARN("Synced FG: the GPU is more than two frames behind and did not catch up within 2 s");

    CloseHandle(event);
}

void SyncedDXGISwapChain::QueueFrames(const QueuedFrame* frames, UINT count)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _pending.assign(frames, frames + count);
}

HRESULT SyncedDXGISwapChain::HandOver(UINT SyncInterval, UINT Flags)
{
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);

    std::unique_lock<std::mutex> lock(_mutex);

    // The game's frame interval, smoothed; a gap of more than 200 ms is a pause, not a frame.
    if (_lastArrival != 0)
    {
        const double dt = (double) (now.QuadPart - _lastArrival);

        if (dt > 0.0 && dt < (double) _qpcFrequency / 5.0)
        {
            if (_interval > 0.0)
                _jitter = _jitter * 0.95 + std::abs(dt - _interval) * 0.05;

            _interval = _interval <= 0.0 ? dt : _interval * 0.9 + dt * 0.1;
        }
    }

    _lastArrival = now.QuadPart;

    if (_pending.empty())
    {
        ID3D12Resource* proxy = CurrentProxy();

        if (proxy != nullptr)
            _pending.push_back({ proxy, D3D12_RESOURCE_STATE_COMMON, false });
    }

    // Everything for this frame is on the game's queue now (its rendering, frame generation's copies and
    // generated frames): the present queue starts on it once this value is reached.
    UINT64 ready = 0;

    if (_readyFence != nullptr)
    {
        ready = ++_readyValue;
        _gameQueue->Signal(_readyFence, ready);
    }

    // The batch is scheduled by the presenter when the GPU has it ready (see PresenterLoop).
    _batches++;

    bool anyGenerated = false;

    for (const auto& f : _pending)
        anyGenerated |= f.generated;

    if (!anyGenerated)
        _batchesBare++;

    for (size_t i = 0; i < _pending.size(); i++)
    {
        Job job {};
        job.frame = _pending[i];
        job.batchIndex = (UINT) i;
        job.batchSize = (UINT) _pending.size();
        job.syncInterval = SyncInterval;
        job.flags = Flags;
        job.readyValue = ready;

        if (!job.frame.generated)
        {
            job.realIndex = ++_realQueued;
            _realFenceValues[job.realIndex % kRealFenceRing] = 0;
            _realFramesQueued++;
        }

        _jobs.push_back(job);
    }

    _pending.clear();
    _handedOver = _realQueued;

    if (!_proxies.empty())
        _proxyIndex = (_proxyIndex + 1) % (UINT) _proxies.size();

    _wake.notify_one();

    // Held back like a real swapchain, but loosely: up to two real frames waiting besides the one being shown.
    // With one, the game was held back two thirds of the time and handed its frames over in the presenter's rhythm
    // instead of its own. Frame generation keeps three output sets, so none is rewritten while still queued.
    LARGE_INTEGER heldFrom {};
    QueryPerformanceCounter(&heldFrom);
    _drain.wait(lock, [this] { return _realFramesQueued < 3 || _stop; });
    lock.unlock();

    LARGE_INTEGER heldTo {};
    QueryPerformanceCounter(&heldTo);
    _heldMs = _heldMs.load() + (double) (heldTo.QuadPart - heldFrom.QuadPart) * 1000.0 / (double) _qpcFrequency;

    // The game renders its next frames into the back buffers the present queue copies from. Before it gets back
    // to the one this frame used -- three frames on with three buffers -- the present queue must be done with it.
    ThrottleGpu(3);
    return S_OK;
}

void SyncedDXGISwapChain::WatchdogLoop()
{
    static const char* stages[] = { "idle", "waiting for its time", "waiting for a slot", "recording",
                                    "executing", "in Present" };

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(_mutex);

            if (_wake.wait_for(lock, std::chrono::milliseconds(200), [this] { return _stop; }))
                return;
        }

        if (_device == nullptr || _device->GetDeviceRemovedReason() == S_OK)
            continue;

        const int stage = std::clamp(_presenterStage.load(), 0, 5);
        LOG_ERROR("Synced FG watchdog: the device is lost; the presenter was {}, {} jobs queued, fence {} of {}",
                  stages[stage], _jobs.size(), _fence != nullptr ? _fence->GetCompletedValue() : 0, _fenceValue);

        Util::GetDeviceRemovedReason(_device);
        return;
    }
}

void SyncedDXGISwapChain::PresenterLoop()
{
    while (true)
    {
        Job job {};

        {
            std::unique_lock<std::mutex> lock(_mutex);
            _wake.wait(lock, [this] { return _stop || !_jobs.empty(); });

            if (_jobs.empty())
                break;

            job = _jobs.front();
            _jobs.pop_front();
            _busy = true;
        }

        if (job.batchIndex == 0)
        {
            // The batch's frames are finished on the GPU before anything of it is scheduled.
            _presenterStage = 1;

            if (_readyFence != nullptr && _readyEvent != nullptr && job.readyValue != 0 &&
                _readyFence->GetCompletedValue() < job.readyValue)
            {
                _readyFence->SetEventOnCompletion(job.readyValue, _readyEvent);
                WaitForSingleObject(_readyEvent, 1000);
            }

            LARGE_INTEGER now {};
            QueryPerformanceCounter(&now);

            // The GPU's own frame interval and irregularity; a gap of more than 200 ms is a pause, not a frame.
            if (_lastReady != 0)
            {
                const double dt = (double) (now.QuadPart - _lastReady);

                if (dt > 0.0 && dt < (double) _qpcFrequency / 5.0)
                {
                    if (_gpuInterval > 0.0)
                        _gpuJitter = _gpuJitter * 0.95 + std::abs(dt - _gpuInterval) * 0.05;

                    _gpuInterval = _gpuInterval <= 0.0 ? dt : _gpuInterval * 0.9 + dt * 0.1;
                }
            }

            _lastReady = now.QuadPart;

            // Spread over one frame, each frame an equal step after the last, the batch starting where the last
            // one's cadence says -- not when this frame happened to be ready, which jitters. A safety margin, three
            // times the irregularity (at most half a frame), lets a frame ready late still start on its slot; never
            // earlier than now, never later than the margin plus one step (no latency creep).
            const double interval = _gpuInterval > 0.0 ? _gpuInterval : _interval;
            const double step = interval / (double) std::max(job.batchSize, 1u);
            const double margin = std::clamp(_gpuJitter * 3.0, 0.0005 * (double) _qpcFrequency,
                                             std::max(interval * 0.5, 0.0005 * (double) _qpcFrequency));
            LONGLONG start = now.QuadPart + (LONGLONG) margin;

            if (_nextBatch != 0)
            {
                if (_nextBatch < now.QuadPart)
                    _batchesLate++;

                start = std::clamp(_nextBatch, now.QuadPart, now.QuadPart + (LONGLONG) (margin + step));
            }

            _batchStart = start;
            _batchStep = step;
            _nextBatch = start + (LONGLONG) (step * (double) job.batchSize);
        }

        job.due = _batchStart + (LONGLONG) (_batchStep * (double) job.batchIndex);

        // Wait for the frame's time, precisely.
        _presenterStage = 1;
        WaitUntil(job.due);

        bool drop = false;

        if (job.frame.generated)
        {
            // Really late only: more than a whole step behind its time. A generated frame a little late is better
            // than a hole in the cadence.
            LARGE_INTEGER now {};
            QueryPerformanceCounter(&now);
            drop = (double) (now.QuadPart - job.due) > _batchStep;
        }

        if (!drop)
            RecordPresent(PresentJob(job));
        else
            RecordPresent(false);

        _presenterStage = 0;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _busy = false;

            // The last value signalled covers this frame's copy (or nothing of it, if it failed): either way all
            // the work submitted before it.
            if (!job.frame.generated)
                _realFenceValues[job.realIndex % kRealFenceRing] = std::max<UINT64>(_fenceValue, 1);

            if (!job.frame.generated && _realFramesQueued > 0)
                _realFramesQueued--;
        }

        _drain.notify_all();
    }
}

bool SyncedDXGISwapChain::PresentJob(const Job& job)
{
    std::lock_guard<std::mutex> lock(_swapchainMutex);

    if (_realBuffers.empty() || job.frame.texture == nullptr || _fence == nullptr)
        return false;

    PresentSlot& slot = _slots[_slot];
    _slot = (_slot + 1) % kSlots;

    if (slot.list == nullptr)
        return false;

    _presenterStage = 2;

    if (slot.fenceValue != 0 && _fence->GetCompletedValue() < slot.fenceValue)
    {
        _fence->SetEventOnCompletion(slot.fenceValue, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 1000);
    }

    _presenterStage = 3;

    if (slot.allocator->Reset() != S_OK || slot.list->Reset(slot.allocator, nullptr) != S_OK)
        return false;

    const UINT index = _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0;
    ID3D12Resource* target = _realBuffers[std::min<size_t>(index, _realBuffers.size() - 1)];

    D3D12_RESOURCE_BARRIER barriers[2] {};
    UINT barrierCount = 0;

    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        if (from == to)
            return;

        D3D12_RESOURCE_BARRIER& b = barriers[barrierCount++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
    };

    transition(job.frame.texture, job.frame.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

    if (barrierCount > 0)
        slot.list->ResourceBarrier(barrierCount, barriers);

    slot.list->CopyResource(target, job.frame.texture);

    barrierCount = 0;
    transition(job.frame.texture, D3D12_RESOURCE_STATE_COPY_SOURCE, job.frame.state);
    transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

    if (barrierCount > 0)
        slot.list->ResourceBarrier(barrierCount, barriers);

    if (slot.list->Close() != S_OK)
        return false;

    _presenterStage = 4;

    // The frame is finished on the game's queue before ours reads it.
    if (_readyFence != nullptr && job.readyValue != 0)
        _queue->Wait(_readyFence, job.readyValue);

    ID3D12CommandList* lists[] = { slot.list };
    _queue->ExecuteCommandLists(1, lists);

    slot.fenceValue = ++_fenceValue;
    _queue->Signal(_fence, slot.fenceValue);

    _presenterStage = 5;
    const HRESULT result = _real->Present(job.syncInterval, job.flags);

    if (FAILED(result))
    {
        LOG_ERROR("Synced FG present failed: {:X}", (UINT) result);

        if (result == DXGI_ERROR_DEVICE_REMOVED && _device != nullptr)
            Util::GetDeviceRemovedReason(_device);

        return false;
    }

    return true;
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    if (riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIDeviceSubObject) || riid == __uuidof(SyncedDXGISwapChain))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain4*) this;
        return S_OK;
    }

    if ((riid == __uuidof(IDXGISwapChain1) && _real1) || (riid == __uuidof(IDXGISwapChain2) && _real2) ||
        (riid == __uuidof(IDXGISwapChain3) && _real3) || (riid == __uuidof(IDXGISwapChain4) && _real4))
    {
        AddRef();
        *ppvObject = (IDXGISwapChain4*) this;
        return S_OK;
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE SyncedDXGISwapChain::AddRef() { return InterlockedIncrement(&_refcount); }

ULONG STDMETHODCALLTYPE SyncedDXGISwapChain::Release()
{
    const ULONG ret = InterlockedDecrement(&_refcount);

    if (ret == 0)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stop = true;
        }

        _wake.notify_all();
        _drain.notify_all();

        if (_thread.joinable())
            _thread.join();

        if (_watchdog.joinable())
            _watchdog.join();

        WaitGpu();
        ReleaseBuffers();

        for (auto& slot : _slots)
        {
            if (slot.list != nullptr)
                slot.list->Release();

            if (slot.allocator != nullptr)
                slot.allocator->Release();
        }

        if (_fence != nullptr)
            _fence->Release();

        if (_readyFence != nullptr)
            _readyFence->Release();

        if (_fenceEvent != nullptr)
            CloseHandle(_fenceEvent);

        if (_timer != nullptr)
            CloseHandle(_timer);

        if (_readyEvent != nullptr)
            CloseHandle(_readyEvent);

        if (_device != nullptr)
            _device->Release();

        // The real swapchain first: it was created on our queue.
        _real->Release();
        _queue->Release();
        _gameQueue->Release();

        delete this;
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetParent(REFIID riid, void** ppParent)
{
    return _real->GetParent(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetDevice(REFIID riid, void** ppDevice)
{
    return _real->GetDevice(riid, ppDevice);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::Present(UINT SyncInterval, UINT Flags)
{
    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    return HandOver(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::Present1(UINT SyncInterval, UINT Flags,
                                                        const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real1->Present1(SyncInterval, Flags, pPresentParameters);

    return HandOver(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (Buffer >= _proxies.size())
        return DXGI_ERROR_INVALID_CALL;

    return _proxies[Buffer]->QueryInterface(riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    {
        std::unique_lock<std::mutex> lock(_mutex);
        WaitIdle(lock);
    }

    std::lock_guard<std::mutex> lock(_swapchainMutex);
    return _real->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return _real->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) { return _real->GetDesc(pDesc); }

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    {
        std::unique_lock<std::mutex> lock(_mutex);
        WaitIdle(lock);
        _pending.clear();
    }

    std::lock_guard<std::mutex> lock(_swapchainMutex);
    WaitGpu();
    ReleaseBuffers();

    const HRESULT result = _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    CreateProxies();
    ReadRefreshRate();

    LOG_DEBUG("Synced FG ResizeBuffers: {:X}", (UINT) result);
    return result;
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                              DXGI_FORMAT Format, UINT SwapChainFlags,
                                                              const UINT* pCreationNodeMask,
                                                              IUnknown* const* ppPresentQueue)
{
    {
        std::unique_lock<std::mutex> lock(_mutex);
        WaitIdle(lock);
        _pending.clear();
    }

    std::lock_guard<std::mutex> lock(_swapchainMutex);
    WaitGpu();
    ReleaseBuffers();

    // Our presenter uses the queue the swapchain was created on, so the per-buffer queues are left alone.
    const HRESULT result = _real3->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
    CreateProxies();
    ReadRefreshRate();

    LOG_DEBUG("Synced FG ResizeBuffers1: {:X}", (UINT) result);
    return result;
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _real->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _real->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _real->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _real->GetLastPresentCount(pLastPresentCount);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) { return _real1->GetDesc1(pDesc); }

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return _real1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetHwnd(HWND* pHwnd) { return _real1->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1->GetCoreWindow(refiid, ppUnk);
}

BOOL STDMETHODCALLTYPE SyncedDXGISwapChain::IsTemporaryMonoSupported(void) { return _real1->IsTemporaryMonoSupported(); }

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1->GetRotation(pRotation);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetSourceSize(UINT Width, UINT Height)
{
    return _real2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency)
{
    return _real2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    return _real2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE SyncedDXGISwapChain::GetFrameLatencyWaitableObject(void)
{
    return _real2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE SyncedDXGISwapChain::GetCurrentBackBufferIndex(void) { return _proxyIndex; }

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                      UINT* pColorSpaceSupport)
{
    return _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    return _real3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE SyncedDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    return _real4->SetHDRMetaData(Type, Size, pMetaData);
}
