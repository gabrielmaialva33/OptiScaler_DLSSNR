#pragma once

// The guide resample for the NR model below its frame size: depth and motion vectors, point-sampled
// from the upscaler's region into two working-size textures. design/reduced-scale-guides.md.
//
// A pass of its own rather than a mode of the composition shader, so that its bytecode can be absent:
// DlssNr_GuideMatch_Dx12.cpp includes the precompiled header only if it exists, and without it
// Available() is false, the pass is never built, and the model keeps the frame-size guides it always
// had. A stale composition shader running a mode it does not know would be the silent version of that.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// One dispatch per frame, reused round-robin without a fence, like the composition pass's ring.
#define DLSSNR_GUIDE_MATCH_HEAPS 16

class DlssNr_GuideMatch_Dx12 : public Shader_Dx12
{
    FrameDescriptorHeap _frameHeaps[DLSSNR_GUIDE_MATCH_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_GUIDE_MATCH_HEAPS] = {};
    void* _mappedConstants[DLSSNR_GUIDE_MATCH_HEAPS] = {};
    uint32_t _heapIndex = 0;

  public:
    // Whether this build carries the resample's bytecode at all.
    static bool Available();

    DlssNr_GuideMatch_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_GuideMatch_Dx12();

    // Depth and motion arrive readable (NON_PIXEL_SHADER_RESOURCE) and are left so; the two outputs
    // arrive and are left in UNORDERED_ACCESS, the caller moving them for the model's read. Regions
    // must be non-empty and inside their textures; the caller has already bounded them.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InDepth, ID3D12Resource* InMotion,
                  uint32_t InDepthBaseX, uint32_t InDepthBaseY, uint32_t InDepthWidth, uint32_t InDepthHeight,
                  uint32_t InMotionBaseX, uint32_t InMotionBaseY, uint32_t InMotionWidth, uint32_t InMotionHeight,
                  ID3D12Resource* OutDepth, ID3D12Resource* OutMotion, uint32_t InOutWidth, uint32_t InOutHeight);
};
