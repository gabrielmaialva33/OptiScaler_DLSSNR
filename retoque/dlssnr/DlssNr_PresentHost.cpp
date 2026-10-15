#include "pch.h"

#include "DlssNr_PresentHost.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_GpuTiming.h"
#include "DlssNr_Identity.h"
#include "DlssNr_PresentColour.h"

#include <shaders/dlssnr/DlssNr_PresentHdr_Dx12.h>
#include <shaders/format_transfer/FT_Dx12.h>

#include <Config.h>
#include <Logger.h>

#include <optional>

namespace DlssNr
{

namespace
{

// Wide float. The model's edit is composed in floating point, and a round trip through the
// backbuffer's 8 bits would quantise it on the way in and again on the way out. It is also the
// format the cold-start harness proved feature 18 accepts.
constexpr DXGI_FORMAT kWorkingFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Said once when the device cannot read and store a frame's format (PresentHost::_FormatUsable).
constexpr const char* kFormatRefusal =
    "the device cannot read this frame's format in a shader and store it through a typed unordered-access view; the "
    "frame is shown as the game rendered it";

// The format the frame's bytes are read and written in by the two conversions.
//
// An sRGB format is the same bytes as its UNORM sibling -- CopyResource moves them between the two unchanged --
// but neither of its views can stand in for the sibling's. A shader resource view decodes them to linear, where the
// pass expects the display-encoded values a UNORM backbuffer reads as; an unordered-access view of one is not legal
// at all. vkd3d-proton accepted the second, and native Windows removed the device with DXGI_ERROR_INVALID_CALL 4 ms
// after the first composition (Metal: Hellsinger, R8G8B8A8_UNORM_SRGB, on Rafael's RTX 3060, 2026-10-14).
//
// B8G8R8X8 maps to its own sibling and is then asked of the device like any other: no typed store there declines
// it, rather than a mapping into another group being invented for it.
DXGI_FORMAT EncodedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:
        return format;
    }
}

// What the device says of one format: loadable through a shader resource view, and storable through a typed
// unordered-access view. Both conversions need both: the one in reads the frame in this format, the one out
// writes it. A typeless format answers no to both, which is right: it has no view to give.
bool DeviceCanTransfer(ID3D12Device* device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support {};
    support.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;

    const bool load = (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
    const bool store = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
                       (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    return load && store;
}

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

// The decode's writes to the working colour are done before the pass touches it, whatever it does first.
void UavBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    cmdList->ResourceBarrier(1, &barrier);
}

// Say why a frame went without the pass, once per distinct reason.
//
// Every frame that declines declines at sixty hertz, so logging each one buries the log and hides the
// one line that matters. Comparing the pointer rather than the text is deliberate: these are string
// literals from a fixed set, so a new pointer is a new reason and nothing here has to allocate or
// compare characters on the present thread.
void ReportFrameSkip(const char* reason)
{
    static const char* last = nullptr;

    if (reason == nullptr || reason == last)
        return;

    last = reason;
    LOG_INFO("DLSS-NR present host: no pass on this frame ({})", reason);
}

} // namespace

PresentHost::PresentHost() = default;
PresentHost::~PresentHost()
{
    Release();
    _hdr.reset();
}

void PresentHost::Release()
{
    _ReleaseBuilt();

    if (_motion != nullptr)
        _motion->Release();
}

bool PresentHost::MotionSynthesized() const { return _motion != nullptr && _motion->LastFrameSynthesized(); }

void PresentHost::_ReleaseBuilt()
{
    _toWorking.reset();
    _fromWorking.reset();
    _guides.Release();

    if (_stage != nullptr)
    {
        _stage->Release();
        _stage = nullptr;
    }
    _stageState = D3D12_RESOURCE_STATE_COMMON;

    for (auto*& list : _setupLists)
    {
        if (list != nullptr)
        {
            list->Release();
            list = nullptr;
        }
    }

    for (auto*& allocator : _setupAllocators)
    {
        if (allocator != nullptr)
        {
            allocator->Release();
            allocator = nullptr;
        }
    }

    _featureAttempted = false;

    _workingState = D3D12_RESOURCE_STATE_COMMON;
    _outputState = D3D12_RESOURCE_STATE_COMMON;
    _width = 0;
    _height = 0;
    _sourceFormat = 0;
    _recorded = false;
    _recordedPass = false;
    _resetOwed = true;
    _motionResetOwed = true;
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

    // The frame's own format, or for an sRGB one its UNORM sibling (EncodedFormat): the output is written through
    // an unordered-access view, and the frame is read through the sibling from a copy of its bytes.
    const DXGI_FORMAT encoded = EncodedFormat(desc.Format);

    _toWorking = std::make_unique<FT_Dx12>("DLSS-NR present working", device, kWorkingFormat);
    _fromWorking = std::make_unique<FT_Dx12>("DLSS-NR present output", device, encoded);

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

    // An sRGB frame's stage: copied into, then read. No render-target or unordered-access use, so no flag for one,
    // and the runtime's own placement rather than whatever an opened shared resource reports.
    if (encoded != desc.Format)
    {
        D3D12_RESOURCE_DESC stageDesc = desc;
        stageDesc.Format = encoded;
        stageDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        stageDesc.Alignment = 0;
        stageDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &stageDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_stage))) ||
            _stage == nullptr)
        {
            LOG_ERROR("DLSS-NR present host: {}x{} stage for the sRGB frame failed", width, height);
            _stage = nullptr;
            Release();
            return false;
        }

        _stage->SetName(L"DLSS-NR present stage");
        _stageState = D3D12_RESOURCE_STATE_COPY_DEST;
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
    _motionResetOwed = true;

    LOG_INFO("DLSS-NR present host: {}x{} built", width, height);
    if (_stage != nullptr)
        LOG_INFO("DLSS-NR present host: the frame is sRGB (format {}); its bytes are read through a copy in format {} "
                 "and the output is written in that format",
                 (int) desc.Format, (int) encoded);
    return true;
}

bool PresentHost::_FormatUsable(ID3D12Device* device, DXGI_FORMAT format)
{
    if (device == _formatDevice && format == _formatAsked)
        return _formatUsable;

    _formatDevice = device;
    _formatAsked = format;

    const DXGI_FORMAT encoded = EncodedFormat(format);
    _formatUsable = DeviceCanTransfer(device, encoded);

    if (!_formatUsable)
        LOG_WARN("DLSS-NR present host: the device cannot load format {} in a shader and store it through a typed "
                 "unordered-access view (the frame's format is {}); the frame is shown as the game rendered it",
                 (int) encoded, (int) format);

    return _formatUsable;
}

bool PresentHost::_EnsureFeature(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (_featureAttempted)
        return true;

    if (queue == nullptr)
        return false;

    // Attempted, not succeeded. Whatever happens below happens once: a model that refuses is a model
    // that will refuse again, and retrying it every frame would be sixty allocations a second behind
    // a picture that is already correct.
    _featureAttempted = true;

    for (unsigned i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_setupAllocators[i]))))
        {
            LOG_ERROR("DLSS-NR present host: setup allocator {} failed", i);
            return false;
        }

        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _setupAllocators[i], nullptr,
                                             IID_PPV_ARGS(&_setupLists[i]))))
        {
            LOG_ERROR("DLSS-NR present host: setup list {} failed", i);
            return false;
        }

        // Named so a DRED report (DlssNr_Dred.h) attributes a removal in this list to the host directly.
        const std::wstring name = std::format(L"DLSS-NR present host setup {}", i);
        _setupAllocators[i]->SetName(name.c_str());
        _setupLists[i]->SetName(name.c_str());
    }

    const auto depthRest = static_cast<D3D12_RESOURCE_STATES>(GuideRestState(false));

    // List one: the guides' zeros, and nothing else.
    if (!_guides.RecordClear(_setupLists[0], depthRest))
        return false;

    if (FAILED(_setupLists[0]->Close()))
    {
        _guides.AbandonRecording();
        return false;
    }

    ID3D12CommandList* first[] = { _setupLists[0] };
    queue->ExecuteCommandLists(1, first);
    _guides.ConfirmExecuted();

    // List two: the model, on a list that has nothing on it. Same queue, so the clear above is
    // ordered before it without a CPU wait.
    // Said once, immediately before the create that has been refusing, and said by the same code the
    // harness uses so the two reports can be diffed rather than read.
    // The sink is named rather than written inline. An inline lambda inside the argument list puts the
    // call over the column limit, and two builds of clang-format 20 -- the Arch package here and the
    // one the CI action carries -- wrap what is left differently, so the check failed on formatting
    // this machine reported as clean. A short name removes the line that has to be wrapped at all.
    const auto toLog = [](void*, const char* line) { LOG_INFO("{}", line); };
    DlssNr::Identity::ReportAll(toLog, nullptr, "production", device, queue);

    const char* reason = "";
    const bool created = EvaluateAtPresent(_setupLists[1], _toWorking->Buffer(), _guides.Depth(), _guides.Motion(),
                                           PresentFrameDefaults(true), queue, &reason);

    if (FAILED(_setupLists[1]->Close()))
    {
        LOG_ERROR("DLSS-NR present host: setup list 1 close failed");
        return false;
    }

    // Executed whatever the answer: NGX can record work on a list and still refuse, and a list that
    // was written to and never run is work the driver is still holding references for.
    ID3D12CommandList* second[] = { _setupLists[1] };
    queue->ExecuteCommandLists(1, second);

    // The reason, not just the answer. A previous run of this same experiment read "the pass declined
    // this frame" as "the model refused to exist" and would have eliminated the wrong hypothesis.
    LOG_INFO("DLSS-NR present host: model creation attempted on an empty list, recorded={} ({})", created, reason);

    // Again, now that the attempt is over. The first report runs before the forwarder and the model
    // are loaded -- EnsureForwarder happens inside the create -- so its module list is short by
    // exactly the modules the question is about. The harness reports once, after its forwarder is
    // loaded, so this is the report that lines up with it.
    DlssNr::Identity::ReportNgxModules(toLog, nullptr);

    if (created)
        _resetOwed = false;

    return true;
}

void PresentHost::_ReportWindow()
{
    const auto now = std::chrono::steady_clock::now();

    // Nothing to say while the pass is off, and the window starts over when it comes back.
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        _windowFrames = 0;
        return;
    }

    if (_windowFrames == 0)
    {
        _windowAt = now;
        _windowSerial = _serial;
    }
    else if (now - _windowAt >= std::chrono::seconds(2))
    {
        // Counted in frames this host was handed, which on a bridge are base frames: the frames frame
        // generation adds never come through here, so this cannot claim them.
        const double seconds = std::chrono::duration<double>(now - _windowAt).count();

        if (Config::Instance()->DlssNrSynthMotion.value_or_default())
            LOG_INFO("DLSS-NR present host: the model ran on {} of the {} base frames handed to it in the last "
                     "{:.1f} s, motion {}",
                     _serial - _windowSerial, _windowFrames, seconds, MotionSynthesized() ? "synthesized" : "zero");
        else
            LOG_INFO(
                "DLSS-NR present host: the model ran on {} of the {} base frames handed to it in the last {:.1f} s",
                _serial - _windowSerial, _windowFrames, seconds);

        _windowFrames = 0;
        _windowAt = now;
        _windowSerial = _serial;
    }

    ++_windowFrames;
}

bool PresentHost::Record(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                         D3D12_RESOURCE_STATES sourceState, ID3D12CommandQueue* queue)
{
    _recorded = false;
    _recordedPass = false;
    _motionRecorded = false;

    if (_failed || device == nullptr || cmdList == nullptr || source == nullptr)
        return false;

    const auto sourceDesc = source->GetDesc();

    // The colour space's record (NoteColourSpace): every frame handed over is looked at, the pass on or off, so a
    // format change while it was off is not missed.
    if (_colourSpaceSet)
    {
        if (_colourFormat == DXGI_FORMAT_UNKNOWN)
            _colourFormat = sourceDesc.Format;
        else if (_colourFormat != sourceDesc.Format)
            _colourValid = false;
    }

    const auto sourceWidth = static_cast<uint32_t>(sourceDesc.Width);
    const auto sourceHeight = sourceDesc.Height;
    const bool builtForSource = _toWorking != nullptr && _fromWorking != nullptr && _width == sourceWidth &&
                                _height == sourceHeight &&
                                _sourceFormat == static_cast<unsigned int>(sourceDesc.Format);

    // Build nothing while the pass is off. A bridge built for synthesized frame generation keeps a host
    // for the neural toggle even when DLSS-NR starts disabled, and that host must stay an empty object
    // -- no buffers, no guides, no model -- until the pass is switched on. Once built, a host that is
    // switched off keeps what it has and goes on exactly as before at that size: the pass itself
    // declines, and asks for the reset the next frame needs. At any other size it waits. Rebuilding
    // there would reallocate for a pass that declines anyway, and would spend the model's one creation
    // on this host's own list, leaving the frame's list to create it once the pass comes back.
    if (!Config::Instance()->DlssNrEnabled.value_or_default() && !builtForSource)
        return false;

    _ReportWindow();

    // A frame the model cannot be built for is declined before anything is spent on it. Swapchains pass
    // through sizes like 1x1 while a window is created or resized (PCSX2 under the D3D11 bridge,
    // 2026-09-27), and building for one of them cost everything this host had: its buffers at the size
    // it was really running at, and the model's creation, attempted and refused. What was built stays,
    // so a frame that returns to that size carries on with no rebuild at all.
    if (sourceWidth < kDlssNrMinExtent || sourceHeight < kDlssNrMinExtent)
    {
        ReportFrameSkip("the frame is below 64 pixels; nothing is built for it and the model is not attempted");
        return false;
    }

    // An HDR frame is declined before anything is built for it, and the history the model has is not the
    // frame that follows when it runs again. What was built stays, as for a frame too small. With HdrPresent
    // on, HDR10 and scRGB are converted instead (docs/dlssnr/design/hdr-present-conversion.md section 5.2);
    // off, the answer is PresentColourRefusal's on the colour space this host was always told.
    PresentColourRecord record;
    record.known = true;
    record.valid = !_colourSpaceSet || _colourValid;
    record.colourSpaceSet = _colourSpaceSet;
    record.format = _colourSpaceSet ? _colourFormat : sourceDesc.Format;
    record.colourSpace = _colourSpace;

    const bool hdrPresent = Config::Instance()->DlssNrHdrPresent.value_or_default();
    const PresentColourDecision switchOn = ResolvePresentColour(sourceDesc.Format, record, true, _colourSpace);
    if (ReportsHdrStatus(sourceDesc.Format, switchOn, hdrPresent))
        NotePresentHdr(switchOn.profile, hdrPresent ? switchOn.reason : nullptr, false);

    const PresentColourDecision decision =
        hdrPresent ? switchOn : ResolvePresentColour(sourceDesc.Format, record, false, _colourSpace);
    const bool hdr = IsHdrProfile(decision.profile);

    const char* refusal = decision.profile == PresentColour::Declined ? decision.reason : nullptr;
    if (refusal == nullptr && hdr && !DlssNr_PresentHdr_Dx12::Available())
        refusal = PresentColourText::kNoShader;

    if (refusal != nullptr)
    {
        _resetOwed = true;
        if (hdr)
            NotePresentHdr(decision.profile, refusal, false);
        ReportFrameSkip(refusal);
        return false;
    }

    // A format the conversions cannot read and write is declined the same way, before anything is built for it
    // and without a view the device would be removed over. What was built stays.
    if (!_FormatUsable(device, sourceDesc.Format))
    {
        _resetOwed = true;
        ReportFrameSkip(kFormatRefusal);
        return false;
    }

    if (!_Ensure(device, source))
    {
        // Built once or not at all. Retrying every frame would mean allocating and failing at sixty
        // hertz behind a frame that is being shown correctly anyway.
        _failed = true;
        LOG_ERROR("DLSS-NR present host: disabled for this swapchain after a build failure");
        return false;
    }

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

    // The HDR conversion's pass, on the first HDR frame and then for the host's life: built after _Ensure, which
    // releases what it rebuilds. A device that cannot store to this format from a shader declines the frame here,
    // before the frame's list carries anything (section 5.4: asked of the device, not assumed).
    if (hdr)
    {
        if (_hdr == nullptr)
            _hdr = std::make_unique<DlssNr_PresentHdr_Dx12>("DLSS-NR present host HDR", device);

        const char* hdrRefusal = !_hdr->BuiltFor(device)              ? PresentColourText::kOtherDevice
                                 : !_hdr->IsInit()                    ? PresentColourText::kNotBuilt
                                 : !_hdr->Supports(sourceDesc.Format) ? PresentColourText::kNoTypedStore
                                                                      : nullptr;
        if (hdrRefusal != nullptr)
        {
            _resetOwed = true;
            NotePresentHdr(decision.profile, hdrRefusal, false);
            ReportFrameSkip(hdrRefusal);
            return false;
        }

        NotePresentHdr(decision.profile, nullptr, true);
    }

    // The guides' zeros and the model's creation happen once, on this host's own lists, before the
    // frame's list carries anything. Their bookkeeping is settled there, so nothing below owes the
    // caller an answer about them.
    if (!_EnsureFeature(device, queue))
        return false;

    // From here on this list carries work of ours, so whatever happens next the caller's answer
    // about whether it executed is an answer we need. Every exit below leaves _recorded true.
    _recorded = true;

    // What the conversion in reads: the source itself, or for an sRGB one its bytes copied unchanged into the UNORM
    // stage, so the working colour holds exactly what a UNORM backbuffer would have put there. One copy within one
    // typeless group; every other format takes the transitions it always did and nothing more.
    auto sourceStateNow = sourceState;
    ID3D12Resource* readable = source;
    if (_stage != nullptr && !hdr)
    {
        Transition(cmdList, source, sourceStateNow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(cmdList, _stage, _stageState, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(_stage, source);
        Transition(cmdList, _stage, _stageState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        readable = _stage;
    }
    else
    {
        Transition(cmdList, source, sourceStateNow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    Transition(cmdList, _toWorking->Buffer(), _workingState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // HDR: the source decoded into the working colour as linear Rec.709 in scRGB units, in place of the plain
    // conversion, and both dispatches timed as one conversion sample (finished before the caller closes the list).
    std::optional<GpuTiming::Conversion> hdrTiming;
    const uint32_t hdrShaderProfile =
        decision.profile == PresentColour::Hdr10 ? PresentHdr::kProfileHdr10 : PresentHdr::kProfileScRgb;
    bool convertedIn = false;
    if (hdr)
    {
        // Enabled exactly when the Dispatch's own sample is: under the driver proxy the menu says timing is off.
        const auto timingSettings = Config::Instance()->GetDlssNrGpuTimingSettings();
        hdrTiming.emplace(device, cmdList,
                          timingSettings.Enabled && !Config::Instance()->DlssNrUseProxy.value_or_default(),
                          timingSettings.Interval);
        hdrTiming->Begin();
        convertedIn = _hdr->Decode(cmdList, source, _toWorking->Buffer(), hdrShaderProfile);
        hdrTiming->End();
        if (convertedIn)
            UavBarrier(cmdList, _toWorking->Buffer());
    }
    else
    {
        convertedIn = _toWorking->Dispatch(cmdList, readable, _toWorking->Buffer());
    }

    if (!convertedIn)
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
    // The game's own guides when there are any, this host's zeros when there are not.
    //
    // A D3D11 title reaches this bridge for two different reasons. One is having no upscaler at all,
    // which is what it was built for, and there the capture is empty every frame and the zeros are
    // the only answer. The other is an upscaler running through the Dx11wDx12 interop, whose evaluate
    // does hand depth and motion over -- and ignoring them to use zeros instead would be throwing
    // away the real thing while holding it.
    ID3D12Resource* depth = _guides.Depth();
    ID3D12Resource* motion = _guides.Motion();
    DlssNrFrameInfo frame = PresentFrameDefaults(_resetOwed, decision.profile);

    // No real motion means no supersampling: the model refuses zero guides above native, so the pass
    // clamps to 1.0 rather than refusing. The captured branch below restores it -- real guides can.
    frame.AllowSupersampling = false;

    ID3D12Resource* capturedDepth = nullptr;
    ID3D12Resource* capturedMotion = nullptr;
    DlssNrFrameInfo captured {};

    if (CapturedPresentGuides(cmdList, &capturedDepth, &capturedMotion, &captured, queue, this, sourceWidth,
                              sourceHeight))
    {
        // The capture describes its own guides -- their subrects, their motion encoding, whether depth
        // is inverted -- and none of that is knowable from here. What stays is this host's account of
        // the frame the guides are being applied to, which the capture has no opinion about.
        const DlssNrFrameInfo defaults = frame;
        frame = captured;
        frame.PresentSource = defaults.PresentSource;
        frame.ColourIsLinearHdr = defaults.ColourIsLinearHdr;
        frame.ExposureTexture = defaults.ExposureTexture;
        frame.PreExposure = defaults.PreExposure;
        frame.ExtentIsStable = defaults.ExtentIsStable;
        frame.OutputState = defaults.OutputState;
        frame.Reset = defaults.Reset;
        frame.PresentWhite = defaults.PresentWhite;
        frame.PresentQuantRel = defaults.PresentQuantRel;

        depth = capturedDepth;
        motion = capturedMotion;

        if (!_reportedCapture)
        {
            _reportedCapture = true;
            LOG_INFO("DLSS-NR present host: using the upscaler's own depth and motion, not the zero guides");
        }
    }

    // [DlssNr] SynthMotion: with no game guides, motion estimated from this frame and the last instead of
    // zero motion. Read from the working colour before the pass edits it in place; depth stays zero.
    if (motion == _guides.Motion() && Config::Instance()->DlssNrSynthMotion.value_or_default())
    {
        if (_motion == nullptr)
            _motion = std::make_unique<SynthMotionGuide>();

        _motionRecorded = true;
        if (auto* synthesized = _motion->Record(device, cmdList, _toWorking->Buffer(), _workingState, _motionResetOwed,
                                                frame, "D3D11 bridge", queue))
        {
            motion = synthesized;
        }
    }

    // Zero guides tell the model nothing moved, and it blends each frame with its history accordingly:
    // ghosts and doubled edges under a moving camera (Generation Zero, 2026-09-24). Opt-in, because a
    // slow isometric camera (Divinity) was measured fine with history, and history is what smooths.
    // Keyed on the motion, not the depth: synthesized motion keeps the zero depth and needs no reset.
    if (motion == _guides.Motion() && Config::Instance()->DlssNrZeroGuideReset.value_or_default())
    {
        // Policy only if nothing real was already owed: a host rebuilt at the same size carries a genuine
        // reset in from PresentFrameDefaults, and that one must still read as a cut.
        frame.ResetIsPolicy = !frame.Reset;
        frame.Reset = true;
    }

    // Which present host this is, for a comparison capture's provenance only.
    frame.BridgeHost = true;

    const char* frameReason = "";
    const bool passed = EvaluateAtPresent(cmdList, _toWorking->Buffer(), depth, motion, frame, queue, &frameReason);

    // The field goes back where the estimator keeps it, on this same list, whether the pass ran or not.
    if (_motion != nullptr)
        _motion->AfterPass(cmdList);

    if (!passed)
    {
        ReportFrameSkip(frameReason);

        // The model did not run. The working colour holds an exact conversion of the frame, and
        // converting it back would be two dispatches to arrive where the caller already is. Say so
        // and let the caller transfer its own source.
        return false;
    }

    Transition(cmdList, _toWorking->Buffer(), _workingState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, _fromWorking->Buffer(), _outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // HDR: back into the source's format with the source as the reference, so a pixel the pass left alone is the
    // game's own code. The source is read again, and handed back in the state it came in.
    bool convertedOut = false;
    if (hdr)
    {
        auto sourceStateAgain = sourceState;
        Transition(cmdList, source, sourceStateAgain, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        hdrTiming->Begin();
        convertedOut = _hdr->Encode(cmdList, source, _toWorking->Buffer(), _fromWorking->Buffer(), hdrShaderProfile);
        hdrTiming->End();
        hdrTiming->Finish(convertedOut);
        Transition(cmdList, source, sourceStateAgain, sourceState);
    }
    else
    {
        convertedOut = _fromWorking->Dispatch(cmdList, _toWorking->Buffer(), _fromWorking->Buffer());
    }

    if (!convertedOut)
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

    if (_motion != nullptr)
        _motion->ConfirmExecuted();

    // The estimator's recording on this list executed, and with it the reset it carried.
    if (_motionRecorded)
        _motionResetOwed = false;
    _motionRecorded = false;

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
    AbandonCapturedGuides();

    // The estimator settles its own abandon: its recording never ran either, and it knows what that
    // leaves it holding.
    if (_motion != nullptr)
        _motion->AbandonRecording();

    // The states recorded onto that list never happened either, so what this host believes about
    // where its resources are is now a description of a recording nobody ran. Everything is rebuilt
    // rather than reasoned about: a wrong StateBefore is a barrier the runtime rejects, and the
    // cheapest way to be sure is to have nothing left to be wrong about.
    LOG_WARN("DLSS-NR present host: a recording was abandoned, rebuilding its resources");
    _ReleaseBuilt();

    _recorded = false;
    _recordedPass = false;
}

} // namespace DlssNr
