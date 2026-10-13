#pragma once

// HDR10 and scRGB at present ([DlssNr] HdrPresent, docs/dlssnr/design/hdr-present-conversion.md): the decode before
// the composition and the encode after it, one shader (precompile/dlssnr_present_hdr.hlsl) in two modes. The maths
// and the numbers it shares with the hosts are dlssnr/DlssNr_PresentHdr.h.
//
// Dispatches only. The resources are the host's -- the backbuffer's copy, the FP16 work texture, the output in the
// swapchain's format -- and so are their barriers, because the two hosts rest them differently. Each call says the
// states it needs; nothing here transitions anything. One object per device; nothing in it depends on the frame's
// size, so a resize does not rebuild it.
//
// Its bytecode may be absent: DlssNr_PresentHdr_Dx12.cpp includes the precompiled header only if it exists, and without
// it Available() is false, an HDR swapchain stays declined and the menu offers nothing.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

#include <dlssnr/DlssNr_PresentHdr.h>

// Two dispatches a frame, reused round-robin with no fence: sixteen frames of coverage, twice what the composition
// keeps for frame generation running the GPU behind the CPU, and four times the present route's list ring.
#define DLSSNR_PRESENT_HDR_NUM_OF_HEAPS 32

class DlssNr_PresentHdr_Dx12 : public Shader_Dx12
{
  public:
    // The Params cbuffer of dlssnr_present_hdr.hlsl, in order, padded to the 256 bytes every constant-buffer view must
    // be sized to. Append only, at the end of both (nr-invariants holds the two to each other).
    struct alignas(256) Constants
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t Profile = 0; // DlssNr::PresentHdr::kProfileHdr10 or kProfileScRgb
        uint32_t Mode = 0;    // DlssNr::PresentHdr::kModeDecode or kModeEncode
    };

    // Whether this build carries the shader.
    static bool Available();

    DlssNr_PresentHdr_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_PresentHdr_Dx12();

    bool BuiltFor(ID3D12Device* device) const { return device == _device; }

    // Whether this device can run the conversion on a frame of this format: the format loadable through a shader
    // resource view, and both it and the FP16 work format writable through a typed UAV store
    // (D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE). Asked of the device once per format and remembered. Proton is not a
    // validator: native D3D12 answers this, and a format it refuses is declined here, not dispatched.
    bool Supports(DXGI_FORMAT format);

    // The original, in the swapchain's format, into the FP16 work texture as linear Rec.709 in scRGB units.
    // original in NON_PIXEL_SHADER_RESOURCE, work in UNORDERED_ACCESS; both left so. The caller owes a barrier between
    // this and the work texture's next use.
    bool Decode(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original, ID3D12Resource* work, uint32_t profile);

    // The work texture back into out, in the swapchain's format, with the original as the reference: a texel the
    // composition left alone is written as the original's code. original and work in NON_PIXEL_SHADER_RESOURCE, out in
    // UNORDERED_ACCESS; all left so. out is the same size and format as original.
    bool Encode(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original, ID3D12Resource* work, ID3D12Resource* out,
                uint32_t profile);

  private:
    bool Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original, ID3D12Resource* processed,
                ID3D12Resource* out, uint32_t profile, uint32_t mode);

    FrameDescriptorHeap _frameHeaps[DLSSNR_PRESENT_HDR_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_PRESENT_HDR_NUM_OF_HEAPS] = {};
    void* _mappedConstants[DLSSNR_PRESENT_HDR_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // Supports() answers, by format: what was asked, and the answer.
    struct FormatAnswer
    {
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool supported = false;
    };
    FormatAnswer _answers[4] = {};
    bool _workSupported = false;
};
