#include "OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using UINT = unsigned;
using HRESULT = int;
constexpr HRESULT S_OK = 0;
constexpr bool FAILED(HRESULT result) { return result < 0; }
#define LOG_ERROR(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define IID_PPV_ARGS(value) value
#define _countof(value) (sizeof(value) / sizeof((value)[0]))

constexpr int D3D12_FILTER_MIN_MAG_MIP_LINEAR = 1;
constexpr int D3D12_TEXTURE_ADDRESS_MODE_CLAMP = 1;
constexpr float D3D12_FLOAT32_MAX = 3.4e38f;
constexpr int D3D12_SHADER_VISIBILITY_ALL = 1;
constexpr int D3D12_HEAP_TYPE_UPLOAD = 1;
constexpr int D3D12_HEAP_FLAG_NONE = 0;
constexpr int D3D12_RESOURCE_STATE_GENERIC_READ = 1;
constexpr unsigned char DlssNr_cso[] = { 1 };

struct D3D12_STATIC_SAMPLER_DESC
{
    int Filter {}, AddressU {}, AddressV {}, AddressW {}, ShaderVisibility {};
    float MaxLOD {};
};
struct D3D12_RESOURCE_DESC
{
    size_t Width;
};
struct CD3DX12_RESOURCE_DESC
{
    static D3D12_RESOURCE_DESC Buffer(size_t size) { return { size }; }
};
struct CD3DX12_HEAP_PROPERTIES
{
    explicit CD3DX12_HEAP_PROPERTIES(int) {}
};
struct D3D12_RANGE
{
    size_t Begin, End;
};
struct D3D12_CONSTANT_BUFFER_VIEW_DESC
{
    uintptr_t BufferLocation {};
    UINT SizeInBytes {};
};
using ConstantBytes = std::array<unsigned char, sizeof(DlssNrConstants)>;

struct ID3D12Resource
{
    ConstantBytes bytes {};
    bool failMap = false, nullMap = false, mapped = false;
    unsigned maps = 0, unmaps = 0, releases = 0;
    HRESULT Map(UINT, const D3D12_RANGE* read, void** output)
    {
        assert(read && read->Begin == 0 && read->End == 0);
        assert(!mapped && !releases);
        ++maps;
        if (failMap)
            return -1;
        if (nullMap)
        {
            *output = nullptr;
            return S_OK;
        }
        mapped = true;
        *output = bytes.data();
        return S_OK;
    }
    void Unmap(UINT, const D3D12_RANGE*)
    {
        assert(mapped && !releases);
        mapped = false;
        ++unmaps;
    }
    uintptr_t GetGPUVirtualAddress() { return reinterpret_cast<uintptr_t>(this); }
    void Release()
    {
        assert(!mapped && releases == 0);
        ++releases;
    }
};

struct ID3D12Device
{
    int failCreateAt = -1, failMapAt = -1, nullMapAt = -1;
    bool failRoot = false, failPipeline = false, failHeaps = false;
    unsigned creates = 0, cbvWrites = 0, srvWrites = 0, uavWrites = 0;
    std::vector<std::unique_ptr<ID3D12Resource>> resources;
    std::array<D3D12_CONSTANT_BUFFER_VIEW_DESC, 48> cbvs {};
    std::array<std::array<ID3D12Resource*, 5>, 48> srvs {};
    std::array<std::array<ID3D12Resource*, 2>, 48> uavs {};
    HRESULT CreateCommittedResource(const CD3DX12_HEAP_PROPERTIES*, int, const D3D12_RESOURCE_DESC* desc, int, void*,
                                    ID3D12Resource** output)
    {
        const int index = static_cast<int>(creates++);
        assert(desc->Width == sizeof(DlssNrConstants));
        if (index == failCreateAt)
            return -1;
        auto resource = std::make_unique<ID3D12Resource>();
        resource->failMap = index == failMapAt;
        resource->nullMap = index == nullMapAt;
        *output = resource.get();
        resources.push_back(std::move(resource));
        return S_OK;
    }
    void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* view, unsigned handle)
    {
        assert(handle % 8 == 7 && view->SizeInBytes % 256 == 0);
        cbvs.at(handle / 8) = *view;
        ++cbvWrites;
    }
};

struct ID3D12DescriptorHeap
{
    ID3D12Device* device {};
    unsigned slot {};
};
struct FrameDescriptorHeap
{
    ID3D12DescriptorHeap heap;
    unsigned GetSrvCPU(unsigned i) { return heap.slot * 8 + i; }
    unsigned GetUavCPU(unsigned i) { return heap.slot * 8 + 5 + i; }
    unsigned GetCbvCPU(unsigned i) { return heap.slot * 8 + 7 + i; }
    unsigned GetTableGPUStart() { return heap.slot; }
    ID3D12DescriptorHeap* GetHeapCSU() { return &heap; }
};
struct ID3D12GraphicsCommandList
{
    ID3D12DescriptorHeap* heap = nullptr;
    std::vector<ConstantBytes> recorded;
    std::vector<std::array<UINT, 3>> groups;
    void SetDescriptorHeaps(size_t count, ID3D12DescriptorHeap** heaps)
    {
        assert(count == 1);
        heap = heaps[0];
    }
    void SetComputeRootSignature(void*) {}
    void SetPipelineState(void*) {}
    void SetComputeRootDescriptorTable(unsigned root, unsigned slot) { assert(root == 0 && slot == heap->slot); }
    void Dispatch(UINT x, UINT y, UINT z)
    {
        auto& view = heap->device->cbvs.at(heap->slot);
        auto* buffer = reinterpret_cast<ID3D12Resource*>(view.BufferLocation);
        assert(buffer && buffer->mapped && !buffer->releases);
        recorded.push_back(buffer->bytes);
        groups.push_back({ x, y, z });
    }
};
struct ID3D12CommandQueue;
namespace DlssNr::Detail
{
struct CoverageSample;
}
namespace DlssNr::Chain
{
class RecordingLease;
}

class Shader_Dx12
{
  protected:
    std::string _name;
    ID3D12Device* _device;
    bool _init = false;
    void* _rootSignature = nullptr;
    void* _pipelineState = nullptr;
    bool SetupRootSignature(ID3D12Device* device, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                            D3D12_STATIC_SAMPLER_DESC*)
    {
        return !device->failRoot;
    }
    bool CreateComputePipeline(ID3D12Device* device, void**, const void*, size_t, const char*)
    {
        return !device->failPipeline;
    }
    bool InitHeaps(ID3D12Device* device, FrameDescriptorHeap* heaps, size_t count)
    {
        if (device->failHeaps)
            return false;
        for (unsigned i = 0; i < count; ++i)
            heaps[i].heap = { device, i };
        return true;
    }
    void CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* resource, unsigned handle)
    {
        device->srvs.at(handle / 8).at(handle % 8) = resource;
        ++device->srvWrites;
    }
    void CreateUnorderedAccessView(ID3D12Device* device, ID3D12Resource* resource, unsigned handle, unsigned mip)
    {
        assert(mip == 0);
        device->uavs.at(handle / 8).at(handle % 8 - 5) = resource;
        ++device->uavWrites;
    }

  public:
    Shader_Dx12(std::string name, ID3D12Device* device) : _name(name), _device(device) {}
    bool IsInit() const { return _init; }
};
