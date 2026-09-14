struct UpscalerFixture
{
    ID3D11Device device11;
    ID3D11DeviceContext context;
    ID3D12Device device;
    ID3D12CommandQueue queue;
    ID3D12CommandAllocator allocator;
    ID3D12GraphicsCommandList list;
    ID3D12Fence fence;
    Backend backend;
    IFeature_Dx11wDx12 bridge;
    NVSDK_NGX_Parameter params;
    UpscalerFixture()
    {
        clockMs = 0;
        wakes.clear();
        waitDurations.clear();
        bridge.dx12Feature = &backend;
        bridge._dx11on12Device = &device;
        bridge.Dx12CommandQueue = &queue;
        bridge.Dx12CommandAllocator[0] = bridge.Dx12CommandAllocator[1] = &allocator;
        bridge.Dx12CommandList[0] = bridge.Dx12CommandList[1] = &list;
        bridge.Dx12Fence = &fence;
        bridge.Dx12FenceEvent = reinterpret_cast<HANDLE>(1);
    }
    bool init() { return bridge.Init(&device11, &context, &params); }
};
int main()
{
    {
        UpscalerFixture f;
        f.queue.completeSignals = false;
        assert(!f.init()); // Old Init reported success even after WAIT_TIMEOUT.
        assert(!f.bridge.IsInited());
        assert(clockMs == 5000);
        assert(f.bridge.Dx12CommandAllocatorFenceValue[0] == 1);
        const auto resets = f.allocator.resets;
        assert(!f.init()); // Retrying must not reset an outstanding creation allocator.
        assert(f.allocator.resets == resets);
        assert(f.backend.inits == 1);
    }
    {
        UpscalerFixture f;
        assert(f.init());
        assert(f.bridge.IsInited());
        assert(waitDurations.empty());
        assert(f.init() && f.backend.inits == 1);
    }
    {
        UpscalerFixture f;
        f.backend.initResult = false;
        assert(!f.init() && !f.bridge.IsInited());
        assert(f.queue.executions == 1 && f.fence.completed == 1);
    }
    {
        UpscalerFixture f;
        f.queue.completeSignals = false;
        wakes.push_back(
            [&](DWORD timeout)
            {
                f.fence.completed = 1;
                clockMs += timeout;
                return WAIT_TIMEOUT;
            });
        assert(f.init()); // Completion at the deadline wins over the event's timeout status.
    }
    for (DWORD wake : { WAIT_OBJECT_0, WAIT_TIMEOUT })
    {
        UpscalerFixture f;
        f.queue.completeSignals = false;
        wakes.push_back(
            [&](DWORD)
            {
                f.fence.completed = UINT64_MAX;
                f.device.reason = DXGI_ERROR_DEVICE_HUNG;
                ++clockMs;
                return wake;
            });
        assert(!f.init());
        assert(!f.bridge.IsInited());
    }
    {
        UpscalerFixture f;
        f.queue.signalResult = E_FAIL;
        f.queue.beforeSignal = [&] { assert(f.bridge.Dx12CommandAllocatorFenceValue[0] == 1); };
        assert(!f.init());
        assert(!f.bridge.IsInited());
        assert(f.bridge.Dx12CommandAllocatorFenceValue[0] == 1);
        assert(!f.bridge.ProcessDx11Textures(&f.params));
        assert(f.allocator.resets == 1); // Init's reset only, no reuse after failed Signal.
    }
    for (HRESULT failure : { E_FAIL, E_UNEXPECTED })
    {
        UpscalerFixture f;
        f.queue.completeSignals = false;
        f.fence.registration = failure;
        assert(!f.init());
        assert(!f.bridge.IsInited());
    }
    {
        UpscalerFixture f;
        f.queue.completeSignals = false;
        wakes.push_back([](DWORD) { return WAIT_FAILED; });
        assert(!f.init() && !f.bridge.IsInited());
    }
    {
        UpscalerFixture f;
        f.allocator.resetResult = E_FAIL;
        assert(!f.init());
        assert(f.backend.inits == 0 && f.queue.executions == 0);
    }
    {
        UpscalerFixture f;
        f.list.closeResult = E_FAIL;
        assert(!f.init() && !f.bridge.IsInited());
        assert(f.queue.executions == 0);
    }
    {
        UpscalerFixture f;
        f.bridge.Dx12CommandAllocatorFenceValue[1] = 20;
        f.bridge._frameCount = 1;
        wakes.push_back(
            [&](DWORD)
            {
                f.fence.completed = 10;
                clockMs += 1000;
                return WAIT_OBJECT_0;
            });
        wakes.push_back(
            [&](DWORD)
            {
                f.fence.completed = 20;
                clockMs += 1000;
                return WAIT_OBJECT_0;
            });
        assert(f.bridge.ProcessDx11Textures(&f.params));
        assert((waitDurations == std::vector<DWORD> { 5000, 4000 }));
        assert(f.fence.registrations == 1 && f.allocator.resets == 1);
    }
    for (DWORD wake : { WAIT_OBJECT_0, WAIT_TIMEOUT })
    {
        UpscalerFixture f;
        f.bridge.Dx12CommandAllocatorFenceValue[0] = 7;
        wakes.push_back(
            [&](DWORD)
            {
                f.fence.completed = UINT64_MAX;
                ++clockMs;
                return wake;
            });
        assert(!f.bridge.ProcessDx11Textures(&f.params));
        assert(f.allocator.resets == 0);
        assert(f.bridge.Dx12CommandAllocatorFenceValue[0] == 7);
    }
    {
        UpscalerFixture f;
        f.bridge.Dx12CommandAllocatorFenceValue[0] = 7;
        wakes.push_back(
            [](DWORD)
            {
                clockMs += 4000;
                return WAIT_OBJECT_0;
            });
        assert(!f.bridge.ProcessDx11Textures(&f.params));
        assert(clockMs == 5000 && f.allocator.resets == 0);
    }
    {
        UpscalerFixture f;
        f.bridge.Dx12CommandAllocatorFenceValue[0] = 7;
        f.fence.completed = UINT64_MAX;
        assert(!f.bridge.ProcessDx11Textures(&f.params));
        assert(f.allocator.resets == 0 && waitDurations.empty());
    }
    {
        UpscalerFixture f;
        f.queue.signalResult = E_FAIL;
        f.queue.beforeSignal = [&] { assert(f.bridge.Dx12CommandAllocatorFenceValue[1] == 1); };
        assert(!f.bridge.SubmitEvaluateForTest(1));
        assert(f.bridge.Dx12CommandAllocatorFenceValue[1] == 1);
        f.bridge._frameCount = 1;
        assert(!f.bridge.ProcessDx11Textures(&f.params));
        assert(f.allocator.resets == 0);
    }
    std::cout << "upscaler bridge fence cases passed\n";
}
