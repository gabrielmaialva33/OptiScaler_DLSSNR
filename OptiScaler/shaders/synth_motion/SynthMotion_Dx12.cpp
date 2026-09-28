#include "pch.h"

#include "SynthMotion_Dx12.h"

#include <State.h>
#include <d3dx/d3dx12.h>

#include "precompile/SynthMotion_ClearFloat_Shader.h"
#include "precompile/SynthMotion_ClearUint_Shader.h"
#include "precompile/SynthMotion_Expand_Shader.h"
#include "precompile/SynthMotion_Filter_Shader.h"
#include "precompile/SynthMotion_LuminancePyramid_Shader.h"
#include "precompile/SynthMotion_PrepareLuma_Shader.h"
#include "precompile/SynthMotion_Scale_Shader.h"
#include "precompile/SynthMotion_ScdDivergence_Shader.h"
#include "precompile/SynthMotion_ScdHistogram_Shader.h"
#include "precompile/SynthMotion_Search_Shader.h"

namespace SynthMotion
{
namespace
{
enum Pass : uint32_t
{
    PassPrepareLuma,
    PassPyramid,
    PassScdHistogram,
    PassScdDivergence,
    PassSearch,
    PassFilter,
    PassScale,
    PassExpand,
    PassClearUint,
    PassClearFloat,
    PassCount
};

struct Blob
{
    const unsigned char* data;
    size_t size;
};

const Blob kBlobs[PassCount] = {
    { SynthMotion_PrepareLuma_cso, sizeof(SynthMotion_PrepareLuma_cso) },
    { SynthMotion_LuminancePyramid_cso, sizeof(SynthMotion_LuminancePyramid_cso) },
    { SynthMotion_ScdHistogram_cso, sizeof(SynthMotion_ScdHistogram_cso) },
    { SynthMotion_ScdDivergence_cso, sizeof(SynthMotion_ScdDivergence_cso) },
    { SynthMotion_Search_cso, sizeof(SynthMotion_Search_cso) },
    { SynthMotion_Filter_cso, sizeof(SynthMotion_Filter_cso) },
    { SynthMotion_Scale_cso, sizeof(SynthMotion_Scale_cso) },
    { SynthMotion_Expand_cso, sizeof(SynthMotion_Expand_cso) },
    { SynthMotion_ClearUint_cso, sizeof(SynthMotion_ClearUint_cso) },
    { SynthMotion_ClearFloat_cso, sizeof(SynthMotion_ClearFloat_cso) },
};

constexpr uint32_t kBlock = 8;                    // FFX_OPTICALFLOW_BLOCK_SIZE: level-0 vectors per 8x8 pixels
constexpr uint32_t kHistogramWidth = 256 * 3 * 3; // FFX: HistogramBins * HistogramsPerDim^2
constexpr uint32_t kWarmupFrames = 5;             // FFX: FrameIndex() <= 5 counts as a scene change

// One descriptor table per dispatch: eight UAVs, unused ones null. The tables depend only on which
// luma pyramid is the current one, so there are two sets, written once per allocation.
constexpr uint32_t kTableSize = 8;
constexpr uint32_t kSlotsPerParity = 32;
constexpr uint32_t kColourSlots = 8; // the colour SRV is a ring: a slot comes back eight records later
constexpr uint32_t kColourBase = 2 * kSlotsPerParity * kTableSize;
constexpr uint32_t kHeapSize = kColourBase + kColourSlots;

constexpr uint32_t kSlotPrepare = 0;
constexpr uint32_t kSlotPyramid = 1;
constexpr uint32_t kSlotScdHistogram = 2;
constexpr uint32_t kSlotScdDivergence = 3;
constexpr uint32_t SlotLevel(uint32_t level, uint32_t which) { return 4 + level * 3 + which; } // 4..24
constexpr uint32_t kSlotExpand = 25;
constexpr uint32_t kSlotClear = 26; // 26..29
static_assert(kSlotClear + 4 <= kSlotsPerParity, "descriptor slots");
static_assert(SlotLevel(Estimator_Dx12::Levels - 1, 2) < kSlotExpand, "descriptor slots");

constexpr uint32_t kReadbackStride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

// FFX's cbOF and cbOF_SPD, unchanged, as root constants.
struct OfConstants
{
    int32_t width;
    int32_t height;
    uint32_t level;
    uint32_t levelCount;
    uint32_t frameIndex;
    uint32_t transferFunction;
    float minLuminance;
    float maxLuminance;
};

struct SpdConstants
{
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffsetX;
    uint32_t workGroupOffsetY;
    uint32_t numWorkGroupsPyramid;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ExpandConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t flowWidth;
    uint32_t flowHeight;
    uint32_t zero;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ClearConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t pad[6];
};

static_assert(sizeof(OfConstants) == 32 && sizeof(SpdConstants) == 32 && sizeof(ExpandConstants) == 32 &&
                  sizeof(ClearConstants) == 32,
              "root constants are eight dwords");

// The SRV format the colour is read through. A TYPELESS frame is read as its UNORM or FLOAT sibling;
// anything that is not an RGBA colour format is refused.
DXGI_FORMAT ColourViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return format;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

// Every internal texture lives in UNORDERED_ACCESS, so a global UAV barrier is all that orders one pass
// after the pass it reads from.
void UavBarrier(ID3D12GraphicsCommandList* cmdList)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    cmdList->ResourceBarrier(1, &barrier);
}

ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                              const wchar_t* name)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                               IID_PPV_ARGS(&resource))))
    {
        return nullptr;
    }

    resource->SetName(name);
    return resource;
}

template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}
} // namespace

Estimator_Dx12::~Estimator_Dx12() { Release(); }

bool Estimator_Dx12::_EnsureDevice(ID3D12Device* device)
{
    if (_device == device && _rootSignature != nullptr)
        return true;

    // A different device: everything made on the old one is parked, not freed.
    if (_device != nullptr && _device != device)
    {
        _Park();

        if (_rootSignature != nullptr)
            _parked.push_back(_rootSignature);
        for (auto* pipeline : _pipelines)
        {
            if (pipeline != nullptr)
                _parked.push_back(pipeline);
        }

        _rootSignature = nullptr;
        _pipelines.clear();
    }

    _device = device;

    // u0..u7 per dispatch, the colour at t0, FFX's two constant buffers as root constants.
    CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kTableSize, 0, 0,
                                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                           D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 parameters[4];
    parameters[0].InitAsDescriptorTable(1, &uavRange);
    parameters[1].InitAsDescriptorTable(1, &srvRange);
    parameters[2].InitAsConstants(8, 0);
    parameters[3].InitAsConstants(8, 1);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc {};
    rootDesc.Init_1_1(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ID3DBlob* signature = nullptr;
    ID3DBlob* error = nullptr;
    auto hr = D3D12SerializeVersionedRootSignature(&rootDesc, &signature, &error);
    if (SUCCEEDED(hr))
        hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                         IID_PPV_ARGS(&_rootSignature));
    SafeRelease(signature);
    SafeRelease(error);

    if (FAILED(hr) || _rootSignature == nullptr)
    {
        LOG_ERROR("synthesized motion: root signature failed ({:X}); no motion field for this session", (UINT) hr);
        return false;
    }

    _pipelines.assign(PassCount, nullptr);
    for (uint32_t pass = 0; pass < PassCount; ++pass)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = _rootSignature;
        psoDesc.CS = { kBlobs[pass].data, kBlobs[pass].size };

        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_pipelines[pass]));
        if (FAILED(hr))
        {
            LOG_ERROR("synthesized motion: pipeline {} failed ({:X}); no motion field for this session", pass,
                      (UINT) hr);
            return false;
        }
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Estimator_Dx12::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)
{
    // Ours, not the game's: resource heuristics watching the device skip it (the DLSS-NR exposure scan
    // would otherwise weigh the 3x1 scene-change textures as exposure candidates).
    ScopedInternalResourceCreation internalResources {};

    _width = width;
    _height = height;

    // FFX's sizes (ffx_opticalflow.cpp getInternalResourceDescriptions): the luma halves by shifting,
    // the 8x8-block flow halves rounding up.
    for (uint32_t level = 0; level < Levels; ++level)
    {
        _lumaWidth[level] = width >> level;
        _lumaHeight[level] = height >> level;
        _flowWidth[level] = level == 0 ? (width + kBlock - 1) / kBlock : (_flowWidth[level - 1] + 1) / 2;
        _flowHeight[level] = level == 0 ? (height + kBlock - 1) / kBlock : (_flowHeight[level - 1] + 1) / 2;
    }

    bool ok = true;
    for (uint32_t pyramid = 0; pyramid < 2 && ok; ++pyramid)
    {
        for (uint32_t level = 0; level < Levels && ok; ++level)
        {
            _luma[pyramid][level] = CreateTexture(device, _lumaWidth[level], _lumaHeight[level], DXGI_FORMAT_R8_UINT,
                                                  pyramid == 0 ? L"SynthMotion_Luma1" : L"SynthMotion_Luma2");
            _flow[pyramid][level] =
                CreateTexture(device, _flowWidth[level], _flowHeight[level], DXGI_FORMAT_R16G16_SINT,
                              pyramid == 0 ? L"SynthMotion_Flow1" : L"SynthMotion_Flow2");
            ok = _luma[pyramid][level] != nullptr && _flow[pyramid][level] != nullptr;
        }
    }

    if (ok)
    {
        _flowFinal =
            CreateTexture(device, _flowWidth[0], _flowHeight[0], DXGI_FORMAT_R16G16_SINT, L"SynthMotion_FlowFinal");
        _scdHistogram = CreateTexture(device, kHistogramWidth, 1, DXGI_FORMAT_R32_UINT, L"SynthMotion_ScdHistogram");
        _scdPreviousHistogram =
            CreateTexture(device, kHistogramWidth, 1, DXGI_FORMAT_R32_FLOAT, L"SynthMotion_ScdPreviousHistogram");
        _scdTemp = CreateTexture(device, 3, 1, DXGI_FORMAT_R32_UINT, L"SynthMotion_ScdTemp");
        _scdOutput = CreateTexture(device, 3, 1, DXGI_FORMAT_R32_UINT, L"SynthMotion_ScdOutput");
        _motion = CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, L"SynthMotion_Motion");

        ok = _flowFinal != nullptr && _scdHistogram != nullptr && _scdPreviousHistogram != nullptr &&
             _scdTemp != nullptr && _scdOutput != nullptr && _motion != nullptr;
    }

    // A heap per allocation, parked with the resources it describes: the tables are written once, and
    // recordings made before a resize may still read the old ones when descriptors are volatile.
    if (ok)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kHeapSize;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        ok = SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))) && _heap != nullptr;
        if (ok)
            _heap->SetName(L"SynthMotion_Descriptors");
    }

    if (ok)
    {
        const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(ReadbackSlots * kReadbackStride);
        ok = SUCCEEDED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readback)));

        void* mapped = nullptr;
        const D3D12_RANGE readRange { 0, ReadbackSlots * kReadbackStride };
        if (ok && SUCCEEDED(_readback->Map(0, &readRange, &mapped)) && mapped != nullptr)
        {
            // Zero until a slot has really been written: no history bit, no cut.
            memset(mapped, 0, ReadbackSlots * kReadbackStride);
            _readbackData = static_cast<const uint32_t*>(mapped);
            _readback->SetName(L"SynthMotion_ScdReadback");
        }
        else
        {
            ok = false;
        }
    }

    if (!ok)
    {
        LOG_ERROR("synthesized motion: could not allocate for {}x{}; no motion field for this session", width, height);
        _Park();
        return false;
    }

    memset(_tableWritten, 0, sizeof(_tableWritten));
    _motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _motionStateBefore = _motionState;
    _nextParity = 0;
    _confirmedFrameIndex = -1;
    _confirmedSinceReset = 0;
    _needReset = true;

    LOG_INFO("synthesized motion: {}x{} allocated, {} pyramid levels, level-0 flow {}x{}", width, height, Levels,
             _flowWidth[0], _flowHeight[0]);
    return true;
}

void Estimator_Dx12::_Park()
{
    auto park = [this](auto*& resource)
    {
        if (resource != nullptr)
        {
            _parked.push_back(resource);
            resource = nullptr;
        }
    };

    for (uint32_t pyramid = 0; pyramid < 2; ++pyramid)
    {
        for (uint32_t level = 0; level < Levels; ++level)
        {
            park(_luma[pyramid][level]);
            park(_flow[pyramid][level]);
        }
    }

    park(_flowFinal);
    park(_scdHistogram);
    park(_scdPreviousHistogram);
    park(_scdTemp);
    park(_scdOutput);
    park(_motion);

    if (_readback != nullptr && _readbackData != nullptr)
    {
        const D3D12_RANGE nothingWritten { 0, 0 };
        _readback->Unmap(0, &nothingWritten);
    }
    _readbackData = nullptr;
    park(_readback);
    park(_heap);

    memset(_tableWritten, 0, sizeof(_tableWritten));
    _colourSlot = 0;
    _width = 0;
    _height = 0;
    _pending = false;
    _needReset = true;
    _confirmedFrameIndex = -1;
    _confirmedSinceReset = 0;
    _nextParity = 0;
    _motionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _motionStateBefore = _motionState;
}

void Estimator_Dx12::_ReleaseParked()
{
    for (auto* object : _parked)
    {
        if (object != nullptr)
            object->Release();
    }
    _parked.clear();
}

void Estimator_Dx12::Release()
{
    _Park();
    _ReleaseParked();

    for (auto*& pipeline : _pipelines)
        SafeRelease(pipeline);
    _pipelines.clear();

    SafeRelease(_rootSignature);
    _device = nullptr;

    _ready = false;
    _sceneCut = false;
    _failed = false;
    _warnedSmall = false;
    _warnedFormat = false;
    _confirmedCount = 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE Estimator_Dx12::_Table(uint32_t parity, uint32_t slot,
                                                   std::initializer_list<ID3D12Resource*> uavs)
{
    const uint32_t first = (parity * kSlotsPerParity + slot) * kTableSize;

    if (!_tableWritten[parity][slot])
    {
        ScopedInternalResourceCreation internalResources {};

        auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(first) * _descriptorSize;

        // Unused entries hold a null view: every descriptor a table can reach is a valid one.
        D3D12_UNORDERED_ACCESS_VIEW_DESC nullDesc = {};
        nullDesc.Format = DXGI_FORMAT_R32_UINT;
        nullDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

        uint32_t index = 0;
        for (auto* resource : uavs)
        {
            _device->CreateUnorderedAccessView(resource, nullptr, nullptr, cpu);
            cpu.ptr += _descriptorSize;
            ++index;
        }

        for (; index < kTableSize; ++index)
        {
            _device->CreateUnorderedAccessView(nullptr, nullptr, &nullDesc, cpu);
            cpu.ptr += _descriptorSize;
        }

        _tableWritten[parity][slot] = true;
    }

    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(first) * _descriptorSize;
    return gpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE Estimator_Dx12::_ColourSrv(ID3D12Resource* colour, DXGI_FORMAT format)
{
    const uint32_t index = kColourBase + _colourSlot;
    _colourSlot = (_colourSlot + 1) % kColourSlots;

    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MostDetailedMip = 0;
    desc.Texture2D.MipLevels = 1;

    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * _descriptorSize;
    _device->CreateShaderResourceView(colour, &desc, cpu);

    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * _descriptorSize;
    return gpu;
}

void Estimator_Dx12::_Dispatch(ID3D12GraphicsCommandList* cmdList, uint32_t pass, D3D12_GPU_DESCRIPTOR_HANDLE table,
                               const void* constants, uint32_t x, uint32_t y, uint32_t z, const void* spdConstants)
{
    cmdList->SetPipelineState(_pipelines[pass]);
    cmdList->SetComputeRootDescriptorTable(0, table);
    cmdList->SetComputeRoot32BitConstants(2, 8, constants, 0);

    if (spdConstants != nullptr)
        cmdList->SetComputeRoot32BitConstants(3, 8, spdConstants, 0);

    cmdList->Dispatch(x > 0 ? x : 1, y > 0 ? y : 1, z > 0 ? z : 1);
    UavBarrier(cmdList);
}

bool Estimator_Dx12::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                            D3D12_RESOURCE_STATES colourState, bool reset)
{
    // A previous recording nobody confirmed or abandoned is treated as dropped.
    if (_pending)
        AbandonRecording();

    _ready = false;
    _sceneCut = false;

    if (_failed || device == nullptr || cmdList == nullptr || colour == nullptr)
        return false;

    const auto colourDesc = colour->GetDesc();
    const DXGI_FORMAT viewFormat = ColourViewFormat(colourDesc.Format);

    if (colourDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colourDesc.SampleDesc.Count != 1 ||
        viewFormat == DXGI_FORMAT_UNKNOWN)
    {
        if (!_warnedFormat)
        {
            _warnedFormat = true;
            LOG_WARN("synthesized motion: cannot read a colour of dimension {}, format {}, {} samples",
                     (UINT) colourDesc.Dimension, (UINT) colourDesc.Format, colourDesc.SampleDesc.Count);
        }
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(colourDesc.Width);
    const uint32_t height = colourDesc.Height;

    // Too small for FFX's seven levels: nothing is spent, and a later frame of a real size builds.
    if (width < MinWidth || height < MinHeight)
    {
        if (!_warnedSmall)
        {
            _warnedSmall = true;
            LOG_INFO("synthesized motion: {}x{} is below {}x{}; no field until the frame is larger", width, height,
                     MinWidth, MinHeight);
        }
        return true;
    }

    if (!_EnsureDevice(device))
    {
        _failed = true;
        return false;
    }

    if (_motion == nullptr || width != _width || height != _height)
    {
        _Park();

        if (!_Allocate(device, width, height))
        {
            _failed = true;
            return false;
        }
    }

    const bool doReset = reset || _needReset;
    const uint32_t frameIndex = doReset ? 0 : static_cast<uint32_t>(_confirmedFrameIndex + 1);

    // The GPU's scene-change bit, from a frame old enough that its copy has certainly landed.
    if (!doReset && _confirmedSinceReset >= ReadbackSlots && _readbackData != nullptr)
    {
        const uint32_t slot = static_cast<uint32_t>((_confirmedCount + 1) % ReadbackSlots);
        const uint32_t history = _readbackData[slot * (kReadbackStride / sizeof(uint32_t)) + 1];
        _sceneCut = (history & 1u) != 0;
    }

    const uint32_t parity = _nextParity;
    ID3D12Resource* const* current = _luma[parity];
    ID3D12Resource* const* previous = _luma[parity ^ 1u];

    ID3D12DescriptorHeap* heaps[] = { _heap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetComputeRootDescriptorTable(1, _ColourSrv(colour, viewFormat));

    _motionStateBefore = _motionState;
    Transition(cmdList, _motion, _motionState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    OfConstants of = {};
    of.width = static_cast<int32_t>(width);
    of.height = static_cast<int32_t>(height);
    of.levelCount = Levels;
    of.frameIndex = frameIndex;
    of.transferFunction = 0; // the callbacks saturate the colour; see synth_motion_callbacks.h
    of.minLuminance = 0.0f;
    of.maxLuminance = 1.0f;

    // FFX clears the scene-change state before its first use and on every reset.
    if (doReset)
    {
        struct Clear
        {
            ID3D12Resource* resource;
            uint32_t width;
            uint32_t pass;
        };
        const Clear clears[] = {
            { _scdHistogram, kHistogramWidth, PassClearUint },
            { _scdPreviousHistogram, kHistogramWidth, PassClearFloat },
            { _scdTemp, 3, PassClearUint },
            { _scdOutput, 3, PassClearUint },
        };

        for (uint32_t i = 0; i < _countof(clears); ++i)
        {
            ClearConstants clear = {};
            clear.width = clears[i].width;
            clear.height = 1;
            _Dispatch(cmdList, clears[i].pass, _Table(0, kSlotClear + i, { clears[i].resource }), &clear,
                      (clears[i].width + 63) / 64, 1, 1);
        }
    }

    // Luma of the current frame, full resolution. The colour is read through an SRV.
    const bool colourNeedsTransition = (colourState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
    if (colourNeedsTransition)
        Transition(cmdList, colour, colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _Dispatch(cmdList, PassPrepareLuma, _Table(parity, kSlotPrepare, { current[0] }), &of, ((width + 1) / 2 + 15) / 16,
              ((height + 1) / 2 + 15) / 16, 1);

    if (colourNeedsTransition)
        Transition(cmdList, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colourState);

    // Levels 1..6 of the current luma (FFX's single-pass downsampler, one 64x64 tile per group).
    {
        const uint32_t groupsX = (width - 1) / 64 + 1;
        const uint32_t groupsY = (height - 1) / 64 + 1;

        SpdConstants spd = {};
        spd.mips = 4; // what FFX's host passes; the shader itself generates six
        spd.numWorkGroups = groupsX * groupsY;
        spd.numWorkGroupsPyramid = groupsX * groupsY;

        _Dispatch(cmdList, PassPyramid,
                  _Table(parity, kSlotPyramid,
                         { current[0], current[1], current[2], current[3], current[4], current[5], current[6] }),
                  &of, groupsX, groupsY, 1, &spd);
    }

    // Scene-change detection: nine luma histograms, then their divergence from last frame's.
    {
        const uint32_t strataWidth = (width / 4) / 3;
        _Dispatch(cmdList, PassScdHistogram, _Table(parity, kSlotScdHistogram, { current[0], _scdHistogram }), &of,
                  (strataWidth + 31) / 32, 16, 9);
        _Dispatch(cmdList, PassScdDivergence,
                  _Table(parity, kSlotScdDivergence, { _scdHistogram, _scdPreviousHistogram, _scdTemp, _scdOutput }),
                  &of, 9, 3, 1);
    }

    // Coarse to fine: search, median filter, then the prediction for the next finer level. The two flow
    // pyramids alternate with the level exactly as in FFX's host (ffx_opticalflow.cpp dispatch()).
    const bool oddFrame = parity != 0;
    for (int32_t level = static_cast<int32_t>(Levels) - 1; level >= 0; --level)
    {
        const bool oddLevel = (level & 1) != 0;
        ID3D12Resource* const* flowA = _flow[oddFrame != oddLevel ? 1 : 0];
        ID3D12Resource* const* flowB = _flow[oddFrame != oddLevel ? 0 : 1];
        const uint32_t l = static_cast<uint32_t>(level);

        of.level = l;

        {
            const uint32_t lumaWidth = _lumaWidth[l] > 0 ? _lumaWidth[l] : 1;
            const uint32_t lumaHeight = _lumaHeight[l] > 0 ? _lumaHeight[l] : 1;
            _Dispatch(cmdList, PassSearch,
                      _Table(parity, SlotLevel(l, 0), { current[l], previous[l], flowA[l], _scdOutput }), &of,
                      ((lumaWidth + 3) / 4 * 16 + 63) / 64, (lumaHeight + 15) / 16, 1);
        }

        ID3D12Resource* filtered = l == 0 ? _flowFinal : flowB[l];
        _Dispatch(cmdList, PassFilter, _Table(parity, SlotLevel(l, 1), { flowA[l], filtered }), &of,
                  (_flowWidth[l] + 15) / 16, (_flowHeight[l] + 3) / 4, 1);

        if (l > 0)
        {
            _Dispatch(cmdList, PassScale,
                      _Table(parity, SlotLevel(l, 2), { current[l], previous[l], flowB[l], flowB[l - 1], _scdOutput }),
                      &of, (_flowWidth[l - 1] + 3) / 4, (_flowHeight[l - 1] + 3) / 4, 1);
        }
    }

    // Our pass: 8x8-block vectors to one current->previous vector per colour pixel, and per pixel zero
    // where the level-0 luma pair shows the pixel standing still (a static overlay; synthesized-motion.md,
    // "Static overlays"). The two lumas are u2 and u3, read through UAVs like everywhere else here.
    {
        ExpandConstants expand = {};
        expand.width = width;
        expand.height = height;
        expand.flowWidth = _flowWidth[0];
        expand.flowHeight = _flowHeight[0];
        expand.zero = (doReset || frameIndex <= kWarmupFrames) ? 1u : 0u;

        _Dispatch(cmdList, PassExpand, _Table(parity, kSlotExpand, { _flowFinal, _motion, current[0], previous[0] }),
                  &expand, (width + 7) / 8, (height + 7) / 8, 1);
    }

    Transition(cmdList, _motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    _motionState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // This frame's scene-change result, for SceneCut() a few confirmed frames from now.
    {
        Transition(cmdList, _scdOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = _readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = static_cast<UINT64>(_confirmedCount % ReadbackSlots) * kReadbackStride;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
        dst.PlacedFootprint.Footprint.Width = 3;
        dst.PlacedFootprint.Footprint.Height = 1;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = _scdOutput;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(cmdList, _scdOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    _recordedFrameIndex = frameIndex;
    _recordedReset = doReset;
    _pending = true;
    _ready = !doReset && frameIndex > kWarmupFrames && !_sceneCut;
    return true;
}

void Estimator_Dx12::ConfirmExecuted()
{
    if (!_pending)
        return;

    _pending = false;
    _confirmedFrameIndex = _recordedFrameIndex;

    if (_recordedReset)
    {
        _needReset = false;
        _confirmedSinceReset = 0;
    }

    ++_confirmedSinceReset;
    ++_confirmedCount;

    // This frame's luma becomes the previous frame's.
    _nextParity ^= 1u;
}

void Estimator_Dx12::AbandonRecording()
{
    if (!_pending)
        return;

    // Nothing recorded ran: the motion field is still in the state it was, and a reset that was asked
    // for is still owed.
    _pending = false;
    _motionState = _motionStateBefore;

    if (_recordedReset)
        _needReset = true;

    _ready = false;
    _sceneCut = false;
}
} // namespace SynthMotion
