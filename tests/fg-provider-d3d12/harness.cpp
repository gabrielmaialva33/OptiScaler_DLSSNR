// Full-DLL synthetic game. The oracle calls the real output plugin, never the game's synthetic GetState.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;
static FILE* logFile;
static void Say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(logFile, format, args);
    va_end(args);
    fflush(logFile);
}
static void Require(bool ok, const char* why)
{
    if (!ok)
    {
        Say("FAIL: %s\n", why);
        ExitProcess(1); // GPU completion may be unknown; do not unwind live resources.
    }
}
static void Hr(HRESULT hr, const char* why)
{
    if (FAILED(hr))
        Say("HRESULT=0x%08lx\n", static_cast<unsigned long>(hr));
    Require(SUCCEEDED(hr), why);
}
static void Sl(sl::Result result, const char* why)
{
    if (result != sl::Result::eOk)
        Say("SL_RESULT=%u\n", static_cast<unsigned>(result));
    Require(result == sl::Result::eOk, why);
}
// Read the loaded PE export directory, bypassing OptiScaler's GetProcAddress interception. No code patching.
static FARPROC NativeExport(HMODULE module, const char* name)
{
    Require(module != nullptr, "output plugin loaded");
    auto base = reinterpret_cast<unsigned char*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    Require(dos->e_magic == IMAGE_DOS_SIGNATURE, "plugin DOS signature");
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    Require(nt->Signature == IMAGE_NT_SIGNATURE, "plugin NT signature");
    auto entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    Require(entry.VirtualAddress && entry.Size, "plugin export directory");
    auto exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + entry.VirtualAddress);
    auto names = reinterpret_cast<DWORD*>(base + exports->AddressOfNames);
    auto ordinals = reinterpret_cast<WORD*>(base + exports->AddressOfNameOrdinals);
    auto functions = reinterpret_cast<DWORD*>(base + exports->AddressOfFunctions);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        if (strcmp(reinterpret_cast<char*>(base + names[i]), name) == 0)
        {
            const DWORD rva = functions[ordinals[i]];
            Require(rva < entry.VirtualAddress || rva >= entry.VirtualAddress + entry.Size,
                    "oracle export must not be a forwarder");
            return reinterpret_cast<FARPROC>(base + rva);
        }
    Require(false, "plugin native export found");
    return nullptr;
}
template <typename T> static T Import(HMODULE module, const char* name)
{
    auto pointer = reinterpret_cast<T>(GetProcAddress(module, name));
    Require(pointer != nullptr, name);
    return pointer;
}
static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &barrier);
}
struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 serial = 0;
    Gpu()
    {
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            Hr(adapter->GetDesc1(&desc), "adapter desc");
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x10de &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                break;
        }
        Require(device != nullptr, "NVIDIA hardware D3D12 device");
        D3D12_COMMAND_QUEUE_DESC desc {};
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "direct queue");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&commands)),
           "command list");
        Hr(commands->Close(), "initial close");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Require(event != nullptr, "event");
    }
    void Wait(ID3D12Fence* target, UINT64 value)
    {
        Hr(target->SetEventOnCompletion(value, event), "fence completion event");
        Require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "GPU completion timeout");
        Hr(device->GetDeviceRemovedReason(), "device removed");
    }
    void Idle()
    {
        Hr(queue->Signal(fence.Get(), ++serial), "signal");
        Wait(fence.Get(), serial);
    }
    void Begin()
    {
        Hr(allocator->Reset(), "allocator reset");
        Hr(commands->Reset(allocator.Get(), nullptr), "command list reset");
    }
    void Submit()
    {
        Hr(commands->Close(), "command list close");
        ID3D12CommandList* lists[] = { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        Idle();
    }
    ComPtr<ID3D12Resource> Resource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = heap;
        ComPtr<ID3D12Resource> resource;
        Hr(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&resource)),
           "resource");
        return resource;
    }
};
struct Texture
{
    ComPtr<ID3D12Resource> image, upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    unsigned char* mapped = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    Texture(Gpu& gpu, UINT width, UINT height, DXGI_FORMAT format)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = format;
        image = gpu.Resource(desc, D3D12_HEAP_TYPE_DEFAULT, state);
        UINT64 bytes;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        upload = gpu.Resource(desc, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE empty {};
        Hr(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "upload map");
    }
    ~Texture() { upload->Unmap(0, nullptr); }
    void Copy(Gpu& gpu, D3D12_RESOURCE_STATES target)
    {
        if (state != D3D12_RESOURCE_STATE_COPY_DEST)
            Transition(gpu.commands.Get(), image.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source {}, destination {};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        destination.pResource = image.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        gpu.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(gpu.commands.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, target);
        state = target;
    }
};
static void Matrix(sl::float4x4& destination, FXMMATRIX value)
{
    XMFLOAT4X4 stored;
    XMStoreFloat4x4(&stored, value);
    memcpy(&destination, &stored, sizeof(stored));
}
static sl::Constants Camera(UINT width, UINT height, bool reset)
{
    sl::Constants result {};
    result.cameraNear = 0.1f;
    result.cameraFar = 100.0f;
    result.cameraFOV = XM_PI / 3.0f;
    result.cameraAspectRatio = static_cast<float>(width) / height;
    const auto projection =
        XMMatrixPerspectiveFovRH(result.cameraFOV, result.cameraAspectRatio, result.cameraNear, result.cameraFar);
    Matrix(result.cameraViewToClip, projection);
    Matrix(result.clipToCameraView, XMMatrixInverse(nullptr, projection));
    Matrix(result.clipToPrevClip, XMMatrixIdentity());
    Matrix(result.prevClipToClip, XMMatrixIdentity());
    Matrix(result.clipToLensClip, XMMatrixIdentity());
    result.cameraPos = { 0.0f, 0.0f, 0.0f };
    result.cameraUp = { 0.0f, 1.0f, 0.0f };
    result.cameraRight = { 1.0f, 0.0f, 0.0f };
    result.cameraFwd = { 0.0f, 0.0f, -1.0f };
    result.cameraPinholeOffset = { 0.0f, 0.0f };
    result.jitterOffset = { 0.0f, 0.0f };
    result.mvecScale = { 1.0f / width, 1.0f / height };
    result.depthInverted = sl::Boolean::eFalse;
    result.cameraMotionIncluded = sl::Boolean::eTrue;
    result.motionVectors3D = sl::Boolean::eFalse;
    result.motionVectorsDilated = sl::Boolean::eTrue;
    result.motionVectorsJittered = sl::Boolean::eFalse;
    result.orthographicProjection = sl::Boolean::eFalse;
    result.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    return result;
}
static void Fill(Texture& color, Texture& depth, Texture& motion, UINT width, UINT height, UINT frame)
{
    // Perspective z=-2 plane; the object's current-to-previous motion is -2 pixels on X.
    const float z = 100.0f / 99.9f - (0.1f * 100.0f / 99.9f) / 2.0f;
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
        {
            const UINT sx = (x + width - (frame * 2) % width) % width;
            auto rgba = color.mapped + y * color.footprint.Footprint.RowPitch + x * 4;
            const bool tile = ((sx / 32) ^ (y / 32)) & 1;
            rgba[0] = static_cast<unsigned char>(tile ? 192 : 32);
            rgba[1] = static_cast<unsigned char>(32 + sx * 192 / width);
            rgba[2] = static_cast<unsigned char>(32 + y * 192 / height);
            rgba[3] = 255;
            reinterpret_cast<float*>(depth.mapped + y * depth.footprint.Footprint.RowPitch)[x] = z;
            auto vectors = reinterpret_cast<float*>(motion.mapped + y * motion.footprint.Footprint.RowPitch);
            vectors[x * 2] = -2.0f;
            vectors[x * 2 + 1] = 0.0f;
        }
}
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}
int main()
{
    logFile = fopen("fg-provider.log", "w");
    if (!logFile)
        return 1;
    Require(LoadLibraryW(L"OptiScaler.dll") != nullptr, "full OptiScaler DLL load");
    auto gameSl = LoadLibraryW(L"input\\sl.interposer.dll");
    Require(gameSl != nullptr, "game Streamline interposer load");
    const auto init = Import<decltype(&slInit)>(gameSl, "slInit");
    const auto setDevice = Import<decltype(&slSetD3DDevice)>(gameSl, "slSetD3DDevice");
    const auto token = Import<decltype(&slGetNewFrameToken)>(gameSl, "slGetNewFrameToken");
    const auto constants = Import<decltype(&slSetConstants)>(gameSl, "slSetConstants");
    const auto tags = Import<decltype(&slSetTagForFrame)>(gameSl, "slSetTagForFrame");
    const auto feature = Import<decltype(&slGetFeatureFunction)>(gameSl, "slGetFeatureFunction");
    wchar_t inputPath[MAX_PATH];
    Require(GetFullPathNameW(L"input", MAX_PATH, inputPath, nullptr) > 0, "input path");
    const wchar_t* paths[] = { inputPath };
    const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeaturePCL, sl::kFeatureReflex };
    sl::Preferences preferences {};
    preferences.applicationId = 0x0F71CA1E;
    preferences.engine = sl::EngineType::eCustom;
    preferences.renderAPI = sl::RenderAPI::eD3D12;
    preferences.pathsToPlugins = paths;
    preferences.numPathsToPlugins = 1;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = 3;
    preferences.flags |= sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.flags &= ~sl::PreferenceFlags::eAllowOTA;
    preferences.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    Sl(init(preferences, sl::kSDKVersion), "game Streamline init");
    Gpu gpu;
    Sl(setDevice(gpu.device.Get()), "game Streamline set device");
    PFun_slDLSSGSetOptions* setOptions = nullptr;
    Sl(feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(setOptions)), "game FG options bind");
    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    Sl(setOptions(sl::ViewportHandle(0), options), "game enables FG");
    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RetoqueFgProvider";
    Require(RegisterClassW(&windowClass), "window class");
    HWND window =
        CreateWindowW(windowClass.lpszClassName, L"Retoque automated DLSS-G provider", WS_OVERLAPPEDWINDOW,
                      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, windowClass.hInstance, nullptr);
    Require(window != nullptr, "window");
    ShowWindow(window, SW_SHOW);
    ComPtr<IDXGIFactory2> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "hooked factory");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = 1280;
    desc.Height = 720;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> initial;
    Hr(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &initial), "FG swapchain");
    ComPtr<IDXGISwapChain3> swapchain;
    Hr(initial.As(&swapchain), "swapchain3");
    initial.Reset();
    // The output plugin is a separate module from the game-style SDK. Read its real state, not synthetic game state.
    wchar_t outputPath[MAX_PATH];
    Require(GetFullPathNameW(L"streamline\\sl.dlss_g.dll", MAX_PATH, outputPath, nullptr) > 0, "output path");
    HMODULE output = GetModuleHandleW(outputPath);
    using PluginFunction = void* (*) (const char*);
    auto pluginFunction = reinterpret_cast<PluginFunction>(NativeExport(output, "slGetPluginFunction"));
    auto getState = reinterpret_cast<PFun_slDLSSGGetState*>(pluginFunction("slDLSSGGetState"));
    Require(getState != nullptr, "real output GetState");
    HMODULE stateOwner = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(getState), &stateOwner) &&
                stateOwner == output,
            "state oracle belongs to actual output plugin");
    Say("ORACLE=real-output-plugin\n");
    UINT frame = 1, totalApp = 0, totalActual = 0;
    for (UINT phase = 0; phase < 4; ++phase)
    {
        const bool enabled = phase != 1;
        const UINT width = phase == 3 ? 1600 : 1280, height = phase == 3 ? 900 : 720;
        const UINT count = enabled ? 96 : 48;
        options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        Sl(setOptions(sl::ViewportHandle(0), options), "game enable transition");
        gpu.Idle();
        if (phase == 3)
        {
            DXGI_SWAP_CHAIN_DESC actual {};
            Hr(swapchain->GetDesc(&actual), "swapchain desc");
            Hr(swapchain->ResizeBuffers(3, width, height, desc.Format, actual.Flags), "FG resize");
        }
        Texture color(gpu, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
        Texture depth(gpu, width, height, DXGI_FORMAT_R32_FLOAT);
        Texture motion(gpu, width, height, DXGI_FORMAT_R32G32_FLOAT);
        UINT app = 0, actual = 0, steadyApp = 0, steadyActual = 0, okStates = 0;
        sl::DLSSGState state {};
        Sl(getState(sl::ViewportHandle(0), state, nullptr), "baseline real state");
        for (UINT local = 0; local < count; ++local, ++frame)
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            sl::FrameToken* current = nullptr;
            Sl(token(current, &frame), "frame token");
            auto camera = Camera(width, height, local == 0);
            Sl(constants(camera, *current, sl::ViewportHandle(0)), "game camera constants");
            Fill(color, depth, motion, width, height, frame);
            gpu.Begin();
            color.Copy(gpu, D3D12_RESOURCE_STATE_COPY_SOURCE);
            depth.Copy(gpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            motion.Copy(gpu, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ComPtr<ID3D12Resource> backbuffer;
            Hr(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "backbuffer");
            Transition(gpu.commands.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.commands->CopyResource(backbuffer.Get(), color.image.Get());
            Transition(gpu.commands.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
            Transition(gpu.commands.Get(), color.image.Get(), color.state,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            color.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            sl::Resource resources[3];
            resources[0].native = depth.image.Get();
            resources[1].native = motion.image.Get();
            resources[2].native = color.image.Get();
            sl::Extent extent { 0, 0, width, height };
            sl::ResourceTag tagged[3] = {
                { &resources[0], sl::kBufferTypeDepth, sl::eValidUntilPresent, &extent },
                { &resources[1], sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &extent },
                { &resources[2], sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &extent }
            };
            for (auto& resource : resources)
                resource.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            Sl(tags(*current, sl::ViewportHandle(0), tagged, 3, gpu.commands.Get()), "game frame tags");
            gpu.Submit();
            Hr(swapchain->Present(0, 0), "real FG present");
            gpu.Idle();
            state = {};
            Sl(getState(sl::ViewportHandle(0), state, nullptr), "real plugin state");
            if (state.inputsProcessingCompletionFence)
                gpu.Wait(static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence),
                         state.lastPresentInputsProcessingCompletionFenceValue);
            ++app;
            actual += state.numFramesActuallyPresented;
            if (local >= 32)
            {
                ++steadyApp;
                steadyActual += state.numFramesActuallyPresented;
                if (state.status != sl::DLSSGStatus::eOk)
                    Say("BAD_STATUS phase=%u local=%u status=%u\n", phase, local, static_cast<unsigned>(state.status));
                Require(state.status == sl::DLSSGStatus::eOk, "real DLSS-G status after warmup");
                ++okStates;
            }
            Say("FRAME phase=%u app=%u actual=%u status=%u max=%u\n", phase, frame, state.numFramesActuallyPresented,
                static_cast<unsigned>(state.status), state.numFramesToGenerateMax);
            Sleep(8);
        }
        Say("PHASE phase=%u enabled=%u app=%u actual=%u steady_app=%u steady_actual=%u ok=%u completed=%u\n", phase,
            enabled, app, actual, steadyApp, steadyActual, okStates, app);
        if (enabled)
            Require(steadyActual >= steadyApp + 16, "real output plugin reported no interpolated-frame surplus");
        else
            Require(steadyActual == steadyApp,
                    "disabled FG must report exactly one actual present per application present");
        totalApp += app;
        totalActual += actual;
        gpu.Idle();
    }
    Say("PASS: FG-PROVIDER app=%u actual=%u phases=4 resizes=1 oracle=real-plugin\n", totalApp, totalActual);
    // OptiScaler and Streamline own process lifetime hooks; let normal process exit run those hooks.
    return 0;
}
