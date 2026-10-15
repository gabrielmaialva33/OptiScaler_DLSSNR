#pragma once

#include <windows.h>

// Multi Frame Generation on Ada.
//
// nvngx_dlssg.dll gates MFG on the architecture id reported by the driver: 0x1b0 is Blackwell, Ada
// is below it. Two sites decide what a card is allowed to do, and both compare against that constant.
//
//   Advertise, the function that publishes DLSSG.MultiFrameCountMax:
//       mov   ebx, 0x1
//       mov   r8d, 0x3            the Blackwell count
//       cmp   edi, 0x1b0
//       cmovl r8d, ebx            below Blackwell the count becomes 1
//
//   Validate, the function that accepts or rejects a requested count:
//       cmp   eax, 0x1b0
//       jl    ada                 Ada takes this branch and accepts only 1
//       cmp   ebx, 0x3
//       jbe   accept
//
// Patched: the count immediates become 5, the cmovl becomes a nop, and the jl becomes two nops. The
// result is a maximum of five generated frames -- 6X -- on any architecture.
//
// The two sites are one decision, so they are written as one: both are found first, then written
// together through MfgPatch::Commit (MfgPatchPlan.h), read back, and put back as they were if any write
// or read-back fails. A provider that advertises five and validates one is never left behind. A gate
// that compares against another constant than 0x1b0 has been rewritten by another unlocker; the module
// is then left alone and the log says so, rather than patched on top of someone else's patch.
//
// Memory only. The file on disk carries an Authenticode signature and is left alone.
//
// Not covered: nvngx_dlssg.dll's frame pacing assumes Blackwell's flip metering hardware, which Ada
// does not have, so spacing above 2X is uneven. The published mods correct it by rewriting a blend
// weight inside the model's PTX; that is not done here.
namespace MfgUnlock
{
// Applies the patches once per module. Silent and harmless when the config option is off, when no
// DLSS-G provider is loaded, or when either signature does not match. Once a module has been handled
// it logs a single "MFG unlock summary (<module> <version>): ..." line naming every patch and whether
// it applied (ok, or ok(already) when there was nothing to write), missed (MISS), was held back
// because its partner gate missed (HELD), belongs to another unlocker (foreign), was undone after a
// failed write (ROLLED-BACK, STALE), could not be undone (BROKEN, an ERROR) or was not attempted
// (skip), so a half-applied module is one WARN rather than a lone warning buried in the log.
//
// provider: the DLSS-G snippet the load hook just mapped. Without one, the game's nvngx_dlssg.dll is
// looked up by name -- which cannot find the driver's own copy, a <hash>.bin under
// ProgramData\NVIDIA\NGX\models\dlssg\, so the load hook hands that one over itself.
void TryApply(HMODULE provider = nullptr);

// Read-only gate census for the step-0 MFG probe (MfgProbe). Runs the same advertise/validate signature
// search TryApply does, but through the plan-only path that records bytes and never writes, so it reports
// whether each gate is present, absent, or already rewritten by another unlocker (Foreign) without
// touching the module. This keeps the probe from carrying a second copy of the signatures.
struct GateReport
{
    enum State
    {
        Found,
        Missing,
        Foreign
    };

    State advertise = Missing;
    State validate = Missing;
};

GateReport ProbeGates(HMODULE provider);
} // namespace MfgUnlock
