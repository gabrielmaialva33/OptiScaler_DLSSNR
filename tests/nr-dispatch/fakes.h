#include "OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using UINT = unsigned;
using UINT64 = unsigned long long;
using UINT16 = unsigned short;
using HRESULT = int;

// Only the members the views are built from. Distinct values matter more than real ones: the point is
// that two resources which differ in any of these must not share a descriptor.
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
};
enum D3D12_RESOURCE_DIMENSION
{
    D3D12_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
};
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
    UINT Height {};
    UINT16 DepthOrArraySize {};
    UINT16 MipLevels {};
    D3D12_RESOURCE_DIMENSION Dimension {};
    DXGI_FORMAT Format {};
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
    // What this resource says it is. The default is a plain 2D texture, so a test that does not care
    // about shape gets one that is valid to build a view from; a test that does care sets it.
    D3D12_RESOURCE_DESC desc { 1920, 1080, 1, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                               DXGI_FORMAT_R16G16B16A16_FLOAT };
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }

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

// What a descriptor slot holds after a view was written into it.
//
// The slot is compared, not counted. A cache that skips a write it should have made leaves the
// previous incarnation's record sitting here, and only comparing contents can see that; call counts
// alone would report the skip as the saving it was supposed to be.
struct ViewRecord
{
    ID3D12Resource* res = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned mip = ~0u;
    bool translateTypeless = false;

    bool operator==(const ViewRecord& o) const
    {
        return res == o.res && format == o.format && mip == o.mip && translateTypeless == o.translateTypeless;
    }
};

struct ID3D12Device
{
    int failCreateAt = -1, failMapAt = -1, nullMapAt = -1;
    bool failRoot = false, failPipeline = false, failHeaps = false;
    unsigned creates = 0, cbvWrites = 0, srvWrites = 0, uavWrites = 0;
    std::vector<std::unique_ptr<ID3D12Resource>> resources;
    std::array<D3D12_CONSTANT_BUFFER_VIEW_DESC, 48> cbvs {};
    std::array<std::array<ViewRecord, 5>, 48> srvs {};
    std::array<std::array<ViewRecord, 2>, 48> uavs {};
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
    // These mirror Shader_Dx12's real signatures, translateTypeless included (97c94b05). The defaults
    // are the real ones, so a production call that stops passing an argument still compiles here and
    // is recorded as the default it actually got -- which is what the descriptor would then hold.
    void CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* resource, unsigned handle,
                                  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, bool translateTypeless = true)
    {
        device->srvs.at(handle / 8).at(handle % 8) = { resource, format, 0, translateTypeless };
        ++device->srvWrites;
    }
    void CreateUnorderedAccessView(ID3D12Device* device, ID3D12Resource* resource, unsigned handle, unsigned mip,
                                   bool translateTypeless = true)
    {
        device->uavs.at(handle / 8).at(handle % 8 - 5) = { resource, DXGI_FORMAT_UNKNOWN, mip, translateTypeless };
        ++device->uavWrites;
    }

  public:
    Shader_Dx12(std::string name, ID3D12Device* device) : _name(name), _device(device) {}
    bool IsInit() const { return _init; }
};
