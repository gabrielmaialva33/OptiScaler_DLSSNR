#include "pch.h"

#include "DlssNr_ZeroGuides.h"

#include <Logger.h>

namespace DlssNr
{

namespace
{

constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;

// Where the pass reads a guide from, and where a guide sits while its clear is still owed.
constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kClearState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

ID3D12Resource* CreateGuide(ID3D12Device* device, DXGI_FORMAT format, uint32_t width, uint32_t height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // Cleared through a UAV and read as an SRV; it is never a render target and never a copy source.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kClearState, nullptr,
                                               IID_PPV_ARGS(&resource))))
    {
        return nullptr;
    }

    return resource;
}

void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

} // namespace

ZeroGuides::~ZeroGuides() { Release(); }

void ZeroGuides::Release()
{
    // Callers hold these only for as long as they hold the device that made them, and the one
    // caller today releases them behind a drain it has already proved. Nothing is freed here that
    // the GPU could still be reading without that drain having happened first.
    if (_depth != nullptr)
    {
        _depth->Release();
        _depth = nullptr;
    }

    if (_motion != nullptr)
    {
        _motion->Release();
        _motion = nullptr;
    }

    if (_clearHeapCpu != nullptr)
    {
        _clearHeapCpu->Release();
        _clearHeapCpu = nullptr;
    }

    if (_clearHeapGpu != nullptr)
    {
        _clearHeapGpu->Release();
        _clearHeapGpu = nullptr;
    }

    _width = 0;
    _height = 0;
    _descriptorStride = 0;
    _clearOwed = false;
    _clearRecorded = false;
}

bool ZeroGuides::Ensure(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (device == nullptr || width == 0 || height == 0)
        return false;

    if (_depth != nullptr && _motion != nullptr && _width == width && _height == height)
        return true;

    Release();
    return _Allocate(device, width, height);
}

bool ZeroGuides::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)
{
    _depth = CreateGuide(device, kDepthFormat, width, height);
    _motion = CreateGuide(device, kMotionFormat, width, height);

    if (_depth == nullptr || _motion == nullptr)
    {
        LOG_ERROR("DLSS-NR zero guides: {}x{} allocation failed", width, height);
        Release();
        return false;
    }

    // Two descriptors for each view, in two heaps. ClearUnorderedAccessViewFloat reads the CPU
    // handle from a heap that must not be shader visible and the GPU handle from one that must be,
    // and must be bound when the clear is recorded. One heap cannot be both.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;

    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_clearHeapCpu))))
    {
        LOG_ERROR("DLSS-NR zero guides: CPU descriptor heap failed");
        Release();
        return false;
    }

    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_clearHeapGpu))))
    {
        LOG_ERROR("DLSS-NR zero guides: shader-visible descriptor heap failed");
        Release();
        return false;
    }

    _descriptorStride = device->GetDescriptorHandleIncrementSize(heapDesc.Type);

    ID3D12Resource* const guides[2] = { _depth, _motion };

    for (uint32_t i = 0; i < 2; ++i)
    {
        auto cpu = _clearHeapCpu->GetCPUDescriptorHandleForHeapStart();
        auto visible = _clearHeapGpu->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(i) * _descriptorStride;
        visible.ptr += static_cast<SIZE_T>(i) * _descriptorStride;

        // Explicitly typed. A view that takes its format from a typeless resource is a different
        // view depending on who reads it, and these are cleared by one caller and read by another.
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Format = guides[i]->GetDesc().Format;
        device->CreateUnorderedAccessView(guides[i], nullptr, &view, cpu);
        device->CopyDescriptorsSimple(1, visible, cpu, heapDesc.Type);
    }

    _width = width;
    _height = height;
    _clearOwed = true;
    _clearRecorded = false;

    LOG_INFO("DLSS-NR zero guides: {}x{} allocated, clear owed", width, height);
    return true;
}

bool ZeroGuides::RecordClear(ID3D12GraphicsCommandList* cmdList)
{
    // Before the owed check, not after. With nothing allocated there is nothing owed either, and
    // answering success to a caller that just asked us to initialize guides we do not have reads as
    // "they are ready" at the one call site that matters.
    if (cmdList == nullptr || _depth == nullptr || _motion == nullptr || _clearHeapCpu == nullptr ||
        _clearHeapGpu == nullptr)
    {
        return false;
    }

    if (!_clearOwed)
        return true;

    // Recording it twice onto two different lists would leave the second clearing a resource the
    // first already moved out of UNORDERED_ACCESS. One outstanding recording at a time.
    if (_clearRecorded)
        return true;

    ID3D12DescriptorHeap* heaps[] = { _clearHeapGpu };
    cmdList->SetDescriptorHeaps(1, heaps);

    ID3D12Resource* const guides[2] = { _depth, _motion };
    const float zero[4] {};

    for (uint32_t i = 0; i < 2; ++i)
    {
        auto cpu = _clearHeapCpu->GetCPUDescriptorHandleForHeapStart();
        auto gpu = _clearHeapGpu->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(i) * _descriptorStride;
        gpu.ptr += static_cast<UINT64>(i) * _descriptorStride;

        cmdList->ClearUnorderedAccessViewFloat(gpu, cpu, guides[i], zero, 0, nullptr);
        Transition(cmdList, guides[i], kClearState, kReadState);
    }

    _clearRecorded = true;
    return true;
}

void ZeroGuides::ConfirmExecuted()
{
    if (!_clearRecorded)
        return;

    _clearOwed = false;
    _clearRecorded = false;
    LOG_INFO("DLSS-NR zero guides: {}x{} cleared to zero, once", _width, _height);
}

void ZeroGuides::AbandonRecording()
{
    // The list carrying the clear never ran. The guides are still in UNORDERED_ACCESS holding
    // whatever the allocator gave them, and the transitions recorded alongside never happened
    // either, so the next attempt may record the identical sequence against the identical state.
    if (!_clearRecorded)
        return;

    _clearRecorded = false;
    LOG_WARN("DLSS-NR zero guides: clear recording was abandoned, zeros are still owed");
}

} // namespace DlssNr
