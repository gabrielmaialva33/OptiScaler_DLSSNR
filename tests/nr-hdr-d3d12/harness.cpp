#include "pch.h"

#include <DirectXPackedVector.h>
#include <d3d12sdklayers.h>
#include <dlssnr/DlssNr_DisplayWhite.h>
#include <shaders/dlssnr/DlssNr_PresentHdr_Dx12.h>

using Microsoft::WRL::ComPtr;
using namespace DlssNr::PresentHdr;
using namespace DirectX::PackedVector;

namespace
{
[[noreturn]] void Fail(const char* text)
{
    std::printf("FAIL: %s\n", text);
    std::fflush(stdout);
    ExitProcess(1); // Never unwind resources whose GPU completion is unknown.
}
void Require(bool test, const char* text)
{
    if (!test)
        Fail(text);
}
void Hr(HRESULT hr, const char* text) { Require(SUCCEEDED(hr), text); }
constexpr UINT W = 1024, H = 8;
struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 serial = 0;
    Gpu()
    {
        ComPtr<ID3D12Debug> debug;
        const bool nativeDebug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (nativeDebug)
            debug->EnableDebugLayer();
        std::printf("DEBUG_LAYER=%s\n", nativeDebug ? "active" : "unavailable (native Windows validation required)");
        Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q {};
        Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
           "list");
        Hr(list->Close(), "initial close");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Require(event != nullptr, "event");
    }
    ~Gpu() { CloseHandle(event); }
    void Begin()
    {
        Hr(allocator->Reset(), "allocator reset");
        Hr(list->Reset(allocator.Get(), nullptr), "list reset");
    }
    void Submit()
    {
        Hr(list->Close(), "close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Hr(queue->Signal(fence.Get(), ++serial), "signal");
        Hr(fence->SetEventOnCompletion(serial, event), "completion event");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU timeout");
        Hr(device->GetDeviceRemovedReason(), "device removed");
        if (messages)
        {
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i)
            {
                SIZE_T bytes = 0;
                messages->GetMessage(i, nullptr, &bytes);
                std::vector<uint8_t> data(bytes);
                auto msg = reinterpret_cast<D3D12_MESSAGE*>(data.data());
                Hr(messages->GetMessage(i, msg, &bytes), "debug message");
                if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    Fail(msg->pDescription);
            }
            messages->ClearStoredMessages();
        }
    }
    ComPtr<ID3D12Resource> Resource(D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        ComPtr<ID3D12Resource> value;
        auto props = CD3DX12_HEAP_PROPERTIES(heap);
        Hr(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&value)),
           "resource");
        return value;
    }
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        return Resource(CD3DX12_RESOURCE_DESC::Tex2D(format, W, H, 1, 1, 1, 0, flags), D3D12_HEAP_TYPE_DEFAULT, state);
    }
    void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
        list->ResourceBarrier(1, &b);
    }
    ComPtr<ID3D12Resource> Upload(ID3D12Resource* texture, const std::vector<uint8_t>& data, UINT pixelBytes)
    {
        auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 size = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &size);
        auto buffer =
            Resource(CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        Hr(buffer->Map(0, &noRead, &mapped), "upload map");
        for (UINT y = 0; y < H; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch,
                        data.data() + y * W * pixelBytes, W * pixelBytes);
        buffer->Unmap(0, nullptr);
        auto from = CD3DX12_TEXTURE_COPY_LOCATION(buffer.Get(), fp);
        auto to = CD3DX12_TEXTURE_COPY_LOCATION(texture, 0);
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        return buffer;
    }
    std::vector<uint8_t> Read(ID3D12Resource* texture, UINT pixelBytes)
    {
        auto desc = texture->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 size = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &size);
        auto buffer =
            Resource(CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        Begin();
        auto from = CD3DX12_TEXTURE_COPY_LOCATION(texture, 0);
        auto to = CD3DX12_TEXTURE_COPY_LOCATION(buffer.Get(), fp);
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Submit();
        void* mapped = nullptr;
        Hr(buffer->Map(0, nullptr, &mapped), "readback map");
        std::vector<uint8_t> result(W * H * pixelBytes);
        for (UINT y = 0; y < H; ++y)
            std::memcpy(result.data() + y * W * pixelBytes,
                        static_cast<uint8_t*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch, W * pixelBytes);
        D3D12_RANGE noWrite { 0, 0 };
        buffer->Unmap(0, &noWrite);
        return result;
    }
};

std::vector<uint8_t> Pixels(uint32_t profile)
{
    std::vector<uint8_t> bytes(W * H * (profile == kProfileHdr10 ? 4 : 8));
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
        {
            if (profile == kProfileHdr10)
            {
                const uint32_t r = x, g = y == 0 ? x : (x * 37 + y * 71) % 1024,
                               b = y == 0 ? x : (x * 131 + y * 19) % 1024;
                const uint32_t pixel = r | (g << 10) | (b << 20) | ((x % 4) << 30);
                std::memcpy(bytes.data() + (y * W + x) * 4, &pixel, 4);
            }
            else
            {
                const HALF pixel[] = { XMConvertFloatToHalf((static_cast<int>(x) - 256) / 8.0f),
                                       XMConvertFloatToHalf(y == 0 ? -0.00001f : y * 20.0f),
                                       XMConvertFloatToHalf(x / 1023.0f), XMConvertFloatToHalf((x % 4) / 3.0f) };
                std::memcpy(bytes.data() + (y * W + x) * 8, pixel, 8);
            }
        }
    return bytes;
}
void Conversion(Gpu& gpu, DlssNr_PresentHdr_Dx12& pass, uint32_t profile)
{
    const auto format = profile == kProfileHdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    const UINT bpp = profile == kProfileHdr10 ? 4 : 8;
    Require(pass.Supports(format), "format support");
    auto original = gpu.Texture(format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto work = gpu.Texture(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto output =
        gpu.Texture(format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto bytes = Pixels(profile);
    gpu.Begin();
    auto upload = gpu.Upload(original.Get(), bytes, bpp);
    gpu.Barrier(original.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Require(!pass.ValidResources(original.Get(), original.Get(), original.Get(), profile, kModeDecode), "reject alias");
    Require(!pass.ValidResources(original.Get(), original.Get(), work.Get(), 99, kModeDecode), "reject profile");
    Require(!pass.ValidResources(original.Get(), work.Get(), original.Get(), profile, kModeEncode),
            "reject output without UAV");
    Require(pass.Decode(gpu.list.Get(), original.Get(), work.Get(), profile), "decode");
    gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Require(pass.Encode(gpu.list.Get(), original.Get(), work.Get(), output.Get(), profile), "encode");
    gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Submit();
    Require(gpu.Read(output.Get(), bpp) == bytes, "identity must be bit exact, including alpha");
    // Cross the32-slot descriptor/constant ring with fenced recordings, checking that no stale slot is observed.
    for (unsigned repeat = 0; repeat < 19; ++repeat)
    {
        gpu.Begin();
        gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(pass.Decode(gpu.list.Get(), original.Get(), work.Get(), profile), "ring decode");
        gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(pass.Encode(gpu.list.Get(), original.Get(), work.Get(), output.Get(), profile), "ring encode");
        gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.Submit();
    }
    Require(gpu.Read(output.Get(), bpp) == bytes, "identity after ring wrap");
    auto noUav = gpu.Texture(format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
    Require(!pass.ValidResources(original.Get(), work.Get(), noUav.Get(), profile, kModeEncode),
            "reject missing UAV flag");
    auto wrongDesc = work->GetDesc();
    wrongDesc.Width /= 2;
    auto wrongSize = gpu.Resource(wrongDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    Require(!pass.ValidResources(original.Get(), wrongSize.Get(), output.Get(), profile, kModeEncode),
            "reject extent mismatch");
    wrongDesc = work->GetDesc();
    wrongDesc.DepthOrArraySize = 2;
    auto array = gpu.Resource(wrongDesc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    Require(!pass.ValidResources(original.Get(), array.Get(), output.Get(), profile, kModeEncode), "reject array view");

    // Known edit in a middle stripe; outside it the production reference bypass must retain original codes.
    gpu.Begin();
    gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Submit();
    auto processed = gpu.Read(work.Get(), 8);
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 300; x < 700; ++x)
            for (UINT c = 0; c < 3; ++c)
            {
                HALF value;
                auto at = processed.data() + ((y * W + x) * 4 + c) * 2;
                std::memcpy(&value, at, 2);
                value = XMConvertFloatToHalf(XMConvertHalfToFloat(value) * 0.8f + 0.01f);
                std::memcpy(at, &value, 2);
            }
    gpu.Begin();
    gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto edit = gpu.Upload(work.Get(), processed, 8);
    gpu.Barrier(work.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Require(pass.Encode(gpu.list.Get(), original.Get(), work.Get(), output.Get(), profile), "edited encode");
    gpu.Barrier(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.Submit();
    const auto changed = gpu.Read(output.Get(), bpp);
    unsigned edits = 0;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
        {
            const size_t index = y * W + x;
            if (x < 300 || x >= 700)
                Require(!std::memcmp(changed.data() + index * bpp, bytes.data() + index * bpp, bpp),
                        "outside edit exact");
            else if (profile == kProfileHdr10)
            {
                HALF half[4];
                std::memcpy(half, processed.data() + index * 8, 8);
                const double light[] = { XMConvertHalfToFloat(half[0]), XMConvertHalfToFloat(half[1]),
                                         XMConvertHalfToFloat(half[2]) };
                double expected[3];
                EncodeHdr10(light, expected);
                uint32_t pixel, before;
                std::memcpy(&pixel, changed.data() + index * 4, 4);
                std::memcpy(&before, bytes.data() + index * 4, 4);
                Require((pixel >> 30) == (before >> 30), "HDR10 alpha");
                for (UINT c = 0; c < 3; ++c)
                    Require(std::abs(static_cast<int>((pixel >> (10 * c)) & 1023) -
                                     static_cast<int>(std::lround(expected[c] * 1023))) <= 1,
                            "edit within one PQ code");
                edits += pixel != before;
            }
            else
            {
                Require(!std::memcmp(changed.data() + index * 8, processed.data() + index * 8, 6),
                        "scRGB signed edit exact");
                Require(!std::memcmp(changed.data() + index * 8 + 6, bytes.data() + index * 8 + 6, 2), "scRGB alpha");
                ++edits;
            }
        }
    Require(edits > 1000, "edit coverage");
    std::printf("PASS: profile %u identity %u pixels exact; known edit %u pixels, untouched exterior/alpha exact\n",
                profile, W * H, edits);
}

void Swapchain(Gpu& gpu)
{
    const wchar_t* name = L"Retoque HDR test";
    WNDCLASSW cls {};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = name;
    RegisterClassW(&cls);
    HWND window = CreateWindowW(name, name, WS_OVERLAPPEDWINDOW, 0, 0, W, H, nullptr, nullptr, cls.hInstance, nullptr);
    Require(window != nullptr, "window");
    ComPtr<IDXGIFactory2> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = W;
    desc.Height = H;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    Hr(factory->CreateSwapChainForHwnd(gpu.queue.Get(), window, &desc, nullptr, nullptr, &base),
       "HDR10-format swapchain");
    ComPtr<IDXGISwapChain3> chain;
    Hr(base.As(&chain), "swapchain3");
    UINT support = 0;
    const auto space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool pq = SUCCEEDED(chain->CheckColorSpaceSupport(space, &support)) &&
                    (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    if (pq)
        Hr(chain->SetColorSpace1(space), "PQ colorspace");
    const auto white = DisplayWhiteForSwapchain(chain.Get());
    std::printf("DISPLAY_WHITE=%.1f nits (%s)\n", white.nits, WhiteSource(white));
    // No claim of HDR scanout when the runtime/display does not advertise PQ. The real ten-bit swapchain still
    // exercises resource compatibility; texture conversion/readback above is mandatory on either display type.
    std::printf("HDR10_PRESENT=%s\n", pq ? "SUPPORTED" : "SKIP (PQ not advertised by runtime/display)");
    ComPtr<ID3D12Resource> backbuffer;
    Hr(chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "swapchain buffer");
    gpu.Begin();
    gpu.Barrier(backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto pixels = Pixels(kProfileHdr10);
    auto upload = gpu.Upload(backbuffer.Get(), pixels, 4);
    gpu.Barrier(backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    gpu.Submit();
    Hr(chain->Present(0, 0), "present");
    gpu.Begin();
    gpu.Submit();
    backbuffer.Reset();
    chain.Reset();
    base.Reset();
    factory.Reset();
    DestroyWindow(window);
}
} // namespace
int main()
{
    Gpu gpu;
    DlssNr_PresentHdr_Dx12 pass("HDR conversion test", gpu.device.Get());
    Require(pass.IsInit(), "conversion pipeline");
    Conversion(gpu, pass, kProfileHdr10);
    Conversion(gpu, pass, kProfileScRgb);
    Swapchain(gpu);
    std::puts("PASS: HDR conversion GPU checks");
}
