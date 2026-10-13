#include "pch.h"

#include "SynthOverlay_Dx12.h"

#include <State.h>
#include <d3dx/d3dx12.h>

#include "precompile/SynthOverlay_Detect_Shader.h"
#include "precompile/SynthOverlay_DetectLayer_Shader.h"
#include "precompile/SynthOverlay_Layer_Shader.h"

namespace SynthMotion
{
namespace
{
// DLSS-NR's thresholds (shaders/dlssnr/DlssNr_UiMask_Dx12.cpp), unchanged: the same rule on the same kind of
// image, and hud-protection.md is where they were tuned against false positives. tests/fg-synth-policy holds
// the two lists equal.
constexpr float kStaticEps = 0.008f;
constexpr float kCoreEps = 0.012f;
constexpr float kMotionTau = 0.02f;
constexpr float kDetailMin = 0.15f;
constexpr float kDecay = 0.985f;
constexpr float kDropTau = 0.2f;
constexpr uint32_t kStreakMin = 8;
constexpr float kSupportMin = 3.0f;
constexpr float kSidesMin = 4.0f;

// Ours: the mask at or above which a pixel is near. The exported mask ramps from 0 to 1 while the protection
// goes from 0.25 to 0.75, so this is a protection of one half: a pixel protected now, or within about 18
// frames of the evidence stopping.
constexpr float kNearMin = 0.5f;

// One descriptor ring for both passes: per record, five SRVs then seven UAVs, contiguous, so each record's
// two tables stay valid until the ring comes round. Two records a frame at most (the mask, then the layer),
// so a slot comes back eight frames later. The mask alone binds the first three and four of them (its bytecode
// declares no more); the layer's permutation all of them; the layer pass three and one.
constexpr uint32_t kSrvs = 5;
constexpr uint32_t kUavs = 7;
constexpr uint32_t kSlotSize = kSrvs + kUavs;
constexpr uint32_t kSlots = 16;
constexpr uint32_t kRootConstants = 16;

// The Params cbuffer of synth_overlay_detect.hlsl, in order, as root constants.
struct DetectConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t valid;
    uint32_t streakMin;
    float staticEps;
    float coreEps;
    float motionTau;
    float detailMin;
    float decay;
    float dropTau;
    float supportMin;
    float sidesMin;
    float nearMin;
    uint32_t linearInput;
    uint32_t layerValid; // the layer's permutation only (gPad0 in the mask's)
    uint32_t pad1;
};

// The Params cbuffer of synth_overlay_layer.hlsl: the first three are read.
struct LayerConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t margin;
    uint32_t pad[kRootConstants - 3];
};

static_assert(sizeof(DetectConstants) == kRootConstants * 4 && sizeof(LayerConstants) == kRootConstants * 4,
              "root constants are sixteen dwords");

// The estimator's rule for reading a frame (SynthMotion_Dx12.cpp, ColourViewFormat): a TYPELESS frame is read
// as its UNORM or FLOAT sibling; anything that is not an RGBA colour format is refused.
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

// A view that reads linear light: the rule's thresholds want an encoded image (synth_overlay_detect.hlsl).
bool LinearView(DXGI_FORMAT view)
{
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

// The layer holds the frame so that FFX's lerp(x, layer, a) gives x back in a real frame. An 8-bit UNORM
// frame fits RGBA8 exactly. Anything else goes to RGBA16F: it keeps a 10-bit UNORM value within a quarter of a
// step, and linear light (an sRGB view) without the banding RGBA8 would give it.
DXGI_FORMAT LayerFormat(DXGI_FORMAT view)
{
    switch (view)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

// The layer's own mask works on 8x8 tiles, one per thread group of the detect pass (UL_TILE in
// precompile/static_overlay_layer.h); the layer pass on 16x16 groups.
constexpr uint32_t kTile = 8;
constexpr uint32_t kLayerGroup = 16;

// Everything here rests in NON_PIXEL_SHADER_RESOURCE, where FSR-FG reads the depth and FFX copies the layer.
constexpr D3D12_RESOURCE_STATES kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// Created in the state it rests in, as every resource here, so nothing is ever in a state a barrier does not
// name.
ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                              const wchar_t* name)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRest, nullptr,
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

Overlay_Dx12::~Overlay_Dx12() { Release(); }

bool Overlay_Dx12::_EnsureDevice(ID3D12Device* device)
{
    if (_device == device && _rootSignature != nullptr)
        return true;

    // A different device: everything made on the old one is parked, not freed.
    if (_device != nullptr && _device != device)
    {
        _Park();

        for (IUnknown* object :
             { static_cast<IUnknown*>(_rootSignature), static_cast<IUnknown*>(_detectPipeline),
               static_cast<IUnknown*>(_detectLayerPipeline), static_cast<IUnknown*>(_layerPipeline) })
        {
            if (object != nullptr)
                _parked.push_back(object);
        }

        _rootSignature = nullptr;
        _detectPipeline = nullptr;
        _detectLayerPipeline = nullptr;
        _layerPipeline = nullptr;
    }

    _device = device;

    // t0..t4 and u0..u6 per record, the constants as root constants at b0: no constant-buffer view to size.
    CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvs, 0, 0,
                                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                           D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavs, 0, 0,
                                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                           D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

    CD3DX12_ROOT_PARAMETER1 parameters[3];
    parameters[0].InitAsDescriptorTable(1, &srvRange);
    parameters[1].InitAsDescriptorTable(1, &uavRange);
    parameters[2].InitAsConstants(kRootConstants, 0);

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
        LOG_ERROR("synthesized FG HUD mask: root signature failed ({:X}); no HUD mask for this session", (UINT) hr);
        return false;
    }

    struct Blob
    {
        const unsigned char* data;
        size_t size;
        ID3D12PipelineState** pipeline;
    };
    const Blob blobs[] = {
        { SynthOverlay_Detect_cso, sizeof(SynthOverlay_Detect_cso), &_detectPipeline },
        { SynthOverlay_DetectLayer_cso, sizeof(SynthOverlay_DetectLayer_cso), &_detectLayerPipeline },
        { SynthOverlay_Layer_cso, sizeof(SynthOverlay_Layer_cso), &_layerPipeline },
    };

    for (const auto& blob : blobs)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = _rootSignature;
        psoDesc.CS = { blob.data, blob.size };

        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(blob.pipeline));
        if (FAILED(hr) || *blob.pipeline == nullptr)
        {
            LOG_ERROR("synthesized FG HUD mask: pipeline failed ({:X}); no HUD mask for this session", (UINT) hr);
            return false;
        }
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Overlay_Dx12::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)
{
    // Ours, not the game's: resource heuristics watching the device skip them.
    ScopedInternalResourceCreation internalResources {};

    _width = width;
    _height = height;

    _luma[0] = CreateTexture(device, width, height, DXGI_FORMAT_R16_FLOAT, L"SynthOverlay_Luma1");
    _luma[1] = CreateTexture(device, width, height, DXGI_FORMAT_R16_FLOAT, L"SynthOverlay_Luma2");
    _acc[0] = CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, L"SynthOverlay_State1");
    _acc[1] = CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, L"SynthOverlay_State2");
    _mask = CreateTexture(device, width, height, DXGI_FORMAT_R8_UNORM, L"SynthOverlay_Mask");
    _depth = CreateTexture(device, width, height, DXGI_FORMAT_R32_FLOAT, L"SynthOverlay_Depth");

    bool ok = _luma[0] != nullptr && _luma[1] != nullptr && _acc[0] != nullptr && _acc[1] != nullptr &&
              _mask != nullptr && _depth != nullptr;

    // A heap per allocation, parked with the resources it describes: recordings made before a resize may
    // still read the old descriptors.
    if (ok)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = kSlots * kSlotSize;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        ok = SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))) && _heap != nullptr;
        if (ok)
            _heap->SetName(L"SynthOverlay_Descriptors");
    }

    if (!ok)
    {
        LOG_ERROR("synthesized FG HUD mask: could not allocate for {}x{}; no HUD mask for this session", width, height);
        _Park();
        return false;
    }

    _current = 0;
    _slot = 0;
    _valid = false;

    LOG_INFO("synthesized FG HUD mask: {}x{} allocated", width, height);
    return true;
}

bool Overlay_Dx12::_EnsureLayer(DXGI_FORMAT format)
{
    if (_layer != nullptr && _layerFormat == format)
        return true;

    if (_layer != nullptr)
    {
        _parked.push_back(_layer);
        _layer = nullptr;
    }

    ScopedInternalResourceCreation internalResources {};

    _layer = CreateTexture(_device, _width, _height, format, L"SynthOverlay_Layer");
    _layerFormat = _layer != nullptr ? format : DXGI_FORMAT_UNKNOWN;
    _layerWritten = false;

    if (_layer == nullptr)
    {
        if (!_warnedLayer)
        {
            _warnedLayer = true;
            LOG_WARN("synthesized FG HUD mask: could not allocate the UI layer ({}x{}, format {})", _width, _height,
                     (UINT) format);
        }
        return false;
    }

    LOG_INFO("synthesized FG HUD mask: UI layer {}x{}, format {}", _width, _height, (UINT) format);
    return true;
}

bool Overlay_Dx12::_EnsureLayerMask()
{
    if (_core != nullptr)
        return true;

    ScopedInternalResourceCreation internalResources {};

    const uint32_t tilesW = (_width + kTile - 1) / kTile;
    const uint32_t tilesH = (_height + kTile - 1) / kTile;

    _layerState[0] = CreateTexture(_device, _width, _height, DXGI_FORMAT_R8G8B8A8_UNORM, L"SynthOverlay_LayerState1");
    _layerState[1] = CreateTexture(_device, _width, _height, DXGI_FORMAT_R8G8B8A8_UNORM, L"SynthOverlay_LayerState2");
    _tiles[0] = CreateTexture(_device, tilesW, tilesH, DXGI_FORMAT_R8G8B8A8_UNORM, L"SynthOverlay_Tiles1");
    _tiles[1] = CreateTexture(_device, tilesW, tilesH, DXGI_FORMAT_R8G8B8A8_UNORM, L"SynthOverlay_Tiles2");
    _core = CreateTexture(_device, _width, _height, DXGI_FORMAT_R8_UNORM, L"SynthOverlay_LayerCore");

    if (_layerState[0] == nullptr || _layerState[1] == nullptr || _tiles[0] == nullptr || _tiles[1] == nullptr ||
        _core == nullptr)
    {
        for (auto** object : { &_layerState[0], &_layerState[1], &_tiles[0], &_tiles[1], &_core })
        {
            if (*object != nullptr)
            {
                _parked.push_back(*object);
                *object = nullptr;
            }
        }

        if (!_warnedLayerMask)
        {
            _warnedLayerMask = true;
            LOG_WARN("synthesized FG HUD mask: could not allocate the UI layer's own mask ({}x{}); no UI layer", _width,
                     _height);
        }
        return false;
    }

    // Nothing the new pairs hold has executed.
    _layerValid = false;

    LOG_INFO("synthesized FG HUD mask: the UI layer's own mask allocated, {}x{} ({}x{} tiles)", _width, _height, tilesW,
             tilesH);
    return true;
}

void Overlay_Dx12::_Park()
{
    auto park = [this](auto*& object)
    {
        if (object != nullptr)
        {
            _parked.push_back(object);
            object = nullptr;
        }
    };

    for (uint32_t i = 0; i < 2; ++i)
    {
        park(_luma[i]);
        park(_acc[i]);
    }

    park(_mask);
    park(_depth);
    park(_layer);
    park(_core);
    for (uint32_t i = 0; i < 2; ++i)
    {
        park(_layerState[i]);
        park(_tiles[i]);
    }
    park(_heap);

    _layerFormat = DXGI_FORMAT_UNKNOWN;
    _width = 0;
    _height = 0;
    _current = 0;
    _slot = 0;
    _valid = false;
    _layerValid = false;
    _pending = false;
    _pendingLayer = false;
    _coreRecorded = false;
    _coreTiles = nullptr;
    _layerPending = false;
    _layerWritten = false;
}

void Overlay_Dx12::_ReleaseParked()
{
    for (auto* object : _parked)
    {
        if (object != nullptr)
            object->Release();
    }
    _parked.clear();
}

void Overlay_Dx12::Release()
{
    _Park();
    _ReleaseParked();

    SafeRelease(_detectPipeline);
    SafeRelease(_detectLayerPipeline);
    SafeRelease(_layerPipeline);
    SafeRelease(_rootSignature);
    _device = nullptr;

    _failed = false;
    _warnedFormat = false;
    _warnedLayer = false;
    _warnedLayerMask = false;
}

uint32_t Overlay_Dx12::_WriteSlot(ID3D12Resource* const* srvs, const DXGI_FORMAT* srvFormats, uint32_t srvCount,
                                  ID3D12Resource* const* uavs, uint32_t uavCount)
{
    const uint32_t first = _slot * kSlotSize;
    _slot = (_slot + 1) % kSlots;

    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(first) * _descriptorSize;

    // Unused entries hold null views: every descriptor a table reaches is a valid one.
    for (uint32_t i = 0; i < kSrvs; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = i < srvCount ? srvFormats[i] : DXGI_FORMAT_R8_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;

        _device->CreateShaderResourceView(i < srvCount ? srvs[i] : nullptr, &desc, cpu);
        cpu.ptr += _descriptorSize;
    }

    for (uint32_t i = 0; i < kUavs; ++i)
    {
        if (i < uavCount)
        {
            _device->CreateUnorderedAccessView(uavs[i], nullptr, nullptr, cpu);
        }
        else
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC nullDesc = {};
            nullDesc.Format = DXGI_FORMAT_R32_FLOAT;
            nullDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            _device->CreateUnorderedAccessView(nullptr, nullptr, &nullDesc, cpu);
        }
        cpu.ptr += _descriptorSize;
    }

    return first;
}

D3D12_GPU_DESCRIPTOR_HANDLE Overlay_Dx12::_Gpu(uint32_t index) const
{
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * _descriptorSize;
    return gpu;
}

bool Overlay_Dx12::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                          D3D12_RESOURCE_STATES colourState, bool reset, bool layer)
{
    // A previous recording nobody confirmed or abandoned is treated as dropped.
    if (_pending || _layerPending)
        AbandonRecording();

    // Set again below when this Record computes the layer's core; a Record that stops before that leaves none.
    _coreRecorded = false;
    _coreTiles = nullptr;

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
            LOG_WARN("synthesized FG HUD mask: cannot read a frame of dimension {}, format {}, {} samples",
                     (UINT) colourDesc.Dimension, (UINT) colourDesc.Format, colourDesc.SampleDesc.Count);
        }
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(colourDesc.Width);
    const uint32_t height = colourDesc.Height;

    if (width < MinWidth || height < MinHeight)
        return true;

    if (!_EnsureDevice(device))
    {
        _failed = true;
        return false;
    }

    if (_depth == nullptr || width != _width || height != _height)
    {
        _Park();

        if (!_Allocate(device, width, height))
        {
            _failed = true;
            return false;
        }
    }

    // The layer's own mask: allocated on first use at this extent. Without it the layer gets nothing this frame,
    // and the mask and depth are recorded all the same.
    const bool withLayer = layer && _EnsureLayerMask();

    const uint32_t previous = _current;
    const uint32_t next = _current ^ 1u;

    ID3D12Resource* srvs[kSrvs] = { colour, _luma[previous], _acc[previous] };
    const DXGI_FORMAT srvFormats[kSrvs] = { viewFormat, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
    ID3D12Resource* uavs[kUavs] = { _luma[next], _acc[next], _mask, _depth };
    uint32_t srvCount = 3;
    uint32_t uavCount = 4;
    if (withLayer)
    {
        srvs[srvCount++] = _layerState[previous];
        srvs[srvCount++] = _tiles[previous];
        uavs[uavCount++] = _layerState[next];
        uavs[uavCount++] = _tiles[next];
        uavs[uavCount++] = _core;
    }
    const uint32_t first = _WriteSlot(srvs, srvFormats, srvCount, uavs, uavCount);

    // The frame is read through an SRV; a state without NON_PIXEL_SHADER_RESOURCE is moved there and back.
    const bool colourNeedsTransition = (colourState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;

    D3D12_RESOURCE_BARRIER before[kUavs + 1];
    D3D12_RESOURCE_BARRIER after[kUavs + 1];
    uint32_t count = 0;
    for (uint32_t i = 0; i < uavCount; ++i)
    {
        before[count] = Transition(uavs[i], kRest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        after[count] = Transition(uavs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRest);
        ++count;
    }
    if (colourNeedsTransition)
    {
        before[count] = Transition(colour, colourState, kRest);
        after[count] = Transition(colour, kRest, colourState);
        ++count;
    }

    DetectConstants constants = {};
    constants.width = width;
    constants.height = height;
    constants.valid = (_valid && !reset) ? 1u : 0u;
    constants.streakMin = kStreakMin;
    constants.staticEps = kStaticEps;
    constants.coreEps = kCoreEps;
    constants.motionTau = kMotionTau;
    constants.detailMin = kDetailMin;
    constants.decay = kDecay;
    constants.dropTau = kDropTau;
    constants.supportMin = kSupportMin;
    constants.sidesMin = kSidesMin;
    constants.nearMin = kNearMin;
    constants.linearInput = LinearView(viewFormat) ? 1u : 0u;
    // The layer's history holds only when the frame before this one computed it too.
    constants.layerValid = (constants.valid != 0 && _layerValid) ? 1u : 0u;

    cmdList->ResourceBarrier(count, before);

    ID3D12DescriptorHeap* heaps[] = { _heap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(withLayer ? _detectLayerPipeline : _detectPipeline);
    cmdList->SetComputeRootDescriptorTable(0, _Gpu(first));
    cmdList->SetComputeRootDescriptorTable(1, _Gpu(first + kSrvs));
    cmdList->SetComputeRoot32BitConstants(2, kRootConstants, &constants, 0);
    // One thread group per 8x8 tile: the layer's permutation writes one tile-map texel per group.
    cmdList->Dispatch((width + kTile - 1) / kTile, (height + kTile - 1) / kTile, 1);

    cmdList->ResourceBarrier(count, after);

    _pending = true;
    _pendingLayer = withLayer;
    _coreRecorded = withLayer;
    _coreTiles = withLayer ? _tiles[next] : nullptr;
    return true;
}

bool Overlay_Dx12::RecordLayer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* presented,
                               D3D12_RESOURCE_STATES presentedState, uint32_t margin)
{
    // The alpha is the core the last Record computed; one that did not ask for the layer left none to read.
    if (_failed || cmdList == nullptr || presented == nullptr || _device == nullptr || _depth == nullptr ||
        _core == nullptr || _coreTiles == nullptr || !_coreRecorded)
    {
        return false;
    }

    const auto desc = presented->GetDesc();
    const DXGI_FORMAT viewFormat = ColourViewFormat(desc.Format);

    // The layer is composed over the frame FFX presents, pixel for pixel: it must be that frame's extent,
    // which is the mask's.
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        viewFormat == DXGI_FORMAT_UNKNOWN || static_cast<uint32_t>(desc.Width) != _width || desc.Height != _height)
    {
        if (!_warnedLayer)
        {
            _warnedLayer = true;
            LOG_WARN("synthesized FG HUD mask: no UI layer from a frame of {}x{}, format {} (mask {}x{})",
                     (UINT) desc.Width, desc.Height, (UINT) desc.Format, _width, _height);
        }
        return false;
    }

    if (!_EnsureLayer(LayerFormat(viewFormat)))
        return false;

    // With the tile map the same Record wrote, whose UL_TILE_CORE bits let a group with no core in reach skip.
    ID3D12Resource* srvs[] = { presented, _core, _coreTiles };
    const DXGI_FORMAT srvFormats[] = { viewFormat, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
    ID3D12Resource* uavs[] = { _layer };
    const uint32_t first = _WriteSlot(srvs, srvFormats, _countof(srvs), uavs, _countof(uavs));

    const bool presentedNeedsTransition = (presentedState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;

    D3D12_RESOURCE_BARRIER before[2];
    D3D12_RESOURCE_BARRIER after[2];
    uint32_t count = 0;
    before[count] = Transition(_layer, kRest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    after[count] = Transition(_layer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRest);
    ++count;
    if (presentedNeedsTransition)
    {
        before[count] = Transition(presented, presentedState, kRest);
        after[count] = Transition(presented, kRest, presentedState);
        ++count;
    }

    LayerConstants constants = {};
    constants.width = _width;
    constants.height = _height;
    constants.margin = margin < MaxLayerMargin ? margin : MaxLayerMargin;

    cmdList->ResourceBarrier(count, before);

    // Possibly another list than the mask's (native D3D12 records this after DLSS-NR's pass): bind everything.
    ID3D12DescriptorHeap* heaps[] = { _heap };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_layerPipeline);
    cmdList->SetComputeRootDescriptorTable(0, _Gpu(first));
    cmdList->SetComputeRootDescriptorTable(1, _Gpu(first + kSrvs));
    cmdList->SetComputeRoot32BitConstants(2, kRootConstants, &constants, 0);
    cmdList->Dispatch((_width + kLayerGroup - 1) / kLayerGroup, (_height + kLayerGroup - 1) / kLayerGroup, 1);

    cmdList->ResourceBarrier(count, after);

    _layerPending = true;
    return true;
}

void Overlay_Dx12::ConfirmExecuted()
{
    if (_pending)
    {
        _current ^= 1u;
        _valid = true;
        _layerValid = _pendingLayer;
        _pending = false;
        _pendingLayer = false;
    }

    if (_layerPending)
    {
        _layerWritten = true;
        _layerPending = false;
    }
}

void Overlay_Dx12::AbandonRecording()
{
    // The pairs stay where they were; a core recorded on the dropped list was never written.
    _pending = false;
    _pendingLayer = false;
    _coreRecorded = false;
    _coreTiles = nullptr;
    _layerPending = false;
}
} // namespace SynthMotion
