#pragma once
#include <cstddef>
#include <cstdint>
using UINT = unsigned;
using UINT64 = uint64_t;
using HRESULT = long;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
#define FAILED(value) ((value) < 0)
#define SUCCEEDED(value) ((value) >= 0)
#define IID_PPV_ARGS(pointer) reinterpret_cast<void**>(pointer)
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT = 0;
constexpr int D3D12_QUERY_HEAP_TYPE_TIMESTAMP = 1;
constexpr int D3D12_QUERY_TYPE_TIMESTAMP = 1;
constexpr int D3D12_HEAP_TYPE_READBACK = 1;
constexpr int D3D12_RESOURCE_DIMENSION_BUFFER = 1;
constexpr int D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1;
constexpr int D3D12_HEAP_FLAG_NONE = 0;
constexpr int D3D12_RESOURCE_STATE_COPY_DEST = 1;
struct D3D12_QUERY_HEAP_DESC
{
    int Type = 0;
    UINT Count = 0;
    UINT NodeMask = 0;
};
struct D3D12_HEAP_PROPERTIES
{
    int Type = 0;
};
struct D3D12_RESOURCE_DESC
{
    int Dimension = 0;
    uint64_t Width = 0;
    UINT Height = 0;
    uint16_t DepthOrArraySize = 0, MipLevels = 0;
    struct
    {
        UINT Count = 0;
    } SampleDesc;
    int Layout = 0;
};
struct D3D12_RANGE
{
    size_t Begin = 0, End = 0;
};
struct ID3D12QueryHeap
{
    virtual ~ID3D12QueryHeap() = default;
    virtual void Release() = 0;
};
struct ID3D12Resource
{
    virtual ~ID3D12Resource() = default;
    virtual void Release() = 0;
    virtual HRESULT Map(UINT, const D3D12_RANGE*, void**) = 0;
    virtual void Unmap(UINT, const D3D12_RANGE*) = 0;
};
struct ID3D12Device
{
    virtual ~ID3D12Device() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual HRESULT CreateQueryHeap(const D3D12_QUERY_HEAP_DESC*, void**) = 0;
    virtual HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, int, const D3D12_RESOURCE_DESC*, int,
                                            const void*, void**) = 0;
};
struct ID3D12CommandList
{
};
struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    virtual ~ID3D12GraphicsCommandList() = default;
    virtual int GetType() = 0;
    virtual HRESULT GetDevice(void**) = 0;
    virtual void EndQuery(ID3D12QueryHeap*, int, UINT) = 0;
    virtual void ResolveQueryData(ID3D12QueryHeap*, int, UINT, UINT, ID3D12Resource*, uint64_t) = 0;
};
struct ID3D12CommandQueue
{
};
inline unsigned GetCurrentProcessId() { return 42; }
