// Host cases for DlssNr::PresentBoundary (retoque/dlssnr/DlssNr_PresentBoundary.h). See README.md.

#include <dlssnr/DlssNr_PresentBoundary.h>

#include <cstdio>

namespace
{
using namespace DlssNr::PresentBoundary;

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// A fresh session: the Auto latch is process-wide, so every case starts from it cleared.
void Fresh()
{
    g_gameDlssgSeen = false;
    g_lastDeclined = false;
}

Inputs Method(uint32_t method)
{
    Inputs in;
    in.hookMethod = method;
    return in;
}

Inputs WithGameDlssg(uint32_t method, int count = 1)
{
    Inputs in = Method(method);
    in.gameDlssgSet = true;
    in.gameDlssgCount = count;
    return in;
}
} // namespace

int main()
{
    std::printf("no frame generation: every route as before\n");
    {
        Fresh();
        check(Decide(Method(1), true) == PresentVerdict::Upscaled, "HookMethod=1 is the upscaled route");
        check(Decide(Method(2), false) == PresentVerdict::Run, "HookMethod=2 runs at present");
        check(Decide(Method(0), false) == PresentVerdict::Upscaled, "Auto before temporal inputs is upscaled");
        check(Decide(Method(0), true) == PresentVerdict::Run, "Auto with temporal inputs runs at present");
        check(UpscaledDefersToPresent(Method(0), true, true), "Auto defers to a live present entry");
        check(!UpscaledDefersToPresent(Method(0), false, true), "Auto does not defer without a live present entry");
        check(UpscaledCapturesForPresent(Method(0), true), "Auto captures for a live present entry");
        check(!g_gameDlssgSeen.load(), "nothing latched");
    }

    std::printf("the game's DLSS-G, HookMethod=2: declined on the entry below it\n");
    {
        Fresh();
        const Inputs in = WithGameDlssg(2);
        check(GameDlssgActive(in), "the game's DLSS-G is active");
        check(Decide(in, true) == PresentVerdict::Declined, "the wrapped entry declines");
        check(UpscaledDefersToPresent(in, true, true), "the upscaled route still defers: Present is what was asked");
        check(UpscaledCapturesForPresent(in, true), "and still captures, for when DLSS-G stops");

        Inputs countOnly = Method(2);
        countOnly.gameDlssgCount = 2;
        check(Decide(countOnly, true) == PresentVerdict::Declined, "a detected frame count alone is enough");

        Inputs modeOnly = Method(2);
        modeOnly.gameDlssgSet = true;
        check(Decide(modeOnly, true) == PresentVerdict::Declined, "a mode other than off alone is enough");
    }

    std::printf("the game's DLSS-G, Auto: upscaled for the session\n");
    {
        Fresh();
        const Inputs in = WithGameDlssg(0);
        check(Decide(in, true) == PresentVerdict::Upscaled, "upscaled even before the latch is noted");
        check(NoteGameDlssg(in), "noting it latches, and says so the first time");
        check(!NoteGameDlssg(in), "and only the first time");
        check(EffectiveMethod(in) == 1, "Auto behaves as HookMethod=1");
        check(!UpscaledDefersToPresent(in, true, true), "the upscaled route keeps the frame");
        check(!UpscaledCapturesForPresent(in, true), "and captures nothing for a present pass");

        // DLSS-G switched off in a menu: the route does not move back, so the model's history is not reset.
        const Inputs off = Method(0);
        check(Decide(off, true) == PresentVerdict::Upscaled, "after DLSS-G goes off, Auto stays upscaled");
        check(!UpscaledDefersToPresent(off, true, true), "and the upscaled route still keeps the frame");
    }

    std::printf("the game's DLSS-G switched off: HookMethod=2 runs again\n");
    {
        Fresh();
        check(Decide(WithGameDlssg(2), true) == PresentVerdict::Declined, "declined while it is on");
        check(Decide(Method(2), true) == PresentVerdict::Run, "runs once it is off (one present per frame again)");
    }

    std::printf("HookMethod=1 is untouched\n");
    {
        Fresh();
        const Inputs in = WithGameDlssg(1);
        check(Decide(in, true) == PresentVerdict::Upscaled, "upscaled with the game's DLSS-G");
        check(!UpscaledDefersToPresent(in, true, true), "never defers");
        check(!UpscaledCapturesForPresent(in, true), "never captures for a present pass");
        NoteGameDlssg(in);
        check(EffectiveMethod(in) == 1, "the latch changes nothing for it");
    }

    std::printf("OptiScaler's own frame generation is not the game's\n");
    {
        Fresh();
        // Its DLSS-G output sets the same two signals; its FG hook runs the pass before FG.
        Inputs hook = WithGameDlssg(2);
        hook.ownFgActive = true;
        hook.fgHook = true;
        check(!GameDlssgActive(hook), "own FG active: not the game's DLSS-G");
        check(Decide(hook, true) == PresentVerdict::Run, "the FG hook runs the pass");

        Inputs wrapped = WithGameDlssg(2);
        wrapped.ownFgActive = true;
        check(Decide(wrapped, true) == PresentVerdict::Run,
              "the wrapped entry is left to its own stand-down, not declined here");

        Inputs autoOwn = WithGameDlssg(0);
        autoOwn.ownFgActive = true;
        check(!NoteGameDlssg(autoOwn), "Auto does not latch on our own FG");
        check(UpscaledDefersToPresent(autoOwn, true, true), "Auto still defers to the FG hook's pass");

        // The fgHook flag alone, without our FG reported active, still runs: the hook only exists for our FG.
        Inputs hookOnly = WithGameDlssg(2);
        hookOnly.fgHook = true;
        check(Decide(hookOnly, true) == PresentVerdict::Run, "a call from the FG hook is never declined");
    }

    std::printf("the boundary (step 1) owns the pass above DLSS-G\n");
    {
        Fresh();
        Inputs present = WithGameDlssg(2);
        present.boundaryLive = true;
        check(Decide(present, true) == PresentVerdict::Run, "HookMethod=2 runs at the boundary");

        Inputs autoBoundary = WithGameDlssg(0);
        autoBoundary.boundaryLive = true;
        NoteGameDlssg(autoBoundary);
        check(EffectiveMethod(autoBoundary) == 0, "a latched Auto is Auto again at the boundary");
        check(Decide(autoBoundary, true) == PresentVerdict::Run, "and runs there");
    }

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }

    std::printf("PASS: a frame is edited before the game's DLSS-G or shown as drawn, never every other one\n");
    return 0;
}
