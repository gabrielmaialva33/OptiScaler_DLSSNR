#pragma once
#include "SysUtils.h"

#include <d3d12.h>
#include <vector>

class IFGFeature_Dx12;

// Frame generation inputs for a D3D11 title that never calls an upscaler (FGInput::Synthesized).
//
// UpscalerInputsDx11wDx12 hands FG the depth and motion an upscaler was given. A title with no upscaler
// has neither, so this stands in with a display-size R16G16_FLOAT motion field and an R32_FLOAT depth,
// both cleared to zero once and never written again: no motion, and with inverted depth, every pixel
// at the far plane. FSR-FG still runs its own optical flow on the frames themselves, and step 1 of
// dlssnr/design/synthesized-frame-generation.md measures how far that gets without ours.
//
// One per bridge swapchain, owned by it. The bridge records the clear onto its copy list, which the
// present queue waits for, and feeds FG just before the presenter's Present. It frees the pair only
// after a drain has proved both queues are done with it: in ResizeBuffers and at teardown.
class SynthInputsDx11wDx12
{
  public:
    SynthInputsDx11wDx12() = default;
    ~SynthInputsDx11wDx12();

    SynthInputsDx11wDx12(const SynthInputsDx11wDx12&) = delete;
    SynthInputsDx11wDx12& operator=(const SynthInputsDx11wDx12&) = delete;

    // Allocate for the presenter's backbuffer size, or keep what already matches, and record the
    // one-time clear onto cmdList if it is still owed. Safe to call every frame. Returns false only on
    // a real failure; nothing owed is success.
    bool RecordInit(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height,
                    DXGI_FORMAT displayFormat);

    // A clear recorded onto a list nobody ran is not a clear. The bridge says which happened.
    void ConfirmExecuted();
    void AbandonRecording();

    // Per presented base frame: evaluate the FG state, start its frame and hand it the pair. Does
    // nothing until the pair exists and its zeros have executed.
    void Feed(IFGFeature_Dx12* fg, ID3D12Device* device);

    // Only after both queues have been drained.
    void Release();

  private:
    bool _Allocate(ID3D12Device* device, UINT width, UINT height);
    void _Retire();
    void _WarnOnce(const char* what);

    ID3D12Resource* _velocity = nullptr;
    ID3D12Resource* _depth = nullptr;
    ID3D12DescriptorHeap* _rtvHeap = nullptr;

    // A pair replaced without a drain in between. Parked, not freed, until the next Release.
    std::vector<ID3D12Resource*> _retired;

    UINT _width = 0;
    UINT _height = 0;
    DXGI_FORMAT _displayFormat = DXGI_FORMAT_UNKNOWN;

    bool _clearOwed = false;
    bool _clearRecorded = false;
    bool _resetOwed = true;
    bool _allocFailed = false;
    bool _fedReported = false;
    bool _warned = false;
};
