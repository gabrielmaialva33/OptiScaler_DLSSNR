#pragma once

#include "DlssNr_ZeroGuides.h"

#include <d3d12.h>
#include <cstdint>
#include <memory>

class FT_Dx12;

namespace DlssNr
{

// The pass, over a frame that reached present without an upscaler ever being called.
//
// Everything the model needs and the frame does not have is owned here: a working colour it can be
// read and written in, zero depth and motion, and the conversions in and out of the backbuffer's own
// format. The caller supplies a source it has already landed on D3D12 and a command list it will
// execute on one queue, and takes Output() as the thing to put on screen.
//
// One host per admitted swapchain. Nothing here is shared between instances, and nothing here
// reaches for a global present, queue or device.
class PresentHost
{
  public:
    PresentHost();
    ~PresentHost();

    PresentHost(const PresentHost&) = delete;
    PresentHost& operator=(const PresentHost&) = delete;

    // Record the whole sequence for this frame: guide initialization if it is still owed, the
    // conversion in, the pass, and the conversion back to the source's format.
    //
    // sourceState is where source arrives and where it is left. The source is the caller's; nothing
    // here writes it.
    //
    // False means this frame gets no neural pass -- the model is off, still building, already failed,
    // or something here could not be built. It is not an error for the caller: the frame still has to
    // be shown, and Output() answers null so the caller transfers the source as it always did.
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                D3D12_RESOURCE_STATES sourceState, ID3D12CommandQueue* queue);

    // What Record produced, in D3D12_RESOURCE_STATE_COPY_SOURCE, or null if this frame has none.
    // Valid only for the recording it came from.
    ID3D12Resource* Output() const;

    // Whether the list carrying the last Record actually executed. Initialization that was recorded
    // and then dropped is not initialization, and the frame serial must not advance over a frame the
    // model never saw.
    void ConfirmExecuted();
    void AbandonRecording();

    // Throw everything away. The caller must already have proved the GPU is done with it.
    void Release();

    // Frames this host has actually put through the model, which is what the model's history is
    // counted in. Not the process-wide frame counter: two hosts advancing one global would each see
    // a history that skipped.
    uint64_t Serial() const { return _serial; }

  private:
    bool _Ensure(ID3D12Device* device, ID3D12Resource* source);

    // Build the model once, on lists of this host's own, before any frame work exists to sit in
    // front of it.
    //
    // Creating the feature is setup, not frame work, and it was being recorded onto the frame's list
    // behind two swapchain transitions, two guide clears, four more transitions and a conversion
    // dispatch. NGX records its own initialization when it creates a feature, and whether it minds
    // what is already on the list it is handed is not something source inspection answers.
    //
    // Two allocators and two lists rather than one reset twice: the first carries the guide clear and
    // the second carries nothing but the create, so the create's list is empty when NGX gets it.
    // Resetting one allocator between two submissions would need a CPU wait to be legal; two of them
    // need nothing, because they are used once and then kept until teardown.
    bool _EnsureFeature(ID3D12Device* device, ID3D12CommandQueue* queue);

    ZeroGuides _guides;

    // The colour the model reads and writes. A wide float format rather than the backbuffer's own,
    // for two reasons: the model's edit is composed in floating point and an 8-bit round trip would
    // quantise it twice, and a B8G8R8A8 unordered-access view is a format-support question this
    // avoids asking.
    std::unique_ptr<FT_Dx12> _toWorking;

    // Back to whatever the caller's frame is in, so the caller's transfer is the copy it already did.
    std::unique_ptr<FT_Dx12> _fromWorking;

    D3D12_RESOURCE_STATES _workingState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES _outputState = D3D12_RESOURCE_STATE_COMMON;

    ID3D12CommandAllocator* _setupAllocators[2] = {};
    ID3D12GraphicsCommandList* _setupLists[2] = {};
    bool _featureAttempted = false;
    bool _reportedCapture = false;

    uint64_t _serial = 0;
    uint32_t _width = 0;
    uint32_t _height = 0;
    unsigned int _sourceFormat = 0;

    bool _recorded = false;
    bool _recordedPass = false;
    bool _resetOwed = true;
    bool _failed = false;
};

} // namespace DlssNr
