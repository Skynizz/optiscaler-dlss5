#include "pch.h"
#include "SyncedSwapchain.h"

#include <Util.h>

SyncedDXGISwapChain::SyncedDXGISwapChain(IDXGISwapChain* real, ID3D12CommandQueue* queue, HWND hwnd)
    : _real(real), _queue(queue), _hwnd(hwnd)
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
    _queue->GetDevice(IID_PPV_ARGS(&_device));

    LARGE_INTEGER frequency {};
    QueryPerformanceFrequency(&frequency);
    _qpcFrequency = std::max<LONGLONG>(frequency.QuadPart, 1);

    if (_device != nullptr)
    {
        _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));
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
    _thread = std::thread(&SyncedDXGISwapChain::PresenterLoop, this);

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
    if (_fence == nullptr || _fenceEvent == nullptr)
        return;

    const UINT64 value = ++_fenceValue;
    _queue->Signal(_fence, value);

    if (_fence->GetCompletedValue() < value)
    {
        _fence->SetEventOnCompletion(value, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 5000);
    }
}

ID3D12Resource* SyncedDXGISwapChain::CurrentProxy()
{
    return _proxyIndex < _proxies.size() ? _proxies[_proxyIndex] : nullptr;
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
            _interval = _interval <= 0.0 ? dt : _interval * 0.9 + dt * 0.1;
    }

    _lastArrival = now.QuadPart;

    if (_pending.empty())
    {
        ID3D12Resource* proxy = CurrentProxy();

        if (proxy != nullptr)
            _pending.push_back({ proxy, D3D12_RESOURCE_STATE_COMMON, false });
    }

    // Spread over one game frame: the first now, each next one an equal step later. The real frame lands one
    // step before the next game frame's first generated one.
    const double step = _pending.empty() ? 0.0 : _interval / (double) _pending.size();

    for (size_t i = 0; i < _pending.size(); i++)
    {
        Job job {};
        job.frame = _pending[i];
        job.due = now.QuadPart + (LONGLONG) (step * (double) i);
        job.syncInterval = SyncInterval;
        job.flags = Flags;
        _jobs.push_back(job);

        if (!job.frame.generated)
            _realFramesQueued++;
    }

    _pending.clear();

    if (!_proxies.empty())
        _proxyIndex = (_proxyIndex + 1) % (UINT) _proxies.size();

    _wake.notify_one();

    // Held back like a real swapchain: never more than one real frame waiting besides the one being shown.
    _drain.wait(lock, [this] { return _realFramesQueued < 2 || _stop; });
    return S_OK;
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

        // Wait for the frame's time: sleep while far, spin the last millisecond and a half.
        while (true)
        {
            LARGE_INTEGER now {};
            QueryPerformanceCounter(&now);
            const double remainingMs = (double) (job.due - now.QuadPart) * 1000.0 / (double) _qpcFrequency;

            if (remainingMs <= 0.0)
                break;

            if (remainingMs > 1.5)
                Sleep(1);
            else
                YieldProcessor();
        }

        bool drop = false;

        if (job.frame.generated)
        {
            // Late: the next frame is already due. A generated frame shown late is worse than none.
            LARGE_INTEGER now {};
            QueryPerformanceCounter(&now);

            std::lock_guard<std::mutex> lock(_mutex);
            drop = !_jobs.empty() && _jobs.front().due <= now.QuadPart;
        }

        if (!drop)
            PresentJob(job);

        {
            std::lock_guard<std::mutex> lock(_mutex);
            _busy = false;

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

    if (slot.fenceValue != 0 && _fence->GetCompletedValue() < slot.fenceValue)
    {
        _fence->SetEventOnCompletion(slot.fenceValue, _fenceEvent);
        WaitForSingleObject(_fenceEvent, 1000);
    }

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

    ID3D12CommandList* lists[] = { slot.list };
    _queue->ExecuteCommandLists(1, lists);

    slot.fenceValue = ++_fenceValue;
    _queue->Signal(_fence, slot.fenceValue);

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

        if (_fenceEvent != nullptr)
            CloseHandle(_fenceEvent);

        if (_device != nullptr)
            _device->Release();

        _queue->Release();
        _real->Release();

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
