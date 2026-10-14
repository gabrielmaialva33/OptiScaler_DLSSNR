// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// Modified for Retoque's LSFG study (branch study/lsfg-d3d12, tools/lsfg-study/MFG_PRESENTER.md): copied from
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_UiComposition.cpp.
// The objects FFX keeps in file statics live in a UiCompositionGpu owned by the swapchain, the SRV formats come from
// the resources themselves instead of the FFX backend's format table, and the shader bytecode headers (verbatim) are
// under precompile/.

#include "pch.h"

#include "MfgSwapChain_UiComposition.h"
#include "MfgSwapChain_Helpers.h"

namespace LsfgMfg
{

namespace UiCompositionShaders
{
#include "precompile/FrameInterpolationSwapchainUiCompositionVS.h"
#include "precompile/FrameInterpolationSwapchainUiCompositionPS.h"
} // namespace UiCompositionShaders

namespace UiCompositionPremulShaders
{
#include "precompile/FrameInterpolationSwapchainUiCompositionPremulVS.h"
#include "precompile/FrameInterpolationSwapchainUiCompositionPremulPS.h"
} // namespace UiCompositionPremulShaders

namespace
{
typedef HRESULT(__stdcall* D3D12SerializeVersionedRootSignatureType)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);

// DXGI_MAX_SWAP_CHAIN_BUFFERS real frames (i.e. * 2), 2 SRV each should be enough
const uint32_t s_uiCompositionDescRingBufferSize = DXGI_MAX_SWAP_CHAIN_BUFFERS * 2 * 2;
const uint32_t s_uiCompositionDescHeapRtvSize = DXGI_MAX_SWAP_CHAIN_BUFFERS * 2;

// create the pipeline state to use for UI composition
// pretty similar to FfxCreatePipelineFunc
bool CreateUiCompositionPipeline(ID3D12Device* dx12Device, DXGI_FORMAT fmt, UiCompositionGpu& gpu)
{
    D3D12_DESCRIPTOR_RANGE1 range;
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER1 rootParameters;
    rootParameters.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters.DescriptorTable.NumDescriptorRanges = 1;
    rootParameters.DescriptorTable.pDescriptorRanges = &range;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootSignatureDesc.Desc_1_1.NumParameters = 1;
    rootSignatureDesc.Desc_1_1.pParameters = &rootParameters;
    rootSignatureDesc.Desc_1_1.NumStaticSamplers = 0;
    rootSignatureDesc.Desc_1_1.pStaticSamplers = nullptr;
    rootSignatureDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3D12RootSignature* dx12RootSignature = nullptr;
    ID3DBlob* signature = nullptr;
    ID3DBlob* error = nullptr;
    HMODULE d3d12ModuleHandle = GetModuleHandleW(L"D3D12.dll");
    if (NULL == d3d12ModuleHandle)
        return false;

    D3D12SerializeVersionedRootSignatureType d3d12SerializeVersionedRootSignatureFunc =
        (D3D12SerializeVersionedRootSignatureType) GetProcAddress(d3d12ModuleHandle,
                                                                  "D3D12SerializeVersionedRootSignature");
    if (nullptr == d3d12SerializeVersionedRootSignatureFunc)
        return false;

    HRESULT result = d3d12SerializeVersionedRootSignatureFunc(&rootSignatureDesc, &signature, &error);
    if (FAILED(result))
    {
        SafeRelease(signature);
        SafeRelease(error);
        return false;
    }

    result = dx12Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                             IID_PPV_ARGS(&dx12RootSignature));
    SafeRelease(signature);
    SafeRelease(error);

    if (FAILED(result))
        return false;

    // create the PSO
    D3D12_RASTERIZER_DESC rasterDesc;
    rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.ForcedSampleCount = 0;
    rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    D3D12_BLEND_DESC blendDesc;
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = {
        FALSE,
        FALSE,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE,
        D3D12_BLEND_ZERO,
        D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_DEPTH_STENCIL_DESC depthStencilDesc;
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depthStencilDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC defaultStencilOp = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                                          D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    depthStencilDesc.FrontFace = defaultStencilOp;
    depthStencilDesc.BackFace = defaultStencilOp;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC dx12PipelineStateDescription = {};
    dx12PipelineStateDescription.RasterizerState = rasterDesc;
    dx12PipelineStateDescription.BlendState = blendDesc;
    dx12PipelineStateDescription.DepthStencilState = depthStencilDesc;
    dx12PipelineStateDescription.DepthStencilState.DepthEnable = FALSE;
    dx12PipelineStateDescription.SampleMask = UINT_MAX;
    dx12PipelineStateDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    dx12PipelineStateDescription.SampleDesc = { 1, 0 };
    dx12PipelineStateDescription.NumRenderTargets = 1;
    dx12PipelineStateDescription.RTVFormats[0] = fmt;

    dx12PipelineStateDescription.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    dx12PipelineStateDescription.pRootSignature = dx12RootSignature;

    gpu.rootSignature = dx12RootSignature;

    {
        dx12PipelineStateDescription.VS.pShaderBytecode = UiCompositionShaders::g_mainVS;
        dx12PipelineStateDescription.VS.BytecodeLength = sizeof(UiCompositionShaders::g_mainVS);
        dx12PipelineStateDescription.PS.pShaderBytecode = UiCompositionShaders::g_mainPS;
        dx12PipelineStateDescription.PS.BytecodeLength = sizeof(UiCompositionShaders::g_mainPS);

        if (FAILED(dx12Device->CreateGraphicsPipelineState(&dx12PipelineStateDescription, IID_PPV_ARGS(&gpu.pipeline))))
            return false;
    }

    {
        dx12PipelineStateDescription.VS.pShaderBytecode = UiCompositionPremulShaders::g_mainVS;
        dx12PipelineStateDescription.VS.BytecodeLength = sizeof(UiCompositionPremulShaders::g_mainVS);
        dx12PipelineStateDescription.PS.pShaderBytecode = UiCompositionPremulShaders::g_mainPS;
        dx12PipelineStateDescription.PS.BytecodeLength = sizeof(UiCompositionPremulShaders::g_mainPS);

        if (FAILED(dx12Device->CreateGraphicsPipelineState(&dx12PipelineStateDescription,
                                                           IID_PPV_ARGS(&gpu.premulPipeline))))
            return false;
    }

    return true;
}

bool verifyUiBlitGpuResources(ID3D12Device* dx12Device, DXGI_FORMAT fmt, UiCompositionGpu& gpu)
{
    if (nullptr == gpu.pipeline || nullptr == gpu.premulPipeline)
    {
        // A failed earlier attempt may have left part of it.
        SafeRelease(gpu.rootSignature);
        SafeRelease(gpu.pipeline);
        SafeRelease(gpu.premulPipeline);

        if (!CreateUiCompositionPipeline(dx12Device, fmt, gpu))
            return false;
    }

    if (nullptr == gpu.descRingBuffer)
    {
        D3D12_DESCRIPTOR_HEAP_DESC descHeap = {};
        descHeap.NumDescriptors = s_uiCompositionDescRingBufferSize;
        descHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        descHeap.NodeMask = 0;
        gpu.descRingBufferBase = 0;
        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&gpu.descRingBuffer))))
            return false;
    }

    if (nullptr == gpu.descHeapRtvCpu)
    {
        D3D12_DESCRIPTOR_HEAP_DESC descHeap = {};
        descHeap.NumDescriptors = s_uiCompositionDescHeapRtvSize;
        descHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descHeap.NodeMask = 0;
        descHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&gpu.descHeapRtvCpu))))
            return false;
    }

    return true;
}
} // namespace

D3D12_RESOURCE_STATES ToD3D12State(uint32_t ffxState)
{
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (ffxState & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (ffxState & FFX_API_RESOURCE_STATE_COMPUTE_READ)
        state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_PIXEL_READ)
        state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_COPY_SRC)
        state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_COPY_DEST)
        state |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (ffxState & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT)
        state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (ffxState & FFX_API_RESOURCE_STATE_RENDER_TARGET)
        state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return state;
}

void UiCompositionGpu::release()
{
    SafeRelease(rootSignature);
    SafeRelease(pipeline);
    SafeRelease(premulPipeline);

    SafeRelease(descRingBuffer);
    descRingBufferBase = 0;

    SafeRelease(descHeapRtvCpu);
    nextRtvDescriptor = 0;
}

ffxReturnCode_t UiComposition(ffxCallbackDescFrameGenerationPresent* params, void* userCtx)
{
    auto* gpu = static_cast<UiCompositionGpu*>(userCtx);
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(params->device);
    ID3D12Resource* pRtResource = (ID3D12Resource*) (params->outputSwapChainBuffer.resource);

    if (gpu == nullptr || dx12Device == nullptr || pRtResource == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;

    // blit backbuffer and composit UI using a VS/PS pass
    D3D12_RESOURCE_DESC desc = pRtResource->GetDesc();

    ID3D12GraphicsCommandList* pCmdList = reinterpret_cast<ID3D12GraphicsCommandList*>(params->commandList);
    ID3D12Resource* pResBackbuffer = (ID3D12Resource*) (params->currentBackBuffer.resource);
    ID3D12Resource* pResUI = (ID3D12Resource*) (params->currentUI.resource);

    if (pCmdList == nullptr || pResBackbuffer == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;

    // The pipelines are needed only to compose a UI layer: a plain copy needs none of them.
    if (nullptr != pResUI && !verifyUiBlitGpuResources(dx12Device, desc.Format, *gpu))
        pResUI = nullptr;

    ID3D12PipelineState* dx12PipelineStateObject = gpu->pipeline;

    for (auto it = &params->header; it; it = it->pNext)
    {
        if (it->type == LSFG_MFG_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA)
        {
            auto* premulAlpha = reinterpret_cast<CallbackDescFrameGenerationPresentPremulAlpha*>(it);
            dx12PipelineStateObject = premulAlpha->usePremulAlpha ? gpu->premulPipeline : gpu->pipeline;
            break;
        }
    }

    if (nullptr == pResUI)
    {
        // just do a resource copy to the real swapchain
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = pResBackbuffer;
        barriers[0].Transition.StateBefore = ToD3D12State(params->currentBackBuffer.state);
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = pRtResource;
        barriers[1].Transition.StateBefore = ToD3D12State(params->outputSwapChainBuffer.state);
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        pCmdList->ResourceBarrier(_countof(barriers), barriers);

        pCmdList->CopyResource(pRtResource, pResBackbuffer);

        for (size_t i = 0; i < _countof(barriers); ++i)
        {
            D3D12_RESOURCE_STATES tmpStateBefore = barriers[i].Transition.StateBefore;
            barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
            barriers[i].Transition.StateAfter = tmpStateBefore;
        }

        pCmdList->ResourceBarrier(_countof(barriers), barriers);
    }
    else
    {
        uint32_t barrierCount = 0;
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = pResBackbuffer;
        barriers[barrierCount].Transition.StateBefore = ToD3D12State(params->currentBackBuffer.state);
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (barriers[barrierCount].Transition.StateBefore != barriers[barrierCount].Transition.StateAfter)
        {
            ++barrierCount;
        }
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = pResUI;
        barriers[barrierCount].Transition.StateBefore = ToD3D12State(params->currentUI.state);
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (barriers[barrierCount].Transition.StateBefore != barriers[barrierCount].Transition.StateAfter)
        {
            ++barrierCount;
        }
        barriers[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[barrierCount].Transition.pResource = pRtResource;
        barriers[barrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[barrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (barriers[barrierCount].Transition.StateBefore != barriers[barrierCount].Transition.StateAfter)
        {
            ++barrierCount;
        }
        pCmdList->ResourceBarrier(barrierCount, barriers);

        // set root signature
        pCmdList->SetGraphicsRootSignature(gpu->rootSignature);

        // set descriptor heap
        ID3D12DescriptorHeap* dx12DescriptorHeap = gpu->descRingBuffer;
        pCmdList->SetDescriptorHeaps(1, &dx12DescriptorHeap);

        D3D12_RENDER_TARGET_VIEW_DESC colorDesc = {};
        colorDesc.Format = desc.Format;
        colorDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        colorDesc.Texture2D.MipSlice = 0;
        colorDesc.Texture2D.PlaneSlice = 0;

        const UINT srvIncrement = dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // set up the descriptor table
        D3D12_GPU_DESCRIPTOR_HANDLE gpuView = dx12DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        gpuView.ptr += gpu->descRingBufferBase * srvIncrement;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuView = dx12DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        cpuView.ptr += gpu->descRingBufferBase * srvIncrement;

        const D3D12_RESOURCE_DESC backbufferDesc = pResBackbuffer->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC dx12SrvDescription = {};
        dx12SrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dx12SrvDescription.Format = convertFormatSrv(backbufferDesc.Format);
        dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dx12SrvDescription.Texture2D.MipLevels = backbufferDesc.MipLevels;
        dx12Device->CreateShaderResourceView(pResBackbuffer, &dx12SrvDescription, cpuView);

        cpuView.ptr += srvIncrement;

        const D3D12_RESOURCE_DESC uiDesc = pResUI->GetDesc();
        dx12SrvDescription.Format = convertFormatSrv(uiDesc.Format);
        dx12SrvDescription.Texture2D.MipLevels = uiDesc.MipLevels;
        dx12Device->CreateShaderResourceView(pResUI, &dx12SrvDescription, cpuView);

        gpu->descRingBufferBase = (gpu->descRingBufferBase + 2) % s_uiCompositionDescRingBufferSize;
        pCmdList->SetGraphicsRootDescriptorTable(0, gpuView);

        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRTV = gpu->descHeapRtvCpu->GetCPUDescriptorHandleForHeapStart();
        backbufferRTV.ptr +=
            gpu->nextRtvDescriptor * dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        gpu->nextRtvDescriptor = (gpu->nextRtvDescriptor + 1) % s_uiCompositionDescHeapRtvSize;
        dx12Device->CreateRenderTargetView(pRtResource, &colorDesc, backbufferRTV);

        D3D12_VIEWPORT vpd = {
            0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f
        };
        D3D12_RECT srd = { 0, 0, (LONG) desc.Width, (LONG) desc.Height };

        pCmdList->OMSetRenderTargets(1, &backbufferRTV, true, NULL);
        pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        pCmdList->SetPipelineState(dx12PipelineStateObject);
        pCmdList->RSSetViewports(1, &vpd);
        pCmdList->RSSetScissorRects(1, &srd);
        pCmdList->DrawInstanced(3, 1, 0, 0);

        for (int i = 0; i < (int) barrierCount; ++i)
        {
            D3D12_RESOURCE_STATES tmpStateBefore = barriers[i].Transition.StateBefore;
            barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
            barriers[i].Transition.StateAfter = tmpStateBefore;
        }

        pCmdList->ResourceBarrier(barrierCount, barriers);
    }

    return FFX_API_RETURN_OK;
}

} // namespace LsfgMfg
