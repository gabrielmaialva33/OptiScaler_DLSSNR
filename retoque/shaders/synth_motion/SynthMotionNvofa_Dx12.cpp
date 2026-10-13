#include "pch.h"

#include "SynthMotionNvofa_Dx12.h"

#include <State.h>
#include <d3dx/d3dx12.h>
#include <nvofa/nvOpticalFlowD3D12.h>

// The two passes are ours (precompile/synth_motion_nvofa_*.hlsl). Until precompile/build.sh has been run
// for them the headers do not exist, and this backend compiles to "unavailable": the key falls back to
// the FidelityFX estimator instead of the build breaking.
#if __has_include("precompile/SynthMotion_NvofaPrep_Shader.h") &&                                                      \
                  __has_include("precompile/SynthMotion_NvofaExpand_Shader.h")
#include "precompile/SynthMotion_NvofaPrep_Shader.h"
#include "precompile/SynthMotion_NvofaExpand_Shader.h"
#define SYNTH_MOTION_NVOFA_SHADERS 1
#else
#define SYNTH_MOTION_NVOFA_SHADERS 0
#endif

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace SynthMotion
{
namespace
{
using PFN_CreateInstanceD3D12 = NV_OF_STATUS(NVOFAPI*)(uint32_t apiVer, NV_OF_D3D12_API_FUNCTION_LIST* functionList);

// nvofapi64.dll, loaded once per process and never unloaded: it is the driver's (or dxvk-nvapi's).
struct Api
{
    std::once_flag once;
    NV_OF_D3D12_API_FUNCTION_LIST list {};
    const char* failure = nullptr;
};

const char* LoadApi(NV_OF_D3D12_API_FUNCTION_LIST& list)
{
    HMODULE module = LoadLibraryW(L"nvofapi64.dll");
    if (module == nullptr)
        return "nvofapi64.dll is not present (the NVIDIA driver ships it on Windows, dxvk-nvapi under Proton)";

    auto create = reinterpret_cast<PFN_CreateInstanceD3D12>(GetProcAddress(module, "NvOFAPICreateInstanceD3D12"));
    if (create == nullptr)
        return "nvofapi64.dll has no D3D12 entry point";

    if (create(NV_OF_API_VERSION, &list) != NV_OF_SUCCESS)
        return "nvofapi64.dll refused the optical flow API 5.0 for D3D12";

    const bool complete = list.nvCreateOpticalFlowD3D12 != nullptr && list.nvOFInit != nullptr &&
                          list.nvOFGetSurfaceFormatCountD3D12 != nullptr && list.nvOFGetSurfaceFormatD3D12 != nullptr &&
                          list.nvOFRegisterResourceD3D12 != nullptr && list.nvOFUnregisterResourceD3D12 != nullptr &&
                          list.nvOFExecuteD3D12 != nullptr && list.nvOFDestroy != nullptr &&
                          list.nvOFGetCaps != nullptr;
    return complete ? nullptr : "nvofapi64.dll returned an incomplete D3D12 function list";
}

Api& GetApi()
{
    static Api api;
    std::call_once(api.once, [] { api.failure = LoadApi(api.list); });
    return api;
}

// The engine's grid: 2x2 where offered (finer vectors at a cost the engine absorbs, Ampere and later),
// otherwise 4x4, which every generation has. dxvk-nvapi offers only 4x4.
constexpr uint32_t kPreferredGrid = 2;
constexpr uint32_t kFallbackGrid = 4;

// Descriptor slots in the one shader-visible heap: a colour SRV ring, then the fixed views.
constexpr uint32_t kColourSlots = 8;
constexpr uint32_t kSlotInputUav0 = kColourSlots; // + current input slot
constexpr uint32_t kSlotFlowSrv = kColourSlots + 2;
constexpr uint32_t kSlotMotionUav = kColourSlots + 3;
constexpr uint32_t kHeapSize = kColourSlots + 4;

constexpr uint32_t kRegistrationTimeoutMs = 2000;
constexpr uint32_t kReleaseTimeoutMs = 2000;
constexpr uint32_t kMaxExecuteFailures = 8;
// Allocation is retried on the next frame after a transient failure, but not forever: each attempt
// creates and destroys an engine session.
constexpr uint32_t kMaxAllocateFailures = 3;

struct PrepConstants
{
    uint32_t inputWidth;
    uint32_t inputHeight;
    uint32_t colourWidth;
    uint32_t colourHeight;
    uint32_t pad[8];
};

struct ExpandConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t flowWidth;
    uint32_t flowHeight;
    float cellWidth;
    float cellHeight;
    float vectorScaleX;
    float vectorScaleY;
    uint32_t zero;
    uint32_t pad[3];
};

constexpr uint32_t kRootConstants = 12;
static_assert(sizeof(PrepConstants) == kRootConstants * 4 && sizeof(ExpandConstants) == kRootConstants * 4,
              "root constants are twelve dwords");

// The SRV format the colour is read through, as the FidelityFX backend reads it.
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

ID3D12Resource* CreateTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const wchar_t* name)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, flags);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
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

bool WaitFence(ID3D12Fence* fence, uint64_t value, uint32_t timeoutMs)
{
    if (fence == nullptr || fence->GetCompletedValue() >= value)
        return true;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    bool done =
        SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(event);
    return done || fence->GetCompletedValue() >= value;
}

bool FormatSupported(const NV_OF_D3D12_API_FUNCTION_LIST& api, NvOFHandle handle, NV_OF_BUFFER_USAGE usage,
                     DXGI_FORMAT wanted)
{
    uint32_t count = 0;
    if (api.nvOFGetSurfaceFormatCountD3D12(handle, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS ||
        count == 0)
    {
        return false;
    }

    std::vector<DXGI_FORMAT> formats(count, DXGI_FORMAT_UNKNOWN);
    if (api.nvOFGetSurfaceFormatD3D12(handle, usage, NV_OF_MODE_OPTICALFLOW, formats.data()) != NV_OF_SUCCESS)
        return false;

    return std::find(formats.begin(), formats.end(), wanted) != formats.end();
}

uint32_t CapValue(const NV_OF_D3D12_API_FUNCTION_LIST& api, NvOFHandle handle, NV_OF_CAPS cap, uint32_t fallback)
{
    uint32_t value = 0;
    uint32_t size = 1;
    return api.nvOFGetCaps(handle, cap, &value, &size) == NV_OF_SUCCESS && size >= 1 ? value : fallback;
}

uint32_t ChooseGrid(const NV_OF_D3D12_API_FUNCTION_LIST& api, NvOFHandle handle)
{
    uint32_t count = 0;
    if (api.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &count) != NV_OF_SUCCESS || count == 0)
        return kFallbackGrid;

    std::vector<uint32_t> grids(count, 0);
    if (api.nvOFGetCaps(handle, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &count) != NV_OF_SUCCESS)
        return kFallbackGrid;

    if (std::find(grids.begin(), grids.end(), kPreferredGrid) != grids.end())
        return kPreferredGrid;
    if (std::find(grids.begin(), grids.end(), kFallbackGrid) != grids.end())
        return kFallbackGrid;
    return grids.front();
}

uint32_t DivideUp(uint32_t value, uint32_t by) { return (value + by - 1) / by; }
} // namespace

struct NvofaEstimator_Dx12::Session
{
    NvOFHandle handle = nullptr;
    ID3D12Resource* input[2] {};
    NvOFGPUBufferHandle inputHandle[2] {};
    ID3D12Resource* flow = nullptr;
    NvOFGPUBufferHandle flowHandle = nullptr;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    uint32_t grid = kFallbackGrid;
    uint32_t flowWidth = 0;
    uint32_t flowHeight = 0;
};

NvofaEstimator_Dx12::~NvofaEstimator_Dx12() { Release(); }

void NvofaEstimator_Dx12::_MarkUnavailable(const char* reason)
{
    _unavailable = reason;
    _ready = false;

    if (_reportedUnavailable)
        return;

    _reportedUnavailable = true;
    LOG_WARN("synthesized motion (NVOFA): unavailable -- {}; the FidelityFX estimator is used instead", reason);
}

bool NvofaEstimator_Dx12::_EnsureApi()
{
    const Api& api = GetApi();
    if (api.failure == nullptr)
        return true;

    _MarkUnavailable(api.failure);
    return false;
}

D3D12_GPU_DESCRIPTOR_HANDLE NvofaEstimator_Dx12::_Descriptor(uint32_t slot) const
{
    auto handle = _heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(slot) * _descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE NvofaEstimator_Dx12::_ColourSrv(ID3D12Resource* colour, DXGI_FORMAT format)
{
    // A ring: a slot is rewritten eight recordings later, long after the list that read it executed.
    const uint32_t slot = _colourSlot;
    _colourSlot = (_colourSlot + 1) % kColourSlots;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * _descriptorSize;
    _device->CreateShaderResourceView(colour, &srv, cpu);
    return _Descriptor(slot);
}

bool NvofaEstimator_Dx12::_EnsureDevice(ID3D12Device* device)
{
    if (_device == device && _rootSignature != nullptr)
        return true;

    // A different device: what was made on the old one is parked, never freed under its GPU.
    if (_device != nullptr && _device != device)
    {
        _Park();
        for (IUnknown* object :
             std::initializer_list<IUnknown*> { _rootSignature, _prep, _expand, _heap, _prepFence, _completionFence })
        {
            if (object != nullptr)
                _parkedObjects.push_back(object);
        }

        _rootSignature = nullptr;
        _prep = nullptr;
        _expand = nullptr;
        _heap = nullptr;
        _prepFence = nullptr;
        _completionFence = nullptr;
        _prepValue = _completionValue = _waitedValue = 0;
    }

    _device = device;

#if SYNTH_MOTION_NVOFA_SHADERS
    ScopedInternalResourceCreation internalResources {};

    // t0 (the colour, or the engine's flow), u0 (the engine's input, or the field), twelve constants.
    CD3DX12_DESCRIPTOR_RANGE1 srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
                                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    CD3DX12_DESCRIPTOR_RANGE1 uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
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
        _MarkUnavailable("its root signature could not be created");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = _rootSignature;
    psoDesc.CS = { SynthMotion_NvofaPrep_cso, sizeof(SynthMotion_NvofaPrep_cso) };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_prep))))
    {
        _MarkUnavailable("its input pipeline could not be created");
        return false;
    }

    psoDesc.CS = { SynthMotion_NvofaExpand_cso, sizeof(SynthMotion_NvofaExpand_cso) };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&_expand))))
    {
        _MarkUnavailable("its output pipeline could not be created");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kHeapSize;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))))
        {
            _MarkUnavailable("its descriptor heap could not be created");
            return false;
        }
    }

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_prepFence))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_completionFence))))
    {
        _MarkUnavailable("its fences could not be created");
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
#else
    _MarkUnavailable("its shaders are not built into this binary (shaders/synth_motion/precompile/build.sh)");
    return false;
#endif
}

void NvofaEstimator_Dx12::_DestroySession(Session* session, bool engineIdle)
{
    if (session == nullptr)
        return;

    // Registered resources are the engine's to let go of first; with the engine possibly still reading
    // them they are kept, deliberately, rather than freed under it.
    if (!engineIdle)
        return;

    const auto& api = GetApi().list;
    NvOFGPUBufferHandle handles[] = { session->inputHandle[0], session->inputHandle[1], session->flowHandle };
    for (NvOFGPUBufferHandle handle : handles)
    {
        if (handle == nullptr || api.nvOFUnregisterResourceD3D12 == nullptr)
            continue;

        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params = {};
        params.hOFGpuBuffer = handle;
        api.nvOFUnregisterResourceD3D12(&params);
    }

    if (session->handle != nullptr && api.nvOFDestroy != nullptr)
        api.nvOFDestroy(session->handle);

    SafeRelease(session->input[0]);
    SafeRelease(session->input[1]);
    SafeRelease(session->flow);
    delete session;
}

bool NvofaEstimator_Dx12::_WaitEngineIdle(uint32_t timeoutMs)
{
    if (_completionFence == nullptr || _completionValue == 0)
        return true;

    return WaitFence(_completionFence, _completionValue, timeoutMs);
}

void NvofaEstimator_Dx12::_Park()
{
    if (_session != nullptr)
        _parked.push_back(_session);
    if (_motion != nullptr)
        _parkedObjects.push_back(_motion);

    _session = nullptr;
    _motion = nullptr;
    _width = 0;
    _height = 0;
    _needReset = true;
    _flowValid = false;
    _havePrevious = false;
    _temporalBroken = true;
}

bool NvofaEstimator_Dx12::_Allocate(ID3D12Device* device, uint32_t width, uint32_t height)
{
    // Ours, not the game's: resource heuristics watching the device skip it.
    ScopedInternalResourceCreation internalResources {};

    const auto& api = GetApi().list;
    auto* session = new Session();

    auto fail = [&](const char* reason, bool permanent)
    {
        LOG_WARN("synthesized motion (NVOFA): {} ({}x{})", reason, width, height);
        _DestroySession(session, true);
        if (permanent || ++_allocateFailures >= kMaxAllocateFailures)
            _MarkUnavailable(reason);
        return false;
    };

    if (api.nvCreateOpticalFlowD3D12(device, &session->handle) != NV_OF_SUCCESS || session->handle == nullptr)
        return fail("the optical flow engine refused this device", true);

    if (!FormatSupported(api, session->handle, NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_R8_UNORM) ||
        !FormatSupported(api, session->handle, NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT))
    {
        return fail("the optical flow engine offers no R8_UNORM input or R16G16_SINT output", true);
    }

    // The input: the colour at up to MaxInputHeight lines, aspect kept, inside what the engine accepts.
    uint32_t inputHeight = std::min(height, MaxInputHeight);
    uint32_t inputWidth = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<double>(width) * inputHeight / static_cast<double>(height))));
    auto limit = [&](uint32_t value, NV_OF_CAPS minCap, NV_OF_CAPS maxCap)
    {
        const uint32_t lo = CapValue(api, session->handle, minCap, 32);
        const uint32_t hi = CapValue(api, session->handle, maxCap, 4096);
        return lo <= hi ? std::clamp(value, lo, hi) : value; // dxvk-nvapi reports neither
    };
    inputWidth = limit(inputWidth, NV_OF_CAPS_WIDTH_MIN, NV_OF_CAPS_WIDTH_MAX);
    inputHeight = limit(inputHeight, NV_OF_CAPS_HEIGHT_MIN, NV_OF_CAPS_HEIGHT_MAX);

    session->inputWidth = inputWidth;
    session->inputHeight = inputHeight;
    session->grid = ChooseGrid(api, session->handle);
    session->flowWidth = DivideUp(inputWidth, session->grid);
    session->flowHeight = DivideUp(inputHeight, session->grid);

    NV_OF_INIT_PARAMS init = {};
    init.width = inputWidth;
    init.height = inputHeight;
    init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(session->grid);
    init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
    init.mode = NV_OF_MODE_OPTICALFLOW;
    init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    init.enableExternalHints = NV_OF_FALSE;
    init.enableOutputCost = NV_OF_FALSE;
    init.hPrivData = nullptr;
    init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
    init.enableRoi = NV_OF_FALSE;
    init.predDirection = NV_OF_PRED_DIRECTION_FORWARD; // current (input) -> previous (reference)
    init.enableGlobalFlow = NV_OF_FALSE;
    init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;

    if (api.nvOFInit(session->handle, &init) != NV_OF_SUCCESS)
        return fail("the optical flow engine refused the session", true);

    session->input[0] =
        CreateTexture(device, inputWidth, inputHeight, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, L"SynthMotionNvofa_Input0");
    session->input[1] =
        CreateTexture(device, inputWidth, inputHeight, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_COMMON, L"SynthMotionNvofa_Input1");
    session->flow = CreateTexture(device, session->flowWidth, session->flowHeight, DXGI_FORMAT_R16G16_SINT,
                                  D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"SynthMotionNvofa_Flow");
    ID3D12Resource* motion =
        CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"SynthMotionNvofa_Motion");

    if (session->input[0] == nullptr || session->input[1] == nullptr || session->flow == nullptr || motion == nullptr)
    {
        SafeRelease(motion);
        return fail("its textures could not be created", false);
    }

    // Registration is fenced: each one waits on the last and signals the next, and the CPU waits for the
    // last one here, once per allocation. Both the driver and dxvk-nvapi accept this shape.
    ID3D12Fence* registration = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&registration))))
    {
        SafeRelease(motion);
        return fail("its registration fence could not be created", false);
    }

    uint64_t registrationValue = 0;
    auto registerResource = [&](ID3D12Resource* resource, NvOFGPUBufferHandle* handle)
    {
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params = {};
        params.resource = resource;
        params.inputFencePoint = { registration, registrationValue };
        params.hOFGpuBuffer = handle;
        params.outputFencePoint = { registration, ++registrationValue };
        return api.nvOFRegisterResourceD3D12(session->handle, &params) == NV_OF_SUCCESS;
    };

    const bool registered = registerResource(session->input[0], &session->inputHandle[0]) &&
                            registerResource(session->input[1], &session->inputHandle[1]) &&
                            registerResource(session->flow, &session->flowHandle) &&
                            session->inputHandle[0] != nullptr && session->inputHandle[1] != nullptr &&
                            session->flowHandle != nullptr;
    if (!registered)
    {
        SafeRelease(registration);
        SafeRelease(motion);
        return fail("the optical flow engine did not register its textures", false);
    }

    // Accepted, but the engine's queue never reached the last registration: it may still be working on
    // those textures and that fence, so all of it is left alive rather than freed under it.
    if (!WaitFence(registration, registrationValue, kRegistrationTimeoutMs))
    {
        SafeRelease(motion);
        LOG_WARN("synthesized motion (NVOFA): registration did not complete within {} ms; its session is kept, "
                 "not freed under the engine",
                 kRegistrationTimeoutMs);
        _MarkUnavailable("the optical flow engine did not complete registration");
        return false;
    }
    SafeRelease(registration);

    auto cpuSlot = [&](uint32_t slot)
    {
        auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(slot) * _descriptorSize;
        return cpu;
    };

    for (uint32_t i = 0; i < 2; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(session->input[i], nullptr, &uav, cpuSlot(kSlotInputUav0 + i));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC flowSrv = {};
    flowSrv.Format = DXGI_FORMAT_R16G16_SINT;
    flowSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    flowSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    flowSrv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(session->flow, &flowSrv, cpuSlot(kSlotFlowSrv));

    D3D12_UNORDERED_ACCESS_VIEW_DESC motionUav = {};
    motionUav.Format = DXGI_FORMAT_R16G16_FLOAT;
    motionUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(motion, nullptr, &motionUav, cpuSlot(kSlotMotionUav));

    _session = session;
    _motion = motion;
    _allocateFailures = 0;
    _width = width;
    _height = height;
    _current = 0;
    _havePrevious = false;
    _flowValid = false;
    _needReset = true;
    _temporalBroken = true;

    if (!_reportedSession)
    {
        _reportedSession = true;
        LOG_INFO("synthesized motion (NVOFA): {}x{} colour, engine input {}x{} grayscale, {}x{} grid ({}x{} vectors), "
                 "medium preset; the field runs {} frame behind",
                 width, height, inputWidth, inputHeight, session->grid, session->grid, session->flowWidth,
                 session->flowHeight, LagFrames);
    }

    return true;
}

bool NvofaEstimator_Dx12::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                                 D3D12_RESOURCE_STATES colourState, bool reset, ID3D12CommandQueue* queue)
{
    _ready = false;

    if (_unavailable != nullptr || device == nullptr || cmdList == nullptr || colour == nullptr || queue == nullptr)
        return false;

    // A recording neither confirmed nor abandoned counts as abandoned.
    if (_pending)
        AbandonRecording();

    const auto desc = colour->GetDesc();
    const DXGI_FORMAT viewFormat = ColourViewFormat(desc.Format);
    if (viewFormat == DXGI_FORMAT_UNKNOWN)
    {
        if (!_warnedFormat)
        {
            _warnedFormat = true;
            LOG_WARN("synthesized motion (NVOFA): colour format {} is not an RGBA colour format; no field",
                     static_cast<uint32_t>(desc.Format));
        }
        return false;
    }

    const auto width = static_cast<uint32_t>(desc.Width);
    const auto height = desc.Height;
    if (width < MinWidth || height < MinHeight)
    {
        if (!_warnedSmall)
        {
            _warnedSmall = true;
            LOG_INFO("synthesized motion (NVOFA): {}x{} is below the {}x{} floor; no field until a larger frame", width,
                     height, MinWidth, MinHeight);
        }
        return true;
    }

    if (!_EnsureApi() || !_EnsureDevice(device))
        return false;

    if (_session == nullptr || _width != width || _height != height)
    {
        if (_session != nullptr)
            _Park();
        if (!_Allocate(device, width, height))
            return false;
    }

    _queue = queue;
    Session& s = *_session;
    const bool resetNow = reset || _needReset;

    // The engine may still be reading the input slot this list is about to overwrite, and the flow texture
    // this list reads is its output: the list waits for it -- on the GPU, queued ahead of the list.
    if (_completionValue > _waitedValue)
    {
        if (FAILED(queue->Wait(_completionFence, _completionValue)))
        {
            // Without that wait neither the slot nor the flow is safe to touch.
            _ready = false;
            return false;
        }
        _waitedValue = _completionValue;
    }

    ID3D12DescriptorHeap* heaps[] = { _heap };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);

    // The field, from the pair the engine measured at the last confirm.
    const bool haveField = _flowValid && !resetNow;
    if (haveField)
    {
        Transition(cmdList, s.flow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, _motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ExpandConstants constants = {};
        constants.width = width;
        constants.height = height;
        constants.flowWidth = s.flowWidth;
        constants.flowHeight = s.flowHeight;
        constants.cellWidth = static_cast<float>(s.grid) * width / s.inputWidth;
        constants.cellHeight = static_cast<float>(s.grid) * height / s.inputHeight;
        constants.vectorScaleX = static_cast<float>(width) / s.inputWidth / 32.0f; // S10.5 -> colour pixels
        constants.vectorScaleY = static_cast<float>(height) / s.inputHeight / 32.0f;
        constants.zero = 0;

        cmdList->SetPipelineState(_expand);
        cmdList->SetComputeRootDescriptorTable(0, _Descriptor(kSlotFlowSrv));
        cmdList->SetComputeRootDescriptorTable(1, _Descriptor(kSlotMotionUav));
        cmdList->SetComputeRoot32BitConstants(2, kRootConstants, &constants, 0);
        cmdList->Dispatch(DivideUp(width, 8), DivideUp(height, 8), 1);

        Transition(cmdList, _motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmdList, s.flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    // This frame's grayscale input, into the slot the next confirm measures from. It ends in COMMON: the
    // engine reads it on another queue.
    const auto colourSrv = _ColourSrv(colour, viewFormat);
    Transition(cmdList, colour, colourState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, s.input[_current], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    PrepConstants prep = {};
    prep.inputWidth = s.inputWidth;
    prep.inputHeight = s.inputHeight;
    prep.colourWidth = width;
    prep.colourHeight = height;

    cmdList->SetPipelineState(_prep);
    cmdList->SetComputeRootDescriptorTable(0, colourSrv);
    cmdList->SetComputeRootDescriptorTable(1, _Descriptor(kSlotInputUav0 + _current));
    cmdList->SetComputeRoot32BitConstants(2, kRootConstants, &prep, 0);
    cmdList->Dispatch(DivideUp(s.inputWidth, 8), DivideUp(s.inputHeight, 8), 1);

    Transition(cmdList, s.input[_current], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(cmdList, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colourState);

    _pending = true;
    _recordedReset = resetNow;
    _ready = haveField;
    return true;
}

void NvofaEstimator_Dx12::ConfirmExecuted()
{
    if (!_pending)
        return;

    _pending = false;

    if (_session == nullptr || _queue == nullptr)
        return;

    Session& s = *_session;

    // Queued after the list that wrote this frame's input: the engine starts only once that has run.
    const uint64_t prepValue = _prepValue + 1;
    if (FAILED(_queue->Signal(_prepFence, prepValue)))
    {
        _flowValid = false;
        _needReset = true;
        return;
    }
    _prepValue = prepValue;

    if (_recordedReset)
    {
        _needReset = false;
        _havePrevious = false;
        _temporalBroken = true;
    }

    _flowValid = false;
    if (_havePrevious)
    {
        NV_OF_FENCE_POINT wait = { _prepFence, prepValue };
        NV_OF_FENCE_POINT signal = { _completionFence, _completionValue + 1 };

        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in = {};
        in.inputFrame = s.inputHandle[_current];         // this frame
        in.referenceFrame = s.inputHandle[_current ^ 1]; // the previous one
        in.disableTemporalHints = _temporalBroken ? NV_OF_TRUE : NV_OF_FALSE;
        in.numFencePoints = 1;
        in.fencePoint = &wait;

        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out = {};
        out.outputBuffer = s.flowHandle;
        out.fencePoint = &signal;

        const NV_OF_STATUS status = GetApi().list.nvOFExecuteD3D12(s.handle, &in, &out);
        if (status == NV_OF_SUCCESS)
        {
            _completionValue = signal.value;
            _flowValid = true;
            _temporalBroken = false;
            _executeFailures = 0;
        }
        else
        {
            _temporalBroken = true;
            if (++_executeFailures == 1)
                LOG_WARN("synthesized motion (NVOFA): the engine refused to execute (status {}); no field this frame",
                         static_cast<int>(status));
            if (_executeFailures >= kMaxExecuteFailures)
                _MarkUnavailable("the engine refused to execute eight times in a row");
        }
    }

    _current ^= 1;
    _havePrevious = true;
}

void NvofaEstimator_Dx12::AbandonRecording()
{
    if (!_pending)
        return;

    // The list never ran: its input was never written and nothing read the field. History stays where it
    // was, and a reset it carried is still owed. A queue wait issued for it is harmless: the engine's
    // fence reaches that value on its own.
    _pending = false;
    _ready = false;
    if (_recordedReset)
        _needReset = true;
}

void NvofaEstimator_Dx12::Release()
{
    // The caller's drain proves its own queue idle; the engine runs on another, so it is waited for here.
    const bool idle = _WaitEngineIdle(kReleaseTimeoutMs);
    if (!idle)
        LOG_WARN("synthesized motion (NVOFA): the engine did not go idle within {} ms; its resources are kept, not "
                 "freed under it",
                 kReleaseTimeoutMs);

    if (_session != nullptr)
        _parked.push_back(_session);
    _session = nullptr;

    if (idle)
    {
        for (Session* session : _parked)
            _DestroySession(session, true);
        _parked.clear();

        for (IUnknown* object : _parkedObjects)
            object->Release();
        _parkedObjects.clear();

        SafeRelease(_motion);
        SafeRelease(_prep);
        SafeRelease(_expand);
        SafeRelease(_rootSignature);
        SafeRelease(_heap);
        SafeRelease(_prepFence);
        SafeRelease(_completionFence);
        _prepValue = _completionValue = _waitedValue = 0;
        _device = nullptr;
    }
    else if (_motion != nullptr)
    {
        _parkedObjects.push_back(_motion);
        _motion = nullptr;
    }

    _queue = nullptr;
    _width = 0;
    _height = 0;
    _current = 0;
    _havePrevious = false;
    _flowValid = false;
    _needReset = true;
    _pending = false;
    _recordedReset = false;
    _temporalBroken = true;
    _ready = false;
    _colourSlot = 0;
}
} // namespace SynthMotion
