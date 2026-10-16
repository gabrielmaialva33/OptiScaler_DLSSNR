int main()
{
    Consumer output;
    State::Instance().currentFG = &output;
    Sl_Inputs input;
    ID3D12Resource texture;
    sl::Resource resource;
    resource.native = &texture;
    resource.state = 17;
    ID3D12GraphicsCommandList commands;
    const sl::BufferType types[] = { sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors, sl::kBufferTypeHUDLessColor,
                                     sl::kBufferTypeUIColorAndAlpha };
    for (auto type : types)
    {
        sl::Extent extent { 64, 128, 1920, 1080 };
        sl::ResourceTag tag(&resource, type, sl::eValidUntilPresent, &extent);
        assert(input.reportResource(tag, &commands, 42));
        const auto& got = output.captured;
        assert(got.left == 128 && got.top == 64 && got.width == 1920 && got.height == 1080);
        assert(got.resource == &texture && got.cmdList == &commands && got.state == 17 && got.frameIndex == 2);
        assert(got.validity == FG_ResourceValidity::UntilPresent);
    }
    sl::ResourceTag whole(&resource, sl::kBufferTypeDepth, sl::eOnlyValidNow);
    assert(input.reportResource(whole, &commands, 0));
    assert(output.captured.left == 0 && output.captured.top == 0);
    assert(output.captured.width == 4096 && output.captured.height == 2160);
    assert(output.captured.validity == FG_ResourceValidity::ValidNow);
    whole.extent = sl::Extent { 100, 200, 0, 300 }; // An absent extent ignores its unused origin.
    assert(input.reportResource(whole, &commands, 0));
    assert(output.captured.left == 0 && output.captured.top == 0 && output.captured.width == 4096);

    sl::Extent edge { 2159, 4095, 1, 1 };
    sl::ResourceTag exact(&resource, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &edge);
    assert(input.reportResource(exact, &commands, 42));
    assert(output.captured.left == 4095 && output.captured.top == 2159 && input.mvsWidth == 1);
    const sl::Extent invalid[] = {
        { 0, 4096, 1, 1 }, { 2160, 0, 1, 1 }, { 0, 4095, 2, 1 }, { 2159, 0, 1, 2 }, { 0, UINT32_MAX, UINT32_MAX, 1 }
    };
    for (const auto& extent : invalid)
    {
        const auto calls = output.calls;
        sl::ResourceTag tag(&resource, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &extent);
        assert(!input.reportResource(tag, &commands, 42));
        assert(output.calls == calls && input.mvsWidth == 1 && input.mvsHeight == 1);
    }
    State::Instance().currentFG = nullptr;
    State::Instance().currentVkFG = &output;
    resource.native = reinterpret_cast<void*>(123);
    resource.width = 800;
    resource.height = 600;
    assert(input.reportResource(whole, &commands, 42));
    assert(output.vkCaptured.image == 123 && output.vkCaptured.width == 800 && output.vkCaptured.height == 600);
    std::cout << "Streamline D3D12 tag origins, fallback, bounds and Vulkan branch passed\n";
}
