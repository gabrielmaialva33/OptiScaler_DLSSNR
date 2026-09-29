
// An overlay initialised on a D3D12 swapchain, with its render targets on both buffers.
struct OverlayFixture
{
    ID3D12Device device;
    IDXGISwapChain swapchain;
    ID3D12DescriptorHeap rtvHeap, srvHeap;
    ID3D12CommandAllocator allocators[2];
    ID3D12GraphicsCommandList list;
    ID3D12CommandQueue queue;
    int backendData = 0;

    OverlayFixture()
    {
        State::Instance() = {};
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
    }
};

void overlayTargetCases()
{
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
        assert(f.swapchain.buffers[0].refs == 1 && imguiShutdowns == 0);
    }

    // At process shutdown nothing is touched, as before.
    {
        OverlayFixture f;
        State::Instance().isShuttingDown = true;
        CleanupRenderTargetDx12(true);
        assert(f.swapchain.buffers[0].refs == 2 && _isInited && _dx12Device);
    }
}

int main()
{
    overlayTargetCases();
    std::cout << "overlay render targets: cleanup, the Generation Zero present-thread race, handle change, shutdown "
                 "passed\n";
}
