#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        std::fflush(stdout);
        ExitProcess(1); // Do not unwind resources if GPU completion is unknown.
    }
}
void Hr(HRESULT result, const char* message) { Require(SUCCEEDED(result), message); }

#define LOG_ERROR(...) ((void) 0)
#include "tag_production.inc"
#include "camera.h"

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t serial = 0;

    Gpu()
    {
        ComPtr<ID3D12Debug> debug;
        bool nativeDebug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        if (nativeDebug)
            debug->EnableDebugLayer();
        std::printf("DEBUG_LAYER=%s\n", nativeDebug ? "active" : "unavailable");
        ComPtr<IDXGIFactory4> factory;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        for (UINT index = 0;; index++)
        {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc {};
            Hr(adapter->GetDesc1(&desc), "adapter description");
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                break;
        }
        Require(device != nullptr, "hardware D3D12 device");
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC desc {};
        Hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                     IID_PPV_ARGS(&commands)),
           "command list");
        Hr(commands->Close(), "initial close");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        Require(event != nullptr, "completion event");
    }
    ~Gpu() { CloseHandle(event); }
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
        Hr(queue->Signal(fence.Get(), ++serial), "fence signal");
        Hr(fence->SetEventOnCompletion(serial, event), "fence event");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU fence timeout");
        Hr(device->GetDeviceRemovedReason(), "device removal");
        if (messages)
        {
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); i++)
            {
                SIZE_T size = 0;
                Hr(messages->GetMessage(i, nullptr, &size), "debug message size");
                std::vector<char> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Hr(messages->GetMessage(i, message, &size), "debug message");
                Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
            }
            messages->ClearStoredMessages();
        }
    }
    ComPtr<ID3D12Resource> Resource(D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = heap;
        ComPtr<ID3D12Resource> resource;
        Hr(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&resource)),
           "resource creation");
        return resource;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return Resource(desc, heap, state);
    }
};

uint32_t Pixel(UINT x, UINT y, UINT seed) { return (x * 2654435761u) ^ (y * 2246822519u) ^ (seed * 3266489917u); }

void TestTags(Gpu& gpu)
{
    Consumer consumer;
    State::Instance().currentFG = &consumer;
    Sl_Inputs inputs;
    unsigned cases = 0;
    for (UINT width : { 257u, 509u })
    {
        const UINT height = width == 257 ? 131 : 263;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        auto texture = gpu.Resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = gpu.Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* mapped = nullptr;
        D3D12_RANGE empty {};
        Hr(upload->Map(0, &empty, reinterpret_cast<void**>(&mapped)), "upload mapping");
        for (UINT y = 0; y < height; y++)
            for (UINT x = 0; x < width; x++)
                reinterpret_cast<uint32_t*>(mapped + y * footprint.Footprint.RowPitch)[x] = Pixel(x, y, width);
        upload->Unmap(0, nullptr);
        gpu.Begin();
        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION native {};
        native.pResource = texture.Get();
        native.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        gpu.commands->CopyTextureRegion(&native, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        gpu.commands->ResourceBarrier(1, &barrier);
        gpu.Submit();

        const sl::BufferType types[] = { sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
                                         sl::kBufferTypeHUDLessColor, sl::kBufferTypeUIColorAndAlpha };
        for (UINT frame = 0; frame < 32; frame++)
        {
            const sl::Extent requested { 3 + frame, 7 + 2 * frame, 111 + frame % 5, 61 + frame % 7 };
            sl::Resource resource;
            resource.native = texture.Get();
            resource.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            sl::ResourceTag tag(&resource, types[frame % 4], sl::eValidUntilPresent, &requested);
            gpu.Begin();
            Require(inputs.reportResource(tag, gpu.commands.Get(), frame + 1), "tag accepted");
            const auto& got = consumer.captured;
            Require(got.resource == texture.Get() && got.cmdList == gpu.commands.Get(), "native tag pointers");
            Require(got.width == requested.width && got.height == requested.height, "tag dimensions");
            auto cropDesc = desc;
            cropDesc.Width = requested.width;
            cropDesc.Height = requested.height;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT crop {};
            gpu.device->GetCopyableFootprints(&cropDesc, 0, 1, 0, &crop, nullptr, nullptr, &bytes);
            auto readback = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION destination {};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = crop;
            D3D12_BOX box { got.left, got.top, 0, got.left + static_cast<UINT>(got.width), got.top + got.height, 1 };
            gpu.commands->CopyTextureRegion(&destination, 0, 0, 0, &native, &box);
            gpu.Submit();
            D3D12_RANGE read { 0, static_cast<SIZE_T>(bytes) };
            Hr(readback->Map(0, &read, reinterpret_cast<void**>(&mapped)), "readback mapping");
            for (UINT y = 0; y < requested.height; y++)
                for (UINT x = 0; x < requested.width; x++)
                    Require(reinterpret_cast<uint32_t*>(mapped + y * crop.Footprint.RowPitch)[x] ==
                                Pixel(requested.left + x, requested.top + y, width),
                            "crop pixels");
            readback->Unmap(0, &empty);
            cases++;
        }
        sl::Resource resource;
        resource.native = texture.Get();
        sl::ResourceTag whole(&resource, sl::kBufferTypeDepth, sl::eValidUntilPresent);
        Require(inputs.reportResource(whole, nullptr, 99), "whole texture accepted");
        Require(consumer.captured.left == 0 && consumer.captured.top == 0 && consumer.captured.width == width &&
                    consumer.captured.height == height,
                "whole texture fallback");
        sl::Extent invalid { height - 1, width - 1, 2, 2 };
        sl::ResourceTag outside(&resource, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &invalid);
        const auto calls = consumer.calls;
        Require(!inputs.reportResource(outside, nullptr, 100) && consumer.calls == calls, "invalid extent rejected");
    }
    std::printf("PASS: D3D12 tag crop readback cases=%u\n", cases);
    State::Instance().currentFG = nullptr;
}

int main()
{
    TestCamera();
    Gpu gpu;
    TestTags(gpu);
    std::puts("PASS: FG inputs D3D12 fixture");
}
