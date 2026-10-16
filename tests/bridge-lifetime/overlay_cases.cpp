
// An overlay initialised on a D3D12 swapchain, with its render targets on both buffers.
struct OverlayFixture
{
    ID3D12Device device;
    ID3D12Fence fence;
    IDXGISwapChain swapchain;
    ID3D12DescriptorHeap rtvHeap, srvHeap;
    ID3D12CommandAllocator allocators[2];
    ID3D12GraphicsCommandList list;
    ID3D12CommandQueue queue;
    int backendData = 0;

    OverlayFixture()
    {
        State::Instance() = {};
        overlayGpuPending = false;
        clockTicks = 0;
        g_menuFence = nullptr;
        g_menuSubmitQueue = nullptr;
        g_menuFenceValue = 0;
        g_menuFenceUnproven = false;
        std::fill(std::begin(g_allocatorFenceValues), std::end(g_allocatorFenceValues), 0);
        duringImGuiShutdown = nullptr;
        imguiShutdowns = 0;
        _dx12Device = true;
        _isInited = true;
        g_pd3dDeviceParam = &device;
        g_pd3dRtvDescHeap = &rtvHeap;
        g_pd3dSrvDescHeap = &srvHeap;
        g_commandAllocators[0] = &allocators[0];
        g_commandAllocators[1] = &allocators[1];
        g_pd3dCommandList = &list;
        g_pd3dCommandQueue = &queue;
        ImGui::io.BackendRendererUserData = &backendData;
        CreateRenderTargetDx12(&device, &swapchain);
        assert(swapchain.buffers[0].refs == 2 && swapchain.buffers[1].refs == 2);
        assert(swapchain.ResizeBuffers() == DXGI_ERROR_INVALID_CALL);
    }
    ~OverlayFixture()
    {
        for (auto& resource : g_mainRenderTargetResource)
            resource = nullptr;
        duringImGuiShutdown = nullptr;
        g_menuFence = nullptr;
        g_menuSubmitQueue = nullptr;
    }
};

void overlayTargetCases()
{
    // A resize must keep overlay resources when the last GPU draw has not completed.
    {
        OverlayFixture f;
        g_menuFence = &f.fence;
        g_menuFenceValue = 1;
        overlayGpuPending = true;
        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 2 && f.rtvHeap.refs == 1 && imguiShutdowns == 0);
        overlayGpuPending = false;
        f.fence.completed = 1;
        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 1 && f.rtvHeap.refs == 0);
    }

    // An ordinary cleanup: the buffers are let go, the backend is shut down, the state is cleared.
    {
        OverlayFixture f;
        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 1 && f.swapchain.buffers[1].refs == 1);
        assert(!_isInited && !_dx12Device && imguiShutdowns == 1);
        assert(f.rtvHeap.refs == 0 && f.srvHeap.refs == 0 && f.list.refs == 0);
        assert(f.swapchain.ResizeBuffers() == S_OK);
    }

    // Generation Zero with DLSS-G on the D3D11 bridge, 2026-09-29, in the order its log gives.
    // - The bridge's resize cleans the overlay on the game thread, which releases the render targets and then
    //   shuts the ImGui backend down.
    // - Meanwhile Streamline's present thread presents an interpolated frame (LocalPresent 594). Its
    //   MenuOverlayDx::Present still finds the overlay initialised -- no "D3D12CommandQueue captured" in the
    //   log -- and no render targets, so it creates them again: "CreateRenderTargetDx12 done!" at 07:49:47.037925.
    // - The cleanup then clears the init state over them.
    // - WrappedIDXGISwapChain4::ResizeBuffers cleans the overlay again, inside Streamline's resize. That cleanup
    //   used to return on the cleared state first and keep the buffers, and the real ResizeBuffers failed with
    //   887A0001, which left DLSS-G with no proxy buffers and the picture frozen.
    {
        OverlayFixture f;
        bool raced = false;
        duringImGuiShutdown = [&]()
        {
            assert(_isInited && _dx12Device && ImGui::io.BackendRendererUserData != nullptr);
            assert(g_mainRenderTargetResource[0] == nullptr);
            CreateRenderTargetDx12(&f.device, &f.swapchain);
            raced = true;
        };
        CleanupRenderTargetDx12(true);
        duringImGuiShutdown = nullptr;
        assert(raced && !_isInited && !_dx12Device);
        assert(f.swapchain.buffers[0].refs == 2 && f.swapchain.buffers[1].refs == 2);

        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 1 && f.swapchain.buffers[1].refs == 1);
        assert(f.swapchain.ResizeBuffers() == S_OK);
        // Nothing else was released twice: the second cleanup found the backend already down.
        assert(imguiShutdowns == 1 && f.rtvHeap.refs == 0 && f.srvHeap.refs == 0);
    }

    // A handle change clears the init state without a cleanup (MenuOverlayDx::Present). The re-init's cleanup
    // lets the old render targets go, so the next present creates them on the swapchain it is given rather than
    // drawing through stale ones.
    {
        OverlayFixture f;
        _isInited = false;
        CleanupRenderTargetDx12(true);
        assert(g_mainRenderTargetResource[0] == nullptr && g_mainRenderTargetResource[1] == nullptr);
        assert(f.swapchain.buffers[0].refs == 1 && imguiShutdowns == 1);
    }

    // At process shutdown nothing is touched, as before.
    {
        OverlayFixture f;
        State::Instance().isShuttingDown = true;
        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 2 && _isInited && _dx12Device);
    }
}

void overlaySubmissionCases()
{
    // Deactivate may join an FG present thread. It must run before taking the overlay mutex.
    {
        OverlayFixture f;
        FakeFg fg;
        State::Instance().currentFG = &fg;
        fg.onDeactivate = []()
        {
            bool acquired = false;
            std::thread present(
                [&]()
                {
                    acquired = g_dx12OverlayMutex.try_lock();
                    if (acquired)
                        g_dx12OverlayMutex.unlock();
                });
            present.join();
            assert(acquired);
        };
        MenuOverlayDx::CleanupRenderTarget(true, 0);
        assert(State::Instance().fgChanged && imguiShutdowns == 1);
    }

    // The real submission helper publishes the obligation before driver entry.
    {
        OverlayFixture f;
        g_menuFence = &f.fence;
        ID3D12CommandList* lists[] = { &f.list };
        f.queue.onExecute = [&]()
        { assert(g_menuFenceValue == 1 && g_allocatorFenceValues[0] == 1 && g_menuFenceUnproven); };
        assert(SubmitMenuDraw(&f.queue, 0, lists));
        assert(f.queue.executes == 1 && f.queue.signals == 1 && !g_menuFenceUnproven && f.queue.refs == 2);
        assert(!WaitForMenuGpu(g_allocatorFenceValues[0]));
        assert(!PrepareMenuAllocator(0) && f.allocators[0].resets == 0);
        f.fence.completed = 1;
        assert(WaitForMenuGpu(g_allocatorFenceValues[0]));
        assert(PrepareMenuAllocator(0) && f.allocators[0].resets == 1);
        assert(CleanupRenderTargetDx12(true));
        assert(f.queue.refs == 1 && f.fence.refs == 0);
    }
    // Failed Signal cannot certify even an otherwise high completed value, or free a live draw.
    {
        OverlayFixture f;
        g_menuFence = &f.fence;
        f.queue.signalResult = -1;
        ID3D12CommandList* lists[] = { &f.list };
        assert(!SubmitMenuDraw(&f.queue, 1, lists));
        f.fence.completed = 100;
        assert(g_menuFenceUnproven && !WaitForMenuGpu(1));
        assert(!CleanupRenderTargetDx12(true));
        assert(f.rtvHeap.refs == 1 && f.list.refs == 1 && f.queue.refs == 2);
        f.device.removed = -1;
        assert(!WaitForMenuGpu(1)); // Device loss permits retirement, never allocator reuse.
        assert(CleanupRenderTargetDx12(true));
        assert(f.rtvHeap.refs == 0 && f.queue.refs == 1);
    }
    // A different queue cannot signal a later value before the previous queue has completed.
    {
        OverlayFixture f;
        ID3D12CommandQueue other;
        g_menuFence = &f.fence;
        ID3D12CommandList* lists[] = { &f.list };
        assert(SubmitMenuDraw(&f.queue, 0, lists));
        assert(!SubmitMenuDraw(&other, 1, lists));
        assert(other.executes == 0 && f.queue.refs == 2 && other.refs == 1);
        f.fence.completed = 1;
        assert(SubmitMenuDraw(&other, 1, lists));
        assert(other.executes == 1 && f.queue.refs == 1 && other.refs == 2);
        f.fence.completed = 2;
        assert(CleanupRenderTargetDx12(true));
        assert(other.refs == 1);
    }
    // A lost device's sentinel is not a successful draw completion.
    {
        OverlayFixture f;
        g_menuFence = &f.fence;
        g_menuFenceValue = 1;
        f.fence.completed = UINT64_MAX;
        assert(!WaitForMenuGpu(1));
        assert(CleanupRenderTargetDx12(true));
    }
    // A target-only cleanup waits too: render targets remain referenced until the last draw finishes.
    {
        OverlayFixture f;
        g_menuFence = &f.fence;
        g_menuFenceValue = 4;
        assert(!CleanupRenderTargetDx12(false));
        assert(f.swapchain.buffers[0].refs == 2);
        f.fence.completed = 4;
        assert(CleanupRenderTargetDx12(false));
        assert(f.swapchain.buffers[0].refs == 1 && f.rtvHeap.refs == 1 && _isInited);
        assert(CleanupRenderTargetDx12(true));
    }
}

int main()
{
    overlaySubmissionCases();
    overlayTargetCases();
    std::cout << "overlay render targets: cleanup, the Generation Zero present-thread race, handle change, shutdown "
                 "passed\n";
}
