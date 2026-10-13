#pragma once

#include <atomic>
#include <optional>

namespace DlssNr
{
// What [DlssNr] PresentSync=auto means for the D3D12 present pass.
//
// The wait exists for frame generation: an interposer that paces the client queue can flip the buffer before the
// pass's write-back retires, and the next pass then re-reads its own output (scottmudge afad5195, nr-present-hook.md).
// Without one, the write-back and the flip are on the same queue, whose order already puts the finished picture on
// screen, and a CPU wait only holds the present thread until the GPU drains. Measured in PCSX2 (present pass, frame
// generation off, emulation unlimited, 2026-10-13, one run each): the pass ran on 106 frames a second with the wait
// and 127 without. So auto keeps the wait whenever frame generation could be involved, and only then.
struct PresentSyncInputs
{
    bool fgHook = false;      // the pass was called from a frame generation present hook
    bool fgOutput = false;    // OptiScaler's own frame generation output is selected
    bool dlssgSeen = false;   // the game set DLSS-G options or a DLSS-G frame count was detected
    bool dlssgLoaded = false; // Streamline's DLSS-G plugin or nvngx_dlssg.dll is loaded in the process
};

inline bool PresentSyncWanted(std::optional<bool> configured, const PresentSyncInputs& in)
{
    if (configured.has_value())
        return *configured;

    return in.fgHook || in.fgOutput || in.dlssgSeen || in.dlssgLoaded;
}

// What the last present pass decided, for the menu to show what auto is doing.
inline std::atomic<bool> g_presentSyncWaits { true };
} // namespace DlssNr
