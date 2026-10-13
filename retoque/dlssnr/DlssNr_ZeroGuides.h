#pragma once

#include <d3d12.h>
#include <cstdint>

namespace DlssNr
{

// Depth and motion for a caller that has neither.
//
// The pass reads engine depth and motion vectors out of the parameter block the game handed its
// upscaler. A host that originates the evaluate itself -- a present-time dispatch in a title that
// never calls an upscaler -- has no such block and no such buffers, and Dispatch rejects a null
// guide before it reaches the encode. These stand in: one R32_FLOAT depth and one R16G16_FLOAT
// motion at the frame's size, filled with zeros once and never written again, so they cost one
// allocation and no per-frame work.
//
// The formats, the zero fill and the two-heap clear are not a guess. They are what
// tests/dlssnr-loopback --cold-nr drove through the production forwarder into feature 18, with no
// game NGX call anywhere in the process: 48 evaluations, 48 successes, all fence-complete, under
// Proton against the installed driver core.
//
// What zeros are *not* is a finished mode. The model reads a permanently still motion field as a
// scene that is not moving, so anything that does move smears. They exist to bring transport up
// with no guide producer in the way; a synthesised motion field is what makes a frame worth
// looking at, and that is separate work.
class ZeroGuides
{
  public:
    ZeroGuides() = default;
    ~ZeroGuides();

    ZeroGuides(const ZeroGuides&) = delete;
    ZeroGuides& operator=(const ZeroGuides&) = delete;

    // Allocate for this size, or keep what is already the right size. Safe to call every frame.
    // A size change throws the old pair away, so the caller must know the GPU is done with them.
    bool Ensure(ID3D12Device* device, uint32_t width, uint32_t height);

    // Record the one-time clear, if it is still owed, onto a list the caller will execute.
    //
    // ClearUnorderedAccessViewFloat wants two descriptors for the same view: the CPU handle must
    // live in a heap that is not shader visible, and the GPU handle must live in one that is and
    // that is bound on this list. A single shader-visible heap satisfies neither half properly,
    // which is the shape the donor's version of this got wrong. This binds its own heap; a caller
    // that had one bound must rebind afterwards.
    //
    // restState is where the guides are left once cleared, and it is the caller's to state rather
    // than ours to assume. The pass transitions a guide from the state it says the guide arrives in,
    // and for the shape this host uses that state is DepthResourceBarrier / MVResourceBarrier out of
    // the config -- keys meant for a game's own buffers, which default to where we would have left
    // them anyway but do not have to. Resting a guide somewhere the pass will not transition it from
    // is a barrier from a state the resource was never in.
    //
    // Returns false only on a real failure. Nothing owed is success.
    bool RecordClear(ID3D12GraphicsCommandList* cmdList,
                     D3D12_RESOURCE_STATES restState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The clear is initialization, and initialization that was recorded onto a list nobody ran is
    // not initialization. The caller says which happened, so a dropped list leaves the guides owing
    // their clear rather than declaring zeros that were never written.
    void ConfirmExecuted();
    void AbandonRecording();

    // Null until Ensure has succeeded. In D3D12_RESOURCE_STATE_UNORDERED_ACCESS while the clear is
    // owed, and in whatever RecordClear was given as restState once it has run.
    ID3D12Resource* Depth() const { return _depth; }
    ID3D12Resource* Motion() const { return _motion; }

    // Whether the guides are allocated and their zeros have actually executed.
    bool Ready() const { return _depth != nullptr && _motion != nullptr && !_clearOwed; }

    void Release();

  private:
    bool _Allocate(ID3D12Device* device, uint32_t width, uint32_t height);

    ID3D12Resource* _depth = nullptr;
    ID3D12Resource* _motion = nullptr;
    ID3D12DescriptorHeap* _clearHeapCpu = nullptr;
    ID3D12DescriptorHeap* _clearHeapGpu = nullptr;
    uint32_t _descriptorStride = 0;
    uint32_t _width = 0;
    uint32_t _height = 0;
    D3D12_RESOURCE_STATES _restState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool _clearOwed = false;
    bool _clearRecorded = false;
};

} // namespace DlssNr
