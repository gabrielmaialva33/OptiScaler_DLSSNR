// Strict host fakes for the zero-guide provider. Every D3D12 rule this code depends on is asserted
// here rather than assumed, because the rule it depends on most -- which heap a Clear's two handles
// may come from -- is invisible on a real driver until the validation layer is on.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using UINT = unsigned;
using UINT64 = unsigned long long;
using SIZE_T = size_t;
using HRESULT = int;
constexpr HRESULT S_OK = 0;
constexpr bool FAILED(HRESULT r) { return r < 0; }
#define IID_PPV_ARGS(x) x
#define LOG_ERROR(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R16G16_FLOAT = 34,
};
enum D3D12_RESOURCE_DIMENSION
{
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3
};
enum D3D12_HEAP_TYPE
{
    D3D12_HEAP_TYPE_DEFAULT = 1
};
enum D3D12_TEXTURE_LAYOUT
{
    D3D12_TEXTURE_LAYOUT_UNKNOWN = 0
};
enum D3D12_RESOURCE_FLAGS
{
    D3D12_RESOURCE_FLAG_NONE = 0,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 8,
};
enum D3D12_HEAP_FLAGS
{
    D3D12_HEAP_FLAG_NONE = 0
};
enum D3D12_DESCRIPTOR_HEAP_TYPE
{
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV = 0
};
enum D3D12_DESCRIPTOR_HEAP_FLAGS
{
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE = 0,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE = 1,
};
enum D3D12_UAV_DIMENSION
{
    D3D12_UAV_DIMENSION_TEXTURE2D = 4
};
enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_COMMON = 0,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 8,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 64,
};
enum D3D12_RESOURCE_BARRIER_TYPE
{
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0
};
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xffffffff;

struct D3D12_HEAP_PROPERTIES
{
    D3D12_HEAP_TYPE Type {};
};
struct DXGI_SAMPLE_DESC
{
    UINT Count {}, Quality {};
};
struct D3D12_RESOURCE_DESC
{
    D3D12_RESOURCE_DIMENSION Dimension {};
    UINT64 Alignment {}, Width {};
    UINT Height {};
    unsigned short DepthOrArraySize {}, MipLevels {};
    DXGI_FORMAT Format {};
    DXGI_SAMPLE_DESC SampleDesc {};
    D3D12_TEXTURE_LAYOUT Layout {};
    D3D12_RESOURCE_FLAGS Flags {};
};
struct D3D12_UNORDERED_ACCESS_VIEW_DESC
{
    DXGI_FORMAT Format {};
    D3D12_UAV_DIMENSION ViewDimension {};
};
struct D3D12_DESCRIPTOR_HEAP_DESC
{
    D3D12_DESCRIPTOR_HEAP_TYPE Type {};
    UINT NumDescriptors {};
    D3D12_DESCRIPTOR_HEAP_FLAGS Flags {};
    UINT NodeMask {};
};
struct D3D12_CPU_DESCRIPTOR_HANDLE
{
    SIZE_T ptr {};
};
struct D3D12_GPU_DESCRIPTOR_HANDLE
{
    UINT64 ptr {};
};
struct D3D12_RESOURCE_BARRIER
{
    D3D12_RESOURCE_BARRIER_TYPE Type {};
    UINT Flags {};
    struct
    {
        struct ID3D12Resource* pResource {};
        UINT Subresource {};
        D3D12_RESOURCE_STATES StateBefore {}, StateAfter {};
    } Transition;
};

struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc {};
    D3D12_RESOURCE_STATES state {};
    unsigned refs = 1;
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
    void Release()
    {
        assert(refs > 0 && "zero guide released twice");
        --refs;
    }
};

// A descriptor slot remembers which heap it belongs to, so a Clear can be checked against the rule
// instead of only against itself.
struct Slot
{
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool shaderVisible = false;
    bool written = false;
};

struct ID3D12DescriptorHeap
{
    D3D12_DESCRIPTOR_HEAP_DESC desc {};
    std::vector<Slot> slots;
    SIZE_T base = 0;
    unsigned refs = 1;
    bool ShaderVisible() const { return (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStart() { return { base }; }

    // A shader-visible heap is the only kind with a GPU handle at all. Asking a CPU-only heap for
    // one is the mistake this catches, not a value to invent.
    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStart()
    {
        assert(ShaderVisible() && "asked a non-shader-visible heap for a GPU handle");
        return { static_cast<UINT64>(base) };
    }
    void Release()
    {
        assert(refs > 0 && "descriptor heap released twice");
        --refs;
    }
};

struct ID3D12Device
{
    int failResourceAt = -1, failHeapAt = -1;
    unsigned resources = 0, heaps = 0;
    std::vector<std::unique_ptr<ID3D12Resource>> ownedResources;
    std::vector<std::unique_ptr<ID3D12DescriptorHeap>> ownedHeaps;

    // Every heap gets its own base so a handle can be traced back to the heap it came from, and the
    // stride is deliberately not 1: an implementation that forgot to scale by it would land two
    // descriptors on the same slot and the second guide would silently share the first's view.
    static constexpr SIZE_T kHeapSpan = 0x10000;
    static constexpr UINT kStride = 32;

    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC* d,
                                    D3D12_RESOURCE_STATES initial, void*, ID3D12Resource** out)
    {
        assert(heap->Type == D3D12_HEAP_TYPE_DEFAULT);
        assert(d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->MipLevels == 1);
        assert(d->DepthOrArraySize == 1 && d->SampleDesc.Count == 1);
        // A texture cleared through a UAV must say so at creation.
        assert((d->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        if (static_cast<int>(resources++) == failResourceAt)
            return -1;
        auto r = std::make_unique<ID3D12Resource>();
        r->desc = *d;
        r->state = initial;
        *out = r.get();
        ownedResources.push_back(std::move(r));
        return S_OK;
    }

    HRESULT CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, ID3D12DescriptorHeap** out)
    {
        assert(d->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && d->NumDescriptors > 0);
        if (static_cast<int>(heaps++) == failHeapAt)
            return -1;
        auto h = std::make_unique<ID3D12DescriptorHeap>();
        h->desc = *d;
        h->slots.resize(d->NumDescriptors);
        h->base = (ownedHeaps.size() + 1) * kHeapSpan;
        *out = h.get();
        ownedHeaps.push_back(std::move(h));
        return S_OK;
    }

    UINT GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE) const { return kStride; }

    ID3D12DescriptorHeap* HeapOf(SIZE_T ptr, UINT& index) const
    {
        for (size_t i = 0; i < ownedHeaps.size(); ++i)
        {
            const SIZE_T base = (i + 1) * kHeapSpan;
            if (ptr >= base && ptr < base + ownedHeaps[i]->slots.size() * kStride)
            {
                const SIZE_T offset = ptr - base;
                assert(offset % kStride == 0 && "descriptor handle is not on a slot boundary");
                index = static_cast<UINT>(offset / kStride);
                return ownedHeaps[i].get();
            }
        }
        return nullptr;
    }

    SIZE_T BaseOf(const ID3D12DescriptorHeap* heap) const
    {
        for (size_t i = 0; i < ownedHeaps.size(); ++i)
            if (ownedHeaps[i].get() == heap)
                return (i + 1) * kHeapSpan;
        assert(false && "unknown heap");
        return 0;
    }

    void CreateUnorderedAccessView(ID3D12Resource* resource, void* counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC* v,
                                   D3D12_CPU_DESCRIPTOR_HANDLE where)
    {
        assert(counter == nullptr);
        assert(v != nullptr && v->ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2D);
        // An untyped view over a guide would be read differently by the clear and by the model.
        assert(v->Format != DXGI_FORMAT_UNKNOWN && v->Format == resource->desc.Format);
        UINT index = 0;
        auto* heap = HeapOf(where.ptr, index);
        assert(heap != nullptr && "UAV written outside any heap");
        heap->slots.at(index) = { resource, v->Format, heap->ShaderVisible(), true };
    }

    void CopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
                               D3D12_DESCRIPTOR_HEAP_TYPE)
    {
        assert(count == 1);
        UINT di = 0, si = 0;
        auto* d = HeapOf(dst.ptr, di);
        auto* s = HeapOf(src.ptr, si);
        assert(d != nullptr && s != nullptr);
        assert(s->slots.at(si).written && "copying a descriptor that was never created");
        auto slot = s->slots.at(si);
        slot.shaderVisible = d->ShaderVisible();
        d->slots.at(di) = slot;
    }
};

struct ClearRecord
{
    ID3D12Resource* resource = nullptr;
    float value[4] {};
    bool cpuHandleWasShaderVisible = false;
    bool gpuHandleWasShaderVisible = false;
    bool heapWasBound = false;
    bool handlesNameSameResource = false;
};

struct ID3D12GraphicsCommandList
{
    ID3D12Device* device = nullptr;
    ID3D12DescriptorHeap* bound = nullptr;
    std::vector<ClearRecord> clears;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;

    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap** heaps)
    {
        assert(count == 1 && heaps[0] != nullptr);
        // Only a shader-visible heap can be bound.
        assert(heaps[0]->ShaderVisible() && "bound a heap that is not shader visible");
        bound = heaps[0];
    }

    void ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                       ID3D12Resource* resource, const float values[4], UINT rects, const void* rect)
    {
        assert(rects == 0 && rect == nullptr);
        assert(resource != nullptr);
        // The resource must be in UNORDERED_ACCESS to be cleared through one.
        assert(resource->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && "clear on a resource not in UAV state");

        UINT ci = 0, gi = 0;
        auto* cpuHeap = device->HeapOf(cpu.ptr, ci);
        auto* gpuHeap = device->HeapOf(static_cast<SIZE_T>(gpu.ptr), gi);
        assert(cpuHeap != nullptr && gpuHeap != nullptr);
        assert(cpuHeap->slots.at(ci).written && gpuHeap->slots.at(gi).written);

        ClearRecord record;
        record.resource = resource;
        std::memcpy(record.value, values, sizeof(record.value));
        record.cpuHandleWasShaderVisible = cpuHeap->ShaderVisible();
        record.gpuHandleWasShaderVisible = gpuHeap->ShaderVisible();
        record.heapWasBound = bound == gpuHeap;
        record.handlesNameSameResource =
            cpuHeap->slots.at(ci).resource == resource && gpuHeap->slots.at(gi).resource == resource;
        clears.push_back(record);
    }

    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* list)
    {
        assert(count == 1);
        const auto& b = list[0];
        assert(b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
        assert(b.Transition.pResource != nullptr);
        // A transition that misstates where the resource is now is the defect this catches.
        assert(b.Transition.pResource->state == b.Transition.StateBefore && "barrier StateBefore does not match");
        b.Transition.pResource->state = b.Transition.StateAfter;
        barriers.push_back(b);
    }
};
