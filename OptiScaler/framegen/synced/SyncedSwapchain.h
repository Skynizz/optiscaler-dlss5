#pragma once

#include "SysUtils.h"

#include <dxgi1_6.h>
#include <d3d12.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// The swapchain of Synced frame generation.
//
// The game renders into back buffers of ours (same size, format and flags as the real ones) and never touches
// the real swapchain. Its Present hands the frame over; a thread of ours presents, at their own times, the
// frames generated between the last real frame and this one and then this one, each copied into the real
// back buffer just before it is presented. Generated frames that would arrive late are dropped -- the real
// frame never is -- and the game is held back when two of its frames are still waiting, as a real swapchain
// would hold it.
//
// With nothing queued by frame generation (off, paused, no inputs yet) a Present presents the game's own frame
// as it was, through the same thread.
class DECLSPEC_UUID("6b3c7c0e-5c1e-4b0b-9a8e-2f1d5a7e93c4") SyncedDXGISwapChain final : public IDXGISwapChain4
{
  public:
    struct QueuedFrame
    {
        ID3D12Resource* texture = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool generated = false;
    };

    // `real` was created on `presentQueue`, a queue of frame generation's own; `gameQueue` is the one the game
    // created its swapchain with, and renders and generates on.
    SyncedDXGISwapChain(IDXGISwapChain* real, ID3D12CommandQueue* gameQueue, ID3D12CommandQueue* presentQueue,
                        HWND hwnd);
    virtual ~SyncedDXGISwapChain();

    // For frame generation, on the game's thread, between the game finishing a frame and its Present.
    ID3D12Resource* CurrentProxy();
    // Bumped whenever the back buffers are rebuilt: anything kept from before is stale.
    UINT64 Generation() const { return _generation; }
    // The frames to present for this Present, in order (generated ones first, the real one last).
    void QueueFrames(const QueuedFrame* frames, UINT count);

    // The display's refresh rate (Hz, 0 if unknown) and the game's frame rate (smoothed, 0 until known).
    double RefreshHz() const { return _refreshHz; }
    double GameFps() const;

    // Waits until the GPU has finished presenting the real frame handed over `framesAgo` Presents ago (and so all
    // the work submitted before it). Frame generation calls it before reusing anything of its own: a game on a
    // waitable swapchain never blocks in Present, and nothing else would stop it running ahead of the GPU until
    // command lists and textures still in use were rewritten -- a hung GPU.
    void ThrottleGpu(UINT framesAgo);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void) override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void) override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void) override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

  private:
    struct Job
    {
        QueuedFrame frame;
        UINT64 realIndex = 0; // for a real frame: its hand-over number
        UINT64 readyValue = 0; // the game's queue has finished this frame at this value of _readyFence
        UINT batchIndex = 0;   // position in its batch (0 = first, scheduled when the GPU has the frame ready)
        UINT batchSize = 1;
        LONGLONG due = 0; // QPC ticks
        UINT syncInterval = 0;
        UINT flags = 0;
    };

    struct PresentSlot
    {
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        UINT64 fenceValue = 0;
    };

    static constexpr UINT kSlots = 8;

    IDXGISwapChain* _real = nullptr;
    IDXGISwapChain1* _real1 = nullptr;
    IDXGISwapChain2* _real2 = nullptr;
    IDXGISwapChain3* _real3 = nullptr;
    IDXGISwapChain4* _real4 = nullptr;
    // The present queue: every copy into the real back buffers and every present happens there, so the waits DXGI
    // puts on a present queue (for a back buffer the display still shows) never land in the game's queue, between
    // its own work and its own cross-queue fences. Presenting on the game's queue hung the GPU at 4x: lists queued
    // behind those waits, never started (DRED).
    ID3D12CommandQueue* _queue = nullptr;
    ID3D12CommandQueue* _gameQueue = nullptr;
    // Signalled on the game's queue at each hand-over, once the frame and its generated frames are recorded; the
    // present queue waits for it before copying them.
    ID3D12Fence* _readyFence = nullptr;
    UINT64 _readyValue = 0;
    ID3D12Device* _device = nullptr;
    HWND _hwnd = nullptr;
    LONG _refcount = 1;

    // The game's back buffers, and the real ones the presenter copies into.
    std::vector<ID3D12Resource*> _proxies;
    std::vector<ID3D12Resource*> _realBuffers;
    UINT _proxyIndex = 0;
    UINT64 _generation = 0;

    // The watchdog: checks the device every 200 ms and logs why it was lost (DRED, when on) at once -- the game's
    // own crash handler kills the process before anything else asks. Also says what the presenter was doing.
    std::thread _watchdog;
    std::atomic<int> _presenterStage { 0 }; // 0 idle, 1 waiting for its time, 2 waiting for a slot, 3 recording,
                                            // 4 executing, 5 in Present
    void WatchdogLoop();

    // The presenter.
    std::thread _thread;
    std::mutex _mutex;              // the job queue and everything below
    std::condition_variable _wake;  // jobs arrived, or stop
    std::condition_variable _drain; // a real frame left the queue, or the queue emptied
    std::deque<Job> _jobs;
    std::vector<QueuedFrame> _pending; // queued by frame generation for the next Present
    UINT _realFramesQueued = 0;
    bool _busy = false;
    bool _stop = false;

    // The fence value signalled after each real frame's copy, by hand-over order (written by the presenter).
    static constexpr UINT kRealFenceRing = 16;
    UINT64 _realFenceValues[kRealFenceRing] {};
    UINT64 _handedOver = 0; // real frames handed over (game thread)
    UINT64 _realQueued = 0; // real frames whose job was queued, numbering the jobs

    std::mutex _swapchainMutex; // the real swapchain: a present never overlaps a resize
    PresentSlot _slots[kSlots] {};
    UINT _slot = 0;
    ID3D12Fence* _fence = nullptr;
    HANDLE _fenceEvent = nullptr;
    UINT64 _fenceValue = 0;

    double _refreshHz = 0.0;
    void ReadRefreshRate();

    // A high-resolution waitable timer: Sleep(1) can last 15 ms without a raised timer resolution, which at 4x
    // (a frame every 4 ms) put the generated frames anywhere.
    HANDLE _timer = nullptr;
    void WaitUntil(LONGLONG due);

    // The cadence: when the next batch's first frame is due, carried from batch to batch so the game's own jitter
    // in handing frames over does not reach the screen.
    LONGLONG _nextBatch = 0;

    // How irregularly the game hands its frames over (mean deviation from its interval, QPC ticks), and the safety
    // margin kept from it: every batch is shown that much later than it could be, so a frame handed over up to
    // that late still starts on its slot instead of leaving a hole -- the stutter of a late frame.
    double _jitter = 0.0;

    // The cadence the presenter keeps, measured where it matters: when the GPU has finished each frame (and its
    // generated frames). GPU-bound -- ray tracing at 30 fps -- the GPU finishes a frame most of a frame after the game
    // hands it over, so a schedule taken from the hand-over had the generated frames ready all at once, bunched or
    // late and dropped: a quarter of them at 30 fps in The Witcher 3.
    HANDLE _readyEvent = nullptr;
    LONGLONG _lastReady = 0;
    double _gpuInterval = 0.0;
    double _gpuJitter = 0.0;
    LONGLONG _batchStart = 0;
    double _batchStep = 0.0;

    // Measured output pacing, logged every few seconds: intervals between presents, and frames dropped.
    LONGLONG _lastPresent = 0;
    LONGLONG _statsStart = 0;
    std::vector<double> _presentIntervals;
    UINT _dropped = 0;
    // Batches since the last line: in all, started late (the frame came after its slot), with no generated
    // frame, and the time the game's thread spent held back in Present (ms).
    std::atomic<UINT> _batches { 0 };
    std::atomic<UINT> _batchesLate { 0 };
    std::atomic<UINT> _batchesBare { 0 };
    std::atomic<double> _heldMs { 0.0 };
    void RecordPresent(bool presented);

    // Pacing: the game's frame interval, smoothed, in QPC ticks.
    LONGLONG _qpcFrequency = 1;
    LONGLONG _lastArrival = 0;
    double _interval = 0.0;

    bool CreateProxies();
    void ReleaseBuffers();
    void WaitIdle(std::unique_lock<std::mutex>& lock);
    void WaitGpu();
    HRESULT HandOver(UINT SyncInterval, UINT Flags);
    void PresenterLoop();
    bool PresentJob(const Job& job);
};
