#include "pch.h"

#include "DlssNr_PresentHost.h"
#include "DlssNrFeature_Dx12.h"

#include <shaders/format_transfer/FT_Dx12.h>

#include <Logger.h>

namespace DlssNr
{

namespace
{

// Wide float. The model's edit is composed in floating point, and a round trip through the
// backbuffer's 8 bits would quantise it on the way in and again on the way out. It is also the
// format the cold-start harness proved feature 18 accepts.
constexpr DXGI_FORMAT kWorkingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES& from,
                D3D12_RESOURCE_STATES to)
{
    if (resource == nullptr || from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
    from = to;
}

} // namespace

PresentHost::PresentHost() = default;
PresentHost::~PresentHost() { Release(); }

void PresentHost::Release()
{
    _toWorking.reset();
    _fromWorking.reset();
    _guides.Release();

    _workingState = D3D12_RESOURCE_STATE_COMMON;
    _outputState = D3D12_RESOURCE_STATE_COMMON;
    _width = 0;
    _height = 0;
    _sourceFormat = 0;
    _recorded = false;
    _recordedPass = false;
    _resetOwed = true;
}

ID3D12Resource* PresentHost::Output() const
{
    if (!_recorded || !_recordedPass || _fromWorking == nullptr)
        return nullptr;

    return _fromWorking->Buffer();
}

bool PresentHost::_Ensure(ID3D12Device* device, ID3D12Resource* source)
{
    const auto desc = source->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const auto height = desc.Height;

    if (_toWorking != nullptr && _fromWorking != nullptr && _width == width && _height == height &&
        _sourceFormat == static_cast<unsigned int>(desc.Format))
    {
        return _guides.Ensure(device, width, height);
    }

    // Anything already built describes a frame that no longer exists. The caller drains before it
    // gets here, so this is a teardown of idle objects, not of work in flight.
    Release();

    _toWorking = std::make_unique<FT_Dx12>("DLSS-NR present working", device, kWorkingFormat);
    _fromWorking = std::make_unique<FT_Dx12>("DLSS-NR present output", device, desc.Format);

    if (_toWorking == nullptr || _fromWorking == nullptr)
    {
        Release();
        return false;
    }

    // The working colour is where the pass reads and writes, so it starts where the pass expects to
    // find it. The output is only ever written by a conversion and read by the caller's copy.
    if (!_toWorking->CreateBufferResource(device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !_fromWorking->CreateBufferResource(device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        LOG_ERROR("DLSS-NR present host: {}x{} transfer buffers failed", width, height);
        Release();
        return false;
    }

    if (!_toWorking->CanRender() || !_fromWorking->CanRender())
    {
        LOG_ERROR("DLSS-NR present host: transfer passes did not initialise");
        Release();
        return false;
    }

    if (!_guides.Ensure(device, width, height))
    {
        Release();
        return false;
    }

    _workingState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    _width = width;
    _height = height;
    _sourceFormat = static_cast<unsigned int>(desc.Format);

    // A frame whose size or format just changed shares no history with the one before it.
    _resetOwed = true;

    LOG_INFO("DLSS-NR present host: {}x{} built", width, height);
    return true;
}

bool PresentHost::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                         D3D12_RESOURCE_STATES sourceState)
{
    _recorded = false;
    _recordedPass = false;

    if (_failed || device == nullptr || cmdList == nullptr || source == nullptr)
        return false;

    if (!_Ensure(device, source))
    {
        // Built once or not at all. Retrying every frame would mean allocating and failing at sixty
        // hertz behind a frame that is being shown correctly anyway.
        _failed = true;
        LOG_ERROR("DLSS-NR present host: disabled for this swapchain after a build failure");
        return false;
    }

    // Zeros before anything reads them. RecordClear binds its own descriptor heap, so it goes first
    // and the transfer passes bind theirs afterwards.
    const auto depthRest = static_cast<D3D12_RESOURCE_STATES>(GuideRestState(false));
    const auto motionRest = static_cast<D3D12_RESOURCE_STATES>(GuideRestState(true));

    if (depthRest != motionRest)
    {
        // One clear recording, one rest state. Two different ones would need two, and nothing in
        // this host's frame wants a guide anywhere but where the pass will look for it.
        _failed = true;
        LOG_ERROR("DLSS-NR present host: DepthResourceBarrier and MVResourceBarrier disagree ({} and {}); the guides "
                  "cannot rest in both, so this swapchain is left without the pass",
                  (unsigned) depthRest, (unsigned) motionRest);
        return false;
    }

    if (!_guides.RecordClear(cmdList, depthRest))
        return false;

    // From here on this list carries work of ours, so whatever happens next the caller's answer
    // about whether it executed is an answer we need. Every exit below leaves _recorded true.
    _recorded = true;

    auto sourceStateNow = sourceState;
    Transition(cmdList, source, sourceStateNow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _toWorking->Buffer(), _workingState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!_toWorking->Dispatch(cmdList, source, _toWorking->Buffer()))
    {
        // Put the caller's resource back before leaving: it arrived in sourceState and the caller is
        // entitled to find it there whatever happened in between.
        Transition(cmdList, source, sourceStateNow, sourceState);
        return false;
    }

    // The source has been read. Hand it back in the state it came in.
    Transition(cmdList, source, sourceStateNow, sourceState);

    // The pass reads and writes the working colour in place, and is told it arrives in
    // UNORDERED_ACCESS, which is where the conversion just left it.
    const bool passed = EvaluateAtPresent(cmdList, _toWorking->Buffer(), _guides.Depth(), _guides.Motion(), _resetOwed);

    if (!passed)
    {
        // The model did not run. The working colour holds an exact conversion of the frame, and
        // converting it back would be two dispatches to arrive where the caller already is. Say so
        // and let the caller transfer its own source.
        return false;
    }

    Transition(cmdList, _toWorking->Buffer(), _workingState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _fromWorking->Buffer(), _outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (!_fromWorking->Dispatch(cmdList, _toWorking->Buffer(), _fromWorking->Buffer()))
        return false;

    // Handed over as a copy source, because the one thing the caller does with it is copy it.
    Transition(cmdList, _fromWorking->Buffer(), _outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    _recordedPass = true;
    return true;
}

void PresentHost::ConfirmExecuted()
{
    if (!_recorded)
        return;

    _guides.ConfirmExecuted();

    if (_recordedPass)
    {
        // The model has seen a frame, so the next one has a history to continue from, and the serial
        // counts frames the model actually saw rather than frames that went past.
        _resetOwed = false;
        ++_serial;
    }

    _recorded = false;
    _recordedPass = false;
}

void PresentHost::AbandonRecording()
{
    if (!_recorded)
        return;

    _guides.AbandonRecording();

    // The states recorded onto that list never happened either, so what this host believes about
    // where its resources are is now a description of a recording nobody ran. Everything is rebuilt
    // rather than reasoned about: a wrong StateBefore is a barrier the runtime rejects, and the
    // cheapest way to be sure is to have nothing left to be wrong about.
    LOG_WARN("DLSS-NR present host: a recording was abandoned, rebuilding its resources");
    Release();

    _recorded = false;
    _recordedPass = false;
}

} // namespace DlssNr
