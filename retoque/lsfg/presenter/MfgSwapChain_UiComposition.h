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
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_UiComposition.h and
// _DebugPacing.h. The pipelines and descriptor heaps FFX keeps in file statics belong to one swapchain here
// (UiCompositionGpu, DebugPacingGpu), so the swapchain that releases them is the one that used them.

#pragma once

#include <d3d12.h>

#include <ffx_api.h>
#include <ffx_framegeneration.h>

namespace LsfgMfg
{

// SDK 2.3.0's ffx_framegeneration.h:246-253; the ffx-api headers OptiScaler builds against predate it. Attached to
// ffxCallbackDescFrameGenerationPresent::header.pNext.
#define LSFG_MFG_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA 0x0002000du
struct CallbackDescFrameGenerationPresentPremulAlpha
{
    ffxApiHeader header;
    bool usePremulAlpha;
};

// The UI composition pass's objects for one swapchain. Released by the swapchain after its threads have exited and its
// queues have drained.
struct UiCompositionGpu
{
    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    ID3D12PipelineState* premulPipeline = nullptr;
    uint32_t descRingBufferBase = 0;
    ID3D12DescriptorHeap* descRingBuffer = nullptr;
    uint32_t nextRtvDescriptor = 0;
    ID3D12DescriptorHeap* descHeapRtvCpu = nullptr;

    void release();
};

// The default present callback: copies the frame to the real backbuffer and composes the UI layer over it (FFX's
// ffxFrameInterpolationUiComposition). userCtx is the swapchain's UiCompositionGpu.
ffxReturnCode_t UiComposition(ffxCallbackDescFrameGenerationPresent* params, void* userCtx);

// The debug pacing bars ([FrameGen] DebugPacingLines): two 16 px columns alternating colour every presented frame.
struct DebugPacingGpu
{
    static constexpr uint32_t kConstantBuffers = 16;

    ID3D12RootSignature* rootSignature = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    uint32_t descRingBufferBase = 0;
    ID3D12DescriptorHeap* descRingBuffer = nullptr;
    uint32_t nextRtvDescriptor = 0;
    ID3D12DescriptorHeap* descHeapRtvCpu = nullptr;
    ID3D12Resource* constantBuffer[kConstantBuffers] = {};
    uint32_t frameIndex = 0;

    void release();
};

ffxReturnCode_t DebugPacing(ffxCallbackDescFrameGenerationPresent* params, DebugPacingGpu& gpu);

// FfxApiResourceState bits to D3D12 (PRESENT and COMMON are both D3D12's 0).
D3D12_RESOURCE_STATES ToD3D12State(uint32_t ffxState);

} // namespace LsfgMfg
