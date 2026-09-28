#include "pch.h"

#include "DlssNr_GuideMatch_Dx12.h"

#include <cstring>

// The resample's bytecode, when the build carries it. See the header: its absence is a supported state,
// not an error, and the build must not depend on it.
#if __has_include("precompile/DlssNr_Guides_Shader.h")
#include "precompile/DlssNr_Guides_Shader.h"
#define DLSSNR_GUIDE_MATCH_BYTECODE 1
#else
#define DLSSNR_GUIDE_MATCH_BYTECODE 0
#endif

namespace
{
// One ordered list with the cbuffer in dlssnr_guides.hlsl; twelve scalars, sixteen-byte rows.
struct GuideMatchConstants
{
    uint32_t DepthBaseX;
    uint32_t DepthBaseY;
    uint32_t DepthWidth;
    uint32_t DepthHeight;
    uint32_t MotionBaseX;
    uint32_t MotionBaseY;
    uint32_t MotionWidth;
    uint32_t MotionHeight;
    uint32_t OutWidth;
    uint32_t OutHeight;
    uint32_t Pad0;
    uint32_t Pad1;
};

static_assert(sizeof(GuideMatchConstants) == 48, "GuideMatchConstants must match cbuffer Params");

constexpr uint32_t kSrvCount = 2;
constexpr uint32_t kUavCount = 2;
constexpr uint32_t kThreads = 8;
} // namespace

bool DlssNr_GuideMatch_Dx12::Available() { return DLSSNR_GUIDE_MATCH_BYTECODE != 0; }

DlssNr_GuideMatch_Dx12::DlssNr_GuideMatch_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr || !Available())
        return;

    // Two inputs, two outputs, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GuideMatchConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_GUIDE_MATCH_HEAPS; ++i)
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

#if DLSSNR_GUIDE_MATCH_BYTECODE
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_Guides_cso, sizeof(DlssNr_Guides_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_GUIDE_MATCH_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_GUIDE_MATCH_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(GuideMatchConstants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _init = true;
}

DlssNr_GuideMatch_Dx12::~DlssNr_GuideMatch_Dx12()
{
    for (uint32_t i = 0; i < DLSSNR_GUIDE_MATCH_HEAPS; ++i)
    {
        if (_constantBuffers[i] == nullptr)
            continue;

        if (_mappedConstants[i] != nullptr)
        {
            _constantBuffers[i]->Unmap(0, nullptr);
            _mappedConstants[i] = nullptr;
        }

        _constantBuffers[i]->Release();
        _constantBuffers[i] = nullptr;
    }
}

bool DlssNr_GuideMatch_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InDepth,
                                      ID3D12Resource* InMotion, uint32_t InDepthBaseX, uint32_t InDepthBaseY,
                                      uint32_t InDepthWidth, uint32_t InDepthHeight, uint32_t InMotionBaseX,
                                      uint32_t InMotionBaseY, uint32_t InMotionWidth, uint32_t InMotionHeight,
                                      ID3D12Resource* OutDepth, ID3D12Resource* OutMotion, uint32_t InOutWidth,
                                      uint32_t InOutHeight)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InDepth == nullptr || InMotion == nullptr ||
        OutDepth == nullptr || OutMotion == nullptr || InDepthWidth == 0 || InDepthHeight == 0 || InMotionWidth == 0 ||
        InMotionHeight == 0 || InOutWidth == 0 || InOutHeight == 0)
    {
        return false;
    }

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_GUIDE_MATCH_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    // Views are re-created every call: one dispatch a frame, and the guides the game hands over can be
    // a different texture from one frame to the next.
    CreateShaderResourceView(_device, InDepth, heap.GetSrvCPU(0), DXGI_FORMAT_UNKNOWN, true);
    CreateShaderResourceView(_device, InMotion, heap.GetSrvCPU(1), DXGI_FORMAT_UNKNOWN, true);
    CreateUnorderedAccessView(_device, OutDepth, heap.GetUavCPU(0), 0, true);
    CreateUnorderedAccessView(_device, OutMotion, heap.GetUavCPU(1), 0, true);

    const GuideMatchConstants constants { InDepthBaseX,
                                          InDepthBaseY,
                                          InDepthWidth,
                                          InDepthHeight,
                                          InMotionBaseX,
                                          InMotionBaseY,
                                          InMotionWidth,
                                          InMotionHeight,
                                          InOutWidth,
                                          InOutHeight,
                                          0,
                                          0 };
    std::memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((InOutWidth + kThreads - 1) / kThreads, (InOutHeight + kThreads - 1) / kThreads, 1);

    // The model reads what was just written; the caller's transition out of UNORDERED_ACCESS orders that,
    // and this orders the two writes against anything else that reads them first.
    D3D12_RESOURCE_BARRIER written[2] {};
    written[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[0].UAV.pResource = OutDepth;
    written[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written[1].UAV.pResource = OutMotion;
    InCmdList->ResourceBarrier(2, written);

    return true;
}
