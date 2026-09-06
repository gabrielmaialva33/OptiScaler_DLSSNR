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
            assert(shader.DispatchPass(&cmd, constants, &source, full ? &model : nullptr,
                                       full ? &original : nullptr, full ? &motion : nullptr,
                                       full ? &previous : nullptr, &target, full ? &keep : nullptr));
            assert(std::memcmp(cmd.recorded.back().data(), &constants, sizeof(constants)) == 0);
            assert(cmd.groups.back() == (std::array<UINT, 3> { (constants.Width + 7) / 8,
                                                              (constants.Height + 7) / 8, 1 }));
            const unsigned slot = frame % 48;
            assert(device.srvs[slot] == (std::array<ID3D12Resource*, 5> {
                &source, full ? &model : &source, full ? &original : &source,
                full ? &motion : &source, full ? &previous : &source }));
            assert(device.uavs[slot] == (std::array<ID3D12Resource*, 2> { &target, full ? &keep : &target }));
            // Writing this pass must not change another slot's constants.
            for (unsigned i = 0; i < 48 && i <= frame; ++i)
            {
                const unsigned latest = i + (frame - i) / 48 * 48;
                assert(device.resources[i]->bytes == cmd.recorded[latest]);
            }
        }
        assert(device.cbvWrites == 48 && device.srvWrites == 145 * 5 && device.uavWrites == 145 * 2);
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
            if (failure == 0) broken.failCreateAt = slot;
            if (failure == 1) broken.failMapAt = slot;
            if (failure == 2) broken.nullMapAt = slot;
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
        { DlssNr_Dx12 shader("failed setup", &broken); assert(!shader.IsInit()); }
        for (const auto& buffer : broken.resources)
            assert(buffer->unmaps == 1 && buffer->releases == 1);
    }
    DlssNr_Dx12 noDevice("no device", nullptr);
    assert(!noDevice.IsInit());
    std::cout << "PASS: production dispatch, 145 passes, isolated ring slots, fixed CBVs, 148 initialization failures\n";
}
