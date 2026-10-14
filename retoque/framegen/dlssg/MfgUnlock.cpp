#include "pch.h"

#include "MfgUnlock.h"
#include "MfgPatchPlan.h"

#include <Config.h>
#include <Util.h>
#include <scanner/scanner.h>
#include <misc/IdentifyGpu.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace
{
// mov ebx,1 / mov r8d,<count> / cmp <reg>,0x1b0 / cmovl r8d,ebx. Both the published count and the
// register holding the architecture vary between builds of nvngx_dlssg.dll, so both are wildcards:
// 309 carried mov r8d,3 / cmp edi, and the dlssg shipped with Crimson Desert carries mov r8d,5 /
// cmp esi. Neither byte identifies the site. What does is the shape around them - the architecture
// constant sitting between a count and a cmovl that falls back to ebx - and it is still one match in
// .text. Note the count is already 5 in that build, so on it the write below only lands the cmovl.
constexpr std::string_view kAdvertisePattern = "BB 01 00 00 00 41 B8 ? 00 00 00 81 ? B0 01 00 00 44 0F 4C C3";

// cmp eax,0x1b0 / jl / cmp ebx,3 / jbe. The only comparison against the architecture constant that
// is followed by a signed branch and a count test.
constexpr std::string_view kValidatePattern = "3D B0 01 00 00 7C ? 83 FB 03 76";

// sl.dlss_g.dll, where the count nvngx published is taken as min(published, 3):
//     mov   r8d, 0x3
//     cmp   ecx, r8d
//     cmovb r8d, ecx
// The wrapper carries its own ceiling, so raising nvngx alone gets three back.
//
// The ceiling is a wildcard so a build that already carries five is recognised rather than missed.
// This register allocation is Streamline 2.7.1's (Cyberpunk 2077) and no other build seen here.
constexpr std::string_view kWrapperClampPattern = "41 B8 ? 00 00 00 41 3B C8 44 0F 42 C1";

// The same clamp in every other wrapper seen: 2.7.3, 2.7.30, 2.8.0, 2.9.0, 2.9.1 and 2.14.1, each one
// match in .text across eleven copies from ten games (2026-10-14).
//     mov   edx, 0x3          five in 2.14, which already allows 6X and needs no write
//     cmp   ecx, edx
//     cmovb edx, ecx
// The r8d pattern alone missed all of them, so on 2.7.3 to 2.9 the unlock stopped at 4X; and the
// "wrapperClamp=MISS" of the Crimson Desert run was this shape on 2.14.1, where it cost nothing.
constexpr std::string_view kWrapperClampPatternEdx = "BA ? 00 00 00 3B CA 0F 42 D1";

// The four gate shapes with the architecture constant's low byte left open. Tried only after the
// exact pattern missed: a hit means the gate is there but compares against something other than
// 0x1b0, which is what another unlocker leaves behind (MFGAdaUnlock rewrites it to 0x190). Such a
// module is left alone rather than patched on top of a patch, and the log says whose it looks like.
constexpr std::string_view kAdvertiseForeignPattern = "BB 01 00 00 00 41 B8 ? 00 00 00 81 ? ? 01 00 00 44 0F 4C C3";
constexpr std::string_view kValidateForeignPattern = "3D ? 01 00 00 7C ? 83 FB 03 76";
constexpr std::string_view kAdvertiseForeignPattern309 = "81 FD ? 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00";
constexpr std::string_view kValidateForeignPattern309 = "3D ? 01 00 00 0F 93 C0";

// Five generated frames, the count both patched sites carry.
constexpr uint8_t kMaxGeneratedFrames = 5;

// 310.9 restructured both gates. The count is no longer an immediate next to the comparison: the
// Blackwell branch starts at five and reads a configured value, and anything below Blackwell is sent
// to a branch that publishes one.
//     cmp ebp, 0x1b0
//     jl  ada          <- neutralised, so every card takes the Blackwell branch
//     mov edi, 0x5
constexpr std::string_view kAdvertisePattern309 = "81 FD B0 01 00 00 0F 8C ? ? ? ? BF 05 00 00 00";

// The capability flag in the same build is a setae rather than a branch.
//     cmp   eax, 0x1b0
//     setae al
constexpr std::string_view kValidatePattern309 = "3D B0 01 00 00 0F 93 C0";

// scanner::GetAddress only walks sections marked executable. Fatbins are data, so they need their own
// search. Returns 0 unless exactly one non-executable section holds the sequence, once.
uintptr_t FindDataBytes(HMODULE module, const uint8_t* needle, size_t length)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    uintptr_t found = 0;
    size_t hits = 0;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];

        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        uint8_t* end = start + s.Misc.VirtualSize;

        for (uint8_t* p = std::search(start, end, needle, needle + length); p != end;
             p = std::search(p + 1, end, needle, needle + length))
        {
            found = reinterpret_cast<uintptr_t>(p);

            if (++hits > 1)
                return 0;
        }
    }

    return hits == 1 ? found : 0;
}

bool WriteBytes(uintptr_t address, const uint8_t* bytes, size_t count)
{
    DWORD oldProtect = 0;

    if (!VirtualProtect((LPVOID) address, count, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        LOG_WARN("VirtualProtect failed at {:X}", address);
        return false;
    }

    std::memcpy((void*) address, bytes, count);

    DWORD ignored = 0;
    VirtualProtect((LPVOID) address, count, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID) address, count);

    return true;
}

std::string Hex(const uint8_t* bytes, size_t count)
{
    std::string out;

    for (size_t i = 0; i < count; ++i)
        out += std::format("{}{:02X}", i == 0 ? "" : " ", bytes[i]);

    return out;
}

// What MfgPatch::Commit reads and writes through in the running process. The module is mapped
// readable, so a read is a copy; a write goes through WriteBytes for the protection change and the
// instruction-cache flush.
struct LiveOps
{
    bool Read(uintptr_t address, uint8_t* out, size_t count)
    {
        std::memcpy(out, (const void*) address, count);
        return true;
    }

    bool Write(uintptr_t address, const uint8_t* bytes, size_t count) { return WriteBytes(address, bytes, count); }
};

// What a gate search found. Foreign carries the architecture constant the gate compares against when
// it is not 0x1b0.
struct GateMatch
{
    enum Kind
    {
        Found,
        Missing,
        Foreign
    } kind = Missing;

    uint32_t foreignArch = 0;
};

GateMatch ForeignGate(uintptr_t constantAt)
{
    GateMatch match;
    match.kind = GateMatch::Foreign;
    std::memcpy(&match.foreignArch, (const void*) constantAt, sizeof(match.foreignArch));
    return match;
}

// Plans the rewrite of the count and the neutralisation of the architecture clamp, so
// MultiFrameCountMax is published as five.
GateMatch PlanAdvertise(HMODULE module, std::vector<MfgPatch::Site>& plan, LiveOps& ops)
{
    if (const auto at309 = scanner::GetAddress(module, kAdvertisePattern309); at309 != 0)
    {
        // The jl is a rel32, six bytes.
        const auto branchAt = at309 + 6;
        const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };

        LOG_INFO("MFG unlock: advertise (310.9) at {:X}, jl {} -> {}", at309,
                 Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)));

        return { MfgPatch::AddSite(plan, ops, branchAt, nop, sizeof(nop)) ? GateMatch::Found : GateMatch::Missing };
    }

    if (const auto address = scanner::GetAddress(module, kAdvertisePattern); address != 0)
    {
        // Offsets within the matched sequence: the r8d immediate, and the cmovl.
        const auto countAt = address + 7;
        const auto cmovAt = address + 17;

        const uint8_t count[] = { kMaxGeneratedFrames };
        const uint8_t nop[] = { 0x0F, 0x1F, 0x40, 0x00 };

        LOG_INFO("MFG unlock: advertise at {:X}, count {} -> {}, cmovl {} -> {}", address, *(const uint8_t*) countAt,
                 kMaxGeneratedFrames, Hex((const uint8_t*) cmovAt, sizeof(nop)), Hex(nop, sizeof(nop)));

        const bool planned = MfgPatch::AddSite(plan, ops, countAt, count, sizeof(count)) &&
                             MfgPatch::AddSite(plan, ops, cmovAt, nop, sizeof(nop));
        return { planned ? GateMatch::Found : GateMatch::Missing };
    }

    // The constant sits at offset 2 in the 310.9 shape and 13 in the older one.
    if (const auto foreign = scanner::GetAddress(module, kAdvertiseForeignPattern309); foreign != 0)
        return ForeignGate(foreign + 2);

    if (const auto foreign = scanner::GetAddress(module, kAdvertiseForeignPattern); foreign != 0)
        return ForeignGate(foreign + 13);

    LOG_WARN("MFG unlock: the advertise signature did not match");
    return {};
}

// Plans dropping the Ada branch and raising the accepted count, so a request for five is not rejected.
GateMatch PlanValidate(HMODULE module, std::vector<MfgPatch::Site>& plan, LiveOps& ops)
{
    if (const auto at309 = scanner::GetAddress(module, kValidatePattern309); at309 != 0)
    {
        // setae al -> mov al, 1, so the flag is set whatever the architecture reports.
        const auto setAt = at309 + 5;
        const uint8_t always[] = { 0xB0, 0x01, 0x90 };

        LOG_INFO("MFG unlock: validate (310.9) at {:X}, setae {} -> {}", at309,
                 Hex((const uint8_t*) setAt, sizeof(always)), Hex(always, sizeof(always)));

        return { MfgPatch::AddSite(plan, ops, setAt, always, sizeof(always)) ? GateMatch::Found : GateMatch::Missing };
    }

    if (const auto address = scanner::GetAddress(module, kValidatePattern); address != 0)
    {
        // Offsets within the matched sequence: the jl, and the immediate of the count test behind it.
        const auto branchAt = address + 5;
        const auto countAt = address + 9;

        const uint8_t nop[] = { 0x90, 0x90 };
        const uint8_t count[] = { kMaxGeneratedFrames };

        LOG_INFO("MFG unlock: validate at {:X}, jl {} -> {}, count {} -> {}", address,
                 Hex((const uint8_t*) branchAt, sizeof(nop)), Hex(nop, sizeof(nop)), *(const uint8_t*) countAt,
                 kMaxGeneratedFrames);

        const bool planned = MfgPatch::AddSite(plan, ops, branchAt, nop, sizeof(nop)) &&
                             MfgPatch::AddSite(plan, ops, countAt, count, sizeof(count));
        return { planned ? GateMatch::Found : GateMatch::Missing };
    }

    // cmp eax, imm32 in both shapes: the constant follows the 3D opcode.
    if (const auto foreign = scanner::GetAddress(module, kValidateForeignPattern309); foreign != 0)
        return ForeignGate(foreign + 1);

    if (const auto foreign = scanner::GetAddress(module, kValidateForeignPattern); foreign != 0)
        return ForeignGate(foreign + 1);

    LOG_WARN("MFG unlock: the validate signature did not match");
    return {};
}

// Gives Ada the Blackwell kernels the module already carries.
//
// nvngx_dlssg.dll ships two builds of the interpolation kernels. Kernel_EstimateIntermMvecsScatter
// reads three f32 fields of its parameter block on sm_120 and one on sm_89, so on Ada every generated
// frame is placed at the same point between the two real ones: the world does not advance between
// them while the interface, composited once per present, does. At 2X there is one frame and nothing
// to distinguish; above it that is the whole symptom.
//
// The sm_120 module uses no instruction Ada lacks. So per container: the Blackwell PTX image is
// relabelled sm_89, its .target directive is rewritten in place (".target sm_120" and
// ".target sm_89 " are both fourteen bytes, and the directive sits in the literal run at the head of
// the LZ4 stream), and the images that were sm_89 -- the Ada PTX and its SASS -- are relabelled to an
// architecture that does not exist so the driver cannot select them. The driver then JITs Blackwell's
// kernel when it asks for Ada's.
//
// Nothing is copied in and no payload changes length. A container without both images is left alone.
//
// Each container is one commit: its directive, its Blackwell label and every parked Ada label land
// together or not at all, so the driver never sees a container where sm_89 answers twice or not at all.
constexpr uint32_t kArchAda = 89;
constexpr uint32_t kArchBlackwell = 120;

// No such shader model. Parks an image where nothing will ask for it.
constexpr uint32_t kArchParked = 122;

// Offsets inside a fatbin image header: payload length, and the architecture the image answers for.
constexpr size_t kImagePayloadSize = 8;
constexpr size_t kImageArch = 28;

// Containers that took the swap, and containers whose commit failed and was undone.
struct KernelSwap
{
    unsigned committed = 0;
    unsigned failed = 0;
    bool broken = false;
};

KernelSwap PatchBlackwellKernels(HMODULE module, LiveOps& ops)
{
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    const uint8_t magic[] = { 0x50, 0xED, 0x55, 0xBA };
    KernelSwap swap;

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const auto& s = section[i];

        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
            continue;

        uint8_t* start = base + s.VirtualAddress;
        uint8_t* end = start + s.Misc.VirtualSize;

        for (uint8_t* c = std::search(start, end, magic, magic + sizeof(magic)); c < end;
             c = std::search(c + 1, end, magic, magic + sizeof(magic)))
        {
            if (c + 16 > end)
                break;

            const auto headerSize = *reinterpret_cast<const uint16_t*>(c + 6);
            const auto fatSize = *reinterpret_cast<const uint64_t*>(c + 8);

            if (headerSize != 0x10 || fatSize == 0 || c + 16 + fatSize > end)
                continue;

            uint8_t* blackwell = nullptr;
            size_t blackwellHeader = 0;
            size_t blackwellPayload = 0;
            std::vector<uint8_t*> ada;

            for (uint8_t* image = c + 16; image < c + 16 + fatSize;)
            {
                const auto kind = *reinterpret_cast<const uint16_t*>(image);
                const auto imageHeader = *reinterpret_cast<const uint32_t*>(image + 4);
                const auto payload = *reinterpret_cast<const uint64_t*>(image + kImagePayloadSize);
                const auto arch = *reinterpret_cast<const uint32_t*>(image + kImageArch);

                if (imageHeader == 0 || payload == 0)
                    break;

                // kind 1 is PTX, 2 is a cubin. Only the PTX can be retargeted; the cubin is parked.
                if (kind == 1 && arch == kArchBlackwell)
                {
                    blackwell = image;
                    blackwellHeader = imageHeader;
                    blackwellPayload = payload;
                }
                else if (arch == kArchAda)
                {
                    ada.push_back(image);
                }

                image += imageHeader + payload;
            }

            if (blackwell == nullptr || ada.empty())
                continue;

            const char from[] = ".target sm_120";
            const char to[] = ".target sm_89 ";
            static_assert(sizeof(from) == sizeof(to), "the directive rewrite must not change length");

            uint8_t* body = blackwell + blackwellHeader;
            uint8_t* bodyEnd = body + blackwellPayload;
            auto at = std::search(body, bodyEnd, from, from + sizeof(from) - 1);

            if (at == bodyEnd)
                continue;

            const uint32_t ada89 = kArchAda;
            const uint32_t parked = kArchParked;

            std::vector<MfgPatch::Site> plan;
            bool planned = MfgPatch::AddSite(plan, ops, reinterpret_cast<uintptr_t>(at),
                                             reinterpret_cast<const uint8_t*>(to), sizeof(to) - 1) &&
                           MfgPatch::AddSite(plan, ops, reinterpret_cast<uintptr_t>(blackwell + kImageArch),
                                             reinterpret_cast<const uint8_t*>(&ada89), sizeof(ada89));

            for (uint8_t* image : ada)
                planned = planned && MfgPatch::AddSite(plan, ops, reinterpret_cast<uintptr_t>(image + kImageArch),
                                                       reinterpret_cast<const uint8_t*>(&parked), sizeof(parked));

            if (!planned)
            {
                ++swap.failed;
                continue;
            }

            switch (MfgPatch::Commit(plan, ops))
            {
            case MfgPatch::Outcome::Committed:
            case MfgPatch::Outcome::AlreadyApplied:
                ++swap.committed;
                break;
            case MfgPatch::Outcome::RollbackFailed:
                swap.broken = true;
                [[fallthrough]];
            default:
                ++swap.failed;
                break;
            }
        }
    }

    LOG_INFO("MFG unlock: {} kernel containers answer Ada with the Blackwell image, {} failed to (the summary says "
             "whether they were put back)",
             swap.committed, swap.failed);

    return swap;
}

// Plans raising the wrapper's own ceiling to match, so the min() keeps what nvngx published. A
// ceiling that is already five or more is planned as a site with nothing to write.
bool PlanWrapperClamp(HMODULE module, std::vector<MfgPatch::Site>& plan, LiveOps& ops)
{
    // The ceiling's immediate sits at offset 2 behind the 41 B8 of mov r8d, and at offset 1 behind the
    // BA of mov edx.
    uintptr_t countAt = 0;
    const char* shape = "";

    if (const auto address = scanner::GetAddress(module, kWrapperClampPattern); address != 0)
    {
        countAt = address + 2;
        shape = "r8d";
    }
    else if (const auto address = scanner::GetAddress(module, kWrapperClampPatternEdx); address != 0)
    {
        countAt = address + 1;
        shape = "edx";
    }
    else
    {
        LOG_WARN("MFG unlock: the wrapper clamp signature did not match, sl.dlss_g.dll's ceiling left alone");
        return false;
    }

    const uint8_t current = *(const uint8_t*) countAt;
    const uint8_t count[] = { std::max(current, kMaxGeneratedFrames) };

    LOG_INFO("MFG unlock: wrapper clamp ({}) at {:X}, count {} -> {}", shape, countAt, current, count[0]);

    return MfgPatch::AddSite(plan, ops, countAt, count, sizeof(count));
}

// Plans removing the driver profile's cap on generated frames. Only seen in Streamline 2.14 so far; the
// wrappers before it (2.7 to 2.9 here) carry no such check, so its absence there is not a miss.
bool PlanSlDrsClamp(HMODULE mod, std::vector<MfgPatch::Site>& plan, LiveOps& ops)
{
    auto base = reinterpret_cast<uint8_t*>(mod);
    if (!base)
        return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const uint8_t sig[15] = {
        0x41, 0x8B, 0x17, 0x39, 0x10, 0x4C, 0x0F, 0x42, 0xC0, 0x41, 0x8B, 0x00, 0x41, 0x89, 0x07
    };
    const size_t sig_len = sizeof(sig);
    const size_t je_offset = 21; // distance from the guarded je to the anchor

    uint8_t* found = nullptr;
    size_t hits = 0;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        uint8_t* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < je_offset + sig_len)
            continue;
        for (size_t off = je_offset; off + sig_len <= size; ++off)
        {
            if (std::memcmp(start + off, sig, sig_len) != 0)
                continue;
            if (found == nullptr)
                found = start + off;
            ++hits;
        }
    }

    if (hits != 1 || found == nullptr)
    {
        LOG_INFO("MFG unlock: found {} DRS max-clamp anchors in sl.dlss_g.dll (expected 1); leaving DRS clamp alone",
                 hits);
        return false;
    }

    uint8_t* je = found - je_offset;
    // The guard must be a near `je rel32` (0F 84 <disp32>), 6 bytes long.
    if (je[0] != 0x0F || je[1] != 0x84)
    {
        LOG_WARN("MFG unlock: the DRS clamp guard is not the expected `je rel32`; leaving it alone");
        return false;
    }
    int32_t je_disp = 0;
    std::memcpy(&je_disp, je + 2, sizeof(je_disp));
    const int32_t target = static_cast<int32_t>(je - base) + 6 + je_disp;

    // Rewrite the 6-byte `je` as an unconditional near `jmp rel32` (5 bytes) + NOP.
    const int32_t jmp_disp = target - (static_cast<int32_t>(je - base) + 5);
    uint8_t replacement[6];
    replacement[0] = 0xE9;
    std::memcpy(replacement + 1, &jmp_disp, 4);
    replacement[5] = 0x90;

    LOG_INFO("MFG unlock: the DRS 'max generated frames' clamp in sl.dlss_g.dll, je -> jmp; the driver profile "
             "can no longer cap MFG to 2x");

    return MfgPatch::AddSite(plan, ops, reinterpret_cast<uintptr_t>(je), replacement, sizeof(replacement));
}

// Per-patch outcome for the one-line summary TryApply prints once per module. A miss is a signature
// that did not match, a fixed-count guard that refused, or a write that failed - every fail-closed
// path a Patch* function can take lands here, so a partially applied patcher cannot pass for a whole
// one. A skip is a patch that was not attempted on this build or was turned off in config, which is
// not a failure and must not read like one.
//
// Held is a gate that matched but was not written because its partner did not: the provider would
// otherwise advertise a count its validator refuses. Foreign is a gate that some other unlocker has
// already rewritten. RolledBack is a write or read-back that failed and was undone; Broken is one that
// could not be undone, the only outcome that leaves a module in an unknown state.
enum class PatchStatus
{
    Ok,
    Already,
    Miss,
    Held,
    Foreign,
    Stale,
    RolledBack,
    Broken,
    Skip
};

const char* Status(PatchStatus status)
{
    switch (status)
    {
    case PatchStatus::Ok:
        return "ok";
    case PatchStatus::Already:
        return "ok(already)";
    case PatchStatus::Miss:
        return "MISS";
    case PatchStatus::Held:
        return "HELD";
    case PatchStatus::Foreign:
        return "foreign";
    case PatchStatus::Stale:
        return "STALE";
    case PatchStatus::RolledBack:
        return "ROLLED-BACK";
    case PatchStatus::Broken:
        return "BROKEN";
    case PatchStatus::Skip:
        return "skip";
    }

    return "?";
}

PatchStatus FromOutcome(MfgPatch::Outcome outcome)
{
    switch (outcome)
    {
    case MfgPatch::Outcome::Committed:
        return PatchStatus::Ok;
    case MfgPatch::Outcome::AlreadyApplied:
        return PatchStatus::Already;
    case MfgPatch::Outcome::Stale:
        return PatchStatus::Stale;
    case MfgPatch::Outcome::RolledBack:
        return PatchStatus::RolledBack;
    case MfgPatch::Outcome::RollbackFailed:
        return PatchStatus::Broken;
    }

    return PatchStatus::Broken;
}

bool Applied(PatchStatus status) { return status == PatchStatus::Ok || status == PatchStatus::Already; }

// Skip and foreign are not faults of ours; everything else that is not applied is.
bool Faulted(PatchStatus status)
{
    return !Applied(status) && status != PatchStatus::Skip && status != PatchStatus::Foreign;
}

// "<file> <version>" for the summary lines, so a report says which build a miss belongs to.
std::string Describe(HMODULE module)
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(module, modulePath, MAX_PATH);

    std::string out = wstring_to_string(std::filesystem::path(modulePath).filename().wstring());

    if (version_t v; Util::GetFileVersion(modulePath, &v))
        out += std::format(" {}.{}.{}.{}", v.major, v.minor, v.patch, v.reserved);
    else
        out += " (no version)";

    return out;
}

version_t ModuleVersion(HMODULE module)
{
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(module, modulePath, MAX_PATH);

    version_t v;
    Util::GetFileVersion(modulePath, &v);
    return v;
}

} // namespace

void MfgUnlock::TryApply(HMODULE provider)
{
    if (!Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default())
        return;

    // Reached from the load hooks and from Streamline calls on whatever thread makes them.
    static std::mutex mutex;
    std::lock_guard lock(mutex);

    // The two modules arrive at different times and each is latched on its own, so whichever is
    // present first is patched then rather than waiting for the other.
    //
    // The snippet is latched per module rather than once. A game can map its own nvngx_dlssg.dll and
    // NGX the driver's OTA copy (ProgramData\NVIDIA\NGX\models\dlssg\versions\<n>\files\<hash>.bin),
    // and the one that loaded first is not necessarily the one NGX goes on to use. The OTA copy has no
    // recognisable name, so only the load hook can hand it over; asked without one, this can find only
    // the game's copy.
    static std::vector<HMODULE> snippetsDone;
    static bool wrapperDone = false;

    LiveOps ops;

    {
        HMODULE module = provider != nullptr ? provider : GetModuleHandleW(L"nvngx_dlssg.dll");

        if (module != nullptr && std::find(snippetsDone.begin(), snippetsDone.end(), module) == snippetsDone.end())
        {
            snippetsDone.push_back(module);

            wchar_t modulePath[MAX_PATH] = {};
            GetModuleFileNameW(module, modulePath, MAX_PATH);
            const std::string moduleName = Describe(module);
            LOG_INFO("MFG unlock: DLSS-G provider at {}", wstring_to_string(modulePath));

            // Both gates are planned before either is written, and written as one commit. A provider
            // whose advertise took and whose validate did not would publish five generated frames and
            // then refuse the game's request for them.
            std::vector<MfgPatch::Site> gates;
            const GateMatch advertiseMatch = PlanAdvertise(module, gates, ops);
            const GateMatch validateMatch = PlanValidate(module, gates, ops);

            const auto unmatched = [](const GateMatch& match)
            {
                return match.kind == GateMatch::Foreign ? PatchStatus::Foreign
                       : match.kind == GateMatch::Found ? PatchStatus::Held
                                                        : PatchStatus::Miss;
            };

            PatchStatus advertise = unmatched(advertiseMatch);
            PatchStatus validate = unmatched(validateMatch);

            if (advertiseMatch.kind == GateMatch::Found && validateMatch.kind == GateMatch::Found)
            {
                const auto outcome = MfgPatch::Commit(gates, ops);
                advertise = validate = FromOutcome(outcome);

                if (!Applied(advertise))
                    LOG_WARN("MFG unlock: the gate commit on {} was {}; the provider keeps NVIDIA's gates", moduleName,
                             MfgPatch::OutcomeName(outcome));
            }

            if (advertiseMatch.kind == GateMatch::Foreign || validateMatch.kind == GateMatch::Foreign)
            {
                const uint32_t arch =
                    advertiseMatch.kind == GateMatch::Foreign ? advertiseMatch.foreignArch : validateMatch.foreignArch;
                LOG_INFO("MFG unlock: {} compares against architecture 0x{:X} where NVIDIA's builds compare against "
                         "0x1B0: most likely another unlocker has been here first (MFGAdaUnlock writes 0x190). "
                         "Left alone, so the two do not patch over each other",
                         moduleName, arch);
            }

            // Default on where it applies: on Ada the unlock alone produces frames that do not advance
            // the picture, so the two belong together. dlssCapable is set from the same field, so an
            // architecture that never reported leaves this off.
            //
            // Ada only, not everything below Blackwell. The one-field read is Ada's kernel, and Ada is
            // where the swap was measured. Below Ada NVIDIA's runtime refuses frame generation outright
            // (docs/dlssnr/design/model-cost-across-architectures.md), so there was never anything to patch
            // -- except under a third-party loader such as dlssg_for_sm86, which makes it run on
            // Ampere. What that loader does to these same kernel images is unknown, and relabelling
            // them underneath it was never tried; AdaBlackwellKernels=true still forces it.
            const auto& gpu = IdentifyGpu::getPrimaryGpu();
            const bool ada =
                gpu.vendorId == VendorId::Nvidia && gpu.nvidiaArchInfo.architecture_id == NV_GPU_ARCHITECTURE_AD100;

            // Only on top of our own gates, unless forced. Without them there is no MFG to fix, and a
            // module another unlocker owns may have had its kernels reworked by it as well.
            const auto& kernelsOption = Config::Instance()->FGDLSSGAdaBlackwellKernels;
            const bool forced = kernelsOption.has_value() && kernelsOption.value();
            const bool ourGates = Applied(advertise) && Applied(validate);

            // Skip is not a miss: a build or a card that does not want the kernel swap says so here,
            // and the summary must not read it as a half-applied patch.
            PatchStatus blackwellKernels = PatchStatus::Skip;
            bool kernelsBroken = false;
            if (kernelsOption.value_or(ada) && (ourGates || forced))
            {
                const KernelSwap swap = PatchBlackwellKernels(module, ops);
                kernelsBroken = swap.broken;
                blackwellKernels = swap.broken          ? PatchStatus::Broken
                                   : swap.failed != 0   ? PatchStatus::RolledBack
                                   : swap.committed > 0 ? PatchStatus::Ok
                                                        : PatchStatus::Miss;
            }

            // One line so a half-applied module cannot hide behind a single buried warning in a
            // 180k-line log: WARN the moment any patch missed, INFO only when the module is whole.
            const auto line = std::format("MFG unlock summary ({}): advertise={} validate={} blackwellKernels={}",
                                          moduleName, Status(advertise), Status(validate), Status(blackwellKernels));

            if (advertise == PatchStatus::Broken || kernelsBroken)
                LOG_ERROR("{}", line);
            else if (Faulted(advertise) || Faulted(validate) || Faulted(blackwellKernels))
                LOG_WARN("{}", line);
            else
                LOG_INFO("{}", line);
        }
    }

    if (!wrapperDone)
    {
        if (auto module = GetModuleHandleW(L"sl.dlss_g.dll"); module != nullptr)
        {
            wrapperDone = true;

            // The two wrapper patches are independent ceilings, so each is its own commit: one missing
            // does not make the other wrong.
            std::vector<MfgPatch::Site> clampPlan;
            const PatchStatus wrapperClamp = PlanWrapperClamp(module, clampPlan, ops)
                                                 ? FromOutcome(MfgPatch::Commit(clampPlan, ops))
                                                 : PatchStatus::Miss;

            std::vector<MfgPatch::Site> drsPlan;
            PatchStatus drsClamp = PatchStatus::Skip;
            if (PlanSlDrsClamp(module, drsPlan, ops))
                drsClamp = FromOutcome(MfgPatch::Commit(drsPlan, ops));
            else if (ModuleVersion(module) >= version_t(2, 14, 0, 0))
                drsClamp = PatchStatus::Miss;

            // Same one-line summary for the wrapper. This is the patch that half-applied in the
            // Crimson Desert run - wrapperClamp missed while everything else took - and the miss was
            // one line lost in the log; here it is the level of the whole summary.
            const auto line = std::format("MFG unlock summary ({}): wrapperClamp={} drsClamp={}", Describe(module),
                                          Status(wrapperClamp), Status(drsClamp));

            if (wrapperClamp == PatchStatus::Broken || drsClamp == PatchStatus::Broken)
                LOG_ERROR("{}", line);
            else if (Faulted(wrapperClamp) || Faulted(drsClamp))
                LOG_WARN("{}", line);
            else
                LOG_INFO("{}", line);
        }
    }
}
