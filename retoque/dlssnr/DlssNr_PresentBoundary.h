#pragma once

#include <atomic>
#include <cstdint>

// The present pass and the game's own DLSS-G (docs/dlssnr/design/nr-before-dlssg.md).
//
// With the game's DLSS-G on, the only present entry the pass has on a D3D12 title is the wrapped swapchain, and that
// swapchain sits below DLSS-G: the interposer presents the real frame and every generated one to it, after DLSS-G has
// already taken the backbuffer as interpolation input. An edit there lands on one present of each cycle, so at 2x
// every other frame reaches the screen unedited, which reads as flicker (nr-present-hook.md, 2026-10-04).
//
// Until the pass runs at the game's own Present, above DLSS-G (the note's option A, the "boundary"), a frame is either
// edited before DLSS-G or shown as the game drew it, never every other one:
//   - HookMethod=2 declines on the wrapped entry while the game's DLSS-G is active, and says why;
//   - Auto stays on the upscaled route, where the pass edits the upscaler's output before DLSS-G interpolates it.
//     Once the game's DLSS-G has been seen, Auto keeps that route for the session, so a game that switches DLSS-G
//     off in its menus does not move the pass between routes, and reset its history, every time.
//   - HookMethod=1 is untouched.
// OptiScaler's own frame generation is not the game's: its FG present hook runs the pass before FG, as it always has.
//
// No Windows types: RunPresentPass and the upscaled route fill Inputs, and tests/nr-present-boundary drives the rest.
namespace DlssNr::PresentBoundary
{
struct Inputs
{
    uint32_t hookMethod = 1;   // [DlssNr] HookMethod: 0 auto, 1 upscaled, 2 present
    bool fgHook = false;       // this present came through OptiScaler's own FG present hook
    bool ownFgActive = false;  // OptiScaler's own frame generation output is active and not paused
    bool gameDlssgSet = false; // the mode last passed to Streamline's DLSS-G is not eOff
    int gameDlssgCount = 0;    // DLSSG.MultiFrameCount at DLSS-G's last NGX evaluate (0 once it stops)
    bool boundaryLive = false; // the pass runs at the game's own Present, above DLSS-G (step 1; never yet)
};

// The game's DLSS-G is making frames from what this present shows. Both signals are also set when OptiScaler's own
// DLSS-G output runs, which is why its frame generation being active rules this out.
inline bool GameDlssgActive(const Inputs& in) { return !in.ownFgActive && (in.gameDlssgSet || in.gameDlssgCount > 0); }

// Once true, stays true for the process. Only Auto reads it.
inline std::atomic<bool> g_gameDlssgSeen { false };

// Records the game's DLSS-G for Auto. Returns true the first time, so the caller can log the change once.
inline bool NoteGameDlssg(const Inputs& in) { return GameDlssgActive(in) && !g_gameDlssgSeen.exchange(true); }

// The placement Auto and the explicit values come down to here. Auto that has seen the game's DLSS-G behaves as 1
// unless the boundary owns the pass, where 2 is correct again.
inline uint32_t EffectiveMethod(const Inputs& in)
{
    if (in.hookMethod == 0 && g_gameDlssgSeen.load() && !in.boundaryLive)
        return 1;

    return in.hookMethod;
}

// Whether the upscaled route hands the frame to a present pass (it captures the temporal inputs and returns), given
// that a present entry is live and, for Auto, has already been handed temporal inputs.
inline bool UpscaledDefersToPresent(const Inputs& in, bool presentHookLive, bool temporalValid)
{
    const uint32_t method = EffectiveMethod(in);
    return method == 2 || (method == 0 && presentHookLive && temporalValid);
}

// Whether the upscaled route captures the temporal inputs a present pass would use.
inline bool UpscaledCapturesForPresent(const Inputs& in, bool presentHookLive)
{
    const uint32_t method = EffectiveMethod(in);
    return method == 2 || (method == 0 && presentHookLive);
}

enum class PresentVerdict
{
    Upscaled, // the upscaled route owns the pass; the present entry only reports its status
    Run,      // this present entry runs the pass
    Declined, // HookMethod=2 under the game's DLSS-G on an entry below it: the frame is shown as drawn
};

inline PresentVerdict Decide(const Inputs& in, bool temporalValid)
{
    const uint32_t method = EffectiveMethod(in);

    if (method != 2 && !(method == 0 && temporalValid))
        return PresentVerdict::Upscaled;

    // Auto under the game's DLSS-G is upscaled even before NoteGameDlssg has latched it.
    if (method == 0 && !in.fgHook && !in.boundaryLive && GameDlssgActive(in))
        return PresentVerdict::Upscaled;

    if (!in.fgHook && !in.boundaryLive && GameDlssgActive(in))
        return PresentVerdict::Declined;

    return PresentVerdict::Run;
}

inline constexpr const char* kDeclinedReason =
    "the game's DLSS-G interpolates before this pass; HookMethod=1 edits the frames it interpolates";

// What the last present entry decided, for the status text.
inline std::atomic<bool> g_lastDeclined { false };
} // namespace DlssNr::PresentBoundary
