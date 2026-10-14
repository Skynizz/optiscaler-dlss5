#include "pch.h"
#include "SyncedFG_Dx12.h"
#include "SyncedSwapchain.h"

#include <hudfix/Hudfix_Dx12.h>
#include <hudfix/Hudfix_Dx11.h>
#include <menu/menu_overlay_dx.h>

#include <magic_enum.hpp>

void SyncedFG_Dx12::ReleaseObjects()
{
    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);

        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;
        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _generator.reset();
    ReleaseTextures();
}

void SyncedFG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;
        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;

        InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
        InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                    IID_PPV_ARGS(&_uiCommandList[i]));
        _uiCommandList[i]->SetName(std::format(L"Synced FG _uiCommandList[{}]", i).c_str());
        _uiCommandList[i]->Close();

        if (_uiFence == nullptr)
        {
            InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
            _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
        InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                    IID_PPV_ARGS(&_scCommandList[i]));
        _scCommandList[i]->SetName(std::format(L"Synced FG _scCommandList[{}]", i).c_str());
        _scCommandList[i]->Close();

        if (_scFence == nullptr)
        {
            InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
            _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }
    }
}

bool SyncedFG_Dx12::CreateSwapchainInternal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue,
                                            DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
{
    IDXGISwapChain* realSwapchain = nullptr;

    if (factory->CreateSwapChain(cmdQueue, desc, &realSwapchain) != S_OK || realSwapchain == nullptr ||
        swapChain == nullptr)
        return false;

    _synced = new SyncedDXGISwapChain(realSwapchain, cmdQueue, desc->OutputWindow);
    *swapChain = _synced;

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = desc->OutputWindow;
    return true;
}

bool SyncedFG_Dx12::CreateSwapchain1Internal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                             DXGI_SWAP_CHAIN_DESC1* desc,
                                             DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                             IDXGISwapChain1** swapChain)
{
    IDXGIFactory2* factory2 = nullptr;

    if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
        return false;

    IDXGISwapChain1* realSwapchain1 = nullptr;
    auto result = factory2->CreateSwapChainForHwnd(cmdQueue, hwnd, desc, pFullscreenDesc, nullptr, &realSwapchain1);
    factory2->Release();

    if (result != S_OK || realSwapchain1 == nullptr || swapChain == nullptr)
        return false;

    _synced = new SyncedDXGISwapChain(realSwapchain1, cmdQueue, hwnd);
    *swapChain = _synced;

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;
    return true;
}

bool SyncedFG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    MenuOverlayDx::CleanupRenderTarget(true, NULL);
    ReleaseObjects();
    _synced = nullptr;
    return true;
}

void SyncedFG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    if (_device != nullptr)
        return;

    _device = device;
    CreateObjects(device);
}

void SyncedFG_Dx12::Activate()
{
    if (!_isActive)
    {
        UpdateTarget();
        _isActive = true;
    }
}

void SyncedFG_Dx12::Deactivate()
{
    if (_isActive)
    {
        _isActive = false;
        _previousValid = false;
    }
}

void SyncedFG_Dx12::DestroyFGContext()
{
    Deactivate();
    ReleaseObjects();
}

bool SyncedFG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);
    DestroyFGContext();
    return true;
}

void SyncedFG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    OwnedLockGuard lock(Mutex, 555);

    if (State::Instance().isShuttingDown)
        return;

    _constants = fgConstants;

    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (_device == nullptr)
            CreateContext(device, fgConstants);

        if (!IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        Deactivate();
    }

    if (State::Instance().fgChanged)
    {
        State::Instance().fgChanged = false;

        Hudfix_Dx12::ResetCounters();
        Hudfix_Dx11::ResetCounters();

        UpdateTarget();

        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }
}

std::optional<double> SyncedFG_Dx12::ReadGpuTime(void* commandQueue)
{
    if (_generator && _generator->IsInit() && commandQueue)
        return _generator->ReadGpuTime((ID3D12CommandQueue*) commandQueue);

    return std::nullopt;
}

void SyncedFG_Dx12::ReleaseTextures()
{
    for (auto& texture : _history)
        SAFE_RELEASE(texture);

    for (auto& set : _outputs)
        for (auto& texture : set)
            SAFE_RELEASE(texture);

    _historyDesc = {};
    _previousValid = false;
}

bool SyncedFG_Dx12::EnsureTextures(ID3D12Resource* proxy)
{
    const D3D12_RESOURCE_DESC desc = proxy->GetDesc();

    if (_history[0] != nullptr && desc.Width == _historyDesc.Width && desc.Height == _historyDesc.Height &&
        desc.Format == _historyDesc.Format)
        return true;

    // Only after the swapchain rebuilt its buffers, which waits for the GPU and the presenter first.
    ReleaseTextures();

    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { desc.Format };

    if (_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)) != S_OK ||
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
    {
        LOG_ERROR("Synced FG: the back buffer format {} cannot be written by a compute shader",
                  magic_enum::enum_name(desc.Format));
        return false;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = desc;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (UINT i = 0; i < kHistory; i++)
    {
        if (_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&_history[i])) != S_OK)
        {
            ReleaseTextures();
            return false;
        }

        _history[i]->SetName(std::format(L"Synced FG frame {}", i).c_str());
    }

    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (UINT s = 0; s < kOutputSets; s++)
    {
        for (UINT k = 0; k < kMaxGenerated; k++)
        {
            if (_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COMMON,
                                                 nullptr, IID_PPV_ARGS(&_outputs[s][k])) != S_OK)
            {
                ReleaseTextures();
                return false;
            }

            _outputs[s][k]->SetName(std::format(L"Synced FG generated {}-{}", s, k).c_str());
        }
    }

    _historyDesc = desc;
    LOG_INFO("Synced FG: frames {}x{} {}", desc.Width, desc.Height, magic_enum::enum_name(desc.Format));
    return true;
}

bool SyncedFG_Dx12::Present()
{
    if (_synced == nullptr || _device == nullptr || _gameCommandQueue == nullptr)
        return false;

    const auto fIndex = GetIndex();
    ID3D12Resource* proxy = _synced->CurrentProxy();

    if (proxy == nullptr || !EnsureTextures(proxy))
        return false;

    // A rebuilt swapchain, or a frame the upscaler did not run on: the last frame is not this one's neighbour.
    static UINT64 lastFrameCount = 0;

    if (_synced->Generation() != _swapchainGeneration || _frameCount != lastFrameCount + 1)
        _previousValid = false;

    _swapchainGeneration = _synced->Generation();
    lastFrameCount = _frameCount;

    auto cmd = GetSCCommandList(fIndex);

    if (cmd == nullptr)
        return false;

    const UINT cur = (UINT) (_presented % kHistory);
    const UINT prev = (UINT) ((_presented + kHistory - 1) % kHistory);
    const UINT set = (UINT) (_presented % kOutputSets);

    // The game's frame, kept: it is this frame's real frame and the next one's "last frame".
    ResourceBarrier(cmd, proxy, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceBarrier(cmd, _history[cur], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(_history[cur], proxy);
    ResourceBarrier(cmd, proxy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    ResourceBarrier(cmd, _history[cur], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);

    const UINT wanted = std::clamp(Config::Instance()->FGSyncedFrames.value_or_default(), 1u, kMaxGenerated);
    UINT generated = 0;

    if (_previousValid && IsActive() && !IsPaused())
    {
        auto velocity = GetResource(FG_ResourceType::Velocity, fIndex);
        auto depth = GetResource(FG_ResourceType::Depth, fIndex);

        if (velocity && depth && velocity->validity != FG_ResourceValidity::ValidNow &&
            depth->validity != FG_ResourceValidity::ValidNow)
        {
            if (_generator == nullptr)
                _generator = std::make_unique<SFG_Dx12>("Synced FG", _device);

            if (_generator->IsInit())
            {
                constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                ID3D12Resource* mv = velocity->GetResource();
                ID3D12Resource* dp = depth->GetResource();

                ResourceBarrier(cmd, _history[prev], D3D12_RESOURCE_STATE_COMMON, read);
                ResourceBarrier(cmd, _history[cur], D3D12_RESOURCE_STATE_COMMON, read);
                ResourceBarrier(cmd, mv, velocity->state, read);
                ResourceBarrier(cmd, dp, depth->state, read);

                SFGConstants c {};
                c.outWidth = (UINT) _historyDesc.Width;
                c.outHeight = _historyDesc.Height;
                c.invertedDepth = _constants.flags[FG_Flags::InvertedDepth] ? 1u : 0u;
                c.mvScaleX = _mvScaleX[fIndex];
                c.mvScaleY = _mvScaleY[fIndex];
                c.mvWidth = (float) std::max<UINT64>(velocity->width, 1);
                c.mvHeight = (float) std::max<UINT>(velocity->height, 1);
                c.depthWidth = (float) std::max<UINT64>(depth->width, 1);
                c.depthHeight = (float) std::max<UINT>(depth->height, 1);
                c.staticThreshold = 0.004f;
                c.debugView = Config::Instance()->FGSyncedDebug.value_or_default() ? 1u : 0u;

                for (UINT k = 0; k < wanted; k++)
                {
                    ID3D12Resource* out = _outputs[set][k];
                    c.t = (float) (k + 1) / (float) (wanted + 1);

                    ResourceBarrier(cmd, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    const bool ok = _generator->Dispatch(cmd, _history[prev], _history[cur], mv, dp, out, c);
                    ResourceBarrier(cmd, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

                    if (!ok)
                        break;

                    generated++;
                }

                ResourceBarrier(cmd, _history[prev], read, D3D12_RESOURCE_STATE_COMMON);
                ResourceBarrier(cmd, _history[cur], read, D3D12_RESOURCE_STATE_COMMON);
                ResourceBarrier(cmd, mv, read, velocity->state);
                ResourceBarrier(cmd, dp, read, depth->state);
            }
        }
    }

    // Recorded work first, on the game's queue, so the presenter's copies come after it.
    if (_scCommandListResetted[fIndex])
    {
        if (_scCommandList[fIndex]->Close() == S_OK)
            _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);

        _scCommandListResetted[fIndex] = false;
    }

    SyncedDXGISwapChain::QueuedFrame frames[kMaxGenerated + 1] {};
    UINT count = 0;

    for (UINT k = 0; k < generated; k++)
        frames[count++] = { _outputs[set][k], D3D12_RESOURCE_STATE_COMMON, true };

    frames[count++] = { _history[cur], D3D12_RESOURCE_STATE_COMMON, false };
    _synced->QueueFrames(frames, count);

    _presented++;
    _previousValid = true;

    // The swapchain's own Present hands these to its presenter.
    return false;
}

bool SyncedFG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr)
        return false;

    auto fIndex = inputResource->frameIndex < 0 ? GetIndex() : inputResource->frameIndex;
    auto& type = inputResource->type;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (type == FG_ResourceType::HudlessColor && Config::Instance()->FGDisableHudless.value_or_default())
        return false;

    // Read at present, long after the upscaler that handed them over: anything only valid now is copied.
    if (IsActive() && inputResource->validity == FG_ResourceValidity::ValidNow)
        inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->cmdList = inputResource->cmdList;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->copy = nullptr;

    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;
        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    SetResourceReady(type, fIndex);
    return true;
}

// The swapchain's queue is kept: the presenter's copies must come after the generation work on the same queue.
void SyncedFG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) {}

void* SyncedFG_Dx12::FrameGenerationContext() { return nullptr; }

void* SyncedFG_Dx12::SwapchainContext() { return _synced; }
