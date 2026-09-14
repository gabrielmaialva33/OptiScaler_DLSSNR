#include "pch.h"
#include "dx11_with_dx12_sc.h"

#include <with_dx12/with_dx12.h>

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <Util.h>
#include <Config.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}
} // namespace

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags)
    : _real(real), _fgSwapChain(fgSC), _dx11Device(pDevice), _handle(hWnd), _refcount(1)
{
    _id = ++scCount;
    _lastFlags = flags;
    _fg = State::Instance().currentFG;
    _fgSwapchainContext = _fg != nullptr ? _fg->SwapchainContext() : nullptr;

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
        if (_dx12Device != nullptr)
            _dx12Device->AddRef();
        if (_dx12CommandQueue != nullptr)
            _dx12CommandQueue->AddRef();
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    {
        std::lock_guard lock(_retiredMutex);
        _nextLive = _live;
        _live = this;
    }
    _CollectRetired();

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    {
        std::lock_guard lock(_retiredMutex);
        auto link = &_live;
        while (*link != nullptr && *link != this)
            link = &(*link)->_nextLive;
        if (*link == this)
            *link = _nextLive;
    }

    // Release (or the retirement collector) has already proved completion, or confirmed
    // device loss. Direct destruction is not a supported ownership path.
    _ReleaseInteropObjects();

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
    SafeRelease(_presentQueue);
    SafeRelease(_dx12CommandQueue);
    SafeRelease(_dx12Device);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4 == nullptr && _fgSwapChain == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        // Drain before FG, overlay, either swapchain, or a shared resource can be destroyed.
        const auto result = _DrainForTeardown(5000);
        const bool deviceLost = _DevicesRemoved();
        if (FAILED(result) && !deviceLost)
        {
            LOG_ERROR("retaining bridge {} after unproven teardown: {:X}", _id, (UINT) result);
            // Keep the entire wrapper, including explicit destination, device and queue references.
            // No worker thread or saved HWND callback may tear down a later FG generation.
            _DetachWrapperGlobals();
            std::lock_guard lock(_retiredMutex);
            _nextRetired = _retired;
            _retired = this;
            return 0;
        }
        _FinishRelease(deviceLost);
        delete this;
    }

    return ret;
}

bool Dx11wDx12SC::_OwnsFgPresenter() const
{
    if (_fgSwapChain == nullptr || State::Instance().currentFGSwapchain != _fgSwapChain || _fg == nullptr ||
        State::Instance().currentFG != _fg || _fg->SwapchainContext() != _fgSwapchainContext)
        return false;

    return _OwnsPresenter();
}

bool Dx11wDx12SC::_OwnsPresenter() const
{
    // FGPreserveSwapChain can hand the SAME presenter/context to a new wrapper. Its most
    // recent wrapper owns teardown, even if an older wrapper still holds COM references.
    std::lock_guard lock(_retiredMutex);
    for (auto item = _live; item != nullptr; item = item->_nextLive)
    {
        if (item->_fgSwapChain == _fgSwapChain)
            return item == this;
    }
    return false;
}

bool Dx11wDx12SC::_OwnsOverlay() const
{
    // CleanupRenderTarget also mutates currentFG. Never use it for a different FG presenter.
    const bool current =
        State::Instance().currentSwapchain == this || State::Instance().currentWrappedSwapchain == this;
    return _OwnsFgPresenter() || (current && _OwnsPresenter() && State::Instance().currentFGSwapchain == nullptr);
}

bool Dx11wDx12SC::_DevicesRemoved() const
{
    // Shared storage can still be used by either API. One removed device does not prove that
    // the other has stopped using it; quarantine that case until a complete drain is possible.
    return _dx12Device != nullptr && FAILED(_dx12Device->GetDeviceRemovedReason()) && _dx11Device != nullptr &&
           FAILED(_dx11Device->GetDeviceRemovedReason());
}

void Dx11wDx12SC::_DetachWrapperGlobals()
{
    const bool wasCurrent =
        State::Instance().currentSwapchain == this || State::Instance().currentWrappedSwapchain == this;

    if (State::Instance().currentSwapchain == this)
        State::Instance().currentSwapchain = nullptr;

    if (State::Instance().currentWrappedSwapchain == this)
        State::Instance().currentWrappedSwapchain = nullptr;

    if (State::Instance().currentRealSwapchain == _real)
        State::Instance().currentRealSwapchain = nullptr;

    // Pointer equality is not ownership here, and this is the one guard above where that
    // distinction bites: the swapchain, wrapper and presenter comparisons each match an object
    // created for this instance alone, while the D3D11 device is shared by every wrapper on it.
    // Two wrappers on one device, the second current, releasing the first: the equality holds
    // and clears the device the live one is still using. wasCurrent is the missing half.
    if (wasCurrent && State::Instance().currentD3D11Device == _dx11Device)
        State::Instance().currentD3D11Device = nullptr;

    // Guarded like every other global above it. This line used to run unconditionally, so
    // releasing any instance cleared the interop mode for all of them.
    if (wasCurrent)
        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;
}

void Dx11wDx12SC::_FinishRelease(bool deviceLost)
{
    // Whether the globals below describe this instance. A game can hold more than one of these
    // -- a launcher window, a tool window, a second viewport -- and releasing one of them must
    // not tell the rest of OptiScaler that there is no interop swapchain any more.
    auto fg = State::Instance().currentFG;
    const auto fgContext = fg != nullptr ? fg->SwapchainContext() : nullptr;
    const bool ownsFg = _OwnsFgPresenter();
    _wasCurrentOnRelease = _OwnsOverlay();
    _DetachWrapperGlobals();

    if (_OwnsPresenter())
        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

    if (ownsFg)
        State::Instance().currentFGSwapchain = nullptr;

    // Completion has been proved (or both devices are lost). Drop our explicit buffer pins
    // before the backend's swapchain teardown, whose release hooks may trim buffer references.
    _ReleaseInteropObjects();

    // FG ownership is presenter identity, not HWND identity or game-facing wrapper identity.
    if (!deviceLost && ownsFg && fgContext != nullptr && fg->Mutex.getOwner() != 1 &&
        fg->SwapchainContext() == fgContext)
    {
        fg->Deactivate();
        fg->ReleaseSwapchain(_handle);
    }
    if (_wasCurrentOnRelease && !deviceLost && (fg == nullptr || fg->Mutex.getOwner() != 1))
        MenuOverlayDx::CleanupRenderTarget(true, _handle);
}

void Dx11wDx12SC::_CollectRetired()
{
    for (;;)
    {
        Dx11wDx12SC* ready = nullptr;
        bool deviceLost = false;
        {
            std::lock_guard lock(_retiredMutex);
            auto link = &_retired;
            while (*link != nullptr)
            {
                auto item = *link;
                const auto result = item->_DrainForTeardown(0);
                deviceLost = item->_DevicesRemoved();
                if (SUCCEEDED(result) || deviceLost)
                {
                    *link = item->_nextRetired;
                    ready = item;
                    break;
                }
                link = &item->_nextRetired;
            }
        }
        if (ready == nullptr)
            return;
        // Backend cleanup can re-enter Release. Never call it while holding the retirement lock.
        ready->_FinishRelease(deviceLost);
        delete ready;
    }
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    if (_resizeIncomplete)
        return DXGI_ERROR_INVALID_CALL;

    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if (_fg == nullptr)
    {
        _fg = State::Instance().currentFG;
        _fgSwapchainContext = _fg != nullptr ? _fg->SwapchainContext() : nullptr;
    }
    if (_fg != State::Instance().currentFG)
        return E_UNEXPECTED;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    _CollectRetired();
    if (_dx11DrainValue != 0)
    {
        const auto result = _DrainForTeardown(5000);
        if (FAILED(result))
            return result;
        _ResetTeardownDrain();
    }
    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto presentQueue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;
    if (presentQueue == nullptr || (_presentQueue != nullptr && _presentQueue != presentQueue))
        return E_UNEXPECTED;
    if (_presentQueue == nullptr)
    {
        _presentQueue = presentQueue;
        _presentQueue->AddRef();
    }
    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    // Before D3D11 writes the shadow, not after. _CopyDx11BackBufferToShared copies into
    // _sharedDx11BackBufferCopies[_currentFakeIndex], and the only wait on that slot used to be the
    // one inside _CopyDx11SharedToDx12FGBackBuffer, which runs two steps later -- so D3D11 could
    // overwrite a shadow D3D12 was still reading for an earlier frame that reused the slot. The
    // fence value is per slot, so in steady state the ring has already come round and this returns
    // at once; it only blocks when the GPU is genuinely still behind.
    if (const auto result = _WaitForCopyAllocator(_currentFakeIndex); FAILED(result))
        return result;

    if (!_CopyDx11BackBufferToShared(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    if (_real != nullptr)
    {
        UINT realFlags = Flags;

        // Do not wait for it
        auto realPresentResult = _real->Present(0, realFlags);

        if (FAILED(realPresentResult))
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
    }

    auto result = _fgSwapChain->Present(SyncInterval, Flags);

    if (SUCCEEDED(result))
        _AdvanceFakeBackBufferIndex();
    else
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X}, caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    const auto drainResult = _DrainForTeardown(5000);
    if (FAILED(drainResult))
    {
        LOG_ERROR("bridge resize preflight failed: {:X}; resources and swapchains retained", (UINT) drainResult);
        return drainResult;
    }

    if (_OwnsOverlay())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ResetTeardownDrain();
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);

    if (SUCCEEDED(realResult))
        _resizeIncomplete = FAILED(fgResult);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetMaximumFrameLatency(MaxLatency);

    return _real2 != nullptr ? _real2->SetMaximumFrameLatency(MaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetMaximumFrameLatency(pMaxLatency);

    return _real2 != nullptr ? _real2->GetMaximumFrameLatency(pMaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFrameLatencyWaitableObject();

    return _real2 != nullptr ? _real2->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    if (_real3 == nullptr)
        return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    const auto drainResult = _DrainForTeardown(5000);
    if (FAILED(drainResult))
    {
        LOG_ERROR("bridge resize preflight failed: {:X}; resources and swapchains retained", (UINT) drainResult);
        return drainResult;
    }

    if (_OwnsOverlay())
        MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ResetTeardownDrain();
    _ReleaseInteropBackBuffers();

    HRESULT realResult =
        _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
    }

    if (SUCCEEDED(realResult))
        _resizeIncomplete = FAILED(fgResult);

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();

        if (Config::Instance()->FGEnabled.value_or_default())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        if (_sharedDx11BackBufferCopies.size() != _bufferCount)
            _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

        if (_openedDx11BackBuffers.size() != _bufferCount)
            _openedDx11BackBuffers.resize(_bufferCount, nullptr);

        if (_sharedBackBufferHandles.size() != _bufferCount)
            _sharedBackBufferHandles.resize(_bufferCount, nullptr);

        if (_openedDx11BackBufferStates.size() != _bufferCount)
            _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    _sharedDx11BackBufferCopies.assign(_bufferCount, nullptr);
    _openedDx11BackBuffers.assign(_bufferCount, nullptr);
    _sharedBackBufferHandles.assign(_bufferCount, nullptr);
    _openedDx11BackBufferStates.assign(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return false;
    }

    if (_sharedDx11BackBufferCopies.size() <= index)
        _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

    if (_openedDx11BackBuffers.size() <= index)
        _openedDx11BackBuffers.resize(_bufferCount, nullptr);

    if (_sharedBackBufferHandles.size() <= index)
        _sharedBackBufferHandles.resize(_bufferCount, nullptr);

    if (_openedDx11BackBufferStates.size() <= index)
        _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[_currentFakeIndex] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return false;
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[_currentFakeIndex] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[_currentFakeIndex]);
        if (FAILED(result) || _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = _sharedDx11BackBufferCopies[_currentFakeIndex]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[_currentFakeIndex]);
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }
    }

    // Dx12 handle to Dx11 shared texture
    result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[_currentFakeIndex],
                                           IID_PPV_ARGS(&_openedDx11BackBuffers[_currentFakeIndex]));
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
        return false;
    }

    _openedDx11BackBufferStates[_currentFakeIndex] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index)
{
    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return false;
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[_currentFakeIndex]);

    _hasInteropWork = true;
    _dx11Context->CopyResource(_sharedDx11BackBufferCopies[_currentFakeIndex], sourceTexture);
    sourceTexture->Release();
    return true;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return false;

    const auto waitValue = _sharedFenceValue++;

    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return false;
    }

    _dx11Context4->Flush();

    // Important:
    // Do not block the FG/present queue on the D3D11 shared fence.
    // Isolate the cross-API wait on the interop copy queue.
    result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

HRESULT Dx11wDx12SC::_WaitForCopyAllocator(UINT slot)
{
    if (slot >= _copyAllocatorFenceValues.size())
        return E_INVALIDARG;
    const auto result =
        WaitForBridgeFence(_copyFence, _dx12Device, _copyFenceEvent, _copyAllocatorFenceValues[slot], 5000);
    if (FAILED(result))
        LOG_ERROR("bridge slot {} fence {} wait failed: {:X}", slot, _copyAllocatorFenceValues[slot], (UINT) result);
    return result;
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index)
{
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || _currentFakeIndex >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    const UINT copySlot = _currentFakeIndex;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return false;

    if (FAILED(_WaitForCopyAllocator(copySlot)))
        return false;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return false;
    }

    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return false;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return false;
    }

    auto sourceBefore = _openedDx11BackBufferStates[copySlot];

    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer, _openedDx11BackBuffers[copySlot]);

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    if (_copyDestinations.size() <= copySlot)
        _copyDestinations.resize(copySlot + 1, nullptr);
    SafeRelease(_copyDestinations[copySlot]);
    _copyDestinations[copySlot] = fgBackBuffer;

    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    _dx12CommandQueue->ExecuteCommandLists(1, lists);

    const auto signalValue = ++_copyFenceValue;

    // Recorded before the Signal, not after. ExecuteCommandLists above has already submitted work
    // that reads this shadow slot, so the slot is busy from that moment whatever happens next. If
    // the Signal fails and we leave the slot's old value in place, the next _WaitForCopyAllocator
    // reads a value the fence has already passed -- or zero, meaning never used -- and reports the
    // slot free while the GPU may still be reading it. Recording first makes a failed Signal wait
    // for a value that never arrives, which fails closed instead of handing back live memory.
    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;

    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy fence signal failed: {:X}. slot {} is left pending on fence {}, which will not "
                  "arrive; waits on it will time out rather than reuse memory the GPU may still be reading",
                  (UINT) result, copySlot, signalValue);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    if (_presentQueue == nullptr || _copyFence == nullptr)
        return false;

    if (_lastInteropCopyFenceValue == 0)
        return true;

    auto result = _presentQueue->Wait(_copyFence, _lastInteropCopyFenceValue);
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

HRESULT Dx11wDx12SC::_WaitForCopyQueueIdle(DWORD timeout)
{
    UINT64 waitValue = _lastInteropCopyFenceValue;
    for (const auto value : _copyAllocatorFenceValues)
        waitValue = std::max(waitValue, value);
    // Preserve accounting even on success; the destructive caller resets it after the full drain.
    return WaitForBridgeFence(_copyFence, _dx12Device, _copyFenceEvent, waitValue, timeout);
}

HRESULT Dx11wDx12SC::_DrainForTeardown(DWORD timeout)
{
    if (!_hasInteropWork)
        return S_OK;
    const auto deadline = GetTickCount64() + timeout;
    const auto remaining = [&]() -> DWORD
    {
        const auto now = GetTickCount64();
        return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
    };
    auto result = _WaitForCopyQueueIdle(remaining());
    if (FAILED(result))
        return result;

    // Cover a D3D11 write even when its following Signal/queue Wait/copy submission failed.
    if (_dx11DrainValue == 0)
    {
        if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12SharedFence == nullptr)
            return E_UNEXPECTED;
        const auto value = _sharedFenceValue++;
        result = _dx11Context4->Signal(_dx11Fence, value);
        if (FAILED(result))
            return result;
        _dx11Context4->Flush();
        _dx11DrainValue = value;
    }
    result = WaitForBridgeFence(_dx12SharedFence, _dx12Device, _copyFenceEvent, _dx11DrainValue, remaining());
    if (FAILED(result))
        return result;

    // Fresh markers include work submitted after the saved copy fence, in particular overlay work.
    // Separate fences are essential: a higher value on one queue proves nothing about another.
    ID3D12CommandQueue* queues[] = { _dx12CommandQueue, _presentQueue };
    for (UINT i = 0; i < 2; ++i)
    {
        if (queues[i] == nullptr)
            continue;
        if (_drainFences[i] == nullptr)
        {
            result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_drainFences[i]));
            if (FAILED(result))
                return result;
        }
        if (!_drainSignaled[i])
        {
            result = queues[i]->Signal(_drainFences[i], 1);
            if (FAILED(result))
                return result;
            _drainSignaled[i] = true;
        }
        result = WaitForBridgeFence(_drainFences[i], _dx12Device, _copyFenceEvent, 1, remaining());
        if (FAILED(result))
            return result;
    }
    return S_OK;
}

void Dx11wDx12SC::_ResetTeardownDrain()
{
    for (UINT i = 0; i < 2; ++i)
    {
        SafeRelease(_drainFences[i]);
        _drainSignaled[i] = false;
    }
    _dx11DrainValue = 0;
    _hasInteropWork = false;
    _lastInteropCopyFenceValue = 0;
    for (auto& value : _copyAllocatorFenceValues)
        value = 0;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    _interopInitialized = false;

    for (auto& resource : _copyDestinations)
        SafeRelease(resource);
    _copyDestinations.clear();
    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

void Dx11wDx12SC::_ReleaseInteropObjects()
{
    // Only called after the preflight drain, or confirmed loss of both participating devices.
    _ResetTeardownDrain();
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;

    _interopInitialized = false;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    // Wrapped into the new range, not merely zeroed when the count reaches zero. A ResizeBuffers
    // that asks for fewer buffers shrinks the shadow arrays just below, and the old index can then
    // sit past their end -- _CopyDx11BackBufferToShared indexes _sharedDx11BackBufferCopies with it
    // directly. Modulo keeps it in range for any count.
    _currentFakeIndex = _bufferCount > 0 ? (_currentFakeIndex % _bufferCount) : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }
