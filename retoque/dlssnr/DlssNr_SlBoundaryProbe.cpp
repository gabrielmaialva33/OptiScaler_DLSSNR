#include "pch.h"

#include "DlssNr_SlBoundaryProbe.h"

#include <Config.h>
#include <State.h>
#include <proxies/KernelBase_Proxy.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>

#include "detours/detours.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace
{
using PFN_CreateDXGIFactory = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_slUpgradeInterface = decltype(&slUpgradeInterface);
using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// Vtable slots: IDXGIFactory::CreateSwapChain, IDXGIFactory2::CreateSwapChainForHwnd, IDXGISwapChain::Present and
// IDXGISwapChain1::Present1.
constexpr size_t kCreateSwapChainSlot = 10;
constexpr size_t kCreateSwapChainForHwndSlot = 15;
constexpr size_t kPresentSlot = 8;
constexpr size_t kPresent1Slot = 22;

std::atomic<bool> g_installed { false };
std::mutex g_hookMutex; // the detour bookkeeping below; never held across a call into the game or the interposer
HMODULE g_module = nullptr;
uintptr_t g_moduleBegin = 0;
uintptr_t g_moduleEnd = 0;

PFN_CreateDXGIFactory o_CreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory o_CreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 o_CreateDXGIFactory2 = nullptr;
PFN_slUpgradeInterface o_slUpgradeInterface = nullptr;
PFN_CreateSwapChain o_CreateSwapChain = nullptr;
PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd = nullptr;
PFN_Present o_Present = nullptr;
PFN_Present1 o_Present1 = nullptr;

// Nesting on this thread inside the game's Present. Only the outermost call is the game's present; a Present1 that
// forwards to Present inside the interposer is the same present.
thread_local int t_depth = 0;

// The window, reset each time a line is written.
std::atomic<uint64_t> g_appPresents { 0 };
std::atomic<uint64_t> g_testPresents { 0 };
std::atomic<uint64_t> g_wrapped { 0 };
std::atomic<uint64_t> g_wrappedNested { 0 };
std::atomic<uint64_t> g_evaluates { 0 };
std::atomic<uint64_t> g_backbufferMatch { 0 };
std::atomic<uint64_t> g_backbufferUnread { 0 };
std::atomic<uint64_t> g_listOnGameQueue { 0 };
std::atomic<uint64_t> g_listOnWrappedQueue { 0 };
std::atomic<uint64_t> g_listOnOtherQueue { 0 };
std::atomic<uint64_t> g_listInsidePresent { 0 };

// Identities, compared and never dereferenced once recorded.
std::atomic<IUnknown*> g_swapchain { nullptr };
std::atomic<unsigned> g_swapchains { 0 };
std::atomic<ID3D12CommandQueue*> g_gameQueue { nullptr };
std::atomic<ID3D12CommandQueue*> g_wrappedQueue { nullptr };
std::atomic<ID3D12Resource*> g_gameBackbuffer { nullptr };
std::atomic<ID3D12CommandList*> g_pendingDlssgList { nullptr };
std::atomic<long long> g_windowStartMs { 0 };

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool InModule(const void* code)
{
    const auto at = reinterpret_cast<uintptr_t>(code);
    return at >= g_moduleBegin && at < g_moduleEnd;
}

// Detours one function the first time it is seen. Code outside sl.interposer.dll is somebody else's, and is refused.
template <typename T> void AttachOnce(T& original, void* target, void* hook, const char* what)
{
    std::lock_guard lock(g_hookMutex);

    if (original != nullptr || target == nullptr)
        return;

    if (!InModule(target))
    {
        LOG_INFO("DLSS-NR boundary probe: {} is not inside sl.interposer.dll; not watched", what);
        return;
    }

    original = reinterpret_cast<T>(target);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) original, hook);

    if (const LONG result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_WARN("DLSS-NR boundary probe: could not detour {} ({})", what, result);
        original = nullptr;
        return;
    }

    LOG_INFO("DLSS-NR boundary probe: watching {}", what);
}

void* Slot(IUnknown* object, size_t slot) { return (*reinterpret_cast<void***>(object))[slot]; }

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags);
HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* parameters);

void WatchSwapchain(IUnknown* swapchain, IUnknown* device)
{
    if (swapchain == nullptr)
        return;

    if (g_swapchain.exchange(swapchain) != swapchain)
        ++g_swapchains;

    ID3D12CommandQueue* queue = nullptr;
    if (device != nullptr && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))) && queue != nullptr)
    {
        g_gameQueue = queue;
        LOG_INFO("DLSS-NR boundary probe: the game's Streamline swapchain {:p}, created on queue {:p} (type {})",
                 (void*) swapchain, (void*) queue, (int) queue->GetDesc().Type);
        queue->Release();
    }
    else
    {
        LOG_INFO("DLSS-NR boundary probe: the game's Streamline swapchain {:p}, creation queue not known",
                 (void*) swapchain);
    }

    IDXGISwapChain* sc = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc))) && sc != nullptr)
    {
        AttachOnce(o_Present, Slot(sc, kPresentSlot), (void*) hkPresent, "the interposer swapchain's Present");
        sc->Release();
    }

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1 != nullptr)
    {
        AttachOnce(o_Present1, Slot(sc1, kPresent1Slot), (void*) hkPresent1, "the interposer swapchain's Present1");
        sc1->Release();
    }
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                            IDXGISwapChain** swapchain)
{
    const HRESULT result = o_CreateSwapChain(factory, device, desc, swapchain);

    if (SUCCEEDED(result) && swapchain != nullptr && g_installed.load())
        WatchSwapchain(*swapchain, device);

    return result;
}

HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                   IDXGIOutput* output, IDXGISwapChain1** swapchain)
{
    const HRESULT result = o_CreateSwapChainForHwnd(factory, device, window, desc, fullscreen, output, swapchain);

    if (SUCCEEDED(result) && swapchain != nullptr && g_installed.load())
        WatchSwapchain(*swapchain, device);

    return result;
}

void WatchFactory(IUnknown* object)
{
    if (object == nullptr)
        return;

    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&factory))) && factory != nullptr)
    {
        AttachOnce(o_CreateSwapChain, Slot(factory, kCreateSwapChainSlot), (void*) hkCreateSwapChain,
                   "the interposer factory's CreateSwapChain");
        factory->Release();
    }

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2 != nullptr)
    {
        AttachOnce(o_CreateSwapChainForHwnd, Slot(factory2, kCreateSwapChainForHwndSlot),
                   (void*) hkCreateSwapChainForHwnd, "the interposer factory's CreateSwapChainForHwnd");
        factory2->Release();
    }
}

HRESULT WINAPI hkCreateDXGIFactory(REFIID riid, void** factory)
{
    const HRESULT result = o_CreateDXGIFactory(riid, factory);

    if (SUCCEEDED(result) && factory != nullptr && g_installed.load())
        WatchFactory(static_cast<IUnknown*>(*factory));

    return result;
}

HRESULT WINAPI hkCreateDXGIFactory1(REFIID riid, void** factory)
{
    const HRESULT result = o_CreateDXGIFactory1(riid, factory);

    if (SUCCEEDED(result) && factory != nullptr && g_installed.load())
        WatchFactory(static_cast<IUnknown*>(*factory));

    return result;
}

HRESULT WINAPI hkCreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    const HRESULT result = o_CreateDXGIFactory2(flags, riid, factory);

    if (SUCCEEDED(result) && factory != nullptr && g_installed.load())
        WatchFactory(static_cast<IUnknown*>(*factory));

    return result;
}

// The game hands slUpgradeInterface a native factory (or swapchain) and gets the interposer's proxy back.
sl::Result hkslUpgradeInterface(void** baseInterface)
{
    const sl::Result result = o_slUpgradeInterface(baseInterface);

    if (result == sl::Result::eOk && baseInterface != nullptr && *baseInterface != nullptr && g_installed.load())
    {
        auto* object = static_cast<IUnknown*>(*baseInterface);
        WatchFactory(object);

        IDXGISwapChain* swapchain = nullptr;
        if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&swapchain))) && swapchain != nullptr)
        {
            WatchSwapchain(swapchain, nullptr);
            swapchain->Release();
        }
    }

    return result;
}

void Report()
{
    const long long now = NowMs();
    long long start = g_windowStartMs.load();

    if (start == 0)
    {
        g_windowStartMs.compare_exchange_strong(start, now);
        return;
    }

    if (now - start < 2000 || !g_windowStartMs.compare_exchange_strong(start, now))
        return;

    const uint64_t app = g_appPresents.exchange(0);
    const uint64_t test = g_testPresents.exchange(0);
    const uint64_t wrapped = g_wrapped.exchange(0);
    const uint64_t nested = g_wrappedNested.exchange(0);
    const uint64_t evaluates = g_evaluates.exchange(0);
    const uint64_t match = g_backbufferMatch.exchange(0);
    const uint64_t unread = g_backbufferUnread.exchange(0);
    const uint64_t onGame = g_listOnGameQueue.exchange(0);
    const uint64_t onWrapped = g_listOnWrappedQueue.exchange(0);
    const uint64_t onOther = g_listOnOtherQueue.exchange(0);
    const uint64_t inside = g_listInsidePresent.exchange(0);

    const auto* gameQueue = g_gameQueue.load();
    const auto* wrappedQueue = g_wrappedQueue.load();
    const char* queues = gameQueue == nullptr || wrappedQueue == nullptr ? "not both known"
                         : gameQueue == wrappedQueue                     ? "the same"
                                                                         : "different";

    LOG_INFO("DLSS-NR boundary probe: {} game presents ({} test), {} at the wrapped entry ({:.2f} each; {} nested in "
             "the game's Present on its thread, {} on other threads) | DLSS-G evaluates {}: list executed on the "
             "game's swapchain queue {}, the wrapped entry's queue {}, another queue {}, {} of them inside the game's "
             "Present | DLSSG.Backbuffer is the game's current buffer {} of {} ({} not readable) | game and wrapped "
             "queues: {} | swapchains seen: {}",
             app, test, wrapped, app != 0 ? (double) wrapped / (double) app : 0.0, nested, wrapped - nested, evaluates,
             onGame, onWrapped, onOther, inside, match, evaluates, unread, queues, g_swapchains.load());
}

// The game's present: counted, its current buffer noted, then forwarded untouched.
void Enter(IUnknown* swapchain, UINT flags)
{
    if (t_depth++ != 0)
        return;

    if (flags & DXGI_PRESENT_TEST)
    {
        ++g_testPresents;
        return;
    }

    ++g_appPresents;

    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3 != nullptr)
    {
        ID3D12Resource* buffer = nullptr;
        if (SUCCEEDED(sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer))) && buffer != nullptr)
        {
            g_gameBackbuffer = buffer;
            buffer->Release();
        }

        sc3->Release();
    }
}

void Leave()
{
    if (--t_depth == 0)
        Report();
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags)
{
    if (!g_installed.load())
        return o_Present(swapchain, syncInterval, flags);

    Enter(swapchain, flags);
    const HRESULT result = o_Present(swapchain, syncInterval, flags);
    Leave();
    return result;
}

HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* swapchain, UINT syncInterval, UINT flags,
                                     const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (!g_installed.load())
        return o_Present1(swapchain, syncInterval, flags, parameters);

    Enter(swapchain, flags);
    const HRESULT result = o_Present1(swapchain, syncInterval, flags, parameters);
    Leave();
    return result;
}

template <typename T> void AttachExport(T& original, const char* name, void* hook)
{
    original = reinterpret_cast<T>(KernelBaseProxy::GetProcAddress_()(g_module, name));

    if (original != nullptr)
        DetourAttach(&(PVOID&) original, hook);
}

template <typename T> void Detach(T& original, void* hook)
{
    if (original != nullptr)
        DetourDetach(&(PVOID&) original, hook);

    original = nullptr;
}
} // namespace

void DlssNr::SlBoundaryProbe::Install(HMODULE interposer)
{
    const Config& cfg = *Config::Instance();

    if (interposer == nullptr || !cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrHookMethod.value_or_default() != 2)
        return;

    if (State::Instance().streamlineVersion.major < 2)
        return;

    std::lock_guard lock(g_hookMutex);

    if (g_installed.load() && g_module == interposer)
        return;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(interposer);
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(interposer) + dos->e_lfanew);

    g_module = interposer;
    g_moduleBegin = reinterpret_cast<uintptr_t>(interposer);
    g_moduleEnd = g_moduleBegin + nt->OptionalHeader.SizeOfImage;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    AttachExport(o_CreateDXGIFactory, "CreateDXGIFactory", (void*) hkCreateDXGIFactory);
    AttachExport(o_CreateDXGIFactory1, "CreateDXGIFactory1", (void*) hkCreateDXGIFactory1);
    AttachExport(o_CreateDXGIFactory2, "CreateDXGIFactory2", (void*) hkCreateDXGIFactory2);
    AttachExport(o_slUpgradeInterface, "slUpgradeInterface", (void*) hkslUpgradeInterface);

    if (const LONG result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_WARN("DLSS-NR boundary probe: could not detour sl.interposer.dll's factory exports ({}); not watching",
                 result);
        o_CreateDXGIFactory = nullptr;
        o_CreateDXGIFactory1 = nullptr;
        o_CreateDXGIFactory2 = nullptr;
        o_slUpgradeInterface = nullptr;
        return;
    }

    g_installed = true;
    LOG_INFO("DLSS-NR boundary probe (nr-before-dlssg.md, step 0): watching the swapchain the game creates through "
             "sl.interposer.dll. It forwards every call and only counts");
}

void DlssNr::SlBoundaryProbe::Uninstall()
{
    std::lock_guard lock(g_hookMutex);

    if (!g_installed.exchange(false))
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    Detach(o_CreateDXGIFactory, (void*) hkCreateDXGIFactory);
    Detach(o_CreateDXGIFactory1, (void*) hkCreateDXGIFactory1);
    Detach(o_CreateDXGIFactory2, (void*) hkCreateDXGIFactory2);
    Detach(o_slUpgradeInterface, (void*) hkslUpgradeInterface);
    Detach(o_CreateSwapChain, (void*) hkCreateSwapChain);
    Detach(o_CreateSwapChainForHwnd, (void*) hkCreateSwapChainForHwnd);
    Detach(o_Present, (void*) hkPresent);
    Detach(o_Present1, (void*) hkPresent1);
    DetourTransactionCommit();

    g_module = nullptr;
    g_moduleBegin = g_moduleEnd = 0;
}

void DlssNr::SlBoundaryProbe::NoteWrappedPresent(ID3D12CommandQueue* queue, bool fgHook)
{
    if (!g_installed.load() || fgHook)
        return;

    ++g_wrapped;
    g_wrappedQueue = queue;

    if (t_depth > 0)
        ++g_wrappedNested;
}

void DlssNr::SlBoundaryProbe::NoteDlssgEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params)
{
    if (!g_installed.load())
        return;

    ++g_evaluates;
    g_pendingDlssgList = list;

    ID3D12Resource* backbuffer = nullptr;
    if (params == nullptr || params->Get("DLSSG.Backbuffer", &backbuffer) != NVSDK_NGX_Result_Success ||
        backbuffer == nullptr)
    {
        // The untyped setter, as GetResource in DlssNr_Dx12.cpp also allows for.
        void* untyped = nullptr;
        if (params != nullptr && params->Get("DLSSG.Backbuffer", &untyped) == NVSDK_NGX_Result_Success)
            backbuffer = static_cast<ID3D12Resource*>(untyped);
    }

    if (backbuffer == nullptr)
        ++g_backbufferUnread;
    else if (backbuffer == g_gameBackbuffer.load())
        ++g_backbufferMatch;
}

void DlssNr::SlBoundaryProbe::NoteExecute(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists)
{
    if (!g_installed.load() || lists == nullptr)
        return;

    ID3D12CommandList* pending = g_pendingDlssgList.load();
    if (pending == nullptr)
        return;

    for (unsigned i = 0; i < count; ++i)
    {
        if (lists[i] != pending || !g_pendingDlssgList.compare_exchange_strong(pending, nullptr))
            continue;

        if (queue == g_gameQueue.load())
            ++g_listOnGameQueue;
        else if (queue == g_wrappedQueue.load())
            ++g_listOnWrappedQueue;
        else
            ++g_listOnOtherQueue;

        if (t_depth > 0)
            ++g_listInsidePresent;

        return;
    }
}
