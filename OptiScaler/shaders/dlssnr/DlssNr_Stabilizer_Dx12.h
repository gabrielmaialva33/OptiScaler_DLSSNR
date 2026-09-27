#pragma once

// The output stabilizer for Neural Rendering: keeps the shown picture where the game's frame did not
// change and follows the model where it did. dlssnr/design/output-stabilizer.md is the reasoning.
//
// One object serves one output size and format on one device. Its history textures are sized at
// construction; a different output gets a new object, and the old one is parked with the pass's other
// retired objects rather than freed, because the recordings that used it may still be on the GPU.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// One dispatch per frame, reused round-robin with no fence, so this is sixteen frames of coverage --
// twice what the composition pass keeps for frame generation running the GPU behind the CPU.
#define DLSSNR_STABILIZER_NUM_OF_HEAPS 16

class DlssNr_Stabilizer_Dx12 : public Shader_Dx12
{
  private:
    // The Params cbuffer of dlssnr_stabilizer.hlsl, in order.
    struct alignas(256) Constants
    {
        float Strength = 0.0f;
        float Tolerance = 0.0f;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t Valid = 0;
        float QuantAbs = 0.0f;
        float QuantRelR = 0.0f;
        float QuantRelG = 0.0f;
        float QuantRelB = 0.0f;
        float WhitePoint = 1.0f;
        uint32_t Passthrough = 0;
    };

    FrameDescriptorHeap _frameHeaps[DLSSNR_STABILIZER_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_STABILIZER_NUM_OF_HEAPS] = {};
    void* _mappedConstants[DLSSNR_STABILIZER_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // Last frame's and this frame's, swapped every dispatch. Both rest in NON_PIXEL_SHADER_RESOURCE
    // between dispatches, so two dispatches on one command list need nothing but their own barriers.
    ID3D12Resource* _shown[2] = {};  // what went out, in the output's own format so it copies back
    ID3D12Resource* _anchor[2] = {}; // the input box as it was when each pixel last moved
    int _current = 0;
    bool _valid = false;

    // The output this object was built for, all of it: Fits compares against these, so an output it
    // refused (mips, slices, samples) is refused once rather than rebuilt and refused every frame.
    UINT64 _width = 0;
    UINT _height = 0;
    DXGI_FORMAT _format = DXGI_FORMAT_UNKNOWN;
    UINT16 _mipLevels = 0;
    UINT16 _arraySize = 0;
    UINT _sampleCount = 0;

    // One unit of the output format, for the creep's minimum step. See the shader.
    float _quantAbs = 0.0f;
    float _quantRel[3] = {};

    ID3D12Resource* CreateHistory(ID3D12Device* device, DXGI_FORMAT format, const wchar_t* name);

  public:
    DlssNr_Stabilizer_Dx12(std::string InName, ID3D12Device* InDevice, const D3D12_RESOURCE_DESC& InOutput);
    ~DlssNr_Stabilizer_Dx12();

    // Whether this object was built for this output on this device.
    bool Fits(ID3D12Device* InDevice, const D3D12_RESOURCE_DESC& InOutput) const;

    // Drop the history: the next dispatch shows the model's answer as it is and starts the anchor over.
    void Invalidate() { _valid = false; }

    // InInput is the frame before the edit, as the upscaler left it -- the composition's own linear copy,
    // not the model's kneed proxy -- with the white point and passthrough flag the composition used.
    // InInput in NON_PIXEL_SHADER_RESOURCE, InOutput in UNORDERED_ACCESS, both in those states on return.
    // InOutput is read as the model's answer and overwritten with the stabilized picture.
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InInput, ID3D12Resource* InOutput,
                  float InStrength, float InTolerance, float InWhitePoint, bool InPassthrough);
};
