int main()
{
    using DlssNr::ZeroGuides;
    unsigned cases = 0;
    const auto CASE = [&cases](const char* name)
    {
        ++cases;
        std::cout << "  " << name << "\n";
    };

    // --- allocation ------------------------------------------------------
    {
        ID3D12Device device;
        ZeroGuides guides;
        assert(!guides.Ready() && guides.Depth() == nullptr && guides.Motion() == nullptr);
        assert(guides.Ensure(&device, 1920, 1080));
        assert(guides.Depth() != nullptr && guides.Motion() != nullptr);
        assert(guides.Depth()->desc.Format == DXGI_FORMAT_R32_FLOAT);
        assert(guides.Motion()->desc.Format == DXGI_FORMAT_R16G16_FLOAT);
        assert(guides.Depth()->desc.Width == 1920 && guides.Depth()->desc.Height == 1080);
        assert(guides.Motion()->desc.Width == 1920 && guides.Motion()->desc.Height == 1080);
        // Allocated is not ready: the zeros have not run.
        assert(!guides.Ready());
        CASE("depth is R32_FLOAT and motion is R16G16_FLOAT at the frame size, and neither is ready yet");

        // The same size again reuses, a different size reallocates.
        auto* depth = guides.Depth();
        assert(guides.Ensure(&device, 1920, 1080) && guides.Depth() == depth);
        assert(device.resources == 2);
        assert(guides.Ensure(&device, 1280, 720) && guides.Depth() != depth);
        assert(device.resources == 4 && depth->refs == 0);
        CASE("the same size is reused, a different size reallocates and releases the old pair");
    }

    // --- the clear, and the rule it has to obey --------------------------
    {
        ID3D12Device device;
        ID3D12GraphicsCommandList list;
        list.device = &device;
        ZeroGuides guides;
        assert(guides.Ensure(&device, 640, 360));
        assert(guides.RecordClear(&list));
        assert(list.clears.size() == 2);

        for (const auto& clear : list.clears)
        {
            // This is the whole point of the two heaps. ClearUnorderedAccessViewFloat reads the CPU
            // handle from a heap that must NOT be shader visible and the GPU handle from one that
            // must be, and that must be bound. A single heap cannot satisfy both.
            assert(!clear.cpuHandleWasShaderVisible);
            assert(clear.gpuHandleWasShaderVisible);
            assert(clear.heapWasBound);
            // And both handles must describe the resource being cleared, not each other's.
            assert(clear.handlesNameSameResource);
            for (float channel : clear.value)
                assert(channel == 0.0f);
        }
        assert(list.clears[0].resource == guides.Depth() && list.clears[1].resource == guides.Motion());
        CASE("each guide is cleared to zero through a CPU-only handle and a bound shader-visible one");

        // Cleared, then moved to where the pass reads them.
        assert(list.barriers.size() == 2);
        for (const auto& barrier : list.barriers)
        {
            assert(barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            assert(barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        assert(guides.Depth()->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CASE("both guides end in the state the pass reads them in");

        // Still not ready. Recording is not executing.
        assert(!guides.Ready());
        // And recording again must not clear a resource the first recording already moved.
        assert(guides.RecordClear(&list) && list.clears.size() == 2);
        CASE("a recorded clear is not an executed one, and it is never recorded twice");

        guides.ConfirmExecuted();
        assert(guides.Ready());
        // Nothing is owed now, so a later call records nothing at all.
        assert(guides.RecordClear(&list) && list.clears.size() == 2);
        CASE("confirming execution makes the guides ready, and the clear is never repeated");
    }

    // --- the caller decides where the guides come to rest ----------------
    {
        // The pass transitions a guide out of the state it believes the guide arrived in, and for
        // the shape this host uses that state comes from config keys meant for a game's own buffers.
        // Resting a guide anywhere else is a barrier from a state it was never in.
        ID3D12Device device;
        ID3D12GraphicsCommandList list;
        list.device = &device;
        ZeroGuides guides;
        assert(guides.Ensure(&device, 320, 180));
        assert(guides.RecordClear(&list, D3D12_RESOURCE_STATE_COMMON));
        assert(list.barriers.size() == 2);
        for (const auto& barrier : list.barriers)
            assert(barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON);
        assert(guides.Depth()->state == D3D12_RESOURCE_STATE_COMMON);
        CASE("the guides rest where the caller asked, not where this file would have guessed");
    }

    // --- resting where they already are ----------------------------------
    {
        // A barrier from a state to itself is rejected, not ignored, so asking for the clear state
        // must record the clear and no transition at all.
        ID3D12Device device;
        ID3D12GraphicsCommandList list;
        list.device = &device;
        ZeroGuides guides;
        assert(guides.Ensure(&device, 320, 180));
        assert(guides.RecordClear(&list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        assert(list.clears.size() == 2 && list.barriers.empty());
        assert(guides.Depth()->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CASE("asking for the state they are already in records no barrier");
    }

    // --- the dropped list ------------------------------------------------
    {
        ID3D12Device device;
        ID3D12GraphicsCommandList dropped, real;
        dropped.device = &device;
        real.device = &device;
        ZeroGuides guides;
        assert(guides.Ensure(&device, 640, 360));
        assert(guides.RecordClear(&dropped));
        assert(dropped.clears.size() == 2);

        // The list was never executed. Zeros that were never written are not initialization, so the
        // guides must still owe their clear -- and confirming afterwards must not resurrect it.
        guides.AbandonRecording();
        assert(!guides.Ready());
        guides.ConfirmExecuted();
        assert(!guides.Ready());
        CASE("an abandoned recording leaves the zeros owed, and a later confirm cannot claim them");

        // The retry records against exactly the state the abandoned attempt left behind.
        guides.Depth()->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        guides.Motion()->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        assert(guides.RecordClear(&real) && real.clears.size() == 2);
        guides.ConfirmExecuted();
        assert(guides.Ready());
        CASE("the retry records the same sequence and succeeds");
    }

    // --- failure leaves nothing behind -----------------------------------
    for (int failure = 0; failure < 4; ++failure)
    {
        ID3D12Device device;
        if (failure < 2)
            device.failResourceAt = failure;
        else
            device.failHeapAt = failure - 2;

        ZeroGuides guides;
        assert(!guides.Ensure(&device, 800, 600));
        assert(guides.Depth() == nullptr && guides.Motion() == nullptr && !guides.Ready());
        for (const auto& resource : device.ownedResources)
            assert(resource->refs == 0);
        for (const auto& heap : device.ownedHeaps)
            assert(heap->refs == 0);
        // And a clear cannot be recorded against nothing.
        ID3D12GraphicsCommandList list;
        list.device = &device;
        assert(!guides.RecordClear(&list) && list.clears.empty());
    }
    CASE("every allocation failure releases what it acquired and refuses to record a clear");

    // --- degenerate input -------------------------------------------------
    {
        ID3D12Device device;
        ZeroGuides guides;
        assert(!guides.Ensure(nullptr, 100, 100));
        assert(!guides.Ensure(&device, 0, 100));
        assert(!guides.Ensure(&device, 100, 0));
        assert(device.resources == 0 && device.heaps == 0);
        CASE("a null device or an empty extent allocates nothing");
    }

    std::cout << "PASS: zero guides, " << cases << " cases\n";
    return 0;
}
