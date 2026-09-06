unsigned deviceCalls = 0, allCalls = 0;
NVSDK_NGX_Result result = 1;
ID3D12Device* expectedDevice = nullptr;
unsigned CoreDeviceShutdown(ID3D12Device* device, unsigned* remaining)
{
    assert(device == expectedDevice && device != nullptr);
    assert(remaining && *remaining == 0);
    *remaining = 7; // A real writable output, which the old one-argument call did not supply.
    ++deviceCalls;
    return result;
}
unsigned CoreAllShutdown()
{
    ++allCalls;
    return result;
}

int main()
{
    static_assert(std::is_same_v<decltype(NVNGXProxy::D3D12_Shutdown1()), PFN_D3D12_Shutdown1>);
    NVNGXProxy::_module.D3D12_Shutdown1 = &CoreDeviceShutdown;
    NVNGXProxy::_module.D3D12_Shutdown = &CoreAllShutdown;
    assert(NVNGXProxy::D3D12_Shutdown1() == nullptr);
    NVNGXProxy::_dx12Inited = true;
    auto call = NVNGXProxy::D3D12_Shutdown1();
    assert(call != nullptr);
    ID3D12Device device;
    expectedDevice = &device;
    assert(call(&device) == 1 && deviceCalls == 1 && allCalls == 0);
    result = 0xBAD00002;
    assert(call(&device) == result && deviceCalls == 2 && allCalls == 0);
    assert(call(nullptr) == result && allCalls == 1 && deviceCalls == 2);
    result = 1;
    assert(call(nullptr) == 1 && allCalls == 2 && deviceCalls == 2);
    NVNGXProxy::_module.D3D12_Shutdown = nullptr;
    assert(call(nullptr) == NVSDK_NGX_Result_FAIL_NotImplemented);
    assert(allCalls == 2 && deviceCalls == 2);
    NVNGXProxy::_module.D3D12_Shutdown1 = nullptr;
    assert(NVNGXProxy::D3D12_Shutdown1() == nullptr);
    std::cout
        << "PASS: production core shutdown adapter, writable output, result propagation and null-device routing\n";
}
