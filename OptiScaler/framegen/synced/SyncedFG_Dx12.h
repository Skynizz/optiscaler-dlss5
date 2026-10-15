#pragma once

#include <framegen/IFGFeature_Dx12.h>
#include <shaders/synced_fg/SFG_Dx12.h>

class SyncedDXGISwapChain;

// Synced frame generation: OptiScaler's own frame generation and multi frame generation (1 to 3 generated
// frames per real frame), from the inputs the upscaler is handed -- motion vectors and depth -- and the frame
// the game presents. No vendor runtime: the whole chain, swapchain and pacing included, is ours.
//
// FGOutput=synced. Off (any other output) creates none of this.
class SyncedFG_Dx12 : public virtual IFGFeature_Dx12
{
  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final { return "Synced FG"; };
    feature_version Version() override final { return { 0, 1, 0 }; };
    HWND Hwnd() override final { return _hwnd; };

    // IFGFeature_Dx12
    bool CreateSwapchainInternal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain) override final;
    bool CreateSwapchain1Internal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain) override final;

    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    std::optional<double> ReadGpuTime(void* commandQueue) override final;

    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override { return true; };

    SyncedFG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        _framesToInterpolate = 1;
        _maxInterpolationCount = kMaxGenerated;
    }

    ~SyncedFG_Dx12() {};

  private:
    static constexpr UINT kMaxGenerated = 3;
    static constexpr UINT kHistory = 3;  // a frame still waiting to be presented is never overwritten
    static constexpr UINT kOutputSets = 2;

    SyncedDXGISwapChain* _synced = nullptr;
    std::unique_ptr<SFG_Dx12> _generator;

    // The game's frames as it presented them, and the generated frames, all at rest in COMMON.
    ID3D12Resource* _history[kHistory] {};
    // Per set, the generated frames and, last, the copy of the real frame the present queue reads. The history is
    // never handed over: the game's queue reads it again for the next frame while the present queue would still
    // be copying it, and a texture used by two queues at once (without simultaneous access) is undefined -- on
    // NVIDIA its compression can be torn and the GPU hang.
    ID3D12Resource* _outputs[kOutputSets][kMaxGenerated + 1] {};
    D3D12_RESOURCE_DESC _historyDesc {};
    UINT64 _swapchainGeneration = 0;
    UINT64 _presented = 0;     // real frames handed over since the history was (re)built
    bool _previousValid = false;

    bool EnsureTextures(ID3D12Resource* proxy);
    void ReleaseTextures();
};
