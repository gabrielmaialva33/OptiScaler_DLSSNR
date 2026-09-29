// The D3D12 overlay's render targets, reduced to what a swapchain resize depends on: public references on the
// swapchain's buffers. The production CreateRenderTargetDx12 and CleanupRenderTargetDx12 are compiled against these.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
using HRESULT = int32_t;
using UINT = unsigned;
using ULONG = unsigned;
using DXGI_FORMAT = int;
constexpr HRESULT S_OK = 0, DXGI_ERROR_INVALID_CALL = -6;
constexpr int D3D12_RTV_DIMENSION_TEXTURE2D = 4;
#define IID_PPV_ARGS(p) p
#define LOG_FUNC() ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
#define SAFE_RELEASE(p)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (p)                                                                                                         \
        {                                                                                                              \
            (p)->Release();                                                                                            \
            (p) = nullptr;                                                                                             \
        }                                                                                                              \
    } while (0)

struct Ref
{
    int refs = 1;
    ULONG AddRef() { return ++refs; }
    ULONG Release()
    {
        assert(refs > 0);
        return --refs;
    }
};
// A swapchain buffer. Its own swapchain holds the first reference; anything above that is a public one.
struct ID3D12Resource : Ref
{
};
struct ID3D12DescriptorHeap : Ref
{
};
struct ID3D12CommandAllocator : Ref
{
};
struct ID3D12GraphicsCommandList : Ref
{
};
struct ID3D12CommandQueue : Ref
{
};
struct D3D12_CPU_DESCRIPTOR_HANDLE
{
    size_t ptr = 0;
};
struct D3D12_RENDER_TARGET_VIEW_DESC
{
    DXGI_FORMAT Format = 0;
    int ViewDimension = 0;
};
struct ID3D12Device : Ref
{
    int views = 0;
    void CreateRenderTargetView(ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)
    {
        ++views;
    }
};
struct DXGI_SWAP_CHAIN_DESC
{
    UINT BufferCount = 0;
    struct
    {
        DXGI_FORMAT Format = 0;
    } BufferDesc;
};
struct IDXGISwapChain
{
    ID3D12Resource buffers[2];
    HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* out)
    {
        out->BufferCount = 2;
        out->BufferDesc.Format = 28;
        return S_OK;
    }
    HRESULT GetBuffer(UINT index, ID3D12Resource** out)
    {
        *out = &buffers[index];
        buffers[index].AddRef();
        return S_OK;
    }
    // vkd3d-proton's rule (dxgi_vk_swap_chain_ChangeProperties): no public reference on any buffer.
    HRESULT ResizeBuffers()
    {
        for (auto& buffer : buffers)
        {
            if (buffer.refs != 1)
                return DXGI_ERROR_INVALID_CALL;
        }
        return S_OK;
    }
};

static int GetCorrectDXGIFormat(int format) { return format; }

// menu_overlay_dx.cpp's statics, with the same names and meanings.
static int const NUM_BACK_BUFFERS = 8;
static bool _dx12Device = false;
static bool _isInited = false;
static ID3D12Device* g_pd3dDeviceParam = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12CommandAllocator* g_commandAllocators[NUM_BACK_BUFFERS] = {};
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {};
struct DescriptorHeapAllocator
{
    int destroys = 0;
    void Destroy() { ++destroys; }
};
static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;

struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
namespace MenuOverlayBase
{
inline bool inited = true;
inline bool IsInited() { return inited; }
} // namespace MenuOverlayBase
namespace ImGui
{
struct IO
{
    void* BackendRendererUserData = nullptr;
};
inline IO io;
inline IO& GetIO() { return io; }
} // namespace ImGui
// What another thread does while the cleanup is inside the ImGui backend's shutdown, which is where the
// cleanup has released the render targets and has not yet cleared the init state.
inline std::function<void()> duringImGuiShutdown;
inline int imguiShutdowns = 0;
inline void ImGui_ImplDX12_Shutdown(bool)
{
    ++imguiShutdowns;
    if (duringImGuiShutdown)
        duringImGuiShutdown();
    ImGui::io.BackendRendererUserData = nullptr;
}
