#pragma once

// Peripheral compression's three passes on D3D12: pack the colour, pack the guides, unpack the model's
// answer. dlssnr/design/peripheral-compression.md.
//
// A pass of its own rather than modes of the composition shader, so its bytecode can be absent: the .cpp
// includes the three precompiled headers only if they exist, and without all three Available() is false, the
// layout is refused with that reason, and the composition runs exactly as it did. A stale composition shader
// running modes it does not know would be the silent version of that (reduced-scale-guides.md made the same
// choice for the guide resample).

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include "DlssNr_Periphery.h"

// Three dispatches a frame, reused round-robin without a fence: thirty-two slots is ten frames of coverage,
// more than the composition pass's ring keeps.
#define DLSSNR_PERIPHERY_HEAPS 32

class DlssNr_Periphery_Dx12 : public Shader_Dx12
{
    FrameDescriptorHeap _frameHeaps[DLSSNR_PERIPHERY_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_PERIPHERY_HEAPS] = {};
    void* _mappedConstants[DLSSNR_PERIPHERY_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // _pipelineState (the base's) is the colour pack; these are the other two.
    ID3D12PipelineState* _guidesPipeline = nullptr;
    ID3D12PipelineState* _unpackPipeline = nullptr;

    // Every descriptor in the table is written for every dispatch: a pass that reads one input binds it to
    // both SRV slots, and one that writes one output binds it to both UAV slots. An unwritten descriptor in a
    // static table is undefined, and on native D3D12 that is a removed device.
    bool Record(ID3D12GraphicsCommandList* InCmdList, ID3D12PipelineState* InPipeline,
                const DlssNr::Periphery::PeripheryConstants& InConstants, ID3D12Resource* InSource0,
                ID3D12Resource* InSource1, ID3D12Resource* OutTarget0, ID3D12Resource* OutTarget1);

  public:
    // Whether this build carries all three passes' bytecode.
    static bool Available();

    DlssNr_Periphery_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Periphery_Dx12();

    // Inputs arrive readable (NON_PIXEL_SHADER_RESOURCE) and are left so; outputs arrive and are left in
    // UNORDERED_ACCESS, with a UAV barrier after the write, the caller moving them for the next read. Each
    // binds its own descriptor heap, root signature and pipeline; the caller re-binds its own afterwards.

    // The frame's proxy (frame size) -> the packed proxy (the layout's model extent).
    bool PackColour(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                    ID3D12Resource* InProxy, ID3D12Resource* OutPacked);

    // Depth and motion, from the regions the model would otherwise have been handed, -> packed depth and
    // packed motion in packed pixels. InMotionScale converts the game's vectors to frame pixels
    // (DlssNr::Periphery::FrameMotionScale). Regions must be non-empty and inside their textures.
    bool PackGuides(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                    ID3D12Resource* InDepth, ID3D12Resource* InMotion, uint32_t InDepthBaseX, uint32_t InDepthBaseY,
                    uint32_t InDepthWidth, uint32_t InDepthHeight, uint32_t InMotionBaseX, uint32_t InMotionBaseY,
                    uint32_t InMotionWidth, uint32_t InMotionHeight, float InMotionScaleX, float InMotionScaleY,
                    ID3D12Resource* OutDepth, ID3D12Resource* OutMotion);

    // The packed proxy and the packed answer -> the same pair on the uniform grid (InGridWidth x InGridHeight).
    bool Unpack(ID3D12GraphicsCommandList* InCmdList, const DlssNr::Periphery::Layout& InLayout,
                ID3D12Resource* InPackedProxy, ID3D12Resource* InPackedAnswer, ID3D12Resource* OutProxy,
                ID3D12Resource* OutAnswer, uint32_t InGridWidth, uint32_t InGridHeight);
};
