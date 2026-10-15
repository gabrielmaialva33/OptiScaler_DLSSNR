#include "pch.h"

#include "MfgProbe.h"
#include "MfgUnlock.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Ntdll_Proxy.h>
#include <proxies/NVNGX_Proxy.h>

#include <nvsdk_ngx_defs.h>

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <vector>

// The Windows half of the step-0 MFG probe. See MfgProbe.h for the contract and mfg-rtx20-30.md for the
// design. This file gathers the facts (the GPU, the provider's PE, the driver, the fatbin census, the one
// NGX capability query) and feeds the std-only drivers in MfgProbe.h, which do the gating, the deciding and
// the formatting. It never writes anything.
//
// Nothing here is a static import. bcrypt's BCryptHash and gdi32's D3DKMT entries are resolved at the
// moment they are needed, which only happens past the gate on a below-Ada card, so the DLL's import table
// is what it was before the probe and no machine maps bcrypt.dll because of it.

namespace
{
MfgProbe::GateCache g_gate;

// Guards only the list of modules already described; no lock is held across the work itself, so a load
// the work triggers (the NGX core reading the provider, bcrypt) can never re-enter a held lock.
std::mutex g_modulesMutex;
std::vector<HMODULE> g_modulesDone;

// The admission line is claimed by exactly one call per process, without a lock.
std::atomic<bool> g_admissionClaimed { false };

MfgProbe::GpuIdentity ReadGpuIdentity()
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    MfgProbe::GpuIdentity id;
    id.known = gpu.vendorId != VendorId::Invalid;
    id.isNvidia = gpu.vendorId == VendorId::Nvidia;
    id.arch = gpu.nvidiaArchInfo.architecture;
    return id;
}

// ---- small readers ------------------------------------------------------------------------------

std::filesystem::path ModulePath(HMODULE module)
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD count = module != nullptr ? GetModuleFileNameW(module, buffer, _countof(buffer)) : 0;
    return count != 0 && count < _countof(buffer) ? std::filesystem::path(buffer) : std::filesystem::path();
}

std::string VersionText(const version_t& v)
{
    if (v.major == 0 && v.minor == 0 && v.patch == 0 && v.reserved == 0)
        return "unknown";
    return std::format("{}.{}.{}.{}", v.major, v.minor, v.patch, v.reserved);
}

// Mirrors DlssNr::NgxInfo::DriverFromLoaderVersion: the driver core's file version (e.g. 32.0.16.1692)
// encodes the driver number (616.92) in its last two fields. On native Windows -- where Rafael's test
// runs -- _nvngx.dll is the real driver's, so this is the real driver version; under Proton it would be
// the Wine loader's Windows-branch build.
std::string DriverFromLoaderVersion(const version_t& v)
{
    if (v.major < 30 || v.patch > 99 || v.reserved > 9999)
        return "unknown";
    const unsigned combined = (static_cast<unsigned>(v.patch) * 10000u + v.reserved) % 100000u;
    return std::format("{}.{:02}", combined / 100, combined % 100);
}

std::string DriverVersion()
{
    HMODULE core = KernelBaseProxy::GetModuleHandleW_()(L"_nvngx.dll");
    if (core == nullptr)
        return "unknown";

    const auto path = ModulePath(core);
    version_t v {};
    if (path.empty() || !Util::GetFileVersion(path.wstring(), &v))
        return "unknown";

    return DriverFromLoaderVersion(v);
}

// Reads the immediate of the provider's NVSDK_NGX_GetGPUArchitecture export. Every build seen is
// mov eax, imm32 (0xB8) / ret, so the four bytes after 0xB8 are the architecture the provider declares
// as its minimum. A foreign image is read, so a fault means "unknown" rather than a crash; the helper
// holds no C++ object that needs unwinding so SEH can live in it.
uint32_t ReadMinArchExportGuarded(const uint8_t* fn)
{
    __try
    {
        if (fn != nullptr && fn[0] == 0xB8)
            return static_cast<uint32_t>(fn[1]) | (static_cast<uint32_t>(fn[2]) << 8) |
                   (static_cast<uint32_t>(fn[3]) << 16) | (static_cast<uint32_t>(fn[4]) << 24);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

uint32_t ReadMinArchExport(HMODULE module)
{
    if (module == nullptr)
        return 0;
    auto fn =
        reinterpret_cast<const uint8_t*>(KernelBaseProxy::GetProcAddress_()(module, "NVSDK_NGX_GetGPUArchitecture"));
    return ReadMinArchExportGuarded(fn);
}

// TimeDateStamp and SizeOfImage from the provider's PE headers. Read-only, SEH-guarded.
void ReadPeStampsGuarded(const uint8_t* base, uint32_t* timeDateStamp, uint32_t* sizeOfImage)
{
    __try
    {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return;
        *timeDateStamp = nt->FileHeader.TimeDateStamp;
        *sizeOfImage = nt->OptionalHeader.SizeOfImage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// The provider's readable, non-executable sections, each clamped to SizeOfImage. The fatbins live there.
struct SectionView
{
    const uint8_t* data;
    size_t size;
};

int ReadDataSectionsGuarded(const uint8_t* base, SectionView* out, int max)
{
    int count = 0;
    __try
    {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return 0;
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return 0;

        const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
        auto section = IMAGE_FIRST_SECTION(nt);

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && count < max; ++i)
        {
            const auto& s = section[i];
            if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(s.Characteristics & IMAGE_SCN_MEM_READ))
                continue;
            if (s.VirtualAddress >= imageSize)
                continue;

            size_t size = s.Misc.VirtualSize;
            if (size > imageSize - s.VirtualAddress)
                size = imageSize - s.VirtualAddress;

            out[count++] = { base + s.VirtualAddress, size };
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    return count;
}

// One section's census, guarded the same way: a fault inside a mapped image this code does not own ends
// that section's count rather than the game.
bool CensusGuarded(const uint8_t* data, size_t size, MfgProbe::Census* out)
{
    __try
    {
        *out = MfgProbe::TakeCensus(data, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

using PfnBCryptHash = decltype(&BCryptHash);

std::string Sha256Prefix(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 0xFFFFFFFFull)
        return "unknown";

    std::vector<uint8_t> bytes(static_cast<size_t>(size));

    FILE* file = _wfopen(path.wstring().c_str(), L"rb");
    if (file == nullptr)
        return "unknown";
    const size_t read = fread(bytes.data(), 1, bytes.size(), file);
    fclose(file);
    if (read != bytes.size())
        return "unknown";

    // Resolved here, past the gate, so bcrypt.dll is not a static import of OptiScaler.dll.
    HMODULE bcrypt = KernelBaseProxy::GetModuleHandleW_()(L"bcrypt.dll");
    bool loadedHere = false;
    if (bcrypt == nullptr)
    {
        bcrypt = NtdllProxy::LoadLibraryExW_Ldr(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        loadedHere = bcrypt != nullptr;
    }
    if (bcrypt == nullptr)
        return "unknown";

    auto hash = reinterpret_cast<PfnBCryptHash>(KernelBaseProxy::GetProcAddress_()(bcrypt, "BCryptHash"));

    uint8_t digest[32] = {};
    const bool ok = hash != nullptr && hash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, bytes.data(),
                                            static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) == 0;

    if (loadedHere)
        NtdllProxy::FreeLibrary_Ldr(bcrypt);

    if (!ok)
        return "unknown";

    std::string out;
    for (int i = 0; i < 6; ++i) // first six bytes -> twelve hex chars, as the note's inventory records
        out += std::format("{:02x}", digest[i]);
    return out;
}

std::string Provenance(const std::filesystem::path& path)
{
    auto lower = path.wstring();
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    if (lower.find(L"\\models\\dlssg\\") != std::wstring::npos)
        return "NGX models";
    if (lower.find(L"driverstore") != std::wstring::npos)
        return "DriverStore";
    return "game";
}

// HAGS, read-only, through gdi32 entries resolved here rather than imported. D3DKMTQueryAdapterInfo is the
// function OptiScaler's SpoofHAGS hook sits on, so with spoofing on this reads the spoofed value; HagsText
// marks it. Each driver thunk is guarded on its own and the adapter handle is closed on every path that
// opened it. Returns 0 unknown, 1 off, 2 on.
using PfnOpenAdapterFromLuid = decltype(&D3DKMTOpenAdapterFromLuid);
using PfnQueryAdapterInfo = decltype(&D3DKMTQueryAdapterInfo);
using PfnCloseAdapter = decltype(&D3DKMTCloseAdapter);

struct KmtFunctions
{
    PfnOpenAdapterFromLuid open = nullptr;
    PfnQueryAdapterInfo query = nullptr;
    PfnCloseAdapter close = nullptr;
};

KmtFunctions ResolveKmt()
{
    KmtFunctions k;
    HMODULE gdi32 = KernelBaseProxy::GetModuleHandleW_()(L"gdi32.dll");
    if (gdi32 == nullptr)
        return k;
    k.open = reinterpret_cast<PfnOpenAdapterFromLuid>(
        KernelBaseProxy::GetProcAddress_()(gdi32, "D3DKMTOpenAdapterFromLuid"));
    k.query =
        reinterpret_cast<PfnQueryAdapterInfo>(KernelBaseProxy::GetProcAddress_()(gdi32, "D3DKMTQueryAdapterInfo"));
    k.close = reinterpret_cast<PfnCloseAdapter>(KernelBaseProxy::GetProcAddress_()(gdi32, "D3DKMTCloseAdapter"));
    return k;
}

int ReadHagsGuarded(const KmtFunctions& k, LUID luid)
{
    if (k.open == nullptr || k.query == nullptr || k.close == nullptr)
        return 0;

    D3DKMT_OPENADAPTERFROMLUID open {};
    open.AdapterLuid = luid;
    __try
    {
        if (k.open(&open) != 0)
            return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    int result = 0;
    D3DKMT_WDDM_2_7_CAPS caps {};
    D3DKMT_QUERYADAPTERINFO query {};
    query.hAdapter = open.hAdapter;
    query.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    query.pPrivateDriverData = &caps;
    query.PrivateDriverDataSize = sizeof(caps);
    __try
    {
        if (k.query(&query) == 0)
            result = caps.HwSchEnabled ? 2 : 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = 0;
    }

    D3DKMT_CLOSEADAPTER close {};
    close.hAdapter = open.hAdapter;
    __try
    {
        k.close(&close);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return result;
}

std::string HagsText(LUID luid)
{
    const int state = ReadHagsGuarded(ResolveKmt(), luid);
    std::string text = state == 2 ? "on" : state == 1 ? "off" : "unknown";
    if (state != 0 && Config::Instance()->SpoofHAGS.value_or_default())
        text += "(spoofed)";
    return text;
}

MfgProbe::GateState ToGateState(MfgUnlock::GateReport::State state)
{
    switch (state)
    {
    case MfgUnlock::GateReport::Found:
        return MfgProbe::GateState::Found;
    case MfgUnlock::GateReport::Foreign:
        return MfgProbe::GateState::Foreign;
    default:
        return MfgProbe::GateState::Missing;
    }
}

// ---- environments -------------------------------------------------------------------------------

class ModuleEnv : public MfgProbe::IModuleProbeEnv
{
  public:
    explicit ModuleEnv(HMODULE module) : _module(module), _gpu(IdentifyGpu::getPrimaryGpu()) {}

    bool IsNvidia() override { return _gpu.vendorId == VendorId::Nvidia; }
    uint32_t ArchId() override { return _gpu.nvidiaArchInfo.architecture; }

    bool ClaimModule() override
    {
        // Null is TryApply's "the game's nvngx_dlssg.dll", resolved the same way it resolves it.
        if (_module == nullptr)
            _module = KernelBaseProxy::GetModuleHandleW_()(L"nvngx_dlssg.dll");
        if (_module == nullptr)
            return false;

        std::lock_guard lock(g_modulesMutex);
        if (std::find(g_modulesDone.begin(), g_modulesDone.end(), _module) != g_modulesDone.end())
            return false;
        g_modulesDone.push_back(_module);
        return true;
    }

    MfgProbe::ForeignLoader DetectForeignLoader() override
    {
        MfgProbe::ForeignLoader foreign;
        foreign.moduleLoaded = KernelBaseProxy::GetModuleHandleW_()(L"sm86_backend.dll") != nullptr;

        const auto exe = ModulePath(KernelBaseProxy::GetModuleHandleW_()(nullptr));
        if (!exe.empty())
        {
            std::error_code ec;
            foreign.iniPresent = std::filesystem::exists(exe.parent_path() / "dlssg_sm86.ini", ec);
        }
        return foreign;
    }

    MfgProbe::Census TakeModuleCensus() override
    {
        MfgProbe::Census total;

        SectionView sections[64] = {};
        const int count = ReadDataSectionsGuarded(reinterpret_cast<const uint8_t*>(_module), sections, 64);

        for (int i = 0; i < count; ++i)
        {
            MfgProbe::Census part;
            if (CensusGuarded(sections[i].data, sections[i].size, &part))
                MfgProbe::Accumulate(total, part);
        }
        return total;
    }

    MfgProbe::ModuleFacts GatherFacts(const MfgProbe::Census& census) override
    {
        MfgProbe::ModuleFacts facts;
        facts.adapterName = _gpu.name;
        facts.arch = _gpu.nvidiaArchInfo.architecture;
        facts.impl = _gpu.nvidiaArchInfo.implementation;
        facts.driver = DriverVersion();
        facts.hags = HagsText(_gpu.luid);

        const auto path = ModulePath(_module);
        facts.providerFile = path.empty() ? std::string("nvngx_dlssg.dll") : path.filename().string();

        version_t v {};
        if (!path.empty() && Util::GetFileVersion(path.wstring(), &v))
            facts.providerVersion = VersionText(v);

        ReadPeStampsGuarded(reinterpret_cast<const uint8_t*>(_module), &facts.timeDateStamp, &facts.sizeOfImage);
        facts.sha256Prefix = path.empty() ? std::string("unknown") : Sha256Prefix(path);
        facts.provenance = path.empty() ? std::string("game") : Provenance(path);
        facts.row = "none"; // the qualification table is step 3; step 0 only records what identifies the build
        facts.minArchExport = ReadMinArchExport(_module);
        facts.census = census;

        // Read-only and silent: the same signature search the unlock uses, without writing or logging.
        const MfgUnlock::GateReport gates = MfgUnlock::ProbeGates(_module);
        facts.advertise = ToGateState(gates.advertise);
        facts.validate = ToGateState(gates.validate);
        return facts;
    }

    void Emit(const std::string& line) override { LOG_INFO("{}", line); }

  private:
    HMODULE _module;
    GpuInformation _gpu;
};

class AdmissionEnv : public MfgProbe::IAdmissionProbeEnv
{
  public:
    AdmissionEnv(IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info)
        : _adapter(adapter), _info(info), _gpu(IdentifyGpu::getPrimaryGpu())
    {
    }

    bool IsNvidia() override { return _gpu.vendorId == VendorId::Nvidia; }
    uint32_t ArchId() override { return _gpu.nvidiaArchInfo.architecture; }

    bool ClaimOnce() override { return !g_admissionClaimed.exchange(true); }

    MfgProbe::AdmissionFacts Gather() override
    {
        MfgProbe::AdmissionFacts facts;

        if (HMODULE dlssg = KernelBaseProxy::GetModuleHandleW_()(L"sl.dlss_g.dll"); dlssg != nullptr)
        {
            facts.slDlssgLoaded = true;
            version_t v {};
            if (Util::GetFileVersion(ModulePath(dlssg).wstring(), &v))
                facts.slDlssgVersion = VersionText(v);
            else
                facts.slDlssgVersion = "loaded";
        }

        if (State::Instance().dlssgMfgMax.has_value())
            facts.dlssgMax = State::Instance().dlssgMfgMax.value();

        // The one capability query the note allows: the NGX core's verdict for feature 11, made the way
        // NVSDK_NGX_D3D12_GetFeatureRequirements makes it on its pass-through path -- the same gate (DLSS on,
        // a DLSS-capable adapter), InitNVNGX if the core is not loaded, then the core's entry with the game's
        // own discovery struct. It creates nothing; on a stock below-Ada card NVIDIA answers 0xbad00001 /
        // AdapterUnsupported (measured). Gather runs once per process (ClaimOnce), so InitNVNGX is tried at
        // most once, and every way the query can be skipped is named in the line.
        const bool canQuery = Config::Instance()->DLSSEnabled.value_or_default() && _gpu.dlssCapable;
        if (!canQuery)
        {
            facts.ngxSkipReason = MfgProbe::kSkipDlssUnavailable;
            return facts;
        }

        if (NVNGXProxy::D3D12_GetFeatureRequirements() == nullptr && NVNGXProxy::NVNGXModule() == nullptr)
            NVNGXProxy::InitNVNGX();

        const auto query = NVNGXProxy::D3D12_GetFeatureRequirements();
        if (query == nullptr || _info == nullptr)
        {
            facts.ngxSkipReason = NVNGXProxy::NVNGXModule() != nullptr ? MfgProbe::kSkipNoEntry : MfgProbe::kSkipNoCore;
            return facts;
        }

        NVSDK_NGX_FeatureRequirement out {};
        const auto result = query(_adapter, _info, &out);
        facts.ngxQueried = true;
        facts.ngxResult = static_cast<uint32_t>(result);
        facts.ngxFlags = static_cast<uint32_t>(out.FeatureSupported);
        facts.ngxMinArch = out.MinHWArchitecture;
        return facts;
    }

    void Emit(const std::string& line) override { LOG_INFO("{}", line); }

  private:
    IDXGIAdapter* _adapter;
    const NVSDK_NGX_FeatureDiscoveryInfo* _info;
    GpuInformation _gpu;
};

} // namespace

void MfgProbe::LogModule(void* providerModule)
{
    // Nothing escapes into the game's LoadLibrary: an exception from a format, an allocation or the
    // filesystem ends the probe, not the load.
    try
    {
        // The gate, memoised: on Ada, Blackwell and non-NVIDIA one atomic load once the GPU is known.
        if (!g_gate.Applies(ReadGpuIdentity))
            return;

        ModuleEnv env(reinterpret_cast<HMODULE>(providerModule));
        RunModuleProbe(env);
    }
    catch (...)
    {
    }
}

void MfgProbe::LogAdmission(IDXGIAdapter* adapter, const void* featureDiscoveryInfo)
{
    try
    {
        auto info = reinterpret_cast<const NVSDK_NGX_FeatureDiscoveryInfo*>(featureDiscoveryInfo);
        if (info == nullptr || info->FeatureID != NVSDK_NGX_Feature_FrameGeneration)
            return;

        if (!g_gate.Applies(ReadGpuIdentity))
            return;

        AdmissionEnv env(adapter, info);
        RunAdmissionProbe(env);
    }
    catch (...)
    {
    }
}
