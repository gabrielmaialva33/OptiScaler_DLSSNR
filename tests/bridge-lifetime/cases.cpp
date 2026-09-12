struct Fixture
{
    ID3D12Device dx11, dx12;
    ID3D12Fence copyFence, sharedFence;
    ID3D12CommandQueue copyQueue, presentQueue;
    Dx11Context context;
    ID3D12Resource shadow;
    ID3D12CommandAllocator allocator;
    ID3D12GraphicsCommandList list;
    Swapchain real, presenter;
    FG fg;
    Dx11wDx12SC* sc = new Dx11wDx12SC;
    Fixture()
    {
        sc->_nextLive = Dx11wDx12SC::_live;
        Dx11wDx12SC::_live = sc;
        State::Instance() = {};
        clockMs = 0;
        wakes.clear();
        waitDurations.clear();
        MenuOverlayDx::cleanups = 0;
        sc->_real = &real;
        sc->_fgSwapChain = &presenter;
        sc->_dx11Device = &dx11;
        sc->_dx12Device = &dx12;
        sc->_dx11Context4 = &context;
        sc->_dx12CommandQueue = &copyQueue;
        sc->_presentQueue = &presentQueue;
        sc->_copyFence = &copyFence;
        sc->_dx11Fence = &sharedFence;
        sc->_dx12SharedFence = &sharedFence;
        sharedFence.AddRef();
        sc->_copyFenceEvent = reinterpret_cast<HANDLE>(1);
        sc->_copyAllocatorFenceValues = { 0 };
        sc->_copyAllocators = { &allocator };
        sc->_copyCommandLists = { &list };
        sc->_openedDx11BackBuffers = { &shadow };
        sc->_openedDx11BackBufferStates = { 0 };
        sc->_fg = &fg;
        sc->_fgSwapchainContext = fg.context;
        auto& state = State::Instance();
        state.currentSwapchain = sc;
        state.currentWrappedSwapchain = sc;
        state.currentRealSwapchain = &real;
        state.currentFGSwapchain = &presenter;
        state.currentFG = &fg;
        state.currentD3D11Device = &dx11;
    }
    ~Fixture()
    {
        if (sc)
        {
            assert(sc->Release() == 0);
        }
    }
};

void waitCases()
{
    ID3D12Device device;
    ID3D12Fence fence;
    auto event = reinterpret_cast<HANDLE>(1);
    clockMs = 0;
    wakes.clear();
    waitDurations.clear();
    assert(WaitForBridgeFence(nullptr, &device, nullptr, 0, 5000) == S_OK);
    assert(WaitForBridgeFence(nullptr, &device, nullptr, 1, 5000) == E_UNEXPECTED);
    fence.completed = 10;
    assert(WaitForBridgeFence(&fence, &device, event, 10, 5000) == S_OK);
    assert(fence.registrations == 0);
    fence.completed = UINT64_MAX;
    assert(WaitForBridgeFence(&fence, &device, event, 10, 5000) == DXGI_ERROR_DEVICE_REMOVED);
    fence.completed = 0;
    wakes.push_back(
        [&](DWORD)
        {
            device.reason = DXGI_ERROR_DEVICE_HUNG;
            fence.completed = UINT64_MAX;
            ++clockMs;
            return WAIT_OBJECT_0;
        });
    assert(WaitForBridgeFence(&fence, &device, event, 10, 5000) == DXGI_ERROR_DEVICE_HUNG);

    device.reason = S_OK;
    fence.completed = 0;
    clockMs = 0;
    assert(WaitForBridgeFence(&fence, &device, event, 10, 5000) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    waitDurations.clear();
    wakes.push_back(
        [&](DWORD)
        {
            fence.completed = 10;
            clockMs += 1000;
            return WAIT_OBJECT_0;
        });
    wakes.push_back(
        [&](DWORD)
        {
            fence.completed = 20;
            clockMs += 1000;
            return WAIT_OBJECT_0;
        });
    assert(WaitForBridgeFence(&fence, &device, event, 20, 5000) == S_OK);
    assert((waitDurations == std::vector<DWORD> { 5000, 4000 }));
    fence.completed = 0;
    clockMs = 0;
    waitDurations.clear();
    wakes.push_back(
        [&](DWORD)
        {
            clockMs += 4000;
            return WAIT_OBJECT_0;
        });
    assert(WaitForBridgeFence(&fence, &device, event, 20, 5000) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    assert(clockMs == 5000);
    assert((waitDurations == std::vector<DWORD> { 5000, 1000 }));
    fence.registration = -88;
    assert(WaitForBridgeFence(&fence, &device, event, 20, 5000) == -88);
    fence.registration = S_OK;
    wakes.push_back(
        [](DWORD)
        {
            lastError = 99;
            return WAIT_FAILED;
        });
    assert(WaitForBridgeFence(&fence, &device, event, 20, 5000) == -99);
}

void helperRemovalCases()
{
    for (bool idle : { false, true })
    {
        Fixture f;
        f.sc->_copyAllocatorFenceValues[0] = 8;
        f.sc->_lastInteropCopyFenceValue = 8;
        wakes.push_back(
            [&](DWORD)
            {
                f.copyFence.completed = UINT64_MAX;
                f.dx12.reason = DXGI_ERROR_DEVICE_HUNG;
                ++clockMs;
                return WAIT_OBJECT_0;
            });
        const auto result = idle ? f.sc->_WaitForCopyQueueIdle(5000) : f.sc->_WaitForCopyAllocator(0);
        assert(result == DXGI_ERROR_DEVICE_HUNG);
        assert(f.sc->_copyAllocatorFenceValues[0] == 8 && f.sc->_lastInteropCopyFenceValue == 8);
        f.dx11.reason = DXGI_ERROR_DEVICE_REMOVED;
    }
}

void failedSignalCases()
{
    for (bool resize1 : { false, true })
    {
        Fixture f;
        if (resize1)
        {
            f.sc->_real3 = &f.real;
            f.real.AddRef();
        }
        f.sc->_hasInteropWork = true;
        f.copyQueue.signalResult = E_FAIL;
        assert(!f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(f.copyQueue.executions == 1);
        assert(f.sc->_copyAllocatorFenceValues[0] == 2 && f.sc->_lastInteropCopyFenceValue == 2);
        assert(f.presenter.buffer.refs == 2); // explicit destination pin survives the failed Signal
        const auto result = resize1 ? f.sc->ResizeBuffers1(2, 800, 600, 0, 0, nullptr, nullptr)
                                    : f.sc->ResizeBuffers(2, 800, 600, 0, 0);
        assert(result == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        assert(f.real.resizes == 0 && f.presenter.resizes == 0 && f.sc->refreshes == 0);
        assert(f.shadow.releases == 0 && f.allocator.releases == 0 && f.presenter.buffer.refs == 2);
        assert(MenuOverlayDx::cleanups == 0 && f.fg.releases == 0);
        const int before = deaths;
        auto retired = f.sc;
        assert(f.sc->Release() == 0);
        f.sc = nullptr;
        assert(deaths == before && Dx11wDx12SC::_retired == retired);
        assert(State::Instance().currentWrappedSwapchain == nullptr);
        assert(f.copyFence.releases == 0 && f.shadow.releases == 0 && f.real.releases == 0);
        assert(f.presenter.releases == 0 && f.copyQueue.releases == 0 && f.context.releases == 0);
        // Only one removed device does not establish safety for shared resources.
        f.dx12.reason = DXGI_ERROR_DEVICE_REMOVED;
        f.copyFence.completed = UINT64_MAX;
        Dx11wDx12SC::_CollectRetired();
        assert(deaths == before && f.shadow.releases == 0);
        f.dx11.reason = DXGI_ERROR_DEVICE_REMOVED;
        Dx11wDx12SC::_CollectRetired();
        assert(deaths == before + 1 && Dx11wDx12SC::_retired == nullptr);
        assert(f.shadow.releases == 1 && f.allocator.releases == 1 && f.presenter.buffer.refs == 1);
        assert(f.presenter.releases == 1 && f.copyQueue.releases == 1 && f.context.releases == 1);
        assert(MenuOverlayDx::cleanups == 0 && f.fg.releases == 0); // distinct device-loss path
    }
}

void ownershipCases()
{
    {
        Fixture f;
        Swapchain replacement;
        auto& state = State::Instance();
        state.currentFGSwapchain = &replacement;
        state.currentSwapchain = &replacement;
        state.currentWrappedSwapchain = &replacement;
        assert(!f.sc->_OwnsFgPresenter());
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
        assert(MenuOverlayDx::cleanups == 0);
        assert(f.sc->ResizeBuffers1(2, 800, 600, 0, 0, nullptr, nullptr) == S_OK);
        assert(MenuOverlayDx::cleanups == 0);
        f.sc->Release();
        f.sc = nullptr; // HWND deliberately unchanged
        assert(f.fg.deactivations == 0 && f.fg.releases == 0 && MenuOverlayDx::cleanups == 0);
        assert(state.currentFGSwapchain == &replacement && state.currentD3D11Device == &f.dx11);
        assert(state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12);
    }
    {
        Fixture f;
        f.fg.context = reinterpret_cast<void*>(999); // same backend, different generation
        assert(!f.sc->_OwnsFgPresenter());
        f.sc->Release();
        f.sc = nullptr;
        assert(f.fg.deactivations == 0 && f.fg.releases == 0 && MenuOverlayDx::cleanups == 0);
    }
    {
        Fixture f;
        State::Instance().currentSwapchain = nullptr;
        State::Instance().currentWrappedSwapchain = nullptr;
        f.sc->Release();
        f.sc = nullptr;
        assert(f.fg.deactivations == 1 && f.fg.releases == 1); // presenter ownership, not wasCurrent
    }
}

void preservedPresenterCase()
{
    Fixture f;
    auto replacement = new Dx11wDx12SC;
    replacement->_fgSwapChain = &f.presenter;
    f.presenter.AddRef();
    replacement->_fg = &f.fg;
    replacement->_fgSwapchainContext = f.fg.context;
    replacement->_nextLive = Dx11wDx12SC::_live;
    Dx11wDx12SC::_live = replacement;
    State::Instance().currentSwapchain = replacement;
    State::Instance().currentWrappedSwapchain = replacement;
    assert(!f.sc->_OwnsFgPresenter() && replacement->_OwnsFgPresenter());
    f.sc->Release();
    f.sc = nullptr;
    assert(f.fg.releases == 0 && MenuOverlayDx::cleanups == 0);
    assert(State::Instance().currentFGSwapchain == &f.presenter);
    replacement->Release();
    assert(f.fg.releases == 1);
}

void drainCases()
{
    Fixture f;
    f.sc->_hasInteropWork = true;
    f.sc->_copyAllocatorFenceValues[0] = 5;
    f.sc->_lastInteropCopyFenceValue = 5;
    f.copyFence.completed = 5;
    f.presentQueue.completeSignals = false;
    assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    assert(f.real.resizes == 0 && f.shadow.releases == 0 && f.sc->_lastInteropCopyFenceValue == 5);
    assert(f.copyQueue.signaled.size() == 1 && f.presentQueue.signaled.size() == 1);
    assert(f.copyQueue.signaled[0] != f.presentQueue.signaled[0]);
    f.presentQueue.signaled[0]->completed = 1;
    assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
    assert(f.real.resizes == 1 && f.presenter.resizes == 1);
    assert(f.shadow.releases == 1 && f.sc->_lastInteropCopyFenceValue == 0);
}

void partialResizeCase()
{
    Fixture f;
    f.presenter.resizeResult = E_FAIL;
    assert(f.sc->ResizeBuffers(3, 800, 600, 0, 0) == E_FAIL);
    assert(f.sc->_resizeIncomplete && f.sc->refreshes == 0);
    f.real.resizeResult = E_FAIL;
    assert(f.sc->ResizeBuffers(3, 800, 600, 0, 0) == E_FAIL);
    assert(f.sc->_resizeIncomplete);
    f.real.resizeResult = S_OK;
    f.presenter.resizeResult = S_OK;
    assert(f.sc->ResizeBuffers(3, 800, 600, 0, 0) == S_OK);
    assert(!f.sc->_resizeIncomplete && f.sc->refreshes == 1);
}

void resizeErrorCases()
{
    for (bool waitFailed : { false, true })
    {
        Fixture f;
        f.sc->_hasInteropWork = true;
        f.sc->_copyAllocatorFenceValues[0] = 6;
        if (waitFailed)
            wakes.push_back(
                [](DWORD)
                {
                    lastError = 87;
                    return WAIT_FAILED;
                });
        else
            f.copyFence.registration = -88;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == (waitFailed ? -87 : -88));
        assert(f.real.resizes == 0 && f.presenter.resizes == 0 && f.shadow.releases == 0);
        f.copyFence.completed = 6;
    }
    {
        Fixture f;
        // D3D11 wrote a shadow, but its Signal failed before any D3D12 copy was submitted.
        f.sc->_hasInteropWork = true;
        f.context.signalResult = -90;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == -90);
        assert(f.real.resizes == 0 && f.shadow.releases == 0);
        f.context.signalResult = S_OK;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
    }
}

void retirementRecoveryCase()
{
    Fixture f;
    f.sc->_hasInteropWork = true;
    f.sc->_copyAllocatorFenceValues[0] = 9;
    f.sc->_lastInteropCopyFenceValue = 9;
    auto retired = f.sc;
    const auto before = deaths;
    f.sc->Release();
    f.sc = nullptr;
    assert(Dx11wDx12SC::_retired == retired && deaths == before);
    Swapchain replacement;
    State::Instance().currentFGSwapchain = &replacement;
    State::Instance().currentSwapchain = &replacement;
    f.copyFence.completed = 9;
    Dx11wDx12SC::_CollectRetired();
    assert(Dx11wDx12SC::_retired == nullptr && deaths == before + 1);
    assert(f.fg.releases == 0 && MenuOverlayDx::cleanups == 0);
    assert(State::Instance().currentFGSwapchain == &replacement);
}

int main()
{
    waitCases();
    helperRemovalCases();
    failedSignalCases();
    ownershipCases();
    preservedPresenterCase();
    drainCases();
    partialResizeCase();
    resizeErrorCases();
    retirementRecoveryCase();
    assert(Dx11wDx12SC::_retired == nullptr);
    std::cout << "bridge lifetime: production wait, copy, resize, release and retirement cases passed\n";
}
