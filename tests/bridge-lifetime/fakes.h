#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>
using HRESULT = int32_t;
using UINT = unsigned;
using UINT64 = uint64_t;
using DWORD = uint32_t;
using ULONG = unsigned;
using LONG = int;
using HWND = void*;
using HANDLE = void*;
using DXGI_FORMAT = int;
constexpr HRESULT S_OK = 0, E_FAIL = -1, E_UNEXPECTED = -2, E_INVALIDARG = -3;
constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = -4, DXGI_ERROR_DEVICE_HUNG = -5;
constexpr DWORD ERROR_TIMEOUT = 1460, WAIT_TIMEOUT = 258, WAIT_OBJECT_0 = 0, WAIT_FAILED = UINT32_MAX;
constexpr UINT DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048, D3D12_FENCE_FLAG_NONE = 0;
constexpr int D3D12_RESOURCE_STATE_COMMON = 0, D3D12_RESOURCE_STATE_PRESENT = 1;
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE = 2, D3D12_RESOURCE_STATE_COPY_DEST = 3;
#define STDMETHODCALLTYPE
#define IID_PPV_ARGS(p) p
#define FAILED(r) ((r) < 0)
#define SUCCEEDED(r) ((r) >= 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
HRESULT HRESULT_FROM_WIN32(DWORD error) { return error ? -static_cast<HRESULT>(error) : S_OK; }
ULONG InterlockedDecrement(LONG* value) { return --*value; }
uint64_t clockMs = 0;
DWORD lastError = 77;
uint64_t GetTickCount64() { return clockMs; }
DWORD GetLastError() { return lastError; }
std::deque<std::function<DWORD(DWORD)>> wakes;
std::vector<DWORD> waitDurations;
DWORD WaitForSingleObject(HANDLE, DWORD timeout)
{
    waitDurations.push_back(timeout);
    if (wakes.empty())
    {
        clockMs += timeout;
        return WAIT_TIMEOUT;
    }
    auto wake = wakes.front();
    wakes.pop_front();
    return wake(timeout);
}
int closeCount = 0;
void CloseHandle(HANDLE) { ++closeCount; }
struct Ref
{
    int refs = 1, releases = 0;
    void AddRef() { ++refs; }
    void Release()
    {
        assert(refs > 0);
        --refs;
        ++releases;
    }
};
using IUnknown = Ref;
template <class T> void SafeRelease(T*& value)
{
    if (value)
    {
        value->Release();
        value = nullptr;
    }
}
void SafeCloseHandle(HANDLE& value)
{
    if (value)
    {
        CloseHandle(value);
        value = nullptr;
    }
}
struct ID3D12Fence : Ref
{
    UINT64 completed = 0;
    HRESULT registration = S_OK;
    int registrations = 0;
    UINT64 GetCompletedValue() { return completed; }
    HRESULT SetEventOnCompletion(UINT64, HANDLE)
    {
        ++registrations;
        return registration;
    }
};
struct ID3D12Device : Ref
{
    HRESULT reason = S_OK, createResult = S_OK;
    std::vector<std::unique_ptr<ID3D12Fence>> fences;
    HRESULT GetDeviceRemovedReason() { return reason; }
    HRESULT CreateFence(UINT64, UINT, ID3D12Fence** out)
    {
        if (FAILED(createResult))
            return createResult;
        fences.push_back(std::make_unique<ID3D12Fence>());
        *out = fences.back().get();
        return S_OK;
    }
};
struct ID3D12CommandList : Ref
{
};
struct ID3D12CommandAllocator : Ref
{
    int resets = 0;
    HRESULT resetResult = S_OK;
    HRESULT Reset()
    {
        ++resets;
        return resetResult;
    }
};
struct ID3D12Resource : Ref
{
};
struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    HRESULT Reset(ID3D12CommandAllocator*, void*) { return S_OK; }
    HRESULT closeResult = S_OK;
    HRESULT Close() { return closeResult; }
    ID3D12Resource* copiedFrom = nullptr;
    ID3D12Resource* copiedTo = nullptr;
    void CopyResource(ID3D12Resource* destination, ID3D12Resource* source)
    {
        copiedTo = destination;
        copiedFrom = source;
    }
};
struct ID3D12CommandQueue : Ref
{
    std::function<void()> beforeSignal;
    HRESULT signalResult = S_OK;
    bool completeSignals = true;
    int executions = 0, signals = 0;
    std::vector<ID3D12Fence*> signaled;
    HRESULT Signal(ID3D12Fence* fence, UINT64 value)
    {
        ++signals;
        if (beforeSignal)
            beforeSignal();
        if (FAILED(signalResult))
            return signalResult;
        signaled.push_back(fence);
        if (completeSignals)
            fence->completed = value;
        return S_OK;
    }
    void ExecuteCommandLists(UINT, ID3D12CommandList**) { ++executions; }
};
struct Dx11Context : Ref
{
    HRESULT signalResult = S_OK;
    HRESULT Signal(ID3D12Fence* fence, UINT64 value)
    {
        if (SUCCEEDED(signalResult))
            fence->completed = value;
        return signalResult;
    }
    void Flush() {}
};
void TransitionResource(ID3D12GraphicsCommandList*, ID3D12Resource*, int, int) {}
struct Swapchain : Ref
{
    int resizes = 0;
    HRESULT resizeResult = S_OK;
    ID3D12Resource buffer;
    UINT GetCurrentBackBufferIndex() { return 0; }
    HRESULT GetBuffer(UINT, ID3D12Resource** out)
    {
        *out = &buffer;
        buffer.AddRef();
        return S_OK;
    }
    HRESULT ResizeBuffers(UINT, UINT, UINT, int, UINT)
    {
        ++resizes;
        return resizeResult;
    }
    HRESULT ResizeBuffers1(UINT, UINT, UINT, int, UINT, const UINT*, IUnknown* const*)
    {
        ++resizes;
        return resizeResult;
    }
};
struct Mutex
{
    int owner = 0;
    int getOwner() { return owner; }
};
struct FG
{
    struct Mutex Mutex;
    void* context = reinterpret_cast<void*>(123);
    int deactivations = 0, releases = 0;
    void* SwapchainContext() { return context; }
    // Frame generation's own queue. Null models an FG object that has not got one, which is a
    // bridge in trouble rather than a case to paper over.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* GetCommandQueue() { return queue; }
    void Deactivate() { ++deactivations; }
    void ReleaseSwapchain(HWND) { ++releases; }
};
enum class SwapchainInteropApi
{
    None,
    Dx11wDx12
};
struct State
{
    void* currentSwapchain = nullptr;
    void* currentWrappedSwapchain = nullptr;
    Swapchain* currentRealSwapchain = nullptr;
    Swapchain* currentFGSwapchain = nullptr;
    ID3D12Device* currentD3D11Device = nullptr;
    FG* currentFG = nullptr;
    void* currentFeature = nullptr;
    SwapchainInteropApi swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;
    bool fgResetCapturedResources = false, fgOnlyUseCapturedResources = false, fgChanged = false;
    bool scChanged = false, SCAllowTearing = false;
    float screenWidth = 0, screenHeight = 0, lastMipBias = 0, lastMipBiasMax = 0;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct Config
{
    struct
    {
        bool value_or_default() { return false; }
    } FGEnabled;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
namespace FGHooks
{
void ClearDx12InteropPresentSC(Swapchain*) {}
} // namespace FGHooks
namespace MenuOverlayDx
{
int cleanups = 0;
void CleanupRenderTarget(bool, HWND) { ++cleanups; }
} // namespace MenuOverlayDx
int deaths = 0;
struct DeathCounter
{
    ~DeathCounter() { ++deaths; }
};
// The neural present host, reduced to the lifecycle the bridge is responsible for. Everything it
// actually does needs a GPU; what it is owed by its caller does not.
namespace DlssNr
{
struct PresentHost
{
    bool recordSucceeds = true;
    bool producesOutput = true;
    ID3D12Resource composed {};

    unsigned records = 0, confirms = 0, abandons = 0, releases = 0;
    ID3D12Resource* lastSource = nullptr;
    int lastSourceState = -1;
    bool recordingOutstanding = false;

    bool Record(void* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source, int sourceState)
    {
        assert(device != nullptr && cmdList != nullptr && source != nullptr);
        // Recording again over an outstanding recording would lose whichever answer came first.
        assert(!recordingOutstanding && "a second Record before the last one was resolved");
        ++records;
        lastSource = source;
        lastSourceState = sourceState;
        recordingOutstanding = true;
        return recordSucceeds;
    }

    ID3D12Resource* Output() { return recordSucceeds && producesOutput ? &composed : nullptr; }

    void ConfirmExecuted()
    {
        ++confirms;
        recordingOutstanding = false;
    }
    void AbandonRecording()
    {
        ++abandons;
        recordingOutstanding = false;
    }
    void Release()
    {
        ++releases;
        recordingOutstanding = false;
    }
};
} // namespace DlssNr

class Dx11wDx12SC
{
  public:
    DeathCounter death;
    ~Dx11wDx12SC();
    ULONG Release();
    HRESULT ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT);
    HRESULT ResizeBuffers1(UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
    HRESULT _WaitForCopyAllocator(UINT);
    HRESULT _WaitForCopyQueueIdle(DWORD);
    HRESULT _DrainForTeardown(DWORD);
    void _ResetTeardownDrain();
    bool _DevicesRemoved() const;
    bool _OwnsFgPresenter() const;
    bool _OwnsPresenter() const;
    bool _OwnsOverlay() const;
    void _FinishRelease(bool);
    void _DetachWrapperGlobals();
    static void _CollectRetired();
    void _ReleaseInteropObjects();
    void _ReleaseInteropBackBuffers();
    bool _CopyDx11SharedToDx12FGBackBuffer(UINT);
    ID3D12CommandQueue* _PresentQueueForFrame();
    int refreshes = 0;
    void _RefreshCachedSwapchainDesc() { ++refreshes; }
    Swapchain *_real = nullptr, *_real1 = nullptr, *_real2 = nullptr, *_real3 = nullptr, *_real4 = nullptr;
    Swapchain* _fgSwapChain = nullptr;
    ID3D12Device *_dx11Device = nullptr, *_dx12Device = nullptr;
    Ref* _dx11Device5 = nullptr;
    Dx11Context *_dx11Context = nullptr, *_dx11Context4 = nullptr;
    ID3D12CommandQueue *_dx12CommandQueue = nullptr, *_presentQueue = nullptr;
    ID3D12Fence *_copyFence = nullptr, *_dx11Fence = nullptr, *_dx12SharedFence = nullptr;
    HANDLE _copyFenceEvent = nullptr, _dx11FenceEvent = nullptr, _sharedFenceHandle = nullptr;
    UINT64 _lastInteropCopyFenceValue = 0, _copyFenceValue = 1, _sharedFenceValue = 1, _dx11DrainValue = 0;
    std::vector<UINT64> _copyAllocatorFenceValues;
    std::vector<ID3D12CommandAllocator*> _copyAllocators;
    std::vector<ID3D12GraphicsCommandList*> _copyCommandLists;
    std::vector<ID3D12Resource*> _copyDestinations, _openedDx11BackBuffers, _sharedDx11BackBufferCopies;
    std::vector<HANDLE> _sharedBackBufferHandles;
    std::vector<int> _openedDx11BackBufferStates;
    ID3D12Fence* _drainFences[2] {};
    bool _drainSignaled[2] {};
    bool _resizeIncomplete = false;
    bool _hasInteropWork = false, _interopInitialized = false, _wasCurrentOnRelease = false;
    LONG _refcount = 1;
    HWND _handle = reinterpret_cast<HWND>(456);
    FG* _fg = nullptr;
    void* _fgSwapchainContext = nullptr;
    UINT _currentFakeIndex = 0;
    Dx11wDx12SC* _nextLive = nullptr;
    std::unique_ptr<DlssNr::PresentHost> _nrHost;
    inline static Dx11wDx12SC* _live = nullptr;
    Dx11wDx12SC* _nextRetired = nullptr;
    inline static Dx11wDx12SC* _retired = nullptr;
    inline static std::mutex _retiredMutex;
};
