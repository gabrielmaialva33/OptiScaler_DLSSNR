// The native D3D11 guide transport's API sequence on a real D3D11 device and a real D3D12 device under Proton (DXVK and
// vkd3d-proton, as a game gets them): game-like depth and motion textures cross into shared textures the way upstream's
// Dx11WithDx12 copy path makes them, D3D11 signals a shared fence and the D3D12 queue waits on it, and a D3D12 list
// copies them into clones made the way the present guide ring makes its clones (CreatePresentGuideClone), with the
// barriers CaptureGuides records (the shared texture COMMON -> COPY_SOURCE -> COMMON). Each clone is read back and
// compared with what D3D11 wrote. No OptiScaler, no NGX, no game.
//
// Proton does not pass stdout through: the result goes to dx11-guides-transport.log and result.json in the working
// directory.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
FILE* g_log = nullptr;

void Log(const std::string& line)
{
    if (g_log != nullptr)
    {
        std::fprintf(g_log, "%s\n", line.c_str());
        std::fflush(g_log);
    }
}

std::string Hex(HRESULT hr)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

template <typename T> void Release(T*& object)
{
    if (object != nullptr)
        object->Release();
    object = nullptr;
}

struct Case
{
    const char* name;
    DXGI_FORMAT format;
    UINT bind;
    bool depth;       // filled by a depth clear, not by UpdateSubresource
    bool ntHandle;    // DontUseNTShared=false: an NT handle; else the legacy one upstream uses by default
    UINT bytes;       // per texel
    bool ownCopy;     // the transport's own copy (Dx11Guides::DepthCopy OwnCopy): subresource 0 into a
                      // shader-resource-only texture; else upstream's copy path (the source's own bind flags)
    bool expectCross; // false: a recorded hazard, reported and not required (upstream's copy of a depth-stencil)
};

// What the transport sends across, for formats a game's depth and motion are made in. Typeless depth-stencil formats
// with a stencil plane (R24G8, R32G8X24) go through upstream's DepthTransfer_Dx11 compute pass instead, which this
// harness does not run.
const Case kCases[] = {
    // A depth bound as a depth-stencil, as the transport carries it: its own shader-resource-only copy.
    { "depth R32_TYPELESS depth-stencil, own copy, legacy handle", DXGI_FORMAT_R32_TYPELESS,
      D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, true, false, 4, true, true },
    { "depth R32_TYPELESS depth-stencil, own copy, NT handle", DXGI_FORMAT_R32_TYPELESS,
      D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, true, true, 4, true, true },
    { "depth R16_TYPELESS depth-stencil, own copy, legacy handle", DXGI_FORMAT_R16_TYPELESS,
      D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, true, false, 2, true, true },
    // A depth that is not a depth-stencil, and every motion format, through upstream's copy path.
    { "depth R32_FLOAT render target, upstream copy, legacy handle", DXGI_FORMAT_R32_FLOAT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, false, false, 4, false, true },
    { "motion R16G16_FLOAT, upstream copy, legacy handle", DXGI_FORMAT_R16G16_FLOAT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, false, false, 4, false, true },
    { "motion R16G16_FLOAT, upstream copy, NT handle", DXGI_FORMAT_R16G16_FLOAT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, false, true, 4, false, true },
    { "motion R32G32_FLOAT, upstream copy, legacy handle", DXGI_FORMAT_R32G32_FLOAT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, false, false, 8, false, true },
    { "motion R16G16B16A16_FLOAT, upstream copy, legacy handle", DXGI_FORMAT_R16G16B16A16_FLOAT,
      D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, false, false, 8, false,
      true },
    // The hazard the own copy exists for: upstream's copy of a depth-stencil keeps the bind, and D3D12 never sees
    // D3D11's copy into it, fence or CPU wait, unless D3D11 accesses it again first (GE-Proton11-7, 2026-10-15). Run
    // in every Mode, with each frame's first texel logged.
    { "depth R32_TYPELESS depth-stencil, upstream copy, legacy handle (hazard)", DXGI_FORMAT_R32_TYPELESS,
      D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, true, false, 4, false, false },
};

// An odd size, so a row pitch that is not the width shows.
constexpr UINT kWidth = 61;
constexpr UINT kHeight = 37;

struct Devices
{
    ID3D11Device5* d3d11 = nullptr;
    ID3D11DeviceContext4* context = nullptr;
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D11Fence* fence11 = nullptr;
    ID3D12Fence* fence12 = nullptr; // the same fence, opened on D3D12
    ID3D12Fence* done = nullptr;    // the D3D12 side's own completion
    UINT64 sharedValue = 0;
    UINT64 doneValue = 0;
};

// Iteration i's contents. Depth: one value per iteration (a clear). Otherwise every byte different from its
// neighbours, from zero and from the other iterations', so a stale, cleared or shifted copy cannot match.
float DepthValue(int iteration) { return 0.125f + 0.0625f * static_cast<float>(iteration); }

// A 16-bit depth's value: an exact UNORM code, so the clear converts without rounding.
float Depth16Value(int iteration) { return static_cast<float>(4096 * (iteration + 1)) / 65535.0f; }

std::vector<uint8_t> Pattern(const Case& c, int iteration)
{
    std::vector<uint8_t> bytes(static_cast<size_t>(kWidth) * kHeight * c.bytes);
    if (c.depth && c.bytes == 2)
    {
        const uint16_t value = static_cast<uint16_t>(4096 * (iteration + 1));
        for (size_t i = 0; i < bytes.size(); i += 2)
            std::memcpy(&bytes[i], &value, 2);
        return bytes;
    }
    if (c.depth)
    {
        const float value = DepthValue(iteration);
        for (size_t i = 0; i < bytes.size(); i += 4)
            std::memcpy(&bytes[i], &value, 4);
        return bytes;
    }
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 37 + 11 + static_cast<size_t>(iteration) * 53) & 0x7F) | 0x01;
    return bytes;
}

constexpr int kIterations = 6;

// What happens on D3D11 between its copy into the shared texture and the D3D12 read. FenceOnly is the transport's
// order and the one the required cases run; the others only take the recorded hazard apart, one change each.
enum class Mode
{
    FenceOnly,    // signal the shared fence and flush; the D3D12 queue waits on it (the production sequence)
    CpuWait,      // the same, then wait on the CPU for D3D11's work (an event query): no access to the shared texture
    TouchInBatch, // a D3D11 copy out of the shared texture, recorded before the signal: an access, no CPU wait
    ReadBack,     // after the signal, a D3D11 copy out of the shared texture, mapped: an access and a CPU wait
};

const char* ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::FenceOnly:
        return "fence only";
    case Mode::CpuWait:
        return "fence + D3D11 CPU wait, no access";
    case Mode::TouchInBatch:
        return "fence + D3D11 read of the shared texture before the signal, no wait";
    case Mode::ReadBack:
        return "fence + D3D11 read-back of the shared texture after the signal (access + CPU wait)";
    }
    return "?";
}

bool g_checkD3D11 = false; // --check-d3d11: the required cases in ReadBack instead of FenceOnly

// One case: the game's texture, the shared copy, its D3D12 opening and the ring's clone made once, as the transport
// keeps them, then kIterations frames of D3D11 writing new contents and D3D12 copying them across behind the fence.
// *texels: every frame's first texel as D3D12 read it, and what D3D11 wrote, so a lag (frame n reads n-1) is told from
// contents that never arrive.
bool CrossOne(Devices& d, const Case& c, Mode mode, std::string* why, std::string* texels)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = c.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = c.bind;

    ID3D11Texture2D* game = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11Texture2D* shared = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Query* query = nullptr;
    ID3D12Resource* opened = nullptr;
    ID3D12Resource* clone = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;

    const auto cleanup = [&]
    {
        Release(list);
        Release(allocator);
        Release(readback);
        Release(clone);
        Release(opened);
        Release(query);
        Release(staging);
        Release(shared);
        Release(dsv);
        Release(game);
        if (event != nullptr)
            CloseHandle(event);
    };

    HRESULT hr = d.d3d11->CreateTexture2D(&desc, nullptr, &game);
    if (FAILED(hr))
    {
        *why = "the game's texture could not be created: " + Hex(hr);
        cleanup();
        return false;
    }

    if (c.depth)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
        dsvDesc.Format = c.format == DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = d.d3d11->CreateDepthStencilView(game, &dsvDesc, &dsv);
        if (FAILED(hr))
        {
            *why = "no depth-stencil view: " + Hex(hr);
            cleanup();
            return false;
        }
    }

    // Upstream's shared copy (Dx11WithDx12::CopyTextureFrom11To12): the source's description, shared, default usage.
    // The transport's own (EnsureOwnDepth): one mip, a shader resource only.
    D3D11_TEXTURE2D_DESC sharedDesc = desc;
    if (c.ownCopy)
    {
        sharedDesc.MipLevels = 1;
        sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    }
    sharedDesc.MiscFlags =
        c.ntHandle ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : D3D11_RESOURCE_MISC_SHARED;
    hr = d.d3d11->CreateTexture2D(&sharedDesc, nullptr, &shared);
    if (FAILED(hr))
    {
        *why = "the shared texture could not be created: " + Hex(hr);
        cleanup();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.BindFlags = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (mode == Mode::TouchInBatch || mode == Mode::ReadBack)
        hr = d.d3d11->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (mode == Mode::CpuWait)
    {
        D3D11_QUERY_DESC queryDesc {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        hr = d.d3d11->CreateQuery(&queryDesc, &query);
    }
    if (FAILED(hr))
    {
        *why = "the staging texture or event query could not be created: " + Hex(hr);
        cleanup();
        return false;
    }

    HANDLE handle = nullptr;
    IDXGIResource1* dxgi = nullptr;
    hr = shared->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
    {
        hr = c.ntHandle ? dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &handle)
                        : dxgi->GetSharedHandle(&handle);
        Release(dxgi);
    }
    if (FAILED(hr) || handle == nullptr)
    {
        *why = "no shared handle: " + Hex(hr);
        cleanup();
        return false;
    }

    hr = d.d3d12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
    if (c.ntHandle)
        CloseHandle(handle);
    if (FAILED(hr) || opened == nullptr)
    {
        *why = "D3D12 could not open the shared texture: " + Hex(hr);
        cleanup();
        return false;
    }

    // The ring's clone (CreatePresentGuideClone): the source's description, a plain layout, no flag that would stop it
    // being read or let it decay, one mip, resting in NON_PIXEL_SHADER_RESOURCE.
    D3D12_RESOURCE_DESC cloneDesc = opened->GetDesc();
    cloneDesc.Alignment = 0;
    cloneDesc.MipLevels = 1;
    cloneDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cloneDesc.Flags &= ~(D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
    cloneDesc.DepthOrArraySize = 1;
    cloneDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr =
        d.d3d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cloneDesc,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&clone));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 total = 0;
    d.d3d12->GetCopyableFootprints(&cloneDesc, 0, 1, 0, &footprint, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = total;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (SUCCEEDED(hr))
        hr = d.d3d12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr))
        hr = d.d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr))
        hr = d.d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr))
        hr = list->Close();
    if (FAILED(hr) || event == nullptr)
    {
        *why = "D3D12 objects: " + Hex(hr);
        cleanup();
        return false;
    }

    // Every frame is tried, so a first-use effect is told from one that lasts.
    int crossedFrames = 0;
    std::string firstFailure;
    for (int iteration = 0; iteration < kIterations; ++iteration)
    {
        bool frameOk = true;
        const auto expected = Pattern(c, iteration);

        // The game's frame.
        if (c.depth)
            d.context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH,
                                             c.bytes == 2 ? Depth16Value(iteration) : DepthValue(iteration), 0);
        else
            d.context->UpdateSubresource(game, 0, nullptr, expected.data(), kWidth * c.bytes, 0);

        // The transport: the copy into the shared texture, then Dx11WithDx12::SyncDx11ToDx12. The game's own flush at
        // Present submits the signal in a game; the harness has no present, so it flushes.
        if (c.ownCopy)
            d.context->CopySubresourceRegion(shared, 0, 0, 0, 0, game, 0, nullptr);
        else
            d.context->CopyResource(shared, game);
        if (mode == Mode::TouchInBatch)
            d.context->CopyResource(staging, shared);
        const UINT64 value = ++d.sharedValue;
        d.context->Signal(d.fence11, value);
        d.context->Flush();

        if (mode == Mode::CpuWait)
        {
            d.context->End(query);
            BOOL done = FALSE;
            const ULONGLONG until = GetTickCount64() + 5000;
            while (d.context->GetData(query, &done, sizeof(done), 0) != S_OK || !done)
            {
                if (GetTickCount64() > until)
                    break;
                Sleep(0);
            }
        }
        else if (mode == Mode::ReadBack)
        {
            d.context->CopyResource(staging, shared);
            D3D11_MAPPED_SUBRESOURCE mapped {};
            if (SUCCEEDED(d.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                d.context->Unmap(staging, 0);
        }

        const auto barrier = [list](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            list->ResourceBarrier(1, &b);
        };

        allocator->Reset();
        list->Reset(allocator, nullptr);

        // CaptureGuides, with the shared texture arriving in COMMON.
        barrier(opened, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(clone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = clone;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = opened;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier(clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(opened, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        // Read the clone back.
        barrier(clone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION fromClone {};
        fromClone.pResource = clone;
        fromClone.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION toBuffer {};
        toBuffer.pResource = readback;
        toBuffer.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        toBuffer.PlacedFootprint = footprint;
        list->CopyTextureRegion(&toBuffer, 0, 0, 0, &fromClone, nullptr);
        barrier(clone, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->Close();

        // The transport's order: the queue waits for D3D11's copy, then runs the list.
        d.queue->Wait(d.fence12, value);
        ID3D12CommandList* lists[] = { list };
        d.queue->ExecuteCommandLists(1, lists);
        const UINT64 doneValue = ++d.doneValue;
        d.queue->Signal(d.done, doneValue);

        if (FAILED(d.done->SetEventOnCompletion(doneValue, event)) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        {
            firstFailure = "frame " + std::to_string(iteration) + ": the D3D12 list did not complete within 5 s";
            break;
        }

        void* mapped = nullptr;
        D3D12_RANGE range { 0, static_cast<SIZE_T>(total) };
        if (FAILED(readback->Map(0, &range, &mapped)))
        {
            firstFailure = "the read-back would not map";
            break;
        }

        const auto* bytes = static_cast<const uint8_t*>(mapped);
        {
            uint32_t got = 0, wanted = 0;
            std::memcpy(&got, bytes + footprint.Offset, c.bytes < 4 ? c.bytes : 4);
            std::memcpy(&wanted, expected.data(), c.bytes < 4 ? c.bytes : 4);
            char text[64];
            std::snprintf(text, sizeof(text), "%s0x%08X/0x%08X", iteration == 0 ? "" : " ", got, wanted);
            *texels += text;
        }
        for (UINT y = 0; y < kHeight && frameOk; ++y)
        {
            const uint8_t* row = bytes + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
            const uint8_t* want = expected.data() + static_cast<size_t>(y) * kWidth * c.bytes;
            if (std::memcmp(row, want, kWidth * c.bytes) != 0)
            {
                frameOk = false;
                uint32_t got = 0, wanted = 0;
                std::memcpy(&got, row, 4);
                std::memcpy(&wanted, want, 4);
                char text[128];
                std::snprintf(text, sizeof(text), "frame %d, row %u: first texel 0x%08X, D3D11 wrote 0x%08X", iteration,
                              y, got, wanted);
                if (firstFailure.empty())
                    firstFailure = text;
            }
        }
        D3D12_RANGE none { 0, 0 };
        readback->Unmap(0, &none);
        crossedFrames += frameOk ? 1 : 0;
    }

    ok = crossedFrames == kIterations;
    if (!ok)
        *why =
            std::to_string(crossedFrames) + " of " + std::to_string(kIterations) + " frames crossed; " + firstFailure;

    cleanup();
    return ok;
}

bool CreateDevices(Devices& d, std::string* why)
{
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device,
                                   nullptr, &context);
    if (FAILED(hr))
    {
        *why = "D3D11CreateDevice: " + Hex(hr);
        return false;
    }
    hr = device->QueryInterface(IID_PPV_ARGS(&d.d3d11));
    if (SUCCEEDED(hr))
        hr = context->QueryInterface(IID_PPV_ARGS(&d.context));
    Release(context);

    // The D3D12 device on the D3D11 device's adapter, as WithDx12::GetD3D12DeviceFromD3D11 makes it.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(hr))
        hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (SUCCEEDED(hr))
        hr = dxgiDevice->GetAdapter(&adapter);
    Release(dxgiDevice);
    Release(device);
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.d3d12));
    Release(adapter);
    if (FAILED(hr))
    {
        *why = "the devices: " + Hex(hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = d.d3d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d.queue));

    // Dx11WithDx12::EnsureSyncResourcesLocked: a shared D3D11 fence, opened on D3D12.
    HANDLE fenceHandle = nullptr;
    if (SUCCEEDED(hr))
        hr = d.d3d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d.fence11));
    if (SUCCEEDED(hr))
        hr = d.fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
    if (SUCCEEDED(hr))
        hr = d.d3d12->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&d.fence12));
    if (fenceHandle != nullptr)
        CloseHandle(fenceHandle);
    if (SUCCEEDED(hr))
        hr = d.d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.done));
    if (FAILED(hr))
    {
        *why = "the queue and fences: " + Hex(hr);
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        g_checkD3D11 = g_checkD3D11 || std::strcmp(argv[i], "--check-d3d11") == 0;

    g_log = std::fopen("dx11-guides-transport.log", "w");
    FILE* result = std::fopen("result.json", "w");

    Devices devices;
    std::string why;
    int crossed = 0, failed = 0;
    std::string first;

    if (!CreateDevices(devices, &why))
    {
        Log("FAIL: " + why);
        if (result != nullptr)
            std::fprintf(result, "{\"status\": \"FAIL\", \"reason\": \"%s\"}\n", why.c_str());
        return 1;
    }

    int cases = 0;
    for (const Case& c : kCases)
    {
        if (!c.expectCross)
        {
            // A recorded hazard: reported either way, required neither way, and taken apart: the same frames under
            // each mode, with every frame's first texel as read (D3D12) / written (D3D11).
            for (Mode mode : { Mode::FenceOnly, Mode::CpuWait, Mode::TouchInBatch, Mode::ReadBack })
            {
                why.clear();
                std::string texels;
                const bool ok = CrossOne(devices, c, mode, &why, &texels);
                Log(std::string("HAZARD ") + c.name + ", " + ModeName(mode) + ": " +
                    (ok ? "every frame crossed" : "does not cross (" + why + ")") +
                    "; first texels read/written: " + texels);
            }
            continue;
        }

        why.clear();
        std::string texels;
        const bool ok = CrossOne(devices, c, g_checkD3D11 ? Mode::ReadBack : Mode::FenceOnly, &why, &texels);

        ++cases;
        Log(std::string("CASE ") + c.name + ": " + (ok ? "every frame crossed byte-exact" : "FAIL (" + why + ")"));
        crossed += ok ? 1 : 0;
        failed += ok ? 0 : 1;
        if (!ok && first.empty())
            first = std::string(c.name) + ": " + why;
    }

    if (failed == 0)
        Log("DX11-GUIDES-TRANSPORT PASS: cases=" + std::to_string(cases) + " crossed=" + std::to_string(crossed));
    else
        Log("DX11-GUIDES-TRANSPORT FAIL: cases=" + std::to_string(cases) + " crossed=" + std::to_string(crossed));

    if (result != nullptr)
    {
        std::fprintf(result, "{\"status\": \"%s\", \"cases\": %d, \"crossed\": %d, \"first_failure\": \"%s\"}\n",
                     failed == 0 ? "PASS" : "FAIL", cases, crossed, first.c_str());
        std::fclose(result);
    }
    if (g_log != nullptr)
        std::fclose(g_log);

    Release(devices.done);
    Release(devices.fence12);
    Release(devices.fence11);
    Release(devices.queue);
    Release(devices.d3d12);
    Release(devices.context);
    Release(devices.d3d11);
    return failed == 0 ? 0 : 1;
}
