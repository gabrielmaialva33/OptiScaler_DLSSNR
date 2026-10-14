#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): what [LSFG] Multiplier does in a running process, given which
// frame-generation swapchain the process latched (tools/lsfg-study/MFG_PRESENTER.md 1.4 and 5). The router
// (LsfgPresenter.cpp) and the menu (LsfgMenu.cpp) both read it from here. Header-only and free of <windows.h>, so
// tests/lsfg-presenter holds it on the host tier.

#include "MfgPacing.h"

namespace LsfgMfg
{

// Which swapchain backs FSR-FG for the rest of the process. Decided when FSR-FG creates its first FG swapchain
// (Lsfg::Presenter::Wanted), because FGHooks detours that swapchain's methods once per process.
enum class Latch
{
    Undecided, // no FG swapchain yet: the first one reads [LSFG] Enabled and Multiplier as they are then
    Ffx,       // FFX's swapchain: one generated frame per real frame, whatever the key says
    Ours,      // our fork of it: the generated-frame count is read at every real frame
};

// [LSFG] Multiplier as the code reads it: 2, 3 or 4, anything else 2.
inline int MultiplierOrDefault(int configured) { return MultiplierValid(configured) ? configured : 2; }

// The generated frames our swapchain asks for at the next real frame. With LSFG off, FFX interpolates and writes one
// frame (Lsfg::TakesFrame), so one. Multiplier 2 is N = 1 through the fork: FFX's x2 pacing, the same callback, the
// same UI composition per frame (MFG_PRESENTER.md 3.2, 3.5).
inline uint32_t GeneratedFramesLive(bool lsfgEnabled, int configured)
{
    return lsfgEnabled ? GeneratedFramesFor(configured) : 1u;
}

// The multiplier presented while LSFG generates, for a configured one: FFX's swapchain always shows x2; ours shows what
// the key says from the next real frame. Undecided: the latch will pick the swapchain that shows it.
inline int ActiveMultiplier(Latch latch, int configured)
{
    return latch == Latch::Ffx ? 2 : MultiplierOrDefault(configured);
}

// Whether choosing `multiplier` now changes what is presented at the next real frame. False means it is saved and
// waits for a restart: 3 and 4 when FFX's swapchain was latched.
inline bool AppliesLive(Latch latch, int multiplier)
{
    return ActiveMultiplier(latch, multiplier) == MultiplierOrDefault(multiplier);
}

} // namespace LsfgMfg
