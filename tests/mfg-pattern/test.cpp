// Host test for the MFG (Multi Frame Generation) byte-pattern matcher. See README.md.
//
// It does not run the patcher. MfgUnlock's Patch* functions call VirtualProtect and walk a loaded
// PE image, both Windows-only. What is portable, and what actually decides whether a patch lands, is
// the signature match: so this compiles the real scanner matcher (FindPattern from
// OptiScaler/scanner/scanner.cpp) and the real MFG signature strings (the kXxxPattern constants from
// OptiScaler/framegen/dlssg/MfgUnlock.cpp) straight out of production, then drives them over
// synthetic byte buffers. run.py slices both in at the marker below, so the strings under test are
// byte-for-byte the ones the shipped DLL patches with, not a copy that can drift.
//
// The property it proves: every architecture signature keys on a cmp opcode against 0x1b0 -- 3D for
// `cmp eax, imm32`, 81 /7 for `cmp r/m32, imm32` -- so the same immediate (B0 01 00 00) reached by a
// `mov r32, 0x1b0` (opcodes B8-BF, or C7 /0) is never mistaken for the gate. A matcher that keyed on
// the immediate alone would patch a mov and corrupt the DLL.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ----- production slices (FindPattern + the MFG signature strings), injected by run.py -------------
// @@PRODUCTION_SLICES@@
// --------------------------------------------------------------------------------------------------

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// FindPattern reads dataStart .. dataStart + maxSize inclusive (its end is dataStart + maxSize + 1),
// so maxSize = size - 1 keeps every access inside the vector; ASan would catch anything past it.
bool matches(const std::vector<uint8_t>& buffer, std::string_view pattern)
{
    return FindPattern(reinterpret_cast<uintptr_t>(buffer.data()), buffer.size() - 1, pattern.data()) != 0;
}

// Wrap a sequence in nop padding, so a match has to be found in the middle of a buffer rather than at
// its head.
std::vector<uint8_t> padded(std::vector<uint8_t> core)
{
    std::vector<uint8_t> out(8, 0x90);
    out.insert(out.end(), core.begin(), core.end());
    out.insert(out.end(), 8, 0x90);
    return out;
}
} // namespace

int main()
{
    // Advertise / validate on nvngx_dlssg.dll compare against the Blackwell arch id 0x1b0 with a cmp;
    // the shape around the cmp is what makes each site unique. A correct encoding of that shape must
    // match, and the same immediate under a mov opcode must not.

    std::printf("kValidatePattern (cmp eax,0x1b0 / jl / cmp ebx,3 / jbe)\n");
    // 3D B0 01 00 00 (cmp eax,0x1b0) 7C 40 (jl) 83 FB 03 (cmp ebx,3) 76 (jbe)
    check(matches(padded({ 0x3D, 0xB0, 0x01, 0x00, 0x00, 0x7C, 0x40, 0x83, 0xFB, 0x03, 0x76 }), kValidatePattern),
          "matches cmp eax,0x1b0 in its branch/count shape");
    // Same shape, but B8 (mov eax,0x1b0) instead of 3D.
    check(!matches(padded({ 0xB8, 0xB0, 0x01, 0x00, 0x00, 0x7C, 0x40, 0x83, 0xFB, 0x03, 0x76 }), kValidatePattern),
          "rejects mov eax,0x1b0 in the same shape");
    // The immediate alone is not the site: no branch/count tail behind it.
    check(!matches(padded({ 0x3D, 0xB0, 0x01, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }), kValidatePattern),
          "rejects a bare cmp eax,0x1b0 with no branch/count tail");

    std::printf("kValidatePattern309 (cmp eax,0x1b0 / setae al)\n");
    check(matches(padded({ 0x3D, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x93, 0xC0 }), kValidatePattern309),
          "matches cmp eax,0x1b0 / setae al");
    check(!matches(padded({ 0xB8, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x93, 0xC0 }), kValidatePattern309),
          "rejects mov eax,0x1b0 / setae al");

    std::printf("kAdvertisePattern (mov ebx,1 / mov r8d,count / cmp r32,0x1b0 / cmovl)\n");
    // BB 01 00 00 00 | 41 B8 05 00 00 00 (mov r8d,5) | 81 FE B0 01 00 00 (cmp esi,0x1b0) | 44 0F 4C C3
    check(matches(padded({ 0xBB, 0x01, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, 0x81, 0xFE, 0xB0, 0x01,
                           0x00, 0x00, 0x44, 0x0F, 0x4C, 0xC3 }),
                  kAdvertisePattern),
          "matches cmp r/m32,0x1b0 (81 /7) in the advertise shape");
    // Arch compare replaced by a mov: C7 C6 B0 01 00 00 (mov esi,0x1b0) where the 81 must be.
    check(!matches(padded({ 0xBB, 0x01, 0x00, 0x00, 0x00, 0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC7, 0xC6, 0xB0, 0x01,
                            0x00, 0x00, 0x44, 0x0F, 0x4C, 0xC3 }),
                   kAdvertisePattern),
          "rejects mov r/m32,0x1b0 (C7 /0) where the cmp must be");

    std::printf("kAdvertisePattern309 (cmp ebp,0x1b0 / jl / mov edi,5)\n");
    // 81 FD B0 01 00 00 (cmp ebp,0x1b0) 0F 8C 11 22 33 44 (jl rel32) BF 05 00 00 00 (mov edi,5)
    check(matches(padded({ 0x81, 0xFD, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x8C, 0x11, 0x22, 0x33, 0x44, 0xBF, 0x05, 0x00,
                           0x00, 0x00 }),
                  kAdvertisePattern309),
          "matches cmp ebp,0x1b0 (81 /7) before the jl");
    // 81 FD replaced by C7 C5 (mov ebp,0x1b0).
    check(!matches(padded({ 0xC7, 0xC5, 0xB0, 0x01, 0x00, 0x00, 0x0F, 0x8C, 0x11, 0x22, 0x33, 0x44, 0xBF, 0x05, 0x00,
                            0x00, 0x00 }),
                   kAdvertisePattern309),
          "rejects mov ebp,0x1b0 (C7 /0) before the jl");

    std::printf("kWrapperClampPattern (mov r8d,3 / cmp ecx,r8d / cmovb)\n");
    // This one legitimately keys on a mov immediate (the wrapper's own ceiling of 3), not on 0x1b0,
    // so a matched mov here is correct -- the discriminator above is the cmp against the arch id, not
    // "never match a mov".
    check(matches(padded({ 0x41, 0xB8, 0x03, 0x00, 0x00, 0x00, 0x41, 0x3B, 0xC8, 0x44, 0x0F, 0x42, 0xC1 }),
                  kWrapperClampPattern),
          "matches the sl.dlss_g.dll min(published,3) clamp");

    std::printf("mov-of-0x1b0 sweep: no architecture signature may match any mov encoding\n");
    // Every mov-immediate encoding of 0x1b0 laid end to end: B8..BF <imm32>, then C7 C0..C7 <imm32>.
    std::vector<uint8_t> movs;
    for (uint8_t op = 0xB8; op <= 0xBF; ++op)
    {
        movs.insert(movs.end(), { op, 0xB0, 0x01, 0x00, 0x00 });
    }
    for (uint8_t modrm = 0xC0; modrm <= 0xC7; ++modrm)
    {
        movs.insert(movs.end(), { 0xC7, modrm, 0xB0, 0x01, 0x00, 0x00 });
    }
    const auto movBuffer = padded(movs);
    check(!matches(movBuffer, kValidatePattern), "kValidatePattern ignores every mov of 0x1b0");
    check(!matches(movBuffer, kValidatePattern309), "kValidatePattern309 ignores every mov of 0x1b0");
    check(!matches(movBuffer, kAdvertisePattern), "kAdvertisePattern ignores every mov of 0x1b0");
    check(!matches(movBuffer, kAdvertisePattern309), "kAdvertisePattern309 ignores every mov of 0x1b0");

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }

    std::printf("PASS: MFG signatures match the cmp-against-0x1b0 sites and reject mov of the same immediate\n");
    return 0;
}
