// Fill every ring slot once, so the next pass over the ring is all hits.
void PrimeRing(DlssNr_Dx12& shader, ID3D12GraphicsCommandList& cmd, ID3D12Resource& source, ID3D12Resource& target)
{
    for (unsigned i = 0; i < 48; ++i)
    {
        DlssNrConstants constants {};
        constants.Width = 16;
        constants.Height = 16;
        assert(shader.DispatchPass(&cmd, constants, &source, nullptr, nullptr, nullptr, nullptr, &target, nullptr));
    }
}

// One more pass over the ring, answering how many views it had to write.
struct Written
{
    unsigned srv, uav;
};
Written Revisit(DlssNr_Dx12& shader, ID3D12Device& device, ID3D12GraphicsCommandList& cmd, ID3D12Resource& source,
                ID3D12Resource& target)
{
    const unsigned srv = device.srvWrites, uav = device.uavWrites;
    PrimeRing(shader, cmd, source, target);
    return { device.srvWrites - srv, device.uavWrites - uav };
}

// A resource that changes shape under a pointer the cache has already seen.
//
// This is the case the pointer alone cannot answer: the allocator hands an address back to a
// differently shaped resource, and a cache keyed on the address alone leaves the previous
// incarnation's view in the slot. Each mutator moves exactly one member of the description, and every
// one of them must force the views to be written again -- which is the standing guard on BindKey's
// comparison, since a member dropped from it would let one of these through.
void ReshapingRetiresDescriptors()
{
    struct Mutation
    {
        const char* what;
        void (*apply)(D3D12_RESOURCE_DESC&);
    };
    const Mutation mutations[] = {
        { "width", [](D3D12_RESOURCE_DESC& d) { d.Width = 1280; } },
        { "height", [](D3D12_RESOURCE_DESC& d) { d.Height = 720; } },
        { "array size", [](D3D12_RESOURCE_DESC& d) { d.DepthOrArraySize = 6; } },
        { "mip count", [](D3D12_RESOURCE_DESC& d) { d.MipLevels = 4; } },
        { "dimension", [](D3D12_RESOURCE_DESC& d) { d.Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN; } },
        { "format", [](D3D12_RESOURCE_DESC& d) { d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; } },
    };

    for (const auto& mutation : mutations)
    {
        ID3D12Device device;
        ID3D12GraphicsCommandList cmd;
        ID3D12Resource source, target;
        DlssNr_Dx12 shader("reshape", &device);
        assert(shader.IsInit());

        PrimeRing(shader, cmd, source, target);
        assert(Revisit(shader, device, cmd, source, target).srv == 0); // unchanged: every view is a hit

        mutation.apply(source.desc);
        const auto after = Revisit(shader, device, cmd, source, target);

        // The source stands in for all five reads, so all five move with it. The two writes name
        // target, which did not change, and must stay reused.
        assert(after.srv == 48 * 5);
        assert(after.uav == 0);

        for (unsigned slot = 0; slot < 48; ++slot)
            for (unsigned i = 0; i < 5; ++i)
                assert(device.srvs[slot][i] == (ViewRecord { &source, DXGI_FORMAT_UNKNOWN, 0, true }));
        (void) mutation.what;
    }
}

// A scratch allocation retires every cached key at once.
//
// Shape equality says the descriptor bytes would be written the same, not that they still name the
// same allocation. An NR texture freed and re-created at the same size can land at the same pointer
// over different memory, and nothing about the resource itself would show it; the generation is the
// only thing that does.
void ScratchAllocationRetiresDescriptors()
{
    ID3D12Device device;
    ID3D12GraphicsCommandList cmd;
    ID3D12Resource source, target;
    DlssNr_Dx12 shader("generation", &device);
    assert(shader.IsInit());

    PrimeRing(shader, cmd, source, target);
    assert(Revisit(shader, device, cmd, source, target).srv == 0);

    ++g_nrScratchGeneration; // what CreateScratch does on every successful allocation

    const auto after = Revisit(shader, device, cmd, source, target);
    assert(after.srv == 48 * 5 && after.uav == 48 * 2);

    // And it settles again afterwards rather than staying invalidated.
    assert(Revisit(shader, device, cmd, source, target).srv == 0);
}

// A saturated generation can no longer retire anything, so it must stop reusing instead.
void SaturatedGenerationStopsReuse()
{
    const uint64_t saved = g_nrScratchGeneration;
    g_nrScratchGeneration = UINT64_MAX;

    ID3D12Device device;
    ID3D12GraphicsCommandList cmd;
    ID3D12Resource source, target;
    DlssNr_Dx12 shader("saturated", &device);
    assert(shader.IsInit());

    PrimeRing(shader, cmd, source, target);
    const auto after = Revisit(shader, device, cmd, source, target);
    assert(after.srv == 48 * 5 && after.uav == 48 * 2);

    g_nrScratchGeneration = saved;
}

int main()
{
    ID3D12Device device;
    ID3D12GraphicsCommandList cmd;
    ID3D12Resource source, model, original, motion, previous, target, keep;
    {
        DlssNr_Dx12 shader("test", &device);
        assert(shader.IsInit());
        assert(device.resources.size() == 48 && device.cbvWrites == 48);
        for (unsigned frame = 0; frame < 145; ++frame)
        {
            DlssNrConstants constants {};
            constants.Width = 13 + frame;
            constants.Height = 7 + frame;
            constants.Mode = frame % 5;
            const bool full = frame % 2;
            assert(shader.DispatchPass(&cmd, constants, &source, full ? &model : nullptr, full ? &original : nullptr,
                                       full ? &motion : nullptr, full ? &previous : nullptr, &target,
                                       full ? &keep : nullptr));
            assert(std::memcmp(cmd.recorded.back().data(), &constants, sizeof(constants)) == 0);
            assert(cmd.groups.back() ==
                   (std::array<UINT, 3> { (constants.Width + 7) / 8, (constants.Height + 7) / 8, 1 }));
            const unsigned slot = frame % 48;
            // Contents, every frame, reused or not. The ring is 48 slots and `full` alternates on
            // `frame`, so a slot always comes back to the same bindings and from frame 48 on every
            // view is a hit -- which is exactly when a cache that reused the wrong descriptor would
            // still be holding the right one by luck if only counts were checked.
            const ViewRecord expectedSrv[5] = {
                { &source, DXGI_FORMAT_UNKNOWN, 0, true },
                { full ? &model : &source, DXGI_FORMAT_UNKNOWN, 0, true },
                { full ? &original : &source, DXGI_FORMAT_UNKNOWN, 0, true },
                { full ? &motion : &source, DXGI_FORMAT_UNKNOWN, 0, true },
                { full ? &previous : &source, DXGI_FORMAT_UNKNOWN, 0, true },
            };
            for (unsigned i = 0; i < 5; ++i)
                assert(device.srvs[slot][i] == expectedSrv[i]);

            const ViewRecord expectedUav[2] = {
                { &target, DXGI_FORMAT_UNKNOWN, 0, true },
                { full ? &keep : &target, DXGI_FORMAT_UNKNOWN, 0, true },
            };
            for (unsigned i = 0; i < 2; ++i)
                assert(device.uavs[slot][i] == expectedUav[i]);
            // Writing this pass must not change another slot's constants.
            for (unsigned i = 0; i < 48 && i <= frame; ++i)
            {
                const unsigned latest = i + (frame - i) / 48 * 48;
                assert(device.resources[i]->bytes == cmd.recorded[latest]);
            }
        }
        // 145 dispatches, 48 ring slots, bindings that never move: only the first pass over the ring
        // writes any view. Before descriptor reuse this was 145 * 5 and 145 * 2.
        assert(device.cbvWrites == 48 && device.srvWrites == 48 * 5 && device.uavWrites == 48 * 2);
        for (const auto& buffer : device.resources)
            assert(buffer->maps == 1 && buffer->unmaps == 0 && buffer->releases == 0);
        DlssNrConstants constants {};
        assert(!shader.DispatchPass(nullptr, constants, &source, nullptr, nullptr, nullptr, nullptr, &target, nullptr));
        assert(!shader.DispatchPass(&cmd, constants, nullptr, nullptr, nullptr, nullptr, nullptr, &target, nullptr));
        assert(!shader.DispatchPass(&cmd, constants, &source, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
        assert(cmd.recorded.size() == 145);
    }
    for (const auto& buffer : device.resources)
        assert(buffer->unmaps == 1 && buffer->releases == 1);

    // Every partial-allocation/map boundary must clean up exactly what it acquired.
    for (unsigned failure = 0; failure < 3; ++failure)
    {
        for (int slot = 0; slot < 48; ++slot)
        {
            ID3D12Device broken;
            if (failure == 0)
                broken.failCreateAt = slot;
            if (failure == 1)
                broken.failMapAt = slot;
            if (failure == 2)
                broken.nullMapAt = slot;
            {
                DlssNr_Dx12 shader("failed init", &broken);
                assert(!shader.IsInit() && broken.cbvWrites == 0);
                assert(!shader.DispatchPass(&cmd, {}, &source, nullptr, nullptr, nullptr, nullptr, &target, nullptr));
            }
            for (unsigned i = 0; i < broken.resources.size(); ++i)
            {
                const auto& buffer = broken.resources[i];
                assert(buffer->releases == 1);
                assert(buffer->unmaps == (i < static_cast<unsigned>(slot) ? 1u : 0u));
            }
        }
    }
    for (unsigned failure = 0; failure < 3; ++failure)
    {
        ID3D12Device broken;
        broken.failRoot = failure == 0;
        broken.failPipeline = failure == 1;
        broken.failHeaps = failure == 2;
        {
            DlssNr_Dx12 shader("failed setup", &broken);
            assert(!shader.IsInit());
        }
        for (const auto& buffer : broken.resources)
            assert(buffer->unmaps == 1 && buffer->releases == 1);
    }
    DlssNr_Dx12 noDevice("no device", nullptr);
    assert(!noDevice.IsInit());
    ReshapingRetiresDescriptors();
    ScratchAllocationRetiresDescriptors();
    SaturatedGenerationStopsReuse();

    std::cout << "PASS: production dispatch, 145 passes, isolated ring slots, fixed CBVs, 148 initialization "
                 "failures, descriptor reuse retired by 6 reshapes, by allocation and by saturation\n";
}
