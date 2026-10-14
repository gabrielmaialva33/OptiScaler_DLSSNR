// A D3D title under Proton's translation layers, for the Vulkan present host's stage 1: `--d3d11` renders and presents
// through DXVK (d3d11.dll, dxgi.dll), `--d3d12` through vkd3d-proton (d3d12.dll, d3d12core.dll) with DXVK's dxgi.dll.
// The translation layers sit beside the executable; OptiScaler is loaded as winmm.dll, so DXVK's dxgi.dll stays the
// real one. Every Vulkan call here is the translation layer's own, made from its own modules and threads -- which is
// the coverage the design asks to prove "separately" from a native Vulkan application.
//
// Like harness.cpp it asserts nothing about the host: it writes its window, its frame count and which DLLs answered to
// result.json, and run.py holds OptiScaler.log against them.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

constexpr unsigned kFrames = 12;

static void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static void Check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        char text[160];
        std::snprintf(text, sizeof(text), "%s returned 0x%08lX", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcW(window, message, w, l);
}

static void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static std::string ModulePath(const wchar_t* name)
{
    char path[MAX_PATH] = {};
    if (HMODULE module = GetModuleHandleW(name))
        GetModuleFileNameA(module, path, MAX_PATH);
    std::string text(path);
    for (char& c : text)
        if (c == '\\')
            c = '/';
    return text;
}

template <typename T> static void Release(T*& object)
{
    if (object != nullptr)
        object->Release();
    object = nullptr;
}

static unsigned RunD3D11(HWND window)
{
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    Require(d3d11 != nullptr, "no d3d11.dll");
    auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    Require(create != nullptr, "no D3D11CreateDeviceAndSwapChain");

    DXGI_SWAP_CHAIN_DESC desc {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain* swapchain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    Check(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapchain,
                 &device, nullptr, &context),
          "D3D11CreateDeviceAndSwapChain");

    for (unsigned frame = 0; frame < kFrames; ++frame)
    {
        Pump();
        ID3D11Texture2D* backbuffer = nullptr;
        Check(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer)), "GetBuffer");
        ID3D11RenderTargetView* view = nullptr;
        Check(device->CreateRenderTargetView(backbuffer, nullptr, &view), "CreateRenderTargetView");
        const float color[4] = { 0.1f, 0.2f, 0.05f * frame, 1.0f };
        context->ClearRenderTargetView(view, color);
        Release(view);
        Release(backbuffer);
        Check(swapchain->Present(1, 0), "Present");
    }

    context->ClearState();
    context->Flush();
    Release(context);
    Release(swapchain);
    Release(device);
    return kFrames;
}

static unsigned RunD3D12(HWND window)
{
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    Require(d3d12 != nullptr && dxgi != nullptr, "no d3d12.dll or dxgi.dll");
    auto createDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory2>(GetProcAddress(dxgi, "CreateDXGIFactory2"));
    Require(createDevice != nullptr && createFactory != nullptr, "no D3D12CreateDevice or CreateDXGIFactory2");

    ID3D12Device* device = nullptr;
    Check(createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device)),
          "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    Check(device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)),
          "CreateCommandQueue");

    IDXGIFactory4* factory = nullptr;
    Check(createFactory(0, __uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)), "CreateDXGIFactory2");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* swapchain1 = nullptr;
    Check(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &swapchain1), "CreateSwapChainForHwnd");
    IDXGISwapChain3* swapchain = nullptr;
    Check(swapchain1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapchain)),
          "IDXGISwapChain3");
    Release(swapchain1);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    Check(device->CreateDescriptorHeap(&heapDesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap)),
          "CreateDescriptorHeap");
    ID3D12CommandAllocator* allocator = nullptr;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                         reinterpret_cast<void**>(&allocator)),
          "CreateCommandAllocator");
    ID3D12GraphicsCommandList* list = nullptr;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                    __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list)),
          "CreateCommandList");
    Check(list->Close(), "Close");
    ID3D12Fence* fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence)),
          "CreateFence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 value = 0;

    for (unsigned frame = 0; frame < kFrames; ++frame)
    {
        Pump();
        ID3D12Resource* backbuffer = nullptr;
        Check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource),
                                   reinterpret_cast<void**>(&backbuffer)),
              "GetBuffer");
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(backbuffer, nullptr, rtv);

        Check(allocator->Reset(), "allocator Reset");
        Check(list->Reset(allocator, nullptr), "list Reset");
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backbuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &barrier);
        const float color[4] = { 0.2f, 0.1f, 0.05f * frame, 1.0f };
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        Check(list->Close(), "Close");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        Check(swapchain->Present(1, 0), "Present");

        Check(queue->Signal(fence, ++value), "Signal");
        Check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
        Require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "the frame never completed");
        Release(backbuffer);
    }

    CloseHandle(event);
    Release(fence);
    Release(list);
    Release(allocator);
    Release(heap);
    Release(swapchain);
    Release(factory);
    Release(queue);
    Release(device);
    return kFrames;
}

int main(int argc, char** argv)
{
    std::ofstream("result.json") << "{\"status\":\"RUNNING\"}\n";
    try
    {
        Require(argc == 2 && (!strcmp(argv[1], "--d3d11") || !strcmp(argv[1], "--d3d12")), "use --d3d11 or --d3d12");
        const bool d3d12 = !strcmp(argv[1], "--d3d12");

        HINSTANCE module = GetModuleHandleW(nullptr);
        WNDCLASSW cls {};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = module;
        cls.lpszClassName = L"OptiScalerVkPresentHostTranslated";
        Require(RegisterClassW(&cls) != 0, "RegisterClass failed");
        // An owned dialog, as in harness.cpp, so a tiling window manager leaves its size alone.
        HWND owner = CreateWindowW(cls.lpszClassName, L"owner", 0, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"OptiScaler present host, translated",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 640, 480, owner, nullptr, module,
                                      nullptr);
        Require(owner != nullptr && window != nullptr, "no Win32 window under Wine");
        ShowWindow(window, SW_SHOW);
        Pump();

        Require(LoadLibraryW(L".\\winmm.dll") != nullptr, "could not load OptiScaler as winmm.dll");

        const unsigned frames = d3d12 ? RunD3D12(window) : RunD3D11(window);

        char hwnd[32];
        std::snprintf(hwnd, sizeof(hwnd), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window)));
        std::ofstream("result.json") << "{\n  \"status\": \"PASS\",\n  \"api\": \"" << (d3d12 ? "d3d12" : "d3d11")
                                     << "\",\n  \"window\": \"" << hwnd << "\",\n  \"frames\": " << frames
                                     << ",\n  \"dxgi\": \"" << ModulePath(L"dxgi.dll") << "\",\n  \"d3d11\": \""
                                     << ModulePath(L"d3d11.dll") << "\",\n  \"d3d12\": \"" << ModulePath(L"d3d12.dll")
                                     << "\",\n  \"d3d12core\": \"" << ModulePath(L"d3d12core.dll")
                                     << "\",\n  \"winmm\": \"" << ModulePath(L"winmm.dll") << "\"\n}\n";
        std::printf("PASS: %u %s frames presented\n", frames, d3d12 ? "D3D12" : "D3D11");
        DestroyWindow(window);
        DestroyWindow(owner);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::ofstream("result.json") << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
