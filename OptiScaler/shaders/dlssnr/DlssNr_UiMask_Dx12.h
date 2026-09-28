#pragma once

// The static-overlay mask for DLSS-NR's UI correction: pixels that stay the same while the pixels around
// them change are taken for interface, and the model is handed them as DLSSNR.UIAlpha so it leaves them
// as they were. dlssnr/design/hud-protection.md is the reasoning.
//
// One object serves one working size on one device. Its history is sized at construction; a different
// size gets a new object, and the old one is parked with the pass's other retired objects rather than
// freed, because the recordings that used it may still be on the GPU.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// One dispatch per frame, reused round-robin with no fence: the same coverage the stabilizer keeps.
#define DLSSNR_UIMASK_NUM_OF_HEAPS 16

class DlssNr_UiMask_Dx12 : public Shader_Dx12
{
  private:
    // The Params cbuffer of dlssnr_uimask.hlsl, in order.
    struct alignas(256) Constants
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t Valid = 0;
        uint32_t StreakMin = 0;
        float StaticEps = 0.0f;
        float MotionTau = 0.0f;
        float DetailMin = 0.0f;
        float Decay = 0.0f;
        float DropTau = 0.0f;
        float CoreEps = 0.0f;
        float SupportMin = 0.0f;
        float SidesMin = 0.0f;
    };

    FrameDescriptorHeap _frameHeaps[DLSSNR_UIMASK_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_UIMASK_NUM_OF_HEAPS] = {};
    void* _mappedConstants[DLSSNR_UIMASK_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // Last frame's and this frame's, swapped every dispatch, so no pixel reads a neighbour another thread
    // is writing. All rest in NON_PIXEL_SHADER_RESOURCE between dispatches.
    ID3D12Resource* _luma[2] = {};
    ID3D12Resource* _acc[2] = {};    // R16G16_FLOAT: .x protection, .y consecutive candidate frames
    ID3D12Resource* _mask = nullptr; // what the model reads; rests in NON_PIXEL_SHADER_RESOURCE
    int _current = 0;
    bool _valid = false;

    UINT _width = 0;
    UINT _height = 0;

    ID3D12Resource* CreateTexture(ID3D12Device* device, DXGI_FORMAT format, const wchar_t* name);

  public:
    DlssNr_UiMask_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight);
    ~DlssNr_UiMask_Dx12();

    // Whether this object was built for this size on this device. A failed object still fits, so it is
    // not rebuilt, and logged again, every frame.
    bool Fits(ID3D12Device* InDevice, UINT InWidth, UINT InHeight) const
    {
        return InDevice == _device && InWidth == _width && InHeight == _height;
    }

    // Drop the history: the next dispatch has nothing to compare with, protects nothing, and starts over.
    void Invalidate() { _valid = false; }

    // InInput is the model's input at this object's size, in NON_PIXEL_SHADER_RESOURCE, left there.
    // Returns the mask (RGBA8, the same value in every channel) in NON_PIXEL_SHADER_RESOURCE, valid until
    // the next Dispatch, or nullptr if this object could not be built. Binds its own descriptor heap and
    // root signature: the caller re-binds its own afterwards.
    ID3D12Resource* Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InInput);
};
