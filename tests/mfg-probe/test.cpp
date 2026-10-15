// Host test for the step-0 MFG-below-Ada probe. See README.md.
//
// MfgProbe.h is included from production unchanged. Everything that decides, counts or formats lives
// there in std-only code; this drives it over synthetic fatbin containers and fake environments, checks
// the exact text of both log lines, and -- when the path of a real nvngx_dlssg.dll is passed as argv[1]
// -- runs the census over it and holds it against the offline inventory in mfg-rtx20-30.md. No DLL is
// ever copied; the real-file case reads it in place and is skipped when it is absent.

#include "MfgProbe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

void checkEq(const std::string& got, const std::string& want, const char* what)
{
    const bool ok = got == want;
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
    {
        std::printf("       want: %s\n", want.c_str());
        std::printf("       got:  %s\n", got.c_str());
        ++g_failures;
    }
}

// ---- synthetic fatbin / cubin builders ----------------------------------------------------------

void putU16(std::vector<uint8_t>& v, size_t at, uint16_t x)
{
    v[at] = x & 0xFF;
    v[at + 1] = (x >> 8) & 0xFF;
}
void putU32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v[at + i] = (x >> (8 * i)) & 0xFF;
}
void putU64(std::vector<uint8_t>& v, size_t at, uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v[at + i] = (x >> (8 * i)) & 0xFF;
}

// One fatbin image: 32-byte header (kind at +0, header size at +4, payload size at +8, arch at +28) plus
// a payload of the given length filled with 0xAA.
std::vector<uint8_t> makeImage(uint16_t kind, uint32_t arch, uint32_t payloadLen)
{
    std::vector<uint8_t> img(32, 0);
    putU16(img, MfgProbe::kImageKind, kind);
    putU32(img, MfgProbe::kImageHeaderSize, 32);
    putU64(img, MfgProbe::kImagePayloadSize, payloadLen);
    putU32(img, MfgProbe::kImageArch, arch);
    img.insert(img.end(), payloadLen, 0xAA);
    return img;
}

// A fatbin container: 16-byte header (magic, header size 0x10 at +6, payload size at +8) wrapping the
// concatenated images.
std::vector<uint8_t> makeContainer(const std::vector<std::vector<uint8_t>>& images)
{
    std::vector<uint8_t> body;
    for (const auto& image : images)
        body.insert(body.end(), image.begin(), image.end());

    std::vector<uint8_t> out(16, 0);
    std::memcpy(out.data(), MfgProbe::kFatbinMagic, sizeof(MfgProbe::kFatbinMagic));
    putU16(out, 6, MfgProbe::kFatbinHeaderSize);
    putU64(out, 8, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// A bare CUDA ELF cubin: \x7fELF, e_machine 190 at +0x12, SM in the low byte of e_flags at +0x30.
std::vector<uint8_t> makeElf(uint32_t sm)
{
    std::vector<uint8_t> e(0x34, 0);
    std::memcpy(e.data(), MfgProbe::kElfMagic, sizeof(MfgProbe::kElfMagic));
    putU16(e, MfgProbe::kElfMachineOffset, MfgProbe::kElfMachineNvidia);
    putU32(e, MfgProbe::kElfFlagsOffset, sm);
    return e;
}

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}
void pad(std::vector<uint8_t>& dst, size_t n) { dst.insert(dst.end(), n, 0x90); }

// ---- fake environments to prove the gate is first ------------------------------------------------

struct CountingModuleEnv : MfgProbe::IModuleProbeEnv
{
    bool nvidia;
    uint32_t arch;

    bool claimResult = true; // what ClaimModule answers (false: no module, or already described)

    int claimCalls = 0;
    int foreignCalls = 0;
    int censusCalls = 0;
    int factsCalls = 0;
    int emitCalls = 0;
    std::vector<std::string> emitted;

    CountingModuleEnv(bool n, uint32_t a) : nvidia(n), arch(a) {}

    bool IsNvidia() override { return nvidia; }
    uint32_t ArchId() override { return arch; }
    bool ClaimModule() override
    {
        ++claimCalls;
        return claimResult;
    }
    MfgProbe::ForeignLoader DetectForeignLoader() override
    {
        ++foreignCalls;
        return {};
    }
    MfgProbe::Census TakeModuleCensus() override
    {
        ++censusCalls;
        return {};
    }
    MfgProbe::ModuleFacts GatherFacts(const MfgProbe::Census&) override
    {
        ++factsCalls;
        return {};
    }
    void Emit(const std::string& line) override
    {
        ++emitCalls;
        emitted.push_back(line);
    }
};

struct CountingAdmissionEnv : MfgProbe::IAdmissionProbeEnv
{
    bool nvidia;
    uint32_t arch;
    bool claimed = false; // the once-only claim, as the production atomic does it
    int claimCalls = 0;
    int gatherCalls = 0;
    int emitCalls = 0;

    CountingAdmissionEnv(bool n, uint32_t a) : nvidia(n), arch(a) {}

    bool IsNvidia() override { return nvidia; }
    uint32_t ArchId() override { return arch; }
    bool ClaimOnce() override
    {
        ++claimCalls;
        const bool first = !claimed;
        claimed = true;
        return first;
    }
    MfgProbe::AdmissionFacts Gather() override
    {
        ++gatherCalls;
        return {};
    }
    void Emit(const std::string&) override { ++emitCalls; }
};

// ---- tests --------------------------------------------------------------------------------------

struct GateCase
{
    bool nvidia;
    uint32_t arch;
    const char* what;
};

// Every adapter the probe must leave alone. NVIDIA with arch 0 is what IdentifyGpu reports when its NvAPI
// query could not run (no nvapi64.dll, fakenvapi, a failed init, an unmatched LUID) -- on an Ada card too,
// which is the case that let the first version of the gate through. AMD and Intel report arch 0 as well.
const GateCase kInert[] = {
    { true, MfgProbe::kArchAda, "Ada (0x190)" },
    { true, 0x1b0, "Blackwell (0x1b0)" },
    { true, 0x1c0, "an architecture above Blackwell (0x1c0)" },
    { true, 0, "NVIDIA with an unread architecture (0)" },
    { true, 0x150, "Volta (0x150), below the Turing floor" },
    { false, 0, "a non-NVIDIA vendor reporting arch 0" },
    { false, 0x170, "a non-NVIDIA vendor with an Ampere-like id" },
};

void testGate()
{
    std::printf("the gate exits before anything else off [Turing, Ada)\n");

    for (const GateCase& c : kInert)
    {
        CountingModuleEnv env(c.nvidia, c.arch);
        MfgProbe::RunModuleProbe(env);
        const bool inert = env.claimCalls == 0 && env.foreignCalls == 0 && env.censusCalls == 0 &&
                           env.factsCalls == 0 && env.emitCalls == 0;
        const std::string what =
            std::string("module probe inert for ") + c.what + " (no claim/foreign/census/facts/emit)";
        check(inert, what.c_str());

        CountingAdmissionEnv adm(c.nvidia, c.arch);
        MfgProbe::RunAdmissionProbe(adm);
        const std::string whatAdm = std::string("admission probe inert for ") + c.what + " (no claim/gather/emit)";
        check(adm.claimCalls == 0 && adm.gatherCalls == 0 && adm.emitCalls == 0, whatAdm.c_str());
    }

    // Ampere and Turing NVIDIA: the whole sequence runs, exactly once each.
    for (uint32_t arch : { 0x170u, 0x160u })
    {
        CountingModuleEnv env(true, arch);
        MfgProbe::RunModuleProbe(env);
        check(env.claimCalls == 1 && env.foreignCalls == 1 && env.censusCalls == 1 && env.factsCalls == 1 &&
                  env.emitCalls == 1,
              arch == 0x170 ? "module probe runs the whole sequence once on Ampere"
                            : "module probe runs the whole sequence once on Turing");
    }

    // A module already described (or none to describe) stops at the claim: nothing read, nothing logged.
    {
        CountingModuleEnv env(true, 0x170);
        env.claimResult = false;
        MfgProbe::RunModuleProbe(env);
        check(env.claimCalls == 1 && env.foreignCalls == 0 && env.censusCalls == 0 && env.factsCalls == 0 &&
                  env.emitCalls == 0,
              "a module that was already described is not read or logged again");
    }

    // The admission line is claimed once: the second query gathers nothing (so the NGX core is brought up
    // and queried at most once per process).
    {
        CountingAdmissionEnv env(true, 0x170);
        MfgProbe::RunAdmissionProbe(env);
        MfgProbe::RunAdmissionProbe(env);
        check(env.claimCalls == 2 && env.gatherCalls == 1 && env.emitCalls == 1,
              "admission probe gathers and emits once on Ampere, however often it is asked");
    }

    // The boundaries themselves.
    check(!MfgProbe::ProbeApplies(true, MfgProbe::kArchAda), "AD100 is excluded");
    check(MfgProbe::ProbeApplies(true, MfgProbe::kArchAda - 1), "one id below AD100 is included");
    check(MfgProbe::ProbeApplies(true, MfgProbe::kArchTuring), "TU100 (0x160) is included");
    check(!MfgProbe::ProbeApplies(true, MfgProbe::kArchTuring - 1), "one id below TU100 is excluded");
    check(!MfgProbe::ProbeApplies(true, 0), "an unread architecture (0) is excluded");
}

// The production entry points' memo of the gate (GateCache). read() stands in for IdentifyGpu.
void testGateCache()
{
    std::printf("the production gate memo\n");

    {
        MfgProbe::GateCache gate;
        int reads = 0;
        const auto ada = [&]
        {
            ++reads;
            return MfgProbe::GpuIdentity { true, true, MfgProbe::kArchAda };
        };
        bool any = false;
        for (int i = 0; i < 100; ++i)
            any = gate.Applies(ada) || any;
        check(!any && reads == 1, "on Ada the GPU is read once, then every call is answered from the memo");
    }

    {
        MfgProbe::GateCache gate;
        int reads = 0;
        const auto unread = [&]
        {
            ++reads;
            return MfgProbe::GpuIdentity { true, true, 0 };
        };
        const bool first = gate.Applies(unread);
        const bool second = gate.Applies(unread);
        check(!first && !second && reads == 1, "NVIDIA with an unread architecture is decided inert and memoised");
    }

    {
        MfgProbe::GateCache gate;
        int reads = 0;
        bool known = false;
        const auto later = [&]
        {
            ++reads;
            return MfgProbe::GpuIdentity { known, true, 0x170 };
        };
        const bool cold1 = gate.Applies(later);
        const bool cold2 = gate.Applies(later);
        known = true;
        const bool warm1 = gate.Applies(later);
        const bool warm2 = gate.Applies(later);
        check(!cold1 && !cold2 && warm1 && warm2 && reads == 3,
              "an unidentified GPU is answered no and not memoised; once identified as Ampere it is memoised");
    }

    {
        MfgProbe::GateCache gate;
        const auto amd = [] { return MfgProbe::GpuIdentity { true, false, 0 }; };
        check(!gate.Applies(amd), "a non-NVIDIA adapter is inert through the memo");
    }
}

void testLoadable()
{
    std::printf("CUDA loadability on sm_86\n");
    check(MfgProbe::LoadableOnSm86(1, 86), "PTX sm_86 loads on sm_86");
    check(MfgProbe::LoadableOnSm86(1, 75), "PTX sm_75 JITs forward onto sm_86");
    check(!MfgProbe::LoadableOnSm86(1, 89), "PTX sm_89 does not load on sm_86 (PTX is forward-only)");
    check(!MfgProbe::LoadableOnSm86(1, 120), "PTX sm_120 does not load on sm_86");
    check(MfgProbe::LoadableOnSm86(2, 86), "SASS sm_86 loads on sm_86");
    check(MfgProbe::LoadableOnSm86(2, 80), "SASS sm_80 loads on sm_86 (same major, lower minor)");
    check(!MfgProbe::LoadableOnSm86(2, 89), "SASS sm_89 does not load on sm_86 (higher minor)");
    check(!MfgProbe::LoadableOnSm86(2, 75), "SASS sm_75 does not load on sm_86 (different major)");
    check(!MfgProbe::LoadableOnSm86(2, 120), "SASS sm_120 does not load on sm_86");
}

void testCensus()
{
    std::printf("the census over synthetic containers and bare cubins\n");

    // A frame container with a Blackwell PTX and a loadable sm_86 SASS.
    {
        auto buf = makeContainer({ makeImage(1, 120, 8), makeImage(2, 86, 8) });
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 1 && c.frameContainers == 1 && c.frameLoadableSm86 == 1,
              "a frame container with a Blackwell PTX and an sm_86 SASS is frame and loadable");
    }

    // A frame container shaped like the real ones: Blackwell PTX + Ada PTX + Ada SASS, none loadable.
    {
        auto buf = makeContainer({ makeImage(1, 120, 8), makeImage(1, 89, 8), makeImage(2, 89, 8) });
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 1 && c.frameContainers == 1 && c.frameLoadableSm86 == 0,
              "a Blackwell+Ada frame container is frame but not loadable on sm_86");
    }

    // A PTX-only sm_89 container is a network container, not a frame one.
    {
        auto buf = makeContainer({ makeImage(1, 89, 8) });
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 1 && c.frameContainers == 0, "an sm_89 PTX-only container is network, not frame");
    }

    // Bare cubins outside any container.
    {
        std::vector<uint8_t> buf;
        pad(buf, 8);
        append(buf, makeElf(86));
        pad(buf, 8);
        append(buf, makeElf(89));
        pad(buf, 8);
        append(buf, makeElf(86));
        pad(buf, 8);
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.networkCubinsSm86 == 2 && c.networkCubinsSm89 == 1, "bare sm_86 and sm_89 cubins are counted by SM");
    }

    // An ELF image sitting inside a container's payload must not be mistaken for a bare cubin. The payload
    // bytes of this image are themselves a valid ELF header.
    {
        auto elfPayload = makeElf(86);
        auto image = makeImage(2, 89, static_cast<uint32_t>(elfPayload.size()));
        std::memcpy(image.data() + 32, elfPayload.data(), elfPayload.size());
        auto buf = makeContainer({ makeImage(1, 120, 8), image });
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.networkCubinsSm86 == 0, "an ELF inside a container is not counted as a bare cubin");
    }

    // A whole synthetic module: 2 network containers + 1 frame container + bare cubins, with padding.
    {
        std::vector<uint8_t> buf;
        pad(buf, 16);
        append(buf, makeContainer({ makeImage(1, 89, 8) }));
        pad(buf, 16);
        append(buf, makeContainer({ makeImage(1, 89, 8) }));
        pad(buf, 16);
        append(buf, makeContainer({ makeImage(1, 120, 8), makeImage(1, 89, 8), makeImage(2, 89, 8) }));
        pad(buf, 16);
        append(buf, makeElf(86));
        append(buf, makeElf(89));
        pad(buf, 16);
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 3 && c.frameContainers == 1 && c.frameLoadableSm86 == 0 && c.networkCubinsSm86 == 1 &&
                  c.networkCubinsSm89 == 1,
              "a mixed module tallies containers, frame kernels and bare cubins together");
    }

    // A container length near 2^64 must not wrap the bound and spin the cursor: before the bound was
    // written as a subtraction, UINT64_MAX-15 put the next container start back on this one forever.
    for (uint64_t huge : { UINT64_MAX, UINT64_MAX - 15, UINT64_MAX - 16 })
    {
        auto buf = makeContainer({ makeImage(1, 120, 8) });
        putU64(buf, 8, huge);
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 0, huge == UINT64_MAX        ? "a container length of 2^64-1 is skipped"
                                 : huge == UINT64_MAX - 15 ? "a container length of 2^64-16 is skipped"
                                                           : "a container length of 2^64-17 is skipped");
    }

    // The same for an image inside a valid container: a payload or header near the limit ends that walk.
    {
        auto bad = makeImage(1, 120, 8);
        putU64(bad, MfgProbe::kImagePayloadSize, UINT64_MAX);
        auto buf = makeContainer({ bad });
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 1 && c.frameContainers == 1, "an image payload of 2^64-1 ends the image walk safely");

        auto bad2 = makeImage(1, 120, 8);
        putU32(bad2, MfgProbe::kImageHeaderSize, 0xFFFFFFFFu);
        auto buf2 = makeContainer({ bad2 });
        auto c2 = MfgProbe::TakeCensus(buf2.data(), buf2.size());
        check(c2.containers == 1, "an image header size of 2^32-1 ends the image walk safely");
    }

    // A truncated container (payload runs past the buffer) is skipped, not read past.
    {
        auto buf = makeContainer({ makeImage(1, 120, 8) });
        buf.resize(20); // header says there is a body, but the buffer ends inside it
        auto c = MfgProbe::TakeCensus(buf.data(), buf.size());
        check(c.containers == 0, "a container whose payload runs past the buffer is skipped");
    }
}

void testModuleLine()
{
    std::printf("the exact module line\n");

    MfgProbe::ModuleFacts f;
    f.adapterName = "NVIDIA GeForce RTX 3060";
    f.arch = 0x170;
    f.impl = 0x21;
    f.driver = "616.92";
    f.hags = "on";
    f.providerFile = "nvngx_dlssg.dll";
    f.providerVersion = "310.9.1.0";
    f.timeDateStamp = 0x6a986031;
    f.sizeOfImage = 0x737000;
    f.sha256Prefix = "ff6e90eb78b8";
    f.provenance = "game";
    f.row = "none";
    f.minArchExport = 0x190;
    f.census.containers = 70;
    f.census.frameContainers = 31;
    f.census.frameLoadableSm86 = 0;
    f.census.networkCubinsSm86 = 39;
    f.advertise = MfgProbe::GateState::Found;
    f.validate = MfgProbe::GateState::Found;

    checkEq(MfgProbe::FormatModuleLine(f),
            "MFG below Ada (probe): adapter \"NVIDIA GeForce RTX 3060\" arch 0x170 impl 0x21 driver 616.92 HAGS on; "
            "provider nvngx_dlssg.dll 310.9.1.0 ts 0x6a986031 image 0x737000 sha256 ff6e90eb78b8 from game row=none; "
            "minArch export 0x190; kernels: 70 containers, frame 0/31 loadable on sm_86, network cubins sm_86 39; "
            "gates advertise=found validate=found; no writes",
            "module line matches the note's step-0 block");

    // A missing/foreign gate pair and a spoofed HAGS render the way the summary expects.
    f.hags = "on(spoofed)";
    f.advertise = MfgProbe::GateState::Foreign;
    f.validate = MfgProbe::GateState::Missing;
    const auto line = MfgProbe::FormatModuleLine(f);
    check(line.find("HAGS on(spoofed);") != std::string::npos, "spoofed HAGS is marked");
    check(line.find("gates advertise=foreign validate=missing;") != std::string::npos,
          "foreign and missing gates render");
}

void testAdmissionLine()
{
    std::printf("the exact admission line\n");

    MfgProbe::AdmissionFacts a;
    a.slDlssgVersion = "2.14.1.0";
    a.slDlssgLoaded = true;
    a.ngxQueried = true;
    a.ngxResult = 0xbad00001;
    a.ngxFlags = 0x4; // AdapterUnsupported
    a.ngxMinArch = 0x170;
    a.dlssgMax = -1;

    checkEq(MfgProbe::FormatAdmissionLine(a),
            "MFG below Ada (probe) admission: sl.dlss_g 2.14.1.0 plugin loaded; NGX feature 11 0xbad00001 flags 0x4 "
            "minArch 0x170; slDLSSGGetState max unset; slIsFeatureSupported/CreateFeature left out (see note); "
            "no writes",
            "admission line with a stock below-Ada refusal (0xbad00001, flags 0x4)");

    // Not-loaded wrapper, a captured max, and each reason the query can be skipped for.
    MfgProbe::AdmissionFacts b;
    b.dlssgMax = 3;
    b.ngxSkipReason = MfgProbe::kSkipNoCore;
    checkEq(MfgProbe::FormatAdmissionLine(b),
            "MFG below Ada (probe) admission: sl.dlss_g not loaded plugin absent; NGX feature 11 not queried "
            "(NGX core not loaded); slDLSSGGetState max 3; slIsFeatureSupported/CreateFeature left out (see note); "
            "no writes",
            "admission line when the NGX core could not be loaded");

    b.ngxSkipReason = MfgProbe::kSkipDlssUnavailable;
    check(MfgProbe::FormatAdmissionLine(b).find("not queried (DLSS off or adapter not DLSS-capable);") !=
              std::string::npos,
          "admission line names DLSS being off or unavailable");

    b.ngxSkipReason = MfgProbe::kSkipNoEntry;
    check(MfgProbe::FormatAdmissionLine(b).find(
              "not queried (NGX core loaded without D3D12_GetFeatureRequirements);") != std::string::npos,
          "admission line names a core without the entry");
}

void testForeignLoader()
{
    std::printf("foreign-loader detection\n");

    check(!MfgProbe::ForeignLoader {}.Any(), "no foreign loader by default");

    MfgProbe::ForeignLoader mod;
    mod.moduleLoaded = true;
    check(mod.Any(), "a loaded sm86_backend.dll is a foreign loader");
    checkEq(MfgProbe::ForeignLoaderLine(mod),
            "MFG below Ada (probe): another frame-generation loader is present (module sm86_backend.dll loaded); "
            "the provider line below may describe its runtime, not the game's own",
            "foreign line names the module");

    MfgProbe::ForeignLoader ini;
    ini.iniPresent = true;
    checkEq(MfgProbe::ForeignLoaderLine(ini),
            "MFG below Ada (probe): another frame-generation loader is present (dlssg_sm86.ini beside the game); "
            "the provider line below may describe its runtime, not the game's own",
            "foreign line names the ini");

    MfgProbe::ForeignLoader both;
    both.moduleLoaded = true;
    both.iniPresent = true;
    checkEq(MfgProbe::ForeignLoaderLine(both),
            "MFG below Ada (probe): another frame-generation loader is present (module sm86_backend.dll loaded, "
            "dlssg_sm86.ini beside the game); the provider line below may describe its runtime, not the game's own",
            "foreign line names both");
}

// ---- the real provider, when a path is handed in ------------------------------------------------

uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void testRealProvider(const char* path)
{
    std::printf("the real nvngx_dlssg.dll (%s)\n", path);

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::printf("  [skip] not present; the offline-inventory check needs a real provider\n");
        return;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 0x200 || bytes[0] != 'M' || bytes[1] != 'Z')
    {
        check(false, "the file is a PE image");
        return;
    }

    const uint8_t* base = bytes.data();
    const uint32_t e_lfanew = rd32(base + 0x3C);
    if (e_lfanew + 0x18 > bytes.size() || std::memcmp(base + e_lfanew, "PE\0\0", 4) != 0)
    {
        check(false, "the PE header is where it should be");
        return;
    }

    const uint16_t numSections = rd16(base + e_lfanew + 6);
    const uint16_t optSize = rd16(base + e_lfanew + 0x14);
    const uint8_t* section = base + e_lfanew + 4 + 20 + optSize;

    // Walk the non-executable sections off disk, feeding each one's raw bytes to the census, exactly as the
    // production glue feeds the mapped sections.
    MfgProbe::Census total;
    for (uint16_t i = 0; i < numSections; ++i)
    {
        const uint8_t* s = section + static_cast<size_t>(i) * 40;
        if (s + 40 > base + bytes.size())
            break;
        const uint32_t virtualSize = rd32(s + 8);
        const uint32_t rawSize = rd32(s + 16);
        const uint32_t rawPtr = rd32(s + 20);
        const uint32_t characteristics = rd32(s + 36);
        if (characteristics & 0x20000000) // IMAGE_SCN_MEM_EXECUTE
            continue;
        size_t len = rawSize < virtualSize ? rawSize : virtualSize;
        if (rawPtr >= bytes.size())
            continue;
        if (rawPtr + len > bytes.size())
            len = bytes.size() - rawPtr;
        MfgProbe::Accumulate(total, MfgProbe::TakeCensus(base + rawPtr, len));
    }

    std::printf("  census: %u containers, frame %u/%u loadable on sm_86, network cubins sm_86 %u (sm_89 %u)\n",
                total.containers, total.frameLoadableSm86, total.frameContainers, total.networkCubinsSm86,
                total.networkCubinsSm89);

    // mfg-rtx20-30.md's offline inventory for 310.9.1.0: 70 fatbin containers (39 network + 31 frame), the
    // 31 frame kernels loadable on nothing an sm_86 device can take, 39 bare sm_86 network cubins (and 39
    // sm_89). An older 310.x build has 32..38 frame containers; accept that range so the check is not
    // pinned to one build, but hold the invariants that matter: no frame kernel is loadable on sm_86, and
    // there are 39 sm_86 network cubins.
    check(total.frameLoadableSm86 == 0, "no frame kernel is loadable on sm_86");
    check(total.networkCubinsSm86 == 39, "39 network cubins are built for sm_86");
    check(total.networkCubinsSm89 == 39, "39 network cubins are built for sm_89");
    check(total.frameContainers >= 31 && total.frameContainers <= 38, "31..38 frame-construction containers");
    check(total.containers == total.frameContainers + 39, "every non-frame container is one of the 39 network kernels");
}

} // namespace

int main(int argc, char** argv)
{
    testGate();
    testGateCache();
    testLoadable();
    testCensus();
    testModuleLine();
    testAdmissionLine();
    testForeignLoader();

    if (argc > 1)
        testRealProvider(argv[1]);
    else
        std::printf("the real nvngx_dlssg.dll\n  [skip] no provider path passed\n");

    std::printf("%s\n", g_failures == 0 ? "all passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
