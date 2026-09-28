#pragma once
#include "SysUtils.h"

#include <d3d12.h>
#include <vector>

class IFGFeature_Dx12;

// Frame generation inputs for a title that never calls an upscaler (FGInput::Synthesized).
//
// UpscalerInputsDx11wDx12 hands FG the depth and motion an upscaler was given. A title with no upscaler
// has neither, so this stands in with a display-size R16G16_FLOAT motion field and an R32_FLOAT depth,
// both cleared to zero once and never written again: no motion, and with inverted depth, every pixel
// at the far plane. FSR-FG still runs its own optical flow on the frames themselves, which step 1 of
// dlssnr/design/synthesized-frame-generation.md measured as enough on its own in Divinity.
//
// Two owners, one per transport, both calling Feed just before the presenter's Present dispatches FG:
// - The D3D11 bridge (one per bridge swapchain) records the clear onto its copy list, which the present
//   queue waits for, and confirms or abandons it with that list (RecordInit, ConfirmExecuted,
//   AbandonRecording).
// - A native D3D12 swapchain has no such list, so RecordInitOnQueue records the clear on a list of its
//   own and executes it on FG's queue, the one FSR-FG's prepare runs on, so the clear lands first.
// Either frees the pair only after a drain has proved the queues are done with it.
class SynthInputs
{
  public:
    SynthInputs() = default;
    ~SynthInputs();

    SynthInputs(const SynthInputs&) = delete;
    SynthInputs& operator=(const SynthInputs&) = delete;

    // Allocate for the presenter's backbuffer size, or keep what already matches, and record the
    // one-time clear onto cmdList if it is still owed. Safe to call every frame. Returns false only on
    // a real failure; nothing owed is success.
    bool RecordInit(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height,
                    DXGI_FORMAT displayFormat);

    // A clear recorded onto a list nobody ran is not a clear. The bridge says which happened.
    void ConfirmExecuted();
    void AbandonRecording();

    // The same, for a caller with no list of its own: the clear goes on this object's list and is
    // executed on queue, then confirmed or abandoned here. Safe to call every frame; it only touches a
    // list when a clear is owed.
    bool RecordInitOnQueue(ID3D12CommandQueue* queue, UINT width, UINT height, DXGI_FORMAT displayFormat);

    // Per presented base frame: evaluate the FG state, start its frame and hand it the pair. Does
    // nothing until the pair exists and its zeros have executed.
    void Feed(IFGFeature_Dx12* fg, ID3D12Device* device);

    // Only after every queue that could still read the pair, or run the clear, has been drained.
    void Release();

  private:
    bool _Allocate(ID3D12Device* device, UINT width, UINT height);
    bool _EnsureOwnList(ID3D12Device* device);
    void _Retire();
    void _WarnOnce(const char* what);

    ID3D12Resource* _velocity = nullptr;
    ID3D12Resource* _depth = nullptr;
    ID3D12DescriptorHeap* _rtvHeap = nullptr;

    // A pair replaced without a drain in between. Parked, not freed, until the next Release.
    std::vector<ID3D12Resource*> _retired;

    // RecordInitOnQueue's own list, created on first use and kept until Release. The fence says when
    // its last clear finished, so the allocator is never reset under the GPU.
    ID3D12Device* _ownDevice = nullptr;
    ID3D12CommandAllocator* _ownAllocator = nullptr;
    ID3D12GraphicsCommandList* _ownList = nullptr;
    ID3D12Fence* _ownFence = nullptr;
    HANDLE _ownFenceEvent = nullptr;
    UINT64 _ownFenceValue = 0;

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
