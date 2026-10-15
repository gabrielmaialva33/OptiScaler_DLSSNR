#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Multi Frame Generation below Ada: step 0, a log-only probe.
//
// docs/dlssnr/design/mfg-rtx20-30.md proposes letting RTX 20/30 owners run NVIDIA's own DLSS-G from the
// game's own provider. Step 0 writes nothing: no memory patch, no file, no ini/Config key. It only looks
// at the provider the game loaded and logs what it finds, so a 30-series tester (Rafael's RTX 3060) can
// send back the one thing nobody here can measure -- what the gates answer on hardware NVIDIA declines.
//
// Everything that decides, counts, or formats lives here, in std-only code, so the host tests can compile
// it without Windows headers. The Windows glue (reading the PE, the GPU, the driver, issuing the one
// NGX capability query) is in MfgProbe.cpp; the two entry points are declared at the bottom behind a
// forward-declared COM type so this header stays portable.
//
// The gate is the whole safety story: the probe runs only on an NVIDIA adapter whose architecture is
// known and below AD100 (0x190). On Ada, Blackwell, non-NVIDIA, and an NVIDIA adapter whose architecture
// was never read, it returns at the gate, before the census, the foreign-loader check, the formatting or
// any emit is reached. The run drivers below (RunModuleProbe, RunAdmissionProbe) take an injected
// environment precisely so a test can prove that inertness by counting that nothing past the gate was
// ever called; GateCache is the production entry points' memo of the same decision, tested the same way.
namespace MfgProbe
{

// TU100 and AD100, the NvAPI architecture ids (NV_GPU_ARCHITECTURE_TU100 / _AD100). The probe's scope is
// [Turing, Ada): Turing (0x160) and Ampere (0x170).
//
// The floor matters as much as the ceiling. IdentifyGpu leaves the architecture at 0 while the vendor is
// still NVIDIA whenever its NvAPI query cannot run (no nvapi64.dll in system32, fakenvapi there, a failed
// NvAPI_Initialize, a LUID that matches no physical GPU). "Below 0x190" alone would let that unknown 0
// through, on an Ada card too. Nothing below Turing runs NGX, so the floor costs nothing real.
constexpr uint32_t kArchTuring = 0x160;
constexpr uint32_t kArchAda = 0x190;

// The probe applies only to an NVIDIA adapter in [Turing, Ada). This is the single decision every entry
// point makes first.
inline bool ProbeApplies(bool isNvidia, uint32_t archId)
{
    return isNvidia && archId >= kArchTuring && archId < kArchAda;
}

// What the gate needs to know about the adapter. known is false while the GPU has not been identified
// yet (IdentifyGpu's cache was cold and its DXGI factory could not be made), which is not cached.
struct GpuIdentity
{
    bool known = false;
    bool isNvidia = false;
    uint32_t arch = 0;
};

// The production entry points' memo of the gate. The first call that sees an identified adapter decides
// once; every later call is one atomic load and a comparison, so on Ada a provider load or a feature-11
// query costs nothing past that. An unidentified adapter is answered "no" and not memoised, so a later
// call can still decide. read() is only called while undecided.
class GateCache
{
  public:
    template <typename ReadGpu> bool Applies(ReadGpu&& read)
    {
        const int state = _state.load(std::memory_order_acquire);
        if (state != kUndecided)
            return state == kApplies;

        const GpuIdentity id = read();
        if (!id.known)
            return false;

        const bool applies = ProbeApplies(id.isNvidia, id.arch);
        _state.store(applies ? kApplies : kInert, std::memory_order_release);
        return applies;
    }

  private:
    static constexpr int kUndecided = 0;
    static constexpr int kInert = 1;
    static constexpr int kApplies = 2;
    std::atomic<int> _state { kUndecided };
};

// ---- the fatbin / cubin census ------------------------------------------------------------------
//
// nvngx_dlssg.dll carries its CUDA kernels two ways: as fatbin containers (one per network kernel, plus
// one per frame-construction kernel) and as bare CUDA ELF cubins outside any container. What the step-0
// line reports is three numbers: how many containers there are, how many of the frame-construction
// containers an sm_86 device could load (zero in every build measured offline -- their only non-PTX
// images are sm_89 SASS, and their PTX is sm_89 or sm_120), and how many bare sm_86 cubins there are
// (39, the network kernels the provider's below-89 branch would pick). See mfg-rtx20-30.md, "What the
// providers ship".

// CUDA's own rule for whether an image loads on a given device:
//   - a PTX image (kind 1) JIT-compiles forward, so it loads on sm_86 iff its target is <= 86;
//   - a SASS/cubin image (kind 2) runs on the same major version at an equal or higher device minor,
//     so on sm_86 (major 8, minor 6) it loads iff its major is 8 and its minor is <= 6.
// arch is encoded as the two-or-three digit sm number: 86 -> major 8 minor 6, 120 -> major 12 minor 0.
inline bool LoadableOnSm86(uint16_t kind, uint32_t arch)
{
    if (kind == 1) // PTX
        return arch <= 86;
    if (kind == 2) // SASS / cubin
        return (arch / 10 == 8) && (arch % 10 <= 6);
    return false;
}

struct Census
{
    unsigned containers = 0;        // fatbin containers found
    unsigned frameContainers = 0;   // of those, the frame-construction ones (they carry a Blackwell PTX)
    unsigned frameLoadableSm86 = 0; // of the frame containers, how many hold an image an sm_86 device could load
    unsigned networkCubinsSm86 = 0; // bare CUDA ELF cubins (outside any container) built for sm_86
    unsigned networkCubinsSm89 = 0; // the same for sm_89 (not loadable on sm_86; kept for the record)
};

// Fatbin container layout (matches MfgUnlock.cpp's kernel walk): magic, 16-byte header, u64 payload size
// at +8; each image inside has kind at +0, header size at +4, payload size at +8, architecture at +28.
constexpr uint8_t kFatbinMagic[4] = { 0x50, 0xED, 0x55, 0xBA };
constexpr size_t kFatbinHeaderSize = 0x10;
constexpr size_t kImageKind = 0;
constexpr size_t kImageHeaderSize = 4;
constexpr size_t kImagePayloadSize = 8;
constexpr size_t kImageArch = 28;

// CUDA ELF (a bare cubin): \x7fELF, 64-bit; e_machine 190 (NVIDIA) at +0x12; the SM number is the low
// byte of e_flags at +0x30.
constexpr uint8_t kElfMagic[4] = { 0x7F, 0x45, 0x4C, 0x46 };
constexpr uint16_t kElfMachineNvidia = 190;
constexpr size_t kElfMachineOffset = 0x12;
constexpr size_t kElfFlagsOffset = 0x30;

namespace detail
{
inline uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t ReadU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t ReadU64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}
} // namespace detail

// Walks one contiguous buffer (in production, one non-executable PE section) and tallies the census over
// it. Containers are counted first and their byte ranges recorded, so a bare-cubin scan afterwards counts
// only the ELF images that sit outside every container -- the SASS inside a fatbin is an ELF too, and it
// must not be mistaken for a bare cubin. Every read is bounds-checked; a truncated or malformed structure
// ends that walk rather than reading past the buffer.
inline Census TakeCensus(const uint8_t* data, size_t size)
{
    Census census;

    if (data == nullptr || size < sizeof(kFatbinMagic))
        return census;

    struct Range
    {
        size_t begin;
        size_t end;
    };
    std::vector<Range> containerRanges;

    for (size_t i = 0; i + 16 <= size;)
    {
        if (std::memcmp(data + i, kFatbinMagic, sizeof(kFatbinMagic)) != 0)
        {
            ++i;
            continue;
        }

        const uint16_t headerSize = detail::ReadU16(data + i + 6);
        const uint64_t fatSize = detail::ReadU64(data + i + 8);

        // Not a container header we understand, or one that runs past the buffer: step over the magic and
        // keep looking rather than trusting the length. The bound is written as a subtraction (i + 16 <= size
        // holds here) so a length near 2^64 cannot wrap the sum and walk the cursor backwards forever.
        if (headerSize != kFatbinHeaderSize || fatSize == 0 || fatSize > size - i - 16)
        {
            ++i;
            continue;
        }

        ++census.containers;
        const size_t imagesBegin = i + 16;
        const size_t imagesEnd = imagesBegin + static_cast<size_t>(fatSize);
        containerRanges.push_back({ i, imagesEnd });

        bool hasBlackwellPtx = false;
        bool loadableOnSm86 = false;

        for (size_t image = imagesBegin; image + kImageArch + 4 <= imagesEnd;)
        {
            const uint16_t kind = detail::ReadU16(data + image + kImageKind);
            const uint32_t imageHeader = detail::ReadU32(data + image + kImageHeaderSize);
            const uint64_t payload = detail::ReadU64(data + image + kImagePayloadSize);
            const uint32_t arch = detail::ReadU32(data + image + kImageArch);

            if (imageHeader == 0 || payload == 0)
                break;

            if (kind == 1 && arch == 120)
                hasBlackwellPtx = true;
            if (LoadableOnSm86(kind, arch))
                loadableOnSm86 = true;

            // Same rule as the container: compare against the room left, never form a sum that can wrap.
            const size_t room = imagesEnd - image;
            if (imageHeader > room || payload > room - imageHeader)
                break;
            image += imageHeader + static_cast<size_t>(payload);
        }

        if (hasBlackwellPtx)
        {
            ++census.frameContainers;
            if (loadableOnSm86)
                ++census.frameLoadableSm86;
        }

        i = imagesEnd;
    }

    const auto insideAContainer = [&](size_t at)
    {
        for (const Range& r : containerRanges)
            if (at >= r.begin && at < r.end)
                return true;
        return false;
    };

    for (size_t i = 0; i + kElfFlagsOffset + 4 <= size; ++i)
    {
        if (std::memcmp(data + i, kElfMagic, sizeof(kElfMagic)) != 0)
            continue;
        if (insideAContainer(i))
            continue;
        if (detail::ReadU16(data + i + kElfMachineOffset) != kElfMachineNvidia)
            continue;

        const uint32_t sm = detail::ReadU32(data + i + kElfFlagsOffset) & 0xFF;
        if (sm == 86)
            ++census.networkCubinsSm86;
        else if (sm == 89)
            ++census.networkCubinsSm89;
    }

    return census;
}

// Sums a census taken section by section (the production glue feeds one section at a time).
inline void Accumulate(Census& total, const Census& part)
{
    total.containers += part.containers;
    total.frameContainers += part.frameContainers;
    total.frameLoadableSm86 += part.frameLoadableSm86;
    total.networkCubinsSm86 += part.networkCubinsSm86;
    total.networkCubinsSm89 += part.networkCubinsSm89;
}

// ---- foreign loader -----------------------------------------------------------------------------
//
// If another frame-generation loader is already in the process (dlssg_for_sm86 loads an "SM86 backend"
// module named sm86_backend.dll and reads a dlssg_sm86.ini beside the game exe), then the provider this
// probe is describing may be that project's bundled runtime, not the game's own. The note says to say so
// first, before the module line, so the reader is not misled.
struct ForeignLoader
{
    bool moduleLoaded = false; // sm86_backend.dll is loaded in the process
    bool iniPresent = false;   // dlssg_sm86.ini sits beside the game exe

    bool Any() const { return moduleLoaded || iniPresent; }
};

inline std::string ForeignLoaderLine(const ForeignLoader& f)
{
    std::string line = "MFG below Ada (probe): another frame-generation loader is present (";
    if (f.moduleLoaded)
        line += "module sm86_backend.dll loaded";
    if (f.moduleLoaded && f.iniPresent)
        line += ", ";
    if (f.iniPresent)
        line += "dlssg_sm86.ini beside the game";
    line += "); the provider line below may describe its runtime, not the game's own";
    return line;
}

// ---- line formatting ----------------------------------------------------------------------------

inline std::string Hex(uint64_t v)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(v));
    return buffer;
}

// A gate's state as the probe sees it, for the summary: the same three outcomes MfgUnlock distinguishes.
enum class GateState
{
    Found,   // the exact NVIDIA gate is there, unpatched
    Missing, // no gate signature matched
    Foreign  // a gate is there but compares against another constant (another unlocker was here first)
};

inline const char* GateStateText(GateState s)
{
    switch (s)
    {
    case GateState::Found:
        return "found";
    case GateState::Missing:
        return "missing";
    case GateState::Foreign:
        return "foreign";
    }
    return "?";
}

struct ModuleFacts
{
    std::string adapterName;
    uint32_t arch = 0;
    uint32_t impl = 0;
    std::string driver = "unknown"; // display-driver version, best-effort
    std::string hags = "unknown";   // "on" / "off" / "on(spoofed)" / "unknown"

    std::string providerFile;
    std::string providerVersion = "unknown";
    uint32_t timeDateStamp = 0;
    uint32_t sizeOfImage = 0;
    std::string sha256Prefix = "unknown"; // first 12 hex chars of the file's sha256
    std::string provenance = "game";      // "game" / "DriverStore" / "NGX models"
    std::string row = "none";             // qualification-table row; always none in step 0

    uint32_t minArchExport = 0; // the immediate of NVSDK_NGX_GetGPUArchitecture (mov eax, imm32)
    Census census;
    GateState advertise = GateState::Missing;
    GateState validate = GateState::Missing;
};

inline std::string FormatModuleLine(const ModuleFacts& f)
{
    std::string line = "MFG below Ada (probe): adapter \"";
    line += f.adapterName;
    line += "\" arch " + Hex(f.arch);
    line += " impl " + Hex(f.impl);
    line += " driver " + f.driver;
    line += " HAGS " + f.hags + "; provider ";
    line += f.providerFile + " " + f.providerVersion;
    line += " ts " + Hex(f.timeDateStamp);
    line += " image " + Hex(f.sizeOfImage);
    line += " sha256 " + f.sha256Prefix;
    line += " from " + f.provenance;
    line += " row=" + f.row + "; minArch export " + Hex(f.minArchExport) + "; kernels: ";
    line += std::to_string(f.census.containers) + " containers, frame ";
    line += std::to_string(f.census.frameLoadableSm86) + "/" + std::to_string(f.census.frameContainers);
    line += " loadable on sm_86, network cubins sm_86 " + std::to_string(f.census.networkCubinsSm86);
    line += "; gates advertise=" + std::string(GateStateText(f.advertise));
    line += " validate=" + std::string(GateStateText(f.validate));
    line += "; no writes";
    return line;
}

struct AdmissionFacts
{
    std::string slDlssgVersion = "not loaded"; // sl.dlss_g.dll file version, or "not loaded"
    bool slDlssgLoaded = false;

    bool ngxQueried = false; // the one capability query (NVNGXProxy::D3D12_GetFeatureRequirements) ran
    uint32_t ngxResult = 0;  // its NVSDK_NGX_Result (0xbad00001 on a stock below-Ada card)
    uint32_t ngxFlags = 0;   // OutSupported.FeatureSupported (0x4 = AdapterUnsupported)
    uint32_t ngxMinArch = 0; // OutSupported.MinHWArchitecture

    // Why the query did not run, when it did not. The line is emitted once either way, so the reason is
    // what keeps a missing verdict from being silent.
    std::string ngxSkipReason = "NGX core not loaded";

    int dlssgMax = -1; // slDLSSGGetState's numFramesToGenerateMax if OptiScaler already captured it; -1 unset
};

// The reasons the production glue gives for a query it did not make, in one place so the test can hold
// the text.
constexpr const char* kSkipDlssUnavailable = "DLSS off or adapter not DLSS-capable";
constexpr const char* kSkipNoEntry = "NGX core loaded without D3D12_GetFeatureRequirements";
constexpr const char* kSkipNoCore = "NGX core not loaded";

inline std::string FormatAdmissionLine(const AdmissionFacts& a)
{
    std::string line = "MFG below Ada (probe) admission: sl.dlss_g ";
    line += a.slDlssgVersion;
    line += std::string(" plugin ") + (a.slDlssgLoaded ? "loaded" : "absent") + "; NGX feature 11 ";
    if (a.ngxQueried)
    {
        line += Hex(a.ngxResult) + " flags " + Hex(a.ngxFlags) + " minArch " + Hex(a.ngxMinArch);
    }
    else
    {
        line += "not queried (" + a.ngxSkipReason + ")";
    }
    line += "; slDLSSGGetState max ";
    line += a.dlssgMax >= 0 ? std::to_string(a.dlssgMax) : std::string("unset");
    // slIsFeatureSupported's real per-adapter verdict and CreateFeature(11) are deliberately left out:
    // OptiScaler's own hooks answer the first as eOk to the game, and the second would have to create a
    // feature to observe. See mfg-rtx20-30.md, "Step 0 as built".
    line += "; slIsFeatureSupported/CreateFeature left out (see note); no writes";
    return line;
}

// ---- injected-environment run drivers -----------------------------------------------------------
//
// The gate is checked once, here, before anything else. The split into these calls is what lets a host
// test prove inertness: a fake environment counts each call, and off [Turing, Ada) -- Ada, Blackwell, a
// non-NVIDIA vendor, an unread architecture -- every counter past the gate must stay at zero. Everything
// stateful the production glue does (resolving and de-duplicating the module, the admission's once-only
// claim, bringing the NGX core up) is an environment call made after the gate, so the count covers it.

struct IModuleProbeEnv
{
    virtual ~IModuleProbeEnv() = default;
    virtual bool IsNvidia() = 0;
    virtual uint32_t ArchId() = 0;
    // Resolves the module to describe and claims it; false when there is none or it was already logged.
    virtual bool ClaimModule() = 0;
    virtual ForeignLoader DetectForeignLoader() = 0;
    virtual Census TakeModuleCensus() = 0;
    virtual ModuleFacts GatherFacts(const Census& census) = 0;
    virtual void Emit(const std::string& line) = 0;
};

inline void RunModuleProbe(IModuleProbeEnv& env)
{
    if (!ProbeApplies(env.IsNvidia(), env.ArchId()))
        return;

    if (!env.ClaimModule())
        return;

    const ForeignLoader foreign = env.DetectForeignLoader();
    if (foreign.Any())
        env.Emit(ForeignLoaderLine(foreign));

    const Census census = env.TakeModuleCensus();
    const ModuleFacts facts = env.GatherFacts(census);
    env.Emit(FormatModuleLine(facts));
}

struct IAdmissionProbeEnv
{
    virtual ~IAdmissionProbeEnv() = default;
    virtual bool IsNvidia() = 0;
    virtual uint32_t ArchId() = 0;
    // The admission line is emitted once per process; true only for the call that claims it.
    virtual bool ClaimOnce() = 0;
    // Reads what is there, brings the NGX core up at most once, and makes the one capability query.
    virtual AdmissionFacts Gather() = 0;
    virtual void Emit(const std::string& line) = 0;
};

inline void RunAdmissionProbe(IAdmissionProbeEnv& env)
{
    if (!ProbeApplies(env.IsNvidia(), env.ArchId()))
        return;

    if (!env.ClaimOnce())
        return;

    env.Emit(FormatAdmissionLine(env.Gather()));
}

} // namespace MfgProbe

// ---- Windows entry points -----------------------------------------------------------------------
//
// Declared with a void* module, a void* feature-discovery pointer and a forward-declared COM adapter so
// this header pulls in no Windows or NGX headers; the call sites already have those. Defined in
// MfgProbe.cpp.
struct IDXGIAdapter;

namespace MfgProbe
{
// Called at the top of MfgUnlock::TryApply, which both provider load sites in LibraryLoad_Hooks.cpp call
// with the module they just mapped -- so the probe sees the provider before any unlock write and before
// NGX initialises it. A null module means "the game's nvngx_dlssg.dll, if loaded", as TryApply reads it.
// Logs the module line (the foreign-loader line first, if one is present) once per module on an NVIDIA
// adapter in [Turing, Ada); a no-op otherwise.
void LogModule(void* providerModule);

// Called when the game asks about a feature's requirements (NVSDK_NGX_D3D12_GetFeatureRequirements in
// NVNGX_DLSS_Dx12.cpp). featureDiscoveryInfo is the game's own NVSDK_NGX_FeatureDiscoveryInfo*. Acts only
// for feature 11 (FrameGeneration), once, on an NVIDIA adapter in [Turing, Ada). It reuses the game's own
// discovery struct for the single capability query the note allows -- the call that function makes to
// NVNGXProxy::D3D12_GetFeatureRequirements on its pass-through path -- and logs the admission line.
void LogAdmission(IDXGIAdapter* adapter, const void* featureDiscoveryInfo);
} // namespace MfgProbe
