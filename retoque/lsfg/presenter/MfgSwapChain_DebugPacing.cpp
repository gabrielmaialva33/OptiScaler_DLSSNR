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
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_DebugPacing.cpp.
// The objects FFX keeps in file statics live in a DebugPacingGpu owned by the swapchain; the shader bytecode headers
// (verbatim) are under precompile/.

#include "pch.h"

#include "MfgSwapChain_UiComposition.h"
#include "MfgSwapChain_Helpers.h"

namespace LsfgMfg
{

namespace DebugPacingShaders
{
#include "precompile/FrameInterpolationSwapchainDebugPacingVS.h"
#include "precompile/FrameInterpolationSwapchainDebugPacingPS.h"
} // namespace DebugPacingShaders

namespace
{
typedef HRESULT(__stdcall* D3D12SerializeVersionedRootSignatureType)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* pRootSignature, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);

// DXGI_MAX_SWAP_CHAIN_BUFFERS real frames (i.e. * 2), 1 CBV each should be enough
const uint32_t s_debugPacingDescRingBufferSize = DXGI_MAX_SWAP_CHAIN_BUFFERS * 2 * 1;
const uint32_t s_debugPacingDescHeapRtvSize = DXGI_MAX_SWAP_CHAIN_BUFFERS * 2;
const unsigned int s_debugPacingConstantBufferSize = 256u;

bool CreateDebugPacingPipeline(ID3D12Device* dx12Device, DXGI_FORMAT fmt, DebugPacingGpu& gpu)
{
    D3D12_DESCRIPTOR_RANGE1 range;
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range.NumDescriptors = 1;
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
    rasterDesc.DepthClipEnable = FALSE;
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
    depthStencilDesc.DepthEnable = FALSE;
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

    dx12PipelineStateDescription.VS.pShaderBytecode = DebugPacingShaders::g_mainVS;
    dx12PipelineStateDescription.VS.BytecodeLength = sizeof(DebugPacingShaders::g_mainVS);
    dx12PipelineStateDescription.PS.pShaderBytecode = DebugPacingShaders::g_mainPS;
    dx12PipelineStateDescription.PS.BytecodeLength = sizeof(DebugPacingShaders::g_mainPS);

    return SUCCEEDED(
        dx12Device->CreateGraphicsPipelineState(&dx12PipelineStateDescription, IID_PPV_ARGS(&gpu.pipeline)));
}

bool verifyDebugPacingGpuResources(ID3D12Device* dx12Device, DXGI_FORMAT fmt, DebugPacingGpu& gpu)
{
    if (nullptr == gpu.pipeline)
    {
        SafeRelease(gpu.rootSignature);
        if (!CreateDebugPacingPipeline(dx12Device, fmt, gpu))
            return false;
    }

    if (nullptr == gpu.descRingBuffer)
    {
        D3D12_DESCRIPTOR_HEAP_DESC descHeap = {};
        descHeap.NumDescriptors = s_debugPacingDescRingBufferSize;
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
        descHeap.NumDescriptors = s_debugPacingDescHeapRtvSize;
        descHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        descHeap.NodeMask = 0;
        descHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&gpu.descHeapRtvCpu))))
            return false;
    }

    if (nullptr == gpu.constantBuffer[DebugPacingGpu::kConstantBuffers - 1])
    {
        D3D12_RESOURCE_DESC constantBufferDesc = {};
        constantBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        constantBufferDesc.Width = s_debugPacingConstantBufferSize;
        constantBufferDesc.Height = 1;
        constantBufferDesc.DepthOrArraySize = 1;
        constantBufferDesc.MipLevels = 1;
        constantBufferDesc.SampleDesc.Count = 1;
        constantBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        constantBufferDesc.Format = DXGI_FORMAT_UNKNOWN;

        D3D12_HEAP_PROPERTIES heapProperties = {};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

        for (uint32_t i = 0; i < DebugPacingGpu::kConstantBuffers; ++i)
        {
            if (gpu.constantBuffer[i] != nullptr)
                continue;
            if (FAILED(dx12Device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &constantBufferDesc,
                                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                           IID_PPV_ARGS(&gpu.constantBuffer[i]))))
            {
                return false;
            }
        }

        gpu.frameIndex = 0;
    }

    return true;
}
} // namespace

void DebugPacingGpu::release()
{
    for (uint32_t i = 0; i < kConstantBuffers; ++i)
    {
        SafeRelease(constantBuffer[i]);
    }
    frameIndex = 0;

    SafeRelease(rootSignature);
    SafeRelease(pipeline);

    SafeRelease(descRingBuffer);
    descRingBufferBase = 0;

    SafeRelease(descHeapRtvCpu);
    nextRtvDescriptor = 0;
}

ffxReturnCode_t DebugPacing(ffxCallbackDescFrameGenerationPresent* params, DebugPacingGpu& gpu)
{
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(params->device);
    ID3D12Resource* pRtResource = (ID3D12Resource*) (params->currentBackBuffer.resource);
    ID3D12GraphicsCommandList* pCmdList = reinterpret_cast<ID3D12GraphicsCommandList*>(params->commandList);

    if (dx12Device == nullptr || pRtResource == nullptr || pCmdList == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;

    D3D12_RESOURCE_DESC desc = pRtResource->GetDesc();

    if (!verifyDebugPacingGpuResources(dx12Device, desc.Format, gpu))
        return FFX_API_RETURN_ERROR;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = pRtResource;
    barrier.Transition.StateBefore = ToD3D12State(params->currentBackBuffer.state);
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
    {
        pCmdList->ResourceBarrier(1, &barrier);
    }

    // set root signature
    pCmdList->SetGraphicsRootSignature(gpu.rootSignature);

    // set descriptor heap
    ID3D12DescriptorHeap* dx12DescriptorHeap = gpu.descRingBuffer;
    pCmdList->SetDescriptorHeaps(1, &dx12DescriptorHeap);

    const UINT cbvIncrement = dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // set up the descriptor table
    D3D12_GPU_DESCRIPTOR_HANDLE gpuView = dx12DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    gpuView.ptr += gpu.descRingBufferBase * cbvIncrement;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuView = dx12DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    cpuView.ptr += gpu.descRingBufferBase * cbvIncrement;

    const uint32_t buffer = gpu.frameIndex % DebugPacingGpu::kConstantBuffers;
    UINT8* gpuData = nullptr;
    if (SUCCEEDED(gpu.constantBuffer[buffer]->Map(0, nullptr, reinterpret_cast<void**>(&gpuData))))
    {
        memcpy(gpuData, &gpu.frameIndex, sizeof(gpu.frameIndex));
        gpu.constantBuffer[buffer]->Unmap(0, nullptr);
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC dx12CbvDescription = {};
    dx12CbvDescription.BufferLocation = gpu.constantBuffer[buffer]->GetGPUVirtualAddress();
    dx12CbvDescription.SizeInBytes = s_debugPacingConstantBufferSize;
    dx12Device->CreateConstantBufferView(&dx12CbvDescription, cpuView);

    // FFX counts modulo its buffer count, which flips the colour pair's phase at the wrap whenever that count is
    // odd; counting freely keeps the alternation exact. The shader reads only the low bit.
    gpu.frameIndex++;

    gpu.descRingBufferBase = (gpu.descRingBufferBase + 1) % s_debugPacingDescRingBufferSize;
    pCmdList->SetGraphicsRootDescriptorTable(0, gpuView);

    D3D12_RENDER_TARGET_VIEW_DESC colorDesc = {};
    colorDesc.Format = desc.Format;
    colorDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    colorDesc.Texture2D.MipSlice = 0;
    colorDesc.Texture2D.PlaneSlice = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRTV = gpu.descHeapRtvCpu->GetCPUDescriptorHandleForHeapStart();
    backbufferRTV.ptr +=
        gpu.nextRtvDescriptor * dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gpu.nextRtvDescriptor = (gpu.nextRtvDescriptor + 1) % s_debugPacingDescHeapRtvSize;
    dx12Device->CreateRenderTargetView(pRtResource, &colorDesc, backbufferRTV);

    D3D12_VIEWPORT vpd = { 0.0f, 0.0f, 32, static_cast<float>(desc.Height), 0.0f, 1.0f };
    D3D12_RECT srd = { 0, 0, (LONG) desc.Width, (LONG) desc.Height };

    pCmdList->OMSetRenderTargets(1, &backbufferRTV, true, NULL);
    pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pCmdList->SetPipelineState(gpu.pipeline);
    pCmdList->RSSetViewports(1, &vpd);
    pCmdList->RSSetScissorRects(1, &srd);
    pCmdList->DrawInstanced(3, 1, 0, 0);

    if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
    {
        D3D12_RESOURCE_STATES tmpStateBefore = barrier.Transition.StateBefore;
        barrier.Transition.StateBefore = barrier.Transition.StateAfter;
        barrier.Transition.StateAfter = tmpStateBefore;

        pCmdList->ResourceBarrier(1, &barrier);
    }

    return FFX_API_RETURN_OK;
}

} // namespace LsfgMfg
