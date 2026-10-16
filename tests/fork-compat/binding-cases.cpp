
using Proxy = StreamlineProxy;
using namespace Boundary;

#define FEATURES(X)                                                                                                    \
    X(slDLSSGSetOptions, sl::kFeatureDLSS_G, 0)                                                                        \
    X(slDLSSGGetState, sl::kFeatureDLSS_G, 0)                                                                          \
    X(slReflexGetState, sl::kFeatureReflex, 1)                                                                         \
    X(slReflexSleep, sl::kFeatureReflex, 1)                                                                            \
    X(slReflexSetOptions, sl::kFeatureReflex, 1)                                                                       \
    X(slReflexSetCameraData, sl::kFeatureReflex, 1)                                                                    \
    X(slReflexGetPredictedCameraData, sl::kFeatureReflex, 1)                                                           \
    X(slPCLSetMarker, sl::kFeaturePCL, 2)                                                                              \
    X(slPCLSetOptions, sl::kFeaturePCL, 2)                                                                             \
    X(slPCLGetState, sl::kFeaturePCL, 2)

static void reset()
{
    State::Instance() = {};
    lookups.clear();
    loads.clear();
    exports.clear();
    inits.clear();
    active.clear();
    bundled.clear();
    addressModules.clear();
    features.clear();
    setDeviceCalls = setVulkanCalls = reflexCalls = 0;
    lastDevice = nullptr;
    deviceSet = false;
    initOk = bundledAvailable = true;
    setDeviceResult = setVulkanResult = reflexResult = sl::Result::eOk;
    receivedVulkan = {};
    Proxy::_isD3D12Inited = Proxy::_isD3D12Requested = false;
    Proxy::_isVulkanInited = Proxy::_isVulkanRequested = false;
    Proxy::_slGetFeatureFunction = &GetFeature;
    Proxy::_slSetD3DDevice = &SetDevice;
    Proxy::_slSetVulkanInfo = &SetVulkan;
    Proxy::_slDLSSGGetPluginFunction = nullptr;
    Proxy::_slReflexGetPluginFunction = nullptr;
    Proxy::_slPCLGetPluginFunction = nullptr;
#define REGISTER(name, feature, group)                                                                                 \
    active[#name] = reinterpret_cast<void*>(&Stub<Proxy::PFN_##name>::Active);                                         \
    bundled[#name] = reinterpret_cast<void*>(&Stub<Proxy::PFN_##name>::Bundled);                                       \
    addressModules[active[#name]] = activeModules[group];                                                              \
    features[#name] = feature;                                                                                         \
    Proxy::_##name = nullptr;
    FEATURES(REGISTER)
#undef REGISTER
}

static void assertGroups(int fallbackGroup = -1)
{
#define CHECK(name, feature, group)                                                                                    \
    assert(reinterpret_cast<void*>(Proxy::_##name) == (group == fallbackGroup ? bundled.at(#name) : active.at(#name)));
    FEATURES(CHECK)
#undef CHECK
    auto& state = State::Instance();
    assert(state.optiSlDLSSG == (fallbackGroup == 0 ? bundledModules[0] : activeModules[0]));
    assert(state.optiSlReflex == (fallbackGroup == 1 ? bundledModules[1] : activeModules[1]));
    assert(state.optiSlPCL == (fallbackGroup == 2 ? bundledModules[2] : activeModules[2]));
}

int main()
{
    unsigned cases = 0;
    reset();
    deviceSet = true;
    assert(Proxy::BindActiveFeatures());
    assert(lookups.size() == 10 && loads.empty() && exports.empty());
    assertGroups();
    ++cases;

    // Each missing required sibling must replace the whole feature group, including
    // its nominated early-return pointer and every optional entry point.
    const std::tuple<const char*, int, size_t> missing[] = {
        { "slDLSSGSetOptions", 0, 2 }, { "slDLSSGGetState", 0, 2 },    { "slReflexGetState", 1, 5 },
        { "slReflexSleep", 1, 5 },     { "slReflexSetOptions", 1, 5 }, { "slPCLSetMarker", 2, 3 },
        { "slPCLSetOptions", 2, 3 },
    };
    for (auto [name, group, expectedExports] : missing)
    {
        reset();
        deviceSet = true;
        active[name] = nullptr;
        assert(Proxy::BindActiveFeatures());
        assertGroups(group);
        assert(loads.size() == 1 && exports.size() == expectedExports);
        ++cases;
    }

    // Optional camera/PCL telemetry misses keep complete required bindings active.
    for (const char* name : { "slReflexSetCameraData", "slReflexGetPredictedCameraData", "slPCLGetState" })
    {
        reset();
        deviceSet = true;
        active[name] = nullptr;
        assert(Proxy::BindActiveFeatures());
        assert(loads.empty() && exports.empty());
        assertGroups();
        ++cases;
    }

    reset();
    deviceSet = true;
    Proxy::_slGetFeatureFunction = nullptr;
    assert(Proxy::BindActiveFeatures());
    assert(lookups.empty() && loads.size() == 3 && exports.size() == 10);
#define BUNDLED(name, feature, group) assert(reinterpret_cast<void*>(Proxy::_##name) == bundled.at(#name));
    FEATURES(BUNDLED)
#undef BUNDLED
    ++cases;

    ID3D12Device device;
    reset();
    Proxy::_isD3D12Requested = true;
    setDeviceResult = sl::Result::eErrorInvalidParameter;
    assert(Proxy::SetD3DDeviceAndBind(&device) == setDeviceResult);
    assert(setDeviceCalls == 1 && lookups.empty() && loads.empty());
    ++cases;

    reset(); // A device registration before a D3D12 request must not bind plugins.
    assert(Proxy::SetD3DDeviceAndBind(&device) == sl::Result::eOk);
    assert(setDeviceCalls == 1 && lookups.empty());
    ++cases;

    reset();
    Proxy::_isD3D12Requested = true;
    assert(Proxy::SetD3DDeviceAndBind(&device) == sl::Result::eOk);
    assert(lastDevice == &device && lookups.size() == 10);
    assertGroups();
    ++cases;

    reset();
    assert(Proxy::InitWithD3D12(&device));
    assert(inits == std::vector { sl::RenderAPI::eD3D12 });
    assert(setDeviceCalls == 1 && reflexCalls == 1);
    assertGroups();
    assert(Proxy::InitWithD3D12(&device));
    assert(inits.size() == 1 && setDeviceCalls == 1 && reflexCalls == 1);
    ++cases;

    reset();
    initOk = false;
    assert(!Proxy::InitWithD3D12(&device));
    assert(setDeviceCalls == 0 && lookups.empty());
    ++cases;

    reset();
    setDeviceResult = sl::Result::eErrorInvalidParameter;
    assert(!Proxy::InitWithD3D12(&device));
    assert(lookups.empty() && loads.empty() && reflexCalls == 0);
    ++cases;

    // The second-device quirk defers a native D3D12 device but never the bridge's.
    reset();
    State::Instance().gameQuirks = GameQuirk::CreateSLOnThe2ndDevice;
    assert(Proxy::InitWithD3D12(&device));
    assert(setDeviceCalls == 0 && lookups.empty() && reflexCalls == 0);
    ++cases;

    reset();
    State::Instance().gameQuirks = GameQuirk::CreateSLOnThe2ndDevice;
    ID3D11Device bridge;
    State::Instance().currentD3D11Device = &bridge;
    assert(Proxy::InitWithD3D12(&device));
    assert(setDeviceCalls == 1 && lookups.size() == 10 && reflexCalls == 1);
    assertGroups();
    ++cases;

    reset();
    assert(!Proxy::DisableReflex()); // Absent export must fail without a null call.
    assert(reflexCalls == 0);
    ++cases;

    reset();
    active["slReflexSetOptions"] = nullptr;
    bundledAvailable = false;
    assert(!Proxy::InitWithD3D12(&device));
    assert(Proxy::_slReflexSetOptions == nullptr && reflexCalls == 0);
    ++cases;

    reset();
    reflexResult = sl::Result::eErrorInvalidParameter;
    assert(!Proxy::InitWithD3D12(&device));
    assert(reflexCalls == 1);
    ++cases;

    reset();
    assert(Proxy::InitWithVulkan());
    assert(inits == std::vector { sl::RenderAPI::eVulkan });
    assert(lookups.empty() && setVulkanCalls == 0);
    assert(Proxy::InitWithVulkan());
    assert(inits.size() == 1);
    assert(!Proxy::InitWithD3D12(&device)); // Interop D3D12 must not take Vulkan's request.
    assert(inits.size() == 1 && setDeviceCalls == 0 && lookups.empty());
    ++cases;

    reset();
    Proxy::_isD3D12Requested = true;
    assert(!Proxy::InitWithVulkan());
    assert(inits.empty() && !Proxy::_isVulkanRequested);
    ++cases;

    reset();
    initOk = false;
    assert(!Proxy::InitWithVulkan());
    assert(!Proxy::_isVulkanRequested);
    ++cases;

    sl::VulkanInfo info;
    info.device = reinterpret_cast<VkDevice>(&device);
    info.graphicsQueueFamily = 7;
    info.computeQueueIndex = 3;
    reset();
    assert(!Proxy::SetVulkanInfo(info));
    assert(setVulkanCalls == 0 && lookups.empty());
    ++cases;

    reset();
    Proxy::_isVulkanRequested = true;
    Proxy::_slSetVulkanInfo = nullptr;
    assert(!Proxy::SetVulkanInfo(info));
    assert(setVulkanCalls == 0 && lookups.empty());
    ++cases;

    reset();
    Proxy::_isVulkanRequested = true;
    setVulkanResult = sl::Result::eErrorInvalidParameter;
    assert(!Proxy::SetVulkanInfo(info));
    assert(setVulkanCalls == 1 && lookups.empty() && reflexCalls == 0);
    ++cases;

    reset();
    assert(Proxy::InitWithVulkan());
    assert(Proxy::SetVulkanInfo(info));
    assert(receivedVulkan.device == info.device && receivedVulkan.graphicsQueueFamily == 7 &&
           receivedVulkan.computeQueueIndex == 3);
    assert(setVulkanCalls == 1 && lookups.size() == 10 && reflexCalls == 1);
    assertGroups();
    assert(Proxy::SetVulkanInfo(info));
    assert(setVulkanCalls == 1 && reflexCalls == 1);
    ++cases;

    reset();
    assert(Proxy::InitWithVulkan());
    active["slReflexSetOptions"] = nullptr;
    bundledAvailable = false;
    assert(!Proxy::SetVulkanInfo(info));
    assert(reflexCalls == 0 && !Proxy::_isVulkanInited);
    ++cases;

    reset();
    deviceSet = true;
    active["slPCLSetOptions"] = nullptr;
    bundled["slPCLSetOptions"] = nullptr;
    assert(!Proxy::BindActiveFeatures());
    assert(Proxy::_slReflexSetOptions != nullptr);
    ++cases;

    // D3D12 deliberately keeps device registration nonfatal when one bundled group is short.
    reset();
    active["slDLSSGGetState"] = nullptr;
    bundled["slDLSSGGetState"] = nullptr;
    assert(Proxy::InitWithD3D12(&device));
    assert(setDeviceCalls == 1 && reflexCalls == 1);
    ++cases;

    reset();
    assert(Proxy::InitWithVulkan());
    active["slDLSSGGetState"] = nullptr;
    bundled["slDLSSGGetState"] = nullptr;
    assert(!Proxy::SetVulkanInfo(info));
    assert(Proxy::_slReflexSetOptions != nullptr && !Proxy::_isVulkanInited);
    ++cases;

    std::cout << "Streamline binding: " << cases << " production binding/init cases passed\n";
}
