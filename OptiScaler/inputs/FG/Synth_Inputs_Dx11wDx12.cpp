#include "pch.h"
#include "Synth_Inputs_Dx11wDx12.h"

#include "MathUtils.h"

#include <State.h>
#include <framegen/IFGFeature_Dx12.h>

using namespace OptiMath;

namespace
{
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

SynthInputsDx11wDx12::~SynthInputsDx11wDx12() { Release(); }

void SynthInputsDx11wDx12::_WarnOnce(const char* what)
{
    if (_warned)
        return;

    _warned = true;
    LOG_WARN("synthesized FG input: {} ({}x{}); frame generation gets no input from it, reported once", what, _width,
             _height);
}

bool SynthInputsDx11wDx12::_Allocate(ID3D12Device* device, UINT width, UINT height)
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

void SynthInputsDx11wDx12::_Retire()
{
    if (_velocity != nullptr)
        _retired.push_back(_velocity);

    if (_depth != nullptr)
        _retired.push_back(_depth);

    _velocity = nullptr;
    _depth = nullptr;
}

bool SynthInputsDx11wDx12::RecordInit(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height,
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

void SynthInputsDx11wDx12::ConfirmExecuted()
{
    if (!_clearRecorded)
        return;

    _clearRecorded = false;
    _clearOwed = false;
}

// The barrier out of the clear state never ran either, so the pair is still where the clear expects
// it, and the next RecordInit records the same clear again.
void SynthInputsDx11wDx12::AbandonRecording() { _clearRecorded = false; }

void SynthInputsDx11wDx12::Feed(IFGFeature_Dx12* fg, ID3D12Device* device)
{
    if (fg == nullptr || device == nullptr || _velocity == nullptr || _depth == nullptr || _clearOwed)
        return;

    auto& cfg = *Config::Instance();
    auto& state = State::Instance();

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

    // No upscaler parameters to read them from, so the config values, with near and far swapped for
    // inverted depth exactly as UpscalerInputsDx11wDx12 does, and its 60 degree fallback.
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
    fg->SetReset(_resetOwed ? 1 : 0);
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
    velocity.resource = _velocity;
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
}

void SynthInputsDx11wDx12::Release()
{
    for (auto* resource : _retired)
    {
        if (resource != nullptr)
            resource->Release();
    }
    _retired.clear();

    SafeRelease(_velocity);
    SafeRelease(_depth);
    SafeRelease(_rtvHeap);

    _width = 0;
    _height = 0;
    _clearOwed = false;
    _clearRecorded = false;
    _resetOwed = true;
    _allocFailed = false;
}
