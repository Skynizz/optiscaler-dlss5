#include "pch.h"
#include "PresentPacer.h"

#include <Util.h>

#include <detours/detours.h>
#include "FGMeter.h"

#include <mutex>

namespace
{
std::mutex g_mutex;

// The real swapchain's Present and Present1 (IDXGISwapChain vtable 8, IDXGISwapChain1 vtable 22).
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;
bool g_hooked = false;

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* self, UINT syncInterval, UINT flags)
{
    const bool real = (flags & DXGI_PRESENT_TEST) == 0;

    if (real)
        FGMeter::BeforePresent(syncInterval, flags);

    const HRESULT result = o_Present(self, syncInterval, flags);

    if (real)
        FGMeter::AfterPresent(self);

    return result;
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* parameters)
{
    const bool real = (flags & DXGI_PRESENT_TEST) == 0;

    if (real)
        FGMeter::BeforePresent(syncInterval, flags);

    const HRESULT result = o_Present1(self, syncInterval, flags, parameters);

    if (real)
        FGMeter::AfterPresent(self);

    return result;
}

std::string ModuleOf(void* address)
{
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR) address, &module) &&
        GetModuleFileNameA(module, name, MAX_PATH) != 0)
    {
        return name;
    }

    return "unknown";
}
} // namespace

void PresentPacer::Hook(IDXGISwapChain* swapChain)
{
    // Only for the frame generation meter or the auto multiplier; neither on, no hook.
    if (g_hooked || swapChain == nullptr || !FGMeter::WantsPresentHook())
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_hooked)
        return;

    g_hooked = true;

    // Streamline's proxy presents its generated frames on the real swapchain beneath it: that one is hooked.
    IUnknown* real = nullptr;
    IDXGISwapChain* target = swapChain;

    if (Util::CheckForRealObject(__FUNCTION__, swapChain, &real) && real != nullptr)
        target = (IDXGISwapChain*) real;

    void** vtable = *reinterpret_cast<void***>(target);
    o_Present = (PFN_Present) vtable[8];

    IDXGISwapChain1* target1 = nullptr;
    if (target->QueryInterface(IID_PPV_ARGS(&target1)) == S_OK)
    {
        o_Present1 = (PFN_Present1) (*reinterpret_cast<void***>(target1))[22];
        target1->Release();
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_Present, hkPresent);

    if (o_Present1 != nullptr)
        DetourAttach(&(PVOID&) o_Present1, hkPresent1);

    const LONG result = DetourTransactionCommit();

    LOG_INFO("Frame generation meter: hooked the real swapchain's Present ({}), result {}", ModuleOf(vtable[8]),
             result);
}
