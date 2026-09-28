#include "pch.h"
#include "DlssNr_NgxInfo.h"

#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>

#include <format>
#include <map>
#include <mutex>

namespace
{
std::mutex g_mutex;
std::map<std::wstring, DlssNr::NgxInfo::Snapshot> g_snapshots;

void CopyVersion(const version_t& in, uint16_t out[4])
{
    out[0] = in.major;
    out[1] = in.minor;
    out[2] = in.patch;
    out[3] = in.reserved;
}

std::string VersionText(const uint16_t v[4])
{
    if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0)
        return "unknown";
    return std::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

std::filesystem::path ModulePath(HMODULE module)
{
    wchar_t buffer[32768] {};
    const DWORD count = module != nullptr ? GetModuleFileNameW(module, buffer, _countof(buffer)) : 0;
    return count != 0 && count < _countof(buffer) ? std::filesystem::path(buffer) : std::filesystem::path();
}

DlssNr::PeScan::Feature18Route ScanLoaderUnguarded(const uint8_t* base)
{
    // The headers are always mapped; SizeOfImage from them bounds the whole scan, and the loader is
    // mapped in full for as long as the NGX proxy holds it.
    const uint32_t size = DlssNr::PeScan::ImageSize(base, 4096);
    if (size == 0)
        return DlssNr::PeScan::Feature18Route::Unknown;

    return DlssNr::PeScan::FindFeature18Route(base, size, reinterpret_cast<uint64_t>(base));
}

// A driver image this code does not own is read, so a read that faults means "not recognised" rather
// than a crash. Nothing here needs unwinding, which is what lets __try live in it.
DlssNr::PeScan::Feature18Route ScanLoader(HMODULE loader)
{
    if (loader == nullptr)
        return DlssNr::PeScan::Feature18Route::Unknown;

    __try
    {
        return ScanLoaderUnguarded(reinterpret_cast<const uint8_t*>(loader));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return DlssNr::PeScan::Feature18Route::Unknown;
    }
}
} // namespace

namespace DlssNr::NgxInfo
{
std::string DriverFromLoaderVersion(const uint16_t version[4])
{
    // 32.0.16.1664 -> 16 * 10000 + 1664 = 161664 -> last five digits 61664 -> "616.64".
    if (version[0] < 30 || version[2] > 99 || version[3] > 9999)
        return {};
    const unsigned combined = (static_cast<unsigned>(version[2]) * 10000u + version[3]) % 100000u;
    return std::format("{}.{:02}", combined / 100, combined % 100);
}

const Snapshot& Describe(const std::filesystem::path& modelPath)
{
    std::lock_guard lock(g_mutex);

    const auto key = modelPath.wstring();
    if (const auto found = g_snapshots.find(key); found != g_snapshots.end())
        return found->second;

    Snapshot s;
    s.modelPath = modelPath;

    const HMODULE loader = NVNGXProxy::NVNGXModule();
    s.loaderPath = ModulePath(loader);
    s.route = ScanLoader(loader);

    version_t v {};
    if (!s.loaderPath.empty() && Util::GetFileVersion(s.loaderPath.wstring(), &v))
        CopyVersion(v, s.loaderVersion);

    if (Util::GetFileVersion(modelPath.wstring(), &v))
        CopyVersion(v, s.modelVersion);

    std::error_code ec;
    s.modelBytes = std::filesystem::file_size(modelPath, ec);
    if (ec)
        s.modelBytes = 0;

    s.loadedModel = ModulePath(GetModuleHandleW(L"nvngx_dlssnr.dll"));

    // Under Proton the number is the wine loader's own Windows-branch build, not the Linux driver's
    // version (615.71.09 ships 32.0.16.1691); see DriverFromLoaderVersion.
    const auto driver = DriverFromLoaderVersion(s.loaderVersion);
    const auto driverText = driver.empty() ? std::string()
                            : State::Instance().isRunningOnLinux
                                ? std::format(" (Windows-branch loader build {}, not the Linux driver version)", driver)
                                : std::format(" (driver {})", driver);
    LOG_INFO("DLSS-NR NGX: loader {} version {}{}; it {}", s.loaderPath.empty() ? "(none)" : s.loaderPath.string(),
             VersionText(s.loaderVersion), driverText, PeScan::Describe(s.route));
    LOG_INFO("DLSS-NR NGX: model {} version {}, {} bytes; {}", modelPath.string(), VersionText(s.modelVersion),
             s.modelBytes,
             s.loadedModel.empty() ? std::string("not loaded in this process yet")
                                   : std::format("already loaded from {}", s.loadedModel.string()));

    if (PeScan::KnownFaultingPairing(s.route, s.modelVersion))
        LOG_WARN("DLSS-NR NGX: this loader creates feature 18 itself and model {} is one it was measured faulting "
                 "with (dlss5-bridge, 32.0.16.1664/1686 with 310.8.0.0). The forwarder and the direct loader call "
                 "the model themselves and do not take that route; UseProxy does, and is refused.",
                 VersionText(s.modelVersion));

    return g_snapshots.emplace(key, std::move(s)).first->second;
}

std::optional<std::filesystem::path> FindModel()
{
    auto found = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");
    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");
    return found;
}

bool LoaderRouteKnownToFault(const std::filesystem::path& modelPath)
{
    const auto& s = Describe(modelPath);
    return PeScan::KnownFaultingPairing(s.route, s.modelVersion);
}
} // namespace DlssNr::NgxInfo
