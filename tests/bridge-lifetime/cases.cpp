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
    // The presenter goes first. When it refuses, the hidden swapchain is left alone and the two still
    // agree, so Present is not refused over it; the game has the error.
    f.presenter.resizeResult = E_FAIL;
    assert(f.sc->ResizeBuffers(3, 800, 600, 0, 0) == E_FAIL);
    assert(f.real.resizes == 0 && !f.sc->_resizeIncomplete && f.sc->refreshes == 0);
    // The presenter took the new size and the hidden one did not: they disagree until a resize succeeds.
    f.presenter.resizeResult = S_OK;
    f.real.resizeResult = E_FAIL;
    assert(f.sc->ResizeBuffers(3, 800, 600, 0, 0) == E_FAIL);
    assert(f.sc->_resizeIncomplete && f.real.resizes == 1);
    f.real.resizeResult = S_OK;
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

// What the bridge owes the neural host, and what it must show while it owes it.
//
// The host is the only thing on this path that can be half-done: its guide initialization is
// recorded onto the same list as the frame, so whether that list ran is the difference between
// zeros that exist and zeros that do not. Only the bridge knows which happened.
// Which queue a present goes on, which is the line the whole no-FG path died on.
//
// Measured in Divinity: Original Sin 2 before this existed: the bridge was created, the neural host
// was attached, and then every single Present returned E_UNEXPECTED -- 51338 of them in two minutes,
// one per frame -- because the queue was asked of a frame-generation object that a game with no
// frame generation never has. Nothing downstream needed FG. One line did.
void presentQueueCases()
{
    ID3D12CommandQueue fgQueue;

    // Frame generation running: its queue, and nothing else's.
    {
        Fixture f;
        f.fg.queue = &fgQueue;
        assert(f.sc->_PresentQueueForFrame() == &fgQueue);
    }

    // No frame generation, hosting the neural pass: the queue this wrapper already owns, which is
    // the one the interop copy executes on and the overlay presents through.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_fg = nullptr;
        assert(f.sc->_PresentQueueForFrame() == &f.copyQueue);
        f.sc->_fg = &f.fg;
    }

    // No frame generation and no neural host: there is nothing this bridge is for, and answering a
    // queue anyway would present a frame nobody composed.
    {
        Fixture f;
        f.sc->_fg = nullptr;
        assert(f.sc->_nrHost == nullptr);
        assert(f.sc->_PresentQueueForFrame() == nullptr);
        f.sc->_fg = &f.fg;
    }

    // An FG object with no queue, on a bridge built for frame generation. Falling back would hide a
    // bridge in trouble behind a queue that was never meant to carry its presentation.
    {
        Fixture f;
        f.fg.queue = nullptr;
        assert(f.sc->_PresentQueueForFrame() == nullptr);
    }

    // The same, with the neural pass hosted: falls back to the wrapper's own queue. With synthesized FG
    // a context FFX failed to create leaves the bridge on its plain presenter, and the hosted pass is
    // then all the title has; an NR-only bridge with a stray FG object needs the same.
    {
        Fixture f;
        f.fg.queue = nullptr;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        assert(f.sc->_PresentQueueForFrame() == &f.copyQueue);
    }

    // And with a queue, FG's queue carries the frame whether or not a host rides along.
    {
        Fixture f;
        f.fg.queue = &fgQueue;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        assert(f.sc->_PresentQueueForFrame() == &fgQueue);
    }
}

void predicateCases()
{
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto set = [&](bool nrEnabled, uint32_t hookMethod, FGInput input, FGOutput output)
    {
        state = {};
        cfg = {};
        cfg.DlssNrEnabled.value = nrEnabled;
        cfg.DlssNrHookMethod.value = hookMethod;
        state.activeFgInput = input;
        state.activeFgOutput = output;
    };

    // Everything at its default: no bridge.
    set(false, 1, FGInput::NoFG, FGOutput::NoFG);
    assert(!Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());

    // The neural pass alone, at present: the bridge exists for it.
    set(true, 2, FGInput::NoFG, FGOutput::NoFG);
    assert(!Dx11wDx12::WantedForFrameGeneration() && Dx11wDx12::WantedForNeuralRendering());

    // Frame generation fed by an upscaler: the pass has its own route after that upscaler, and a
    // second one in the bridge would be a second writer on the frame.
    set(true, 2, FGInput::Upscaler, FGOutput::FSRFG);
    assert(Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());

    // Synthesized FG has no upscaler: the pass rides the same bridge, once per base frame.
    set(true, 2, FGInput::Synthesized, FGOutput::FSRFG);
    assert(Dx11wDx12::WantedForFrameGeneration() && Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());

    // Not at present, so not in the bridge either.
    set(true, 1, FGInput::Synthesized, FGOutput::FSRFG);
    assert(Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());

    // Off at creation, at present, under synthesized FG: an idle host, so the toggle has a route that
    // is not the wrapped FFX swapchain.
    set(false, 2, FGInput::Synthesized, FGOutput::FSRFG);
    assert(!Dx11wDx12::WantedForNeuralRendering() && Dx11wDx12::WantedIdleForNeuralRendering());

    // No FG output means no FG bridge, and an idle host has nothing to ride along with.
    set(false, 2, FGInput::Synthesized, FGOutput::NoFG);
    assert(!Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedIdleForNeuralRendering());

    // Synthesized with an output it does not feed: FG will be refused, so no bridge is built for it.
    set(true, 2, FGInput::Synthesized, FGOutput::XeFG);
    assert(!Dx11wDx12::WantedForFrameGeneration());
    assert(Dx11wDx12::WantedForNeuralRendering());
    set(true, 2, FGInput::Synthesized, FGOutput::Reprojection);
    assert(!Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedIdleForNeuralRendering());

    // DLSS-G is fed like FSR FG (synthesized-frame-generation.md, "DLSS-G output"): the bridge is built for it,
    // with the pass once per base frame when NR is at present, and an idle host when NR is off there.
    set(false, 1, FGInput::Synthesized, FGOutput::DLSSG);
    assert(Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());
    set(true, 2, FGInput::Synthesized, FGOutput::DLSSG);
    assert(Dx11wDx12::WantedForFrameGeneration() && Dx11wDx12::WantedForNeuralRendering());
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());
    set(false, 2, FGInput::Synthesized, FGOutput::DLSSG);
    assert(Dx11wDx12::WantedForFrameGeneration() && !Dx11wDx12::WantedForNeuralRendering());
    assert(Dx11wDx12::WantedIdleForNeuralRendering());

    // An upscaler-fed FG bridge never keeps one.
    set(false, 2, FGInput::Upscaler, FGOutput::FSRFG);
    assert(!Dx11wDx12::WantedIdleForNeuralRendering());

    state = {};
    cfg = {};
}

void neuralHostCases()
{
    // The ordinary frame: the pass recorded, the list executed, and what reaches the presenter is
    // the pass's answer rather than the frame that came in.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        auto* host = f.sc->_nrHost.get();
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(host->records == 1 && host->confirms == 1 && host->abandons == 0);
        assert(host->lastSource == f.sc->_openedDx11BackBuffers[0]);
        assert(f.list.copiedFrom == &host->composed && f.list.copiedTo == &f.presenter.buffer);
        assert(!host->recordingOutstanding);
    }

    // The pass declined this frame. The frame still has to be shown, and what is shown is the
    // game's own colour -- not a half-composed working copy, and not nothing.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        auto* host = f.sc->_nrHost.get();
        host->recordSucceeds = false;
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(host->records == 1 && host->confirms == 1);
        assert(f.list.copiedFrom == f.sc->_openedDx11BackBuffers[0]);
    }

    // Recorded, but produced no output. Same answer: show the game's frame.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        auto* host = f.sc->_nrHost.get();
        host->producesOutput = false;
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(f.list.copiedFrom == f.sc->_openedDx11BackBuffers[0]);
        assert(host->confirms == 1);
    }

    // The list could not be closed, so nothing on it ran. The host must be told, or it will spend
    // the rest of the session believing it wrote zeros it never wrote.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        auto* host = f.sc->_nrHost.get();
        f.sc->_hasInteropWork = true;
        f.list.closeResult = E_FAIL;
        assert(!f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(host->records == 1 && host->abandons == 1 && host->confirms == 0);
        assert(!host->recordingOutstanding);
    }

    // Teardown frees the host's textures behind the same proved drain as the rest of the interop.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        auto* host = f.sc->_nrHost.get();
        f.sc->_ReleaseInteropObjects();
        assert(host->releases == 1);
    }

    // And a bridge built for frame generation has no host at all, which must remain an ordinary
    // frame rather than a null dereference.
    {
        Fixture f;
        f.sc->_hasInteropWork = true;
        assert(f.sc->_nrHost == nullptr);
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(f.list.copiedFrom == f.sc->_openedDx11BackBuffers[0]);
    }
}

void synthInputCases()
{
    // The ordinary frame: the clear is recorded at the presenter's backbuffer size and confirmed with
    // the list that carried it.
    {
        Fixture f;
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* synth = f.sc->_synthInputs.get();
        f.presenter.buffer.desc = { 3440, 1440, 24 };
        f.sc->_hasInteropWork = true;
        const unsigned baseFramesBefore = SynthMotion::Handoff::baseFrames;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(synth->records == 1 && synth->confirms == 1 && synth->abandons == 0);
        assert(synth->lastWidth == 3440 && synth->lastHeight == 1440);
        assert(!synth->recordingOutstanding);
        assert(f.list.copiedFrom == f.sc->_openedDx11BackBuffers[0]);

        // One base frame per copy, and the motion estimate from the game's own frame (not the neural
        // host's output) in the copy-source state the copy leaves it in, confirmed with the same list.
        assert(SynthMotion::Handoff::baseFrames == baseFramesBefore + 1);
        assert(synth->motionRecords == 1);
        assert(synth->lastMotionColour == f.sc->_openedDx11BackBuffers[0]);
        assert(synth->lastMotionColourState == D3D12_RESOURCE_STATE_COPY_SOURCE);

        // The HUD mask from the game's frame too, and with no neural host its UI layer from that same frame,
        // which is what the presenter gets; both in the copy-source state, on the same list.
        assert(synth->overlayRecords == 1);
        assert(synth->lastOverlayColour == f.sc->_openedDx11BackBuffers[0]);
        assert(synth->lastOverlayPresented == f.sc->_openedDx11BackBuffers[0]);
        assert(synth->lastOverlayColourState == D3D12_RESOURCE_STATE_COPY_SOURCE);
        assert(synth->lastOverlayPresentedState == D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    // With a neural host that ran, the mask still reads the game's frame, and the UI layer the host's output:
    // the frame that goes on screen, so the layer composes back what real frames show.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* synth = f.sc->_synthInputs.get();
        auto* host = f.sc->_nrHost.get();
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(f.list.copiedFrom == &host->composed);
        assert(synth->overlayRecords == 1);
        assert(synth->lastOverlayColour == f.sc->_openedDx11BackBuffers[0]);
        assert(synth->lastOverlayPresented == &host->composed);
        assert(synth->lastOverlayPresentedState == D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    // The list could not be closed: the clear never ran and must stay owed.
    {
        Fixture f;
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* synth = f.sc->_synthInputs.get();
        f.sc->_hasInteropWork = true;
        f.list.closeResult = E_FAIL;
        assert(!f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(synth->records == 1 && synth->abandons == 1 && synth->confirms == 0);
    }

    // A resize frees the pair, and only once the drain in front of it has been proved.
    {
        Fixture f;
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* synth = f.sc->_synthInputs.get();
        f.sc->_hasInteropWork = true;
        f.sc->_copyAllocatorFenceValues[0] = 5;
        f.sc->_lastInteropCopyFenceValue = 5;
        f.copyFence.completed = 5;
        f.presentQueue.completeSignals = false;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        assert(synth->releases == 0);
        f.presentQueue.signaled[0]->completed = 1;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
        assert(synth->releases == 1);
    }

    // Upstream's skip-resize: identical parameters leave the presenter alone, while the bridge side
    // of the resize still runs behind its drain. The host is kept; the synthesized pair is freed and
    // re-cleared at the same size on the next copy, which is safe (drained) and costs one reallocation.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* host = f.sc->_nrHost.get();
        auto* synth = f.sc->_synthInputs.get();
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 800;
        f.presenter.desc.BufferDesc.Height = 600;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
        assert(f.presenter.resizes == 0 && f.real.resizes == 1);
        assert(host->releases == 0 && f.sc->_nrHost != nullptr);
        assert(synth->releases == 1 && f.sc->_synthInputs != nullptr);
        assert(f.fg.targetUpdates == 0);
    }

    // The same with XeFG: the equivalent resize first takes XeFG's present barrier and restarts it.
    {
        Fixture f;
        State::Instance().activeFgOutput = FGOutput::XeFG;
        f.fg.active = true;
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 800;
        f.presenter.desc.BufferDesc.Height = 600;
        const auto deactivations = f.fg.deactivations;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
        assert(f.presenter.resizes == 0 && f.fg.targetUpdates == 1 && f.fg.deactivations == deactivations + 1);
        assert(Dx11wDx12Sync::PresentResizeMutex().try_lock());
        Dx11wDx12Sync::PresentResizeMutex().unlock();
    }

    // DXGI's "0 means the window's size". The real D3D11 swapchain lives on a hidden 1x1 window and must
    // never be handed a zero (PCSX2 on D3D11 sends 0x0 while windowed; before the resolution the real
    // swapchain became 1x1 and the host starved on it). The presenter sits in the game's window and is
    // handed the zero as given, first; with no description to read back, the real one falls back to the
    // game window's client area.
    {
        Fixture f;
        g_clientRect = RECT { 0, 0, 841, 1356 };
        assert(f.sc->ResizeBuffers(2, 0, 0, 0, 0) == S_OK);
        assert(f.presenter.lastResizeWidth == 0 && f.presenter.lastResizeHeight == 0);
        assert(f.real.lastResizeWidth == 841 && f.real.lastResizeHeight == 1356);
        assert(f.presenter.resizedAt < f.real.resizedAt);
        // Explicit sizes pass through untouched, and so does a zero in only one axis' partner.
        assert(f.sc->ResizeBuffers(2, 1280, 0, 0, 0) == S_OK);
        assert(f.presenter.lastResizeWidth == 1280 && f.presenter.lastResizeHeight == 0);
        assert(f.real.lastResizeWidth == 1280 && f.real.lastResizeHeight == 1356);
        assert(f.sc->ResizeBuffers1(2, 0, 0, 0, 0, nullptr, nullptr) == S_OK);
        assert(f.presenter.lastResizeWidth == 0 && f.presenter.lastResizeHeight == 0);
        assert(f.real.lastResizeWidth == 841 && f.real.lastResizeHeight == 1356);
    }

    // Teardown frees it behind the same drain as the rest of the interop.
    {
        Fixture f;
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* synth = f.sc->_synthInputs.get();
        f.sc->_ReleaseInteropObjects();
        assert(synth->releases == 1);
    }
}

void coexistenceCases()
{
    // The neural pass and synthesized FG on one copy: the pass records first, the presenter receives
    // its output rather than the game's frame, and both are confirmed with the list that carried them.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* host = f.sc->_nrHost.get();
        auto* synth = f.sc->_synthInputs.get();
        f.presenter.buffer.desc = { 3440, 1440, 24 };
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(host->records == 1 && host->confirms == 1 && host->abandons == 0);
        assert(synth->records == 1 && synth->confirms == 1 && synth->abandons == 0);
        assert(f.list.copiedFrom == &host->composed && f.list.copiedTo == &f.presenter.buffer);
        assert(!host->recordingOutstanding && !synth->recordingOutstanding);
    }

    // An idle host (the pass off) declines, and the frame FG interpolates is the game's own.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        f.sc->_nrHost->recordSucceeds = false;
        f.sc->_hasInteropWork = true;
        assert(f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(f.list.copiedFrom == f.sc->_openedDx11BackBuffers[0]);
        assert(f.sc->_synthInputs->confirms == 1);
    }

    // The list could not be closed: neither the pass nor the clear ran, and each is told so.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* host = f.sc->_nrHost.get();
        auto* synth = f.sc->_synthInputs.get();
        f.sc->_hasInteropWork = true;
        f.list.closeResult = E_FAIL;
        assert(!f.sc->_CopyDx11SharedToDx12FGBackBuffer(0));
        assert(host->abandons == 1 && host->confirms == 0);
        assert(synth->abandons == 1 && synth->confirms == 0);
    }

    // Teardown frees both behind the one drain.
    {
        Fixture f;
        f.sc->_nrHost = std::make_unique<DlssNr::PresentHost>();
        f.sc->_synthInputs = std::make_unique<SynthInputs>();
        auto* host = f.sc->_nrHost.get();
        auto* synth = f.sc->_synthInputs.get();
        f.sc->_ReleaseInteropObjects();
        assert(host->releases == 1 && synth->releases == 1);
    }
}

void fullscreenCases()
{
    // Generation Zero, 2026-09-28: a presenter taken out of exclusive fullscreen owes a ResizeBuffers,
    // and the game's own resize after SetFullscreenState asks for the size the presenter already has --
    // the one upstream's IsSame skips. With the resize owed it reaches the presenter, first, with the
    // game's zeros; DXGI sizes it against the window it sees then (the output, once the borderless
    // placement has landed), and the hidden swapchain follows the presenter, not the client rect the
    // wrapper could sample.
    {
        Fixture f;
        g_clientRect = RECT { 0, 0, 1280, 720 };
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 1280;
        f.presenter.desc.BufferDesc.Height = 720;
        f.presenter.fullscreen = TRUE;
        assert(f.sc->SetFullscreenState(TRUE, nullptr) == S_OK);
        assert(!f.presenter.fullscreen && f.presenter.fullscreenChanges == 1 && f.sc->borderlessEntries == 1);
        assert(f.sc->_presenterResizeOwed && f.sc->_presenterRecoveryArmed);
        assert(State::Instance().SCExclusiveFullscreen && !State::Instance().realExclusiveFullscreen);
        f.presenter.windowWidth = 1920;
        f.presenter.windowHeight = 1080;
        assert(f.sc->ResizeBuffers(2, 0, 0, 0, 0) == S_OK);
        assert(f.presenter.resizes == 1 && f.presenter.lastResizeWidth == 0 && f.presenter.lastResizeHeight == 0);
        assert(f.real.lastResizeWidth == 1920 && f.real.lastResizeHeight == 1080);
        assert(f.presenter.resizedAt < f.real.resizedAt);
        assert(!f.sc->_presenterResizeOwed && !f.sc->_resizeIncomplete);
        // Once paid, an unchanged resize is skipped again, as upstream wants, and the hidden swapchain is
        // still sized from the presenter.
        assert(f.sc->ResizeBuffers(2, 1920, 1080, 0, 0) == S_OK);
        assert(f.presenter.resizes == 1 && f.real.resizes == 2 && f.real.lastResizeWidth == 1920);
        // Leaving the emulated fullscreen is only a window change: nothing is owed.
        assert(f.sc->SetFullscreenState(FALSE, nullptr) == S_OK);
        assert(f.sc->borderlessExits == 1 && !f.sc->_presenterResizeOwed && f.presenter.fullscreenChanges == 1);
    }

    // PCSX2 windowed, D3D11: 0x0 on every window resize, with the presenter's description readable. The
    // zero is compared against the window, not against the presenter's own size, so a changed window is
    // never skipped as unchanged; the real swapchain gets the presenter's extent; and an unchanged 0x0
    // leaves the presenter alone while the real one keeps the window's size, never the hidden 1x1.
    {
        Fixture f;
        g_clientRect = RECT { 0, 0, 841, 1356 };
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 800;
        f.presenter.desc.BufferDesc.Height = 600;
        assert(f.sc->ResizeBuffers(2, 0, 0, 0, 0) == S_OK);
        assert(f.presenter.resizes == 1 && f.presenter.lastResizeWidth == 0 && f.presenter.lastResizeHeight == 0);
        assert(f.real.lastResizeWidth == 841 && f.real.lastResizeHeight == 1356);
        assert(f.sc->ResizeBuffers(2, 0, 0, 0, 0) == S_OK);
        assert(f.presenter.resizes == 1 && f.real.resizes == 2);
        assert(f.real.lastResizeWidth == 841 && f.real.lastResizeHeight == 1356);
        assert(!f.sc->_resizeIncomplete);
    }

    // The game's flags as the presenter can take them: GDI_COMPATIBLE is invalid on flip model, and
    // ALLOW_TEARING and FRAME_LATENCY_WAITABLE_OBJECT stay as the presenter was created -- a ResizeBuffers
    // that changes either is refused. The hidden swapchain is the game's and gets the game's flags.
    {
        Fixture f;
        f.presenter.descResult = S_OK;
        f.presenter.desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        const UINT game = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                          DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, game) == S_OK);
        assert(f.presenter.lastResizeFlags ==
               (DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
        assert(f.real.lastResizeFlags == game);
        // Unknown presenter: only what is invalid on flip model is dropped.
        f.presenter.descResult = E_FAIL;
        assert(Dx11wDx12::PresenterResizeFlags(&f.presenter, game) ==
               (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
    }

    // A refused Present (887A0001) after a transition: one resize of the presenter alone, at the size,
    // count and flags it has, behind the drain; logged once; never a loop.
    {
        Fixture f;
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 800;
        f.presenter.desc.BufferDesc.Height = 600;
        f.presenter.desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        const int warnings = logWarnings;
        // No transition since the last resize: nothing to recover, nothing resized, nothing said.
        assert(FAILED(f.sc->_RecoverPresenter()));
        assert(f.presenter.resizes == 0 && logWarnings == warnings);
        // The game's SetFullscreenState on the plain presenter is a transition.
        assert(f.sc->SetFullscreenState(TRUE, nullptr) == S_OK);
        assert(SUCCEEDED(f.sc->_RecoverPresenter()));
        assert(f.presenter.resizes == 1 && f.presenter.lastResizeWidth == 800 && f.presenter.lastResizeHeight == 600);
        assert(f.presenter.lastResizeFlags == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        // The game holds the hidden swapchain's buffers mid-frame; its resize would fail, so it is not made.
        assert(f.real.resizes == 0);
        // The interop buffers were let go before the presenter's were replaced.
        assert(f.shadow.releases == 1 && f.sc->_openedDx11BackBuffers.empty());
        assert(logWarnings == warnings + 1);
        // The same refusal again: spent until the next transition.
        assert(FAILED(f.sc->_RecoverPresenter()));
        assert(f.presenter.resizes == 1);
        // A game resize is a transition too (skipped as unchanged here, so the presenter is not resized by
        // it). A presenter that refuses its recovery resize costs one attempt, and says nothing more.
        assert(f.sc->ResizeBuffers(2, 800, 600, 0, 0) == S_OK);
        assert(f.presenter.resizes == 1);
        f.presenter.resizeResult = E_FAIL;
        assert(f.sc->_RecoverPresenter() == E_FAIL);
        assert(f.presenter.resizes == 2 && logWarnings == warnings + 1);
        assert(FAILED(f.sc->_RecoverPresenter()));
        assert(f.presenter.resizes == 2 && logWarnings == warnings + 1);
    }

    // Frame generation's presenter (FSR-FG's swapchain, not the plain interop one) is emulated too: the
    // game's SetFullscreenState never reaches it, it stays windowed, and the game is told it is fullscreen.
    // Handing it the call left Generation Zero refusing every Present with 887A0001 (2026-09-28).
    {
        FGHooks::interopPresenter = false;
        Fixture f;
        assert(f.sc->_EmulatesFullscreen());
        assert(f.sc->SetFullscreenState(TRUE, nullptr) == S_OK);
        assert(!f.presenter.fullscreen && f.sc->borderlessEntries == 1 && f.sc->_emulatedFullscreen);
        assert(f.sc->SetFullscreenState(FALSE, nullptr) == S_OK);
        assert(!f.presenter.fullscreen && f.sc->borderlessExits == 1 && !f.sc->_emulatedFullscreen);
        FGHooks::interopPresenter = true;
    }

    // DXGI put the presenter into exclusive fullscreen behind the wrapper (its own Alt+Enter): taken back
    // to a window, the game's fullscreen emulated, then resized; the output reference is returned.
    {
        Fixture f;
        f.presenter.descResult = S_OK;
        f.presenter.desc.BufferCount = 2;
        f.presenter.desc.BufferDesc.Width = 1920;
        f.presenter.desc.BufferDesc.Height = 1080;
        f.presenter.fullscreen = TRUE;
        assert(SUCCEEDED(f.sc->_RecoverPresenter()));
        assert(!f.presenter.fullscreen && f.sc->borderlessEntries == 1);
        assert(f.presenter.resizes == 1 && f.presenter.lastResizeWidth == 1920 && !f.sc->_presenterResizeOwed);
        assert(f.presenter.output.refs == 1 && f.real.resizes == 0);
    }

    // At creation: the hidden swapchain follows the presenter when DXGI sized the two apart, keeping its
    // own flags, and is left alone when they agree or the presenter cannot be read.
    {
        Swapchain hidden, presenter;
        hidden.descResult = S_OK;
        presenter.descResult = S_OK;
        hidden.desc.BufferDesc.Width = 841;
        hidden.desc.BufferDesc.Height = 1356;
        hidden.desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        presenter.desc.BufferDesc.Width = 1920;
        presenter.desc.BufferDesc.Height = 1080;
        Dx11wDx12::MatchHiddenToPresenter(&hidden, &presenter);
        assert(hidden.resizes == 1 && hidden.lastResizeWidth == 1920 && hidden.lastResizeHeight == 1080);
        assert(hidden.lastResizeFlags == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        Dx11wDx12::MatchHiddenToPresenter(&hidden, &presenter);
        assert(hidden.resizes == 1);
        presenter.descResult = E_FAIL;
        presenter.desc.BufferDesc.Width = 640;
        Dx11wDx12::MatchHiddenToPresenter(&hidden, &presenter);
        assert(hidden.resizes == 1);
    }
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
    presentQueueCases();
    neuralHostCases();
    synthInputCases();
    coexistenceCases();
    fullscreenCases();
    predicateCases();
    assert(Dx11wDx12SC::_retired == nullptr);
    std::cout << "bridge lifetime: production wait, copy, resize, release, retirement, neural host, synthesized input, "
                 "coexistence, fullscreen and predicate cases passed\n";
}
