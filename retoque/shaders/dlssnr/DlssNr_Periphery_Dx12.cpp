#include "pch.h"

#include "DlssNr_Periphery_Dx12.h"

#include <cstring>

// The three passes' bytecode, when the build carries it. See the header: its absence is a supported state,
// not an error, and the build must not depend on it. All three or none: a layout the model reads in packed
// space has to be packed and unpacked both.
#if __has_include("precompile/DlssNr_PeripheryColour_Shader.h") &&                                                     \
                  __has_include("precompile/DlssNr_PeripheryGuides_Shader.h") &&                                       \
                                __has_include("precompile/DlssNr_PeripheryUnpack_Shader.h")
#include "precompile/DlssNr_PeripheryColour_Shader.h"
#include "precompile/DlssNr_PeripheryGuides_Shader.h"
#include "precompile/DlssNr_PeripheryUnpack_Shader.h"
#define DLSSNR_PERIPHERY_BYTECODE 1
#else
#define DLSSNR_PERIPHERY_BYTECODE 0
#endif

namespace
{
// The name the constant-buffer views are sized by, so tests/nr-invariants finds the struct and checks that it
// is alignas(256).
using PeripheryConstants = DlssNr::Periphery::PeripheryConstants;

constexpr uint32_t kSrvCount = 2;
constexpr uint32_t kUavCount = 2;
constexpr uint32_t kThreads = 8;
} // namespace

bool DlssNr_Periphery_Dx12::Available() { return DLSSNR_PERIPHERY_BYTECODE != 0; }

DlssNr_Periphery_Dx12::DlssNr_Periphery_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr || !Available())
        return;

    // Ours, not the game's: the exposure scan must not take these for a candidate.
    ScopedInternalResourceCreation internalResources {};

    // Two inputs, two outputs, one constant buffer, and the clamped linear sampler the unpack reads with.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PeripheryConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_PERIPHERY_HEAPS; ++i)
    {
        if (InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&_constantBuffers[i])) != S_OK)
        {
            LOG_ERROR("[{0}] constant buffer {1} could not be created", _name, i);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        if (FAILED(_constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i])) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{0}] constant buffer {1} could not be mapped", _name, i);
            return;
        }
    }

#if DLSSNR_PERIPHERY_BYTECODE
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_PeripheryColour_cso,
                               sizeof(DlssNr_PeripheryColour_cso), nullptr) ||
        !CreateComputePipeline(InDevice, &_guidesPipeline, DlssNr_PeripheryGuides_cso,
                               sizeof(DlssNr_PeripheryGuides_cso), nullptr) ||
        !CreateComputePipeline(InDevice, &_unpackPipeline, DlssNr_PeripheryUnpack_cso,
                               sizeof(DlssNr_PeripheryUnpack_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipelines", _name);
        return;
    }
#endif

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_PERIPHERY_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_PERIPHERY_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(PeripheryConstants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _init = true;
}

DlssNr_Periphery_Dx12::~DlssNr_Periphery_Dx12()
{
    // Deleted only at shutdown, once the recordings that used it are done, as the guide resample is.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    SAFE_RELEASE(_guidesPipeline);
    SAFE_RELEASE(_unpackPipeline);

    for (uint32_t i = 0; i < DLSSNR_PERIPHERY_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

bool DlssNr_Periphery_Dx12::Record(ID3D12GraphicsCommandList* InCmdList, ID3D12PipelineState* InPipeline,
                                   const DlssNr::Periphery::PeripheryConstants& InConstants, ID3D12Resource* InSource0,
                                   ID3D12Resource* InSource1, ID3D12Resource* OutTarget0, ID3D12Resource* OutTarget1)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InPipeline == nullptr || InSource0 == nullptr ||
        InSource1 == nullptr || OutTarget0 == nullptr || OutTarget1 == nullptr || InConstants.OutWidth == 0 ||
        InConstants.OutHeight == 0)
    {
        return false;
    }

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_PERIPHERY_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    // Views are re-created every call: three dispatches a frame, and the resources behind them change with
    // the frame's format, the guides the game hands over and every rebuild.
    CreateShaderResourceView(_device, InSource0, heap.GetSrvCPU(0), DXGI_FORMAT_UNKNOWN, true);
    CreateShaderResourceView(_device, InSource1, heap.GetSrvCPU(1), DXGI_FORMAT_UNKNOWN, true);
    CreateUnorderedAccessView(_device, OutTarget0, heap.GetUavCPU(0), 0, true);
    CreateUnorderedAccessView(_device, OutTarget1, heap.GetUavCPU(1), 0, true);

    std::memcpy(_mappedConstants[slot], &InConstants, sizeof(InConstants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(InPipeline);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((InConstants.OutWidth + kThreads - 1) / kThreads,
                        (InConstants.OutHeight + kThreads - 1) / kThreads, 1);

    // Whatever reads these next is ordered after the writes; the caller's transition out of UNORDERED_ACCESS
    // does that too, and this covers a reader that comes first.
    D3D12_RESOURCE_BARRIER written[2] {};
    written[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[0].UAV.pResource = OutTarget0;
    written[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[1].UAV.pResource = OutTarget1;
    InCmdList->ResourceBarrier(OutTarget1 != OutTarget0 ? 2 : 1, written);

    return true;
}

bool DlssNr_Periphery_Dx12::PackColour(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                                       ID3D12Resource* InProxy, ID3D12Resource* OutPacked)
{
    if (!InLayout.active)
        return false;

    auto constants = DlssNr::Periphery::MakeConstants(InLayout);
    constants.OutWidth = InLayout.modelW;
    constants.OutHeight = InLayout.modelH;

    // One input and one output: each stands in for the slot its pass does not use.
    return Record(InCmdList, _pipelineState, constants, InProxy, InProxy, OutPacked, OutPacked);
}

bool DlssNr_Periphery_Dx12::PackGuides(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                                       ID3D12Resource* InDepth, ID3D12Resource* InMotion, uint32_t InDepthBaseX,
                                       uint32_t InDepthBaseY, uint32_t InDepthWidth, uint32_t InDepthHeight,
                                       uint32_t InMotionBaseX, uint32_t InMotionBaseY, uint32_t InMotionWidth,
                                       uint32_t InMotionHeight, float InMotionScaleX, float InMotionScaleY,
                                       ID3D12Resource* OutDepth, ID3D12Resource* OutMotion)
{
    if (!InLayout.active || InDepthWidth == 0 || InDepthHeight == 0 || InMotionWidth == 0 || InMotionHeight == 0)
        return false;

    auto constants = DlssNr::Periphery::MakeConstants(InLayout);
    constants.OutWidth = InLayout.modelW;
    constants.OutHeight = InLayout.modelH;
    constants.DepthBaseX = InDepthBaseX;
    constants.DepthBaseY = InDepthBaseY;
    constants.DepthWidth = InDepthWidth;
    constants.DepthHeight = InDepthHeight;
    constants.MotionBaseX = InMotionBaseX;
    constants.MotionBaseY = InMotionBaseY;
    constants.MotionWidth = InMotionWidth;
    constants.MotionHeight = InMotionHeight;
    constants.MotionScaleX = InMotionScaleX;
    constants.MotionScaleY = InMotionScaleY;

    return Record(InCmdList, _guidesPipeline, constants, InDepth, InMotion, OutDepth, OutMotion);
}

bool DlssNr_Periphery_Dx12::Unpack(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                                   ID3D12Resource* InPackedProxy, ID3D12Resource* InPackedAnswer,
                                   ID3D12Resource* OutProxy, ID3D12Resource* OutAnswer, uint32_t InGridWidth,
                                   uint32_t InGridHeight)
{
    if (!InLayout.active)
        return false;

    auto constants = DlssNr::Periphery::MakeConstants(InLayout);
    constants.OutWidth = InGridWidth;
    constants.OutHeight = InGridHeight;

    return Record(InCmdList, _unpackPipeline, constants, InPackedProxy, InPackedAnswer, OutProxy, OutAnswer);
}
