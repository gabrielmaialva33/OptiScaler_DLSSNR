// Host boundary fakes around the unmodified Streamline binding/init functions.
// SDK function-pointer signatures, feature IDs, options and VulkanInfo are real.
#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <vulkan/vulkan.h>
#include <sl_helpers_vk.h>

#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
using HMODULE = void*;
using LPCWSTR = const wchar_t*;
constexpr unsigned GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;
constexpr unsigned GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
struct ID3D12Device
{
};
struct ID3D11Device
{
};
namespace GameQuirk
{
constexpr unsigned CreateSLOnThe2ndDevice = 1;
}
struct ScopedSkipDxgiLoadChecks
{
};
namespace spdlog
{
inline void info(const char*) {}
} // namespace spdlog

struct State
{
    unsigned gameQuirks = 0;
    ID3D11Device* currentD3D11Device = nullptr;
    HMODULE optiSlDLSSG = nullptr, optiSlReflex = nullptr, optiSlPCL = nullptr;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct FakePath
{
    std::wstring value() const { return L"/bundled"; }
};
struct Config
{
    FakePath MainDllPath;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

namespace Boundary
{
inline int modules[6] {};
inline HMODULE activeModules[] = { &modules[0], &modules[1], &modules[2] };
inline HMODULE bundledModules[] = { &modules[3], &modules[4], &modules[5] };
inline std::map<std::string, void*> active, bundled;
inline std::map<std::string, sl::Feature> features;
inline std::map<void*, HMODULE> addressModules;
inline std::vector<std::string> lookups, loads, exports;
inline std::vector<sl::RenderAPI> inits;
inline unsigned setDeviceCalls = 0, setVulkanCalls = 0, reflexCalls = 0;
inline void* lastDevice = nullptr;
inline bool deviceSet = false, initOk = true, bundledAvailable = true;
inline sl::Result setDeviceResult = sl::Result::eOk, setVulkanResult = sl::Result::eOk;
inline sl::Result reflexResult = sl::Result::eOk;
inline sl::VulkanInfo receivedVulkan;

inline sl::Result Record(const sl::ReflexOptions& options)
{
    assert(options.mode == sl::ReflexMode::eOff);
    assert(!options.useMarkersToOptimize);
    ++reflexCalls;
    return reflexResult;
}
template <typename... Args> sl::Result Record(Args&&...) { return sl::Result::eOk; }
template <typename T> struct Stub;
template <typename... Args> struct Stub<sl::Result (*)(Args...)>
{
    static sl::Result Active(Args... args) { return Record(args...); }
    static sl::Result Bundled(Args... args) { return Record(args...); }
};
inline sl::Result GetFeature(sl::Feature feature, const char* name, void*& target)
{
    assert(deviceSet); // SDK ordering: binding only after successful device registration.
    assert(features.at(name) == feature);
    lookups.emplace_back(name);
    target = active.at(name);
    return target ? sl::Result::eOk : sl::Result::eErrorFeatureMissing;
}
inline sl::Result SetDevice(void* device)
{
    ++setDeviceCalls;
    lastDevice = device;
    deviceSet = setDeviceResult == sl::Result::eOk;
    return setDeviceResult;
}
inline sl::Result SetVulkan(const sl::VulkanInfo& info)
{
    ++setVulkanCalls;
    receivedVulkan = info;
    deviceSet = setVulkanResult == sl::Result::eOk;
    return setVulkanResult;
}
inline void* Plugin(const char* name)
{
    exports.emplace_back(name);
    return bundledAvailable ? bundled.at(name) : nullptr;
}
inline void* ProcAddress(HMODULE module, const char* name)
{
    assert(module == bundledModules[0] || module == bundledModules[1] || module == bundledModules[2]);
    assert(std::string(name) == "slGetPluginFunction");
    return reinterpret_cast<void*>(&Plugin);
}
} // namespace Boundary
inline bool GetModuleHandleExW(unsigned flags, LPCWSTR address, HMODULE* module)
{
    assert(flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT));
    *module = Boundary::addressModules.at(const_cast<void*>(reinterpret_cast<const void*>(address)));
    return true;
}
namespace KernelBaseProxy
{
inline auto GetProcAddress_() { return &Boundary::ProcAddress; }
} // namespace KernelBaseProxy
namespace NtdllProxy
{
inline HMODULE LoadLibraryExW_Ldr(const char* path, void*, void*)
{
    const auto filename = std::filesystem::path(path).filename().string();
    Boundary::loads.push_back(filename);
    if (!Boundary::bundledAvailable)
        return nullptr;
    if (filename == "sl.dlss_g.dll")
        return Boundary::bundledModules[0];
    if (filename == "sl.reflex.dll")
        return Boundary::bundledModules[1];
    assert(filename == "sl.pcl.dll");
    return Boundary::bundledModules[2];
}
} // namespace NtdllProxy

class StreamlineProxy
{
  public:
    // @aliases@
    // @fields@
    static bool InitStreamline(sl::RenderAPI api)
    {
        Boundary::inits.push_back(api);
        return Boundary::initOk;
    }
    // @production@
};
