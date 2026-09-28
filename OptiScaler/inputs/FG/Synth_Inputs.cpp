#include "pch.h"
#include "Synth_Inputs.h"

#include "MathUtils.h"

#include <State.h>
#include <framegen/IFGFeature_Dx12.h>
#include <shaders/synth_motion/SynthMotion_Dx12.h>
#include <shaders/synth_motion/SynthMotion_Handoff.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace OptiMath;

namespace
{
// A gap this long between two estimates means the frame the estimator last saw is not the previous
// one (FG paused, the key toggled, DLSS-NR supplied the field meanwhile). Well above any frame time.
constexpr long long kMotionStaleMs = 250;

// R16G16_FLOAT, and every fourth pixel of a sampled row is enough for a median.
constexpr UINT kSampleBytesPerPixel = 4;
constexpr UINT kSampleStride = 4;

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

UINT AlignUp(UINT value, UINT alignment) { return (value + alignment - 1) / alignment * alignment; }

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x3ffu;
    uint32_t bits;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            // Subnormal: normalise it.
            int e = -1;
            do
            {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);

            bits = sign | ((uint32_t) (127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    }
    else if (exponent == 0x1f)
    {
        bits = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }

    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool MotionWanted() { return Config::Instance()->FGSynthesizedMotion.value_or_default(); }

// Where the pair rests between frames, and the state FG is told it arrives in. FSR-FG's prepare reads
// both from compute; with FG_ResourceValidity::UntilPresent it uses them in place, without a copy.
constexpr D3D12_RESOURCE_STATES kRestState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// Cleared as render targets: ClearRenderTargetView needs one CPU descriptor in a heap that is not
// shader visible, and binds nothing on the caller's list.
constexpr D3D12_RESOURCE_STATES kClearState = D3D12_RESOURCE_STATE_RENDER_TARGET;

void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmdList->ResourceBarrier(1, &barrier);
}

ID3D12Resource* CreateTarget(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kClearState, &clear,
                                               IID_PPV_ARGS(&resource))))
    {
        return nullptr;
    }

    resource->SetName(name);
    return resource;
}

template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}
} // namespace

SynthInputs::SynthInputs() = default;
SynthInputs::~SynthInputs() { Release(); }

void SynthInputs::_WarnOnce(const char* what)
{
    if (_warned)
        return;

    _warned = true;
    LOG_WARN("synthesized FG input: {} ({}x{}); frame generation gets no input from it, reported once", what, _width,
             _height);
}

bool SynthInputs::_Allocate(ID3D12Device* device, UINT width, UINT height)
{
    _velocity = CreateTarget(device, width, height, DXGI_FORMAT_R16G16_FLOAT, L"SynthFG_Velocity");
    _depth = CreateTarget(device, width, height, DXGI_FORMAT_R32_FLOAT, L"SynthFG_Depth");

    if (_rtvHeap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_rtvHeap))))
            _rtvHeap = nullptr;
    }

    if (_velocity == nullptr || _depth == nullptr || _rtvHeap == nullptr)
    {
        SafeRelease(_velocity);
        SafeRelease(_depth);
        return false;
    }

    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle = _rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(_velocity, nullptr, handle);
    handle.ptr += stride;
    device->CreateRenderTargetView(_depth, nullptr, handle);

    _width = width;
    _height = height;
    _clearOwed = true;
    _clearRecorded = false;
    _resetOwed = true;
    return true;
}

void SynthInputs::_Retire()
{
    if (_velocity != nullptr)
        _retired.push_back(_velocity);

    if (_depth != nullptr)
        _retired.push_back(_depth);

    _velocity = nullptr;
    _depth = nullptr;
}

bool SynthInputs::RecordInit(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height,
                             DXGI_FORMAT displayFormat)
{
    if (device == nullptr || cmdList == nullptr || width == 0 || height == 0)
        return false;

    _displayFormat = displayFormat;

    // Sizes change through ResizeBuffers, which drains both queues and calls Release first, so the
    // pair is normally gone by the time a new size shows up here. One still present was never proved
    // idle, and is parked rather than freed.
    if (_velocity != nullptr && (_width != width || _height != height))
        _Retire();

    if (_velocity == nullptr)
    {
        // A failed allocation is not retried every frame at the same size.
        if (_allocFailed && _width == width && _height == height)
            return false;

        _width = width;
        _height = height;
        _allocFailed = !_Allocate(device, width, height);

        if (_allocFailed)
        {
            _WarnOnce("could not allocate the motion and depth fields");
            return false;
        }
    }

    if (!_clearOwed)
        return true;

    const float zero[4] = {};
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle = _rtvHeap->GetCPUDescriptorHandleForHeapStart();

    ID3D12Resource* targets[] = { _velocity, _depth };
    for (auto* target : targets)
    {
        cmdList->ClearRenderTargetView(handle, zero, 0, nullptr);
        Transition(cmdList, target, kClearState, kRestState);
        handle.ptr += stride;
    }

    _clearRecorded = true;
    return true;
}

void SynthInputs::ConfirmExecuted()
{
    _ConfirmMotion();

    if (!_clearRecorded)
        return;

    _clearRecorded = false;
    _clearOwed = false;
}

// The barrier out of the clear state never ran either, so the pair is still where the clear expects
// it, and the next RecordInit records the same clear again.
void SynthInputs::AbandonRecording()
{
    _AbandonMotion();
    _clearRecorded = false;
}

bool SynthInputs::_EnsureOwnList(ID3D12Device* device)
{
    if (_ownList != nullptr)
    {
        // Built on another device. Release, which runs after the drain that ends a swapchain, is what
        // clears these; a list still here for a different device has not been proved idle.
        return _ownDevice == device;
    }

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_ownAllocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _ownAllocator, nullptr,
                                         IID_PPV_ARGS(&_ownList))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_ownFence))))
    {
        SafeRelease(_ownList);
        SafeRelease(_ownAllocator);
        SafeRelease(_ownFence);
        return false;
    }

    _ownFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (_ownFenceEvent == nullptr || FAILED(_ownList->Close()))
    {
        if (_ownFenceEvent != nullptr)
            CloseHandle(_ownFenceEvent);

        _ownFenceEvent = nullptr;
        SafeRelease(_ownList);
        SafeRelease(_ownAllocator);
        SafeRelease(_ownFence);
        return false;
    }

    // Identity only, for the check above: the list and the pair are released before the device can go.
    _ownDevice = device;
    _ownFenceValue = 0;
    return true;
}

bool SynthInputs::RecordInitOnQueue(ID3D12CommandQueue* queue, UINT width, UINT height, DXGI_FORMAT displayFormat)
{
    if (queue == nullptr || width == 0 || height == 0)
        return false;

    // Every frame after the first: the pair is there at this size and its zeros have run.
    if (_velocity != nullptr && !_clearOwed && _width == width && _height == height)
    {
        _displayFormat = displayFormat;
        return true;
    }

    // A failed allocation is not retried every frame at the same size, and costs no list work either.
    if (_velocity == nullptr && _allocFailed && _width == width && _height == height)
        return false;

    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    bool result = false;

    do
    {
        if (!_EnsureOwnList(device))
        {
            _WarnOnce("could not create the list for the one-time clear");
            break;
        }

        // The last clear must be finished before its allocator is reset. It is two clears; the wait is
        // only ever reached on a resize, and a clear that never finishes leaves the list alone.
        if (_ownFence->GetCompletedValue() < _ownFenceValue)
        {
            if (FAILED(_ownFence->SetEventOnCompletion(_ownFenceValue, _ownFenceEvent)) ||
                WaitForSingleObject(_ownFenceEvent, 2000) != WAIT_OBJECT_0)
            {
                _WarnOnce("the previous one-time clear did not finish");
                break;
            }
        }

        if (FAILED(_ownAllocator->Reset()) || FAILED(_ownList->Reset(_ownAllocator, nullptr)))
        {
            _WarnOnce("could not reset the list for the one-time clear");
            break;
        }

        if (!RecordInit(device, _ownList, width, height, displayFormat))
        {
            _ownList->Close();
            break;
        }

        if (FAILED(_ownList->Close()))
        {
            AbandonRecording();
            _WarnOnce("could not close the list for the one-time clear");
            break;
        }

        if (_clearRecorded)
        {
            // FG's queue is where FSR-FG's prepare executes (FSRFG_Dx12::Present, then Dispatch), so a
            // clear submitted on it here is ordered ahead of the first read of the pair.
            ID3D12CommandList* lists[] = { _ownList };
            queue->ExecuteCommandLists(1, lists);

            if (FAILED(queue->Signal(_ownFence, ++_ownFenceValue)))
                _WarnOnce("could not signal the one-time clear's fence");

            ConfirmExecuted();
        }

        result = true;
    } while (false);

    device->Release();
    return result;
}

bool SynthInputs::RecordMotion(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                               D3D12_RESOURCE_STATES colourState)
{
    _frameMotion = nullptr;
    _frameSceneCut = false;

    if (!MotionWanted() || device == nullptr || cmdList == nullptr || colour == nullptr)
        return true;

    return _RecordMotionOn(device, cmdList, colour, colourState);
}

bool SynthInputs::_EnsureMotionLists(ID3D12Device* device)
{
    if (_motionFence != nullptr)
    {
        // Built on another device and not yet released after a drain: not proved idle, so not reused.
        return _motionDevice == device;
    }

    bool ok = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_motionFence)));

    for (UINT i = 0; ok && i < kMotionLists; ++i)
    {
        ok = SUCCEEDED(
                 device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_motionAllocators[i]))) &&
             SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _motionAllocators[i], nullptr,
                                                 IID_PPV_ARGS(&_motionLists[i]))) &&
             SUCCEEDED(_motionLists[i]->Close());
    }

    if (ok)
    {
        _motionFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        ok = _motionFenceEvent != nullptr;
    }

    if (!ok)
    {
        for (UINT i = 0; i < kMotionLists; ++i)
        {
            SafeRelease(_motionLists[i]);
            SafeRelease(_motionAllocators[i]);
        }

        SafeRelease(_motionFence);
        return false;
    }

    // Identity only: everything here is released before the device can go.
    _motionDevice = device;
    _motionFenceValue = 0;
    _motionSlot = 0;

    for (auto& value : _motionFenceValues)
        value = 0;

    return true;
}

bool SynthInputs::RecordMotionOnQueue(ID3D12CommandQueue* queue, ID3D12Resource* colour,
                                      D3D12_RESOURCE_STATES colourState)
{
    _frameMotion = nullptr;
    _frameSceneCut = false;

    if (!MotionWanted() || queue == nullptr || colour == nullptr)
        return true;

    ID3D12Device* device = nullptr;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return false;

    bool result = false;

    do
    {
        if (!_EnsureMotionLists(device))
        {
            _WarnOnce("could not create the lists for the motion estimate");
            break;
        }

        const UINT slot = _motionSlot;

        // Normally long finished: the ring is three frames deep.
        if (_motionFence->GetCompletedValue() < _motionFenceValues[slot])
        {
            if (FAILED(_motionFence->SetEventOnCompletion(_motionFenceValues[slot], _motionFenceEvent)) ||
                WaitForSingleObject(_motionFenceEvent, 2000) != WAIT_OBJECT_0)
            {
                _WarnOnce("a motion estimate did not finish");
                break;
            }
        }

        if (FAILED(_motionAllocators[slot]->Reset()) ||
            FAILED(_motionLists[slot]->Reset(_motionAllocators[slot], nullptr)))
        {
            _WarnOnce("could not reset the list for the motion estimate");
            break;
        }

        const bool recorded = _RecordMotionOn(device, _motionLists[slot], colour, colourState);

        if (FAILED(_motionLists[slot]->Close()) || !recorded)
        {
            _AbandonMotion();
            break;
        }

        // FG's queue: FSR-FG's prepare runs on it after this (fg->Present(), then Dispatch), and DLSS-NR's
        // present pass after that, so both read the field once it exists.
        ID3D12CommandList* lists[] = { _motionLists[slot] };
        queue->ExecuteCommandLists(1, lists);

        _motionFenceValues[slot] = ++_motionFenceValue;
        if (FAILED(queue->Signal(_motionFence, _motionFenceValues[slot])))
            _WarnOnce("could not signal the motion estimate's fence");

        _motionSlot = (slot + 1) % kMotionLists;
        _ConfirmMotion();
        result = true;
    } while (false);

    device->Release();
    return result;
}

bool SynthInputs::_RecordMotionOn(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                                  D3D12_RESOURCE_STATES colourState)
{
    const auto desc = colour->GetDesc();
    const auto width = static_cast<UINT>(desc.Width);
    const auto height = desc.Height;

    using namespace SynthMotion::Handoff;
    Field shared {};

    if (Take(Owner::FrameGen, device, width, height, &shared))
    {
        // DLSS-NR estimated this base frame already, on this device, at this extent.
        _peerWait = {};
        _frameMotion = shared.motion;
        _frameSceneCut = shared.sceneCut;

        if (!_reportedTaken)
        {
            _reportedTaken = true;
            LOG_INFO("synthesized FG input: motion taken from DLSS-NR's estimate of the frame ({}x{}), no second "
                     "estimate",
                     width, height);
        }
    }
    else if (WaitForPeer(_peerWait, _estimator != nullptr, PeerWarming(Owner::FrameGen, device, width, height),
                         NowMs()))
    {
        // DLSS-NR's estimator recorded this frame and is still warming up (the D3D11 bridge at start-up). A
        // second one would warm over the same frames and then sit idle once DLSS-NR publishes, so nothing is
        // recorded: zero motion meanwhile, which is all a warming estimator would have given.
        if (!_reportedPeerWait)
        {
            _reportedPeerWait = true;
            LOG_INFO("synthesized FG input: DLSS-NR's motion estimate is warming up ({}x{}); waiting for it rather "
                     "than running a second estimator",
                     width, height);
        }
    }
    else
    {
        if (_estimator == nullptr)
        {
            if (_peerWait.sinceMs >= 0)
            {
                LOG_INFO("synthesized FG input: stopped waiting for DLSS-NR's motion estimate after {} ms, {} fresh "
                         "frame(s) in a row ({}); running our own",
                         NowMs() - _peerWait.sinceMs, _peerWait.freshFrames,
                         PeerWarming(Owner::FrameGen, device, width, height) ? "still warming" : "no longer announced");
                _peerWait = {};
            }

            _estimator = std::make_unique<SynthMotion::Estimator_Dx12>();
        }

        const long long now = NowMs();
        const bool stale = _lastEstimateMs == 0 || now - _lastEstimateMs > kMotionStaleMs;
        _lastEstimateMs = now;

        if (!_estimator->Record(device, cmdList, colour, colourState, stale))
        {
            if (!_reportedMotionFailure)
            {
                _reportedMotionFailure = true;
                LOG_WARN("synthesized FG input: the motion estimator did not record ({}x{}); FG keeps zero motion",
                         width, height);
            }

            return false;
        }

        _estimatorRecorded = true;

        if (!_reportedEstimator)
        {
            _reportedEstimator = true;
            LOG_INFO("synthesized FG input: frame generation runs its own motion estimate ({}x{})", width, height);
        }

        if (_estimator->Ready() && _estimator->Motion() != nullptr)
        {
            _frameMotion = _estimator->Motion();
            _frameSceneCut = _estimator->SceneCut();
            Publish(Owner::FrameGen, device, _frameMotion, width, height, _frameSceneCut);
        }
        else
        {
            // DLSS-NR's present pass runs after this on native D3D12: it waits for this field instead of
            // building its own estimator.
            AnnounceWarming(Owner::FrameGen, device, width, height);
        }
    }

    if (_frameMotion != nullptr && _EnsureReadback(device, width))
        _RecordSample(cmdList, _frameMotion, width, height);

    return true;
}

bool SynthInputs::_EnsureReadback(ID3D12Device* device, UINT width)
{
    if (_readback != nullptr && _sampleWidth == width)
        return true;

    if (_readback != nullptr)
    {
        // Another width without a Release in between: parked, never freed under the GPU.
        _readback->Unmap(0, nullptr);
        _retired.push_back(_readback);
        _readback = nullptr;
        _readbackData = nullptr;
    }

    // Placed footprints start on 512-byte boundaries, so every row gets a 512-aligned pitch of its own.
    const UINT rowPitch = AlignUp(width * kSampleBytesPerPixel, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (UINT64) rowPitch * kSampleRows * kSampleSlots;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* readback = nullptr;
    {
        // Ours, not the game's: nothing that watches the game's resources should see it.
        ScopedInternalResourceCreation internalResources {};

        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback))))
        {
            _WarnOnce("could not allocate the motion sample readback");
            return false;
        }
    }

    void* mapped = nullptr;
    if (FAILED(readback->Map(0, nullptr, &mapped)) || mapped == nullptr)
    {
        readback->Release();
        _WarnOnce("could not map the motion sample readback");
        return false;
    }

    readback->SetName(L"SynthFG_MotionSamples");
    _readback = readback;
    _readbackData = static_cast<const uint8_t*>(mapped);
    _sampleWidth = width;
    _sampleRowPitch = rowPitch;
    _sampleSlot = 0;
    _sampleRecorded = false;

    for (auto& valid : _slotValid)
        valid = false;

    return true;
}

void SynthInputs::_RecordSample(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* field, UINT width, UINT height)
{
    // Where the estimator, and DLSS-NR after its pass, leave the field.
    constexpr D3D12_RESOURCE_STATES kFieldState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    Transition(cmdList, field, kFieldState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    for (UINT i = 0; i < kSampleRows; ++i)
    {
        const UINT y = std::min(height - 1, (2 * i + 1) * height / (2 * kSampleRows));

        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = _readback;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = (UINT64) (_sampleSlot * kSampleRows + i) * _sampleRowPitch;
        destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
        destination.PlacedFootprint.Footprint.Width = width;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = _sampleRowPitch;

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = field;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        const D3D12_BOX box = { 0, y, 0, width, y + 1, 1 };
        cmdList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    }

    Transition(cmdList, field, D3D12_RESOURCE_STATE_COPY_SOURCE, kFieldState);
    _sampleRecorded = true;
}

void SynthInputs::_ReadSamples()
{
    if (_readbackData == nullptr)
        return;

    // Oldest first; normally exactly one slot comes due per frame.
    for (;;)
    {
        int due = -1;

        for (UINT s = 0; s < kSampleSlots; ++s)
        {
            if (_slotValid[s] && _motionConfirmed >= _slotFrame[s] + kSampleLatency &&
                (due < 0 || _slotFrame[s] < _slotFrame[due]))
            {
                due = (int) s;
            }
        }

        if (due < 0)
            return;

        _slotValid[due] = false;
        _magnitudes.clear();
        bool allZero = true;

        const uint8_t* base = _readbackData + (size_t) due * kSampleRows * _sampleRowPitch;

        for (UINT i = 0; i < kSampleRows; ++i)
        {
            const auto* row = reinterpret_cast<const uint16_t*>(base + (size_t) i * _sampleRowPitch);

            for (UINT x = 0; x < _sampleWidth; x += kSampleStride)
            {
                const uint16_t hx = row[2 * x];
                const uint16_t hy = row[2 * x + 1];

                // Negative zero is zero.
                if (((hx | hy) & 0x7fffu) != 0)
                    allZero = false;

                const float fx = HalfToFloat(hx);
                const float fy = HalfToFloat(hy);
                const float magnitude = std::sqrt(fx * fx + fy * fy);

                if (std::isfinite(magnitude))
                    _magnitudes.push_back(magnitude);
            }
        }

        float median = 0.0f;
        if (!_magnitudes.empty())
        {
            auto middle = _magnitudes.begin() + _magnitudes.size() / 2;
            std::nth_element(_magnitudes.begin(), middle, _magnitudes.end());
            median = *middle;
        }

        // The field's own scene cut already reached this frame's Reset through _frameSceneCut.
        _policy.ObserveMotion(median, allZero, false);
    }
}

void SynthInputs::_ConfirmMotion()
{
    if (!_estimatorRecorded && !_sampleRecorded)
        return;

    if (_estimatorRecorded && _estimator != nullptr)
        _estimator->ConfirmExecuted();

    _estimatorRecorded = false;
    ++_motionConfirmed;

    if (_sampleRecorded)
    {
        _slotValid[_sampleSlot] = true;
        _slotFrame[_sampleSlot] = _motionConfirmed;
        _sampleSlot = (_sampleSlot + 1) % kSampleSlots;
        _sampleRecorded = false;
    }
}

void SynthInputs::_AbandonMotion()
{
    if (_estimatorRecorded && _estimator != nullptr)
    {
        // Published for a list nobody ran: nobody may take it.
        SynthMotion::Handoff::Withdraw(_estimator->Motion());
        _estimator->AbandonRecording();
    }

    _estimatorRecorded = false;
    _sampleRecorded = false;
    _frameMotion = nullptr;
    _frameSceneCut = false;
}

void SynthInputs::_ReleaseMotion()
{
    SynthMotion::Handoff::WithdrawWarming(SynthMotion::Handoff::Owner::FrameGen);

    if (_estimator != nullptr)
    {
        SynthMotion::Handoff::Withdraw(_estimator->Motion());
        _estimator->Release();
        _estimator.reset();
    }

    _lastEstimateMs = 0;
    _peerWait = {};
    _estimatorRecorded = false;
    _frameMotion = nullptr;
    _frameSceneCut = false;

    for (UINT i = 0; i < kMotionLists; ++i)
    {
        SafeRelease(_motionLists[i]);
        SafeRelease(_motionAllocators[i]);
        _motionFenceValues[i] = 0;
    }

    SafeRelease(_motionFence);

    if (_motionFenceEvent != nullptr)
    {
        CloseHandle(_motionFenceEvent);
        _motionFenceEvent = nullptr;
    }

    _motionDevice = nullptr;
    _motionFenceValue = 0;
    _motionSlot = 0;

    if (_readback != nullptr)
    {
        _readback->Unmap(0, nullptr);
        SafeRelease(_readback);
    }

    _readbackData = nullptr;
    _sampleWidth = 0;
    _sampleRowPitch = 0;
    _sampleSlot = 0;
    _sampleRecorded = false;
    _motionConfirmed = 0;

    for (UINT s = 0; s < kSampleSlots; ++s)
    {
        _slotValid[s] = false;
        _slotFrame[s] = 0;
    }
}

void SynthInputs::Feed(IFGFeature_Dx12* fg, ID3D12Device* device)
{
    if (fg == nullptr || device == nullptr || _velocity == nullptr || _depth == nullptr || _clearOwed)
    {
        _frameMotion = nullptr;
        _frameSceneCut = false;
        return;
    }

    auto& cfg = *Config::Instance();
    auto& state = State::Instance();

    // The policy sees every base frame, generated or not: the clock for the floor, the late motion
    // samples for the fast-motion response and the duplicate advice.
    _policy.Configure(cfg.FGSynthesizedFastMotion.value_or_default(), cfg.FGSynthesizedMinFps.value_or_default());
    _policy.ObserveBaseFrame((double) NowMs());
    _ReadSamples();

    if (_policy.TakeDuplicateAdvice())
    {
        LOG_INFO("synthesized FG input: the game presents repeated frames ({} of the last {} motion samples identical, "
                 "interleaved with moving ones). Frame generation cannot fix that cadence; use the emulator's own "
                 "option to skip presenting duplicate frames (PCSX2: Skip Presenting Duplicate Frames)",
                 _policy.IdenticalInWindow(), SynthFgPolicy::kWindow);
    }

    const auto action = _policy.Decide();
    ID3D12Resource* frameMotion = _frameMotion;
    const bool frameSceneCut = _frameSceneCut;
    _frameMotion = nullptr;
    _frameSceneCut = false;

    // Only a field at the presenter's extent can stand in for the zero one.
    if (frameMotion != nullptr)
    {
        const auto motionDesc = frameMotion->GetDesc();
        if ((UINT) motionDesc.Width != _width || motionDesc.Height != _height)
            frameMotion = nullptr;
    }

    FG_Constants fgConstants {};
    fgConstants.displayWidth = _width;
    fgConstants.displayHeight = _height;

    // Zero depth is the far plane only with inverted depth. The motion field is at display size.
    fgConstants.flags |= FG_Flags::InvertedDepth;
    fgConstants.flags |= FG_Flags::DisplayResolutionMVs;

    if (state.hdrOutputActive || _displayFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
        fgConstants.flags |= FG_Flags::Hdr;

    if (cfg.FGAsync.value_or_default())
        fgConstants.flags |= FG_Flags::Async;

    fg->EvaluateState(device, fgConstants);

    // Below the floor FG is not fed: it pauses itself after a few presents and resumes when the rate
    // comes back (synthesized-frame-generation.md, "Low-fps floor").
    if (action == SynthFgPolicy::Action::Skip)
    {
        if (!_reportedSkip)
        {
            _reportedSkip = true;
            LOG_INFO("synthesized FG input: base rate {:.1f} fps is below SynthesizedMinFps {:.1f}; not generating "
                     "until it recovers (reported once)",
                     _policy.BaseFps(), cfg.FGSynthesizedMinFps.value_or_default());
        }

        return;
    }

    // No upscaler parameters to read them from, so the config values, with near and far swapped for
    // inverted depth exactly as the upscaler inputs do, and its 60 degree fallback.
    const float cameraNear = cfg.FsrCameraFar.value_or_default();
    const float cameraFar = cfg.FsrCameraNear.value_or_default();
    float cameraVFov = GetRadiansFromDeg(60);

    if (cfg.FsrVerticalFov.has_value())
        cameraVFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
    else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        cameraVFov = GetVerticalFovFromHorizontal(GetRadiansFromDeg(cfg.FsrHorizontalFov.value()), (float) _width,
                                                  (float) _height);

    fg->StartNewFrame();

    fg->SetCameraValues(cameraNear, cameraFar, cameraVFov, (float) _width / (float) _height, 0.0f);
    fg->SetFrameTimeDelta(state.lastFGFrameTime);
    fg->SetMVScale(1.0f, 1.0f);
    fg->SetJitter(0.0f, 0.0f);
    // Reset repeats the frame instead of interpolating it (FSR's interpolation copies the back buffer on a
    // reset): the history start of a size, a scene cut, and the fast-motion response.
    fg->SetReset((_resetOwed || frameSceneCut || action == SynthFgPolicy::Action::Reset) ? 1 : 0);
    fg->SetInterpolationRect(_width, _height);

    if (state.isShuttingDown || !fg->IsActive() || !cfg.FGEnabled.value_or_default() ||
        state.currentSwapchain == nullptr)
    {
        return;
    }

    if (fg->Mutex.getOwner() == 2)
    {
        LOG_TRACE("Waiting for present!");
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }

    auto cmdList = fg->GetUICommandList();

    Dx12Resource velocity {};
    velocity.type = FG_ResourceType::Velocity;
    velocity.cmdList = cmdList;
    velocity.resource = frameMotion != nullptr ? frameMotion : _velocity;
    velocity.width = _width;
    velocity.height = _height;
    velocity.state = kRestState;
    velocity.validity = FG_ResourceValidity::UntilPresent;

    Dx12Resource depth {};
    depth.type = FG_ResourceType::Depth;
    depth.cmdList = cmdList;
    depth.resource = _depth;
    depth.width = _width;
    depth.height = _height;
    depth.state = kRestState;
    depth.validity = FG_ResourceValidity::UntilPresent;

    const bool velocitySet = fg->SetResource(&velocity);
    const bool depthSet = fg->SetResource(&depth);

    if (!velocitySet || !depthSet)
        return;

    // History starts over once FG has actually taken the first pair of a size, not before: a paused
    // FG refuses resources, and a reset spent on a refused frame is a reset lost.
    _resetOwed = false;

    if (!_fedReported)
    {
        _fedReported = true;
        LOG_INFO("synthesized FG input feeding {}: {}x{} zero motion (R16G16_FLOAT) and constant depth (R32_FLOAT, "
                 "inverted), display format {}",
                 fg->Name(), _width, _height, (UINT) _displayFormat);
    }

    if (frameMotion != nullptr && !_reportedMotionFed)
    {
        _reportedMotionFed = true;
        LOG_INFO("synthesized FG input: FSR-FG now gets the synthesized motion field ({}x{}, current to previous in "
                 "pixels, motion vector scale 1 x 1)",
                 _width, _height);
    }
}

void SynthInputs::Release()
{
    _ReleaseMotion();

    for (auto* resource : _retired)
    {
        if (resource != nullptr)
            resource->Release();
    }
    _retired.clear();

    SafeRelease(_velocity);
    SafeRelease(_depth);
    SafeRelease(_rtvHeap);

    SafeRelease(_ownList);
    SafeRelease(_ownAllocator);
    SafeRelease(_ownFence);

    if (_ownFenceEvent != nullptr)
    {
        CloseHandle(_ownFenceEvent);
        _ownFenceEvent = nullptr;
    }

    _ownDevice = nullptr;
    _ownFenceValue = 0;

    _width = 0;
    _height = 0;
    _clearOwed = false;
    _clearRecorded = false;
    _resetOwed = true;
    _allocFailed = false;
}
