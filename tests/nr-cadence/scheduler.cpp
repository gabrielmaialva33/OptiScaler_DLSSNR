// Model cadence's scheduler (retoque/dlssnr/DlssNr_Cadence.h), the production header, driven the way
// DlssNr_Dx12::Dispatch drives it: Decide once per call, then Recorded, Carried or Drop for what happened.
#include <dlssnr/DlssNr_Cadence.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace DlssNr::Cadence;

namespace
{
int g_cases = 0;

Inputs Running(uint32_t cadence)
{
    Inputs in {};
    in.requested = Requested(cadence);
    return in;
}

Signature Shape()
{
    Signature s {};
    s.width = 3440;
    s.height = 1440;
    s.workWidth = 2580;
    s.workHeight = 1080;
    s.format = 10;
    s.route = Route(false, false, false);
    s.settings = Mix(kMixSeed, 1u);
    s.feature = 1;
    return s;
}

// One frame as Dispatch runs it: decide, then the model records its edit (when it ran and succeeded) or the
// carry succeeds. modelOk=false is a model frame that failed or completed only part of its chain.
struct Driver
{
    Scheduler scheduler;
    uint64_t present = 0;
    int64_t ms = 0;
    bool resetPending = false; // g_nr.reset: cleared by a frame the model ran on, never by a carried one
    std::string pattern;       // M model, c carried

    Decision Frame(const Inputs& in, const Signature& s, bool modelOk = true, uint64_t presentStep = 1,
                   int64_t msStep = 16)
    {
        present += presentStep;
        ms += msStep;
        const Decision d = scheduler.Decide(in, s, present, ms, resetPending);
        if (d.runModel)
        {
            pattern += 'M';
            resetPending = false;
            if (modelOk && d.record)
                scheduler.Recorded();
            else
                scheduler.Drop(false, modelOk);
        }
        else
        {
            pattern += 'c';
            scheduler.Carried();
        }
        return d;
    }
};

void Requested_ClampsToOffOutsideTwoToFour()
{
    assert(Requested(0) == 1 && Requested(1) == 1 && Requested(2) == 2 && Requested(3) == 3 && Requested(4) == 4);
    assert(Requested(5) == 1 && Requested(9) == 1 && Requested(0xFFFFFFFFu) == 1);
    ++g_cases;
}

void Off_RunsEveryFrameAndKeepsNoSurfaces()
{
    Driver d;
    const auto in = Running(1);
    for (int i = 0; i < 8; ++i)
    {
        const auto decision = d.Frame(in, Shape());
        assert(decision.runModel && decision.reason == Reason::Off && decision.cadence == 1 && !decision.handChain &&
               !decision.record);
    }
    assert(d.pattern == "MMMMMMMM" && !d.scheduler.HasEdit());
    // What Dispatch asks before building anything: with the cadence off, nothing is allocated or dispatched,
    // and anything built before is given back.
    assert(!BuildsSurfaces(Refusal(in)) && ReleasesSurfaces(Refusal(in)));
    assert(BuildsSurfaces(Refusal(Running(2))) && !ReleasesSurfaces(Refusal(Running(2))));
    assert(std::strcmp(Describe(Reason::Off, 1), "Off: the model runs every frame.") == 0);
    ++g_cases;
}

void Cadence_IsAgeBased()
{
    for (uint32_t n = 2; n <= 4; ++n)
    {
        Driver d;
        const auto in = Running(n);
        for (int i = 0; i < 12; ++i)
            d.Frame(in, Shape());
        std::string expected;
        for (int i = 0; i < 12; ++i)
            expected += i % n == 0 ? 'M' : 'c';
        assert(d.pattern == expected);
    }
    ++g_cases;
}

void Cadence_CountsPresentsThePassRanAt_NotPresentIds()
{
    // Frame generation presents every rendered frame two or more times, so the present id advances by 2..4 per
    // call. Counted by present id the model would run every frame; counted by presents the pass ran at, the
    // cadence is unchanged.
    for (uint64_t step : { 2u, 3u, 4u })
    {
        Driver d;
        for (int i = 0; i < 9; ++i)
            d.Frame(Running(3), Shape(), true, step);
        assert(d.pattern == "MccMccMcc");
    }
    ++g_cases;
}

void Cadence_DroppedCallShiftsThePatternAndNeverFlipsIt()
{
    // Presents 1 2 [3 missing] 4 5 6 7: the call that did not happen is simply not counted; age is relative to
    // the last model frame, so there is no global parity to flip.
    Driver d;
    const auto in = Running(2);
    d.Frame(in, Shape());          // 1 M
    d.Frame(in, Shape());          // 2 c
    d.Frame(in, Shape(), true, 2); // 4 M
    d.Frame(in, Shape());          // 5 c
    d.Frame(in, Shape());          // 6 M
    assert(d.pattern == "McMcM");
    ++g_cases;
}

void Decision_HandsTheChainOnlyToAScheduledModelFrame()
{
    Driver d;
    const auto in = Running(3);
    auto first = d.Frame(in, Shape());
    assert(first.reason == Reason::NoEdit && !first.handChain);
    auto c1 = d.Frame(in, Shape());
    assert(!c1.runModel && c1.reason == Reason::Carried && c1.chainStart && c1.age == 1);
    auto c2 = d.Frame(in, Shape());
    assert(!c2.runModel && !c2.chainStart && c2.age == 2);
    auto model = d.Frame(in, Shape());
    assert(model.runModel && model.reason == Reason::Scheduled && model.handChain && model.age == 3);
    // A model frame forced right after a model frame has no carried frames behind it: the game's vectors.
    d.resetPending = true;
    auto forced = d.Frame(in, Shape());
    assert(forced.runModel && forced.reason == Reason::Reset && !forced.handChain);
    ++g_cases;
}

void Reset_IsHeldUntilAModelFrameRuns()
{
    Driver d;
    const auto in = Running(4);
    d.Frame(in, Shape()); // M
    d.Frame(in, Shape()); // c
    // A reset arriving on a frame that would carry runs the model instead.
    d.resetPending = true;
    auto decision = d.Frame(in, Shape());
    assert(decision.runModel && decision.reason == Reason::Reset);
    assert(!d.resetPending); // cleared by the model frame, which is the only thing that may clear it
    // A reset raised after a carried frame was decided (the frame's own resets come before the decision, so
    // only an outside one can) stays pending across it and forces the next frame.
    d.Frame(in, Shape()); // c
    d.resetPending = true;
    auto next = d.Frame(in, Shape());
    assert(next.runModel && next.reason == Reason::Reset);
    // Never a carried frame while a reset is pending, at any age.
    for (int i = 0; i < 6; ++i)
    {
        d.resetPending = true;
        assert(d.Frame(in, Shape()).runModel);
    }
    assert(d.pattern == "McMcMMMMMMM");
    ++g_cases;
}

void Changes_ForceAModelFrameAndDropTheEdit()
{
    const auto in = Running(2);
    auto expectChanged = [&](auto mutate)
    {
        Driver d;
        d.Frame(in, Shape()); // M: the edit is stored
        Signature s = Shape();
        mutate(s);
        auto decision = d.Frame(in, s); // would carry
        assert(decision.runModel && decision.reason == Reason::Changed && !d.scheduler.CarriedSinceModel());
        auto after = d.Frame(in, s); // the new shape has its own edit now
        assert(!after.runModel && after.chainStart);
    };
    expectChanged([](Signature& s) { s.workWidth = 1720; });                  // working size
    expectChanged([](Signature& s) { s.width = 2560; });                      // frame size
    expectChanged([](Signature& s) { s.format = 26; });                       // colour format
    expectChanged([](Signature& s) { s.route = Route(false, true, false); }); // after RR
    expectChanged([](Signature& s) { s.route = Route(false, false, true); }); // present route
    expectChanged([](Signature& s) { s.settings = Mix(s.settings, 2u); });    // what the model reads
    expectChanged([](Signature& s) { s.feature += 1; });                      // a rebuilt feature
    ++g_cases;
}

void Settings_HashSeparatesWhatTheModelReads()
{
    // The renderer folds each model-affecting value in; any single change must move the hash.
    auto fold = [](uint32_t preset, uint32_t style, float intensity, float structure, float tone, float skin, bool mask,
                   uint32_t passes)
    {
        uint64_t h = Mix(kMixSeed, passes);
        h = Mix(h, preset);
        h = Mix(h, style);
        h = Mix(h, intensity);
        h = Mix(h, structure);
        h = Mix(h, tone);
        h = Mix(h, skin);
        return Mix(h, mask ? 1u : 0u);
    };
    const uint64_t base = fold(0, 0, 1.0f, 1.0f, 1.0f, -1.0f, true, 1);
    std::set<uint64_t> seen { base };
    for (uint64_t h : { fold(1, 0, 1.0f, 1.0f, 1.0f, -1.0f, true, 1), fold(0, 1, 1.0f, 1.0f, 1.0f, -1.0f, true, 1),
                        fold(0, 0, 0.9f, 1.0f, 1.0f, -1.0f, true, 1), fold(0, 0, 1.0f, 1.1f, 1.0f, -1.0f, true, 1),
                        fold(0, 0, 1.0f, 1.0f, 0.8f, -1.0f, true, 1), fold(0, 0, 1.0f, 1.0f, 1.0f, 0.0f, true, 1),
                        fold(0, 0, 1.0f, 1.0f, 1.0f, -1.0f, false, 1), fold(0, 0, 1.0f, 1.0f, 1.0f, -1.0f, true, 2) })
        assert(seen.insert(h).second);
    assert(fold(0, 0, 1.0f, 1.0f, 1.0f, -1.0f, true, 1) == base);
    ++g_cases;
}

void CompositionChanges_DoNotForceAModelFrame()
{
    // Composition sliders are not in the signature at all; an unchanged signature keeps carrying.
    Driver d;
    const auto in = Running(4);
    for (int i = 0; i < 8; ++i)
        d.Frame(in, Shape());
    assert(d.pattern == "McccMccc");
    ++g_cases;
}

void Gap_ForcesAModelFrame()
{
    Driver d;
    const auto in = Running(3);
    d.Frame(in, Shape());
    auto within = d.Frame(in, Shape(), true, 1, kGapMs);
    assert(!within.runModel);
    auto gap = d.Frame(in, Shape(), true, 1, kGapMs + 1);
    assert(gap.runModel && gap.reason == Reason::Gap && !gap.handChain);
    ++g_cases;
}

void SecondCallInOnePresent_StandsDownForItAndTheNext()
{
    Driver d;
    const auto in = Running(2);
    d.Frame(in, Shape());                      // present 1, M
    auto same = d.Frame(in, Shape(), true, 0); // present 1 again
    assert(same.runModel && same.reason == Reason::SamePresent && !same.handChain);
    // That second model frame stores nothing: the next present runs the model too.
    assert(!same.record);
    auto next = d.Frame(in, Shape());
    assert(next.runModel && next.reason == Reason::NoEdit);
    auto after = d.Frame(in, Shape());
    assert(!after.runModel);
    // A present counter that never advances never carries.
    Driver stuck;
    for (int i = 0; i < 6; ++i)
        stuck.Frame(in, Shape(), true, i == 0 ? 1 : 0);
    assert(stuck.pattern == "MMMMMM");
    // A route whose counter never moved at all (still 0: it never presents through the wrapped swapchain) is told
    // apart by the renderer and refused under its own name, not reported as a second call in one present.
    Inputs uncounted = Running(2);
    uncounted.presentsCounted = false;
    Driver zero;
    for (int i = 0; i < 4; ++i)
    {
        const auto decision = zero.Frame(uncounted, Shape(), true, 0);
        assert(decision.runModel && decision.reason == Reason::NoPresentCount && !decision.record);
    }
    ++g_cases;
}

void FailedModelFrame_LeavesNothingToCarry()
{
    Driver d;
    const auto in = Running(2);
    d.Frame(in, Shape(), false); // the model failed, or completed only part of the chain
    auto next = d.Frame(in, Shape());
    assert(next.runModel && next.reason == Reason::NoEdit);
    ++g_cases;
}

void Refusals_EachRunTheModelWithTheirOwnReason()
{
    struct Case
    {
        const char* name;
        void (*apply)(Inputs&);
        Reason reason;
        bool releases; // given back; every other refusal keeps what is built and builds nothing
        const char* says;
    };
    const Case cases[] = {
        { "native Vulkan", [](Inputs& i) { i.nativeVulkan = true; }, Reason::NativeVulkan, true, "native Vulkan" },
        { "driver proxy", [](Inputs& i) { i.driverProxy = true; }, Reason::DriverProxy, true, "driver proxy" },
        { "no shader", [](Inputs& i) { i.available = false; }, Reason::Unavailable, true, "no model cadence shader" },
        { "no present count", [](Inputs& i) { i.presentsCounted = false; }, Reason::NoPresentCount, false,
          "cannot count frames" },
        { "before upscale", [](Inputs& i) { i.beforeUpscale = true; }, Reason::BeforeUpscale, false,
          "before the upscaler" },
        { "zero guides", [](Inputs& i) { i.gameGuides = false; }, Reason::NoGameGuides, false, "no game depth" },
        { "frame generation", [](Inputs& i) { i.frameGeneration = true; }, Reason::FrameGeneration, false,
          "frame generation is active" },
        { "hold", [](Inputs& i) { i.hold = true; }, Reason::FrameHold, false, "Hold frame" },
        { "capture", [](Inputs& i) { i.capture = true; }, Reason::Capture, false, "capture" },
        { "no surfaces", [](Inputs& i) { i.surfaces = false; }, Reason::NoSurfaces, false, "could not be allocated" },
        { "reversible replace", [](Inputs& i) { i.reversibleReplace = true; }, Reason::ReversibleReplace, false,
          "replace curves" },
    };
    std::set<std::string> said;
    for (const auto& c : cases)
    {
        Driver d;
        Inputs in = Running(3);
        d.Frame(in, Shape()); // M, edit stored
        c.apply(in);
        for (int i = 0; i < 4; ++i)
        {
            const auto decision = d.Frame(in, Shape());
            assert(decision.runModel && decision.reason == c.reason && decision.cadence == 1 && !decision.handChain &&
                   !decision.record);
            assert(IsRefusal(decision.reason) && !IsForced(decision.reason));
        }
        assert(!d.scheduler.HasEdit());
        assert(ReleasesSurfaces(c.reason) == c.releases && !BuildsSurfaces(c.reason));
        const std::string text = Describe(c.reason, 3);
        assert(text.find(c.says) != std::string::npos);
        assert(said.insert(text).second);
        // Lifting the condition resumes the cadence after one model frame.
        d.resetPending = false;
        Inputs clear = Running(3);
        const auto resumed = d.Frame(clear, Shape());
        assert(resumed.runModel && resumed.reason == Reason::NoEdit);
        assert(!d.Frame(clear, Shape()).runModel);
    }
    ++g_cases;
}

void FrameGeneration_RunsWhenOptedIn()
{
    Driver d;
    Inputs in = Running(2);
    in.frameGeneration = true;
    in.allowWithFrameGen = true;
    for (int i = 0; i < 6; ++i)
        d.Frame(in, Shape());
    assert(d.pattern == "McMcMc");
    in.allowWithFrameGen = false;
    assert(Refusal(in) == Reason::FrameGeneration);
    ++g_cases;
}

void Refusal_NamesWhatTheUserCanChangeLeast()
{
    Inputs in = Running(2);
    in.capture = true;
    in.hold = true;
    in.frameGeneration = true;
    in.gameGuides = false;
    in.beforeUpscale = true;
    assert(Refusal(in) == Reason::BeforeUpscale);
    in.driverProxy = true;
    assert(Refusal(in) == Reason::DriverProxy);
    in.requested = Requested(1);
    assert(Refusal(in) == Reason::Off);
    // The replace curve is one click away: it is named after what the user can do little about, and before
    // frame generation, which it sits beside.
    Inputs replace = Running(2);
    replace.reversibleReplace = true;
    replace.frameGeneration = true;
    assert(Refusal(replace) == Reason::ReversibleReplace);
    replace.gameGuides = false;
    assert(Refusal(replace) == Reason::NoGameGuides);
    ++g_cases;
}

void Status_TextsAreDistinctAndCadenceSpecific()
{
    assert(std::string(Describe(Reason::Carried, 2)).find("every 2nd") != std::string::npos);
    assert(std::string(Describe(Reason::Scheduled, 3)).find("every 3rd") != std::string::npos);
    assert(std::string(Describe(Reason::Carried, 4)).find("every 4th") != std::string::npos);
    std::set<std::string> texts;
    for (int r = static_cast<int>(Reason::None); r <= static_cast<int>(Reason::ReversibleReplace); ++r)
    {
        const auto reason = static_cast<Reason>(r);
        if (reason == Reason::Scheduled)
            continue; // the same line as Carried: one running cadence, one status
        assert(texts.insert(Describe(reason, 2)).second);
        assert(std::strchr(Describe(reason, 2), '%') == nullptr); // the pt-BR pack rejects a bare %
    }
    ++g_cases;
}

void TapScale_FollowsTheWorkingSize()
{
    assert(TapScale(3440, 3440) == 1.0f);
    assert(TapScale(1720, 3440) == 0.5f);
    assert(TapScale(860, 3440) == 1.0f / 3.0f); // clamped
    assert(TapScale(6880, 3440) == 2.0f);
    assert(TapScale(100, 0) == 1.0f);
    assert(LowExtent(2580) == 162 && LowExtent(16) == 1 && LowExtent(17) == 2);
    ++g_cases;
}

void StaleHistory_ResetsWhenHistoryIsOlderThanMotion()
{
    // Steady scheduled frames must NOT reset: they receive the accumulated motion chain.
    for (uint32_t n = 2; n <= 4; ++n)
    {
        Driver d;
        const auto in = Running(n);
        for (int i = 0; i < 16; ++i)
        {
            const auto decision = d.Frame(in, Shape());
            assert(!decision.staleHistory);
        }
    }

    // Steady cadence=1 (Off) never resets.
    {
        Driver d;
        const auto in = Running(1);
        for (int i = 0; i < 8; ++i)
        {
            const auto decision = d.Frame(in, Shape());
            assert(!decision.staleHistory);
        }
    }

    // Path 1: Gap after carried frames forces a model frame without chain motion -> staleHistory.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M: edit stored, framesSinceModel = 0
        d.Frame(in, Shape()); // c: carried, framesSinceModel = 1
        const auto gap = d.Frame(in, Shape(), true, 1, kGapMs + 1);
        assert(gap.runModel && gap.reason == Reason::Gap && !gap.handChain && gap.staleHistory);
    }

    // Path 2: Changed signature after carried frames forces a model frame -> staleHistory.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        d.Frame(in, Shape()); // c
        Signature s2 = Shape();
        s2.workWidth = 1720;
        const auto changed = d.Frame(in, s2);
        assert(changed.runModel && changed.reason == Reason::Changed && !changed.handChain && changed.staleHistory);
    }

    // Path 3: Failed carry leaves model history older than 1 frame -> next frame (NoEdit) is staleHistory.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        // Carried frame where Carry() fails:
        const auto c = d.scheduler.Decide(in, Shape(), ++d.present, d.ms += 16, d.resetPending);
        assert(!c.runModel && c.reason == Reason::Carried);
        d.scheduler.Drop(true); // carry failed, records that a frame passed without model
        const auto next = d.Frame(in, Shape());
        assert(next.runModel && next.reason == Reason::NoEdit && !next.handChain && next.staleHistory);
    }

    // A refused frame after carried ones runs the model on the game's vectors, so it resets; the refused frames
    // after it renewed the history, so lifting the refusal does not reset again.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        d.Frame(in, Shape()); // c
        auto off = in;
        off.requested = 1;
        const auto refused = d.Frame(off, Shape());
        assert(refused.runModel && refused.reason == Reason::Off && refused.staleHistory);
        assert(!d.Frame(off, Shape()).staleHistory);
        const auto back = d.Frame(in, Shape());
        assert(back.runModel && back.reason == Reason::NoEdit && !back.staleHistory);
    }

    // A second view in one present runs the model without storing its edit, as every call does without cadence:
    // the forced frame that follows does not reset.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        d.Frame(in, Shape()); // c
        const auto second = d.Frame(in, Shape(), true, 0);
        assert(second.runModel && second.reason == Reason::SamePresent && second.staleHistory);
        const auto next = d.Frame(in, Shape());
        assert(next.runModel && next.reason == Reason::NoEdit && !next.staleHistory);
    }

    // A model frame that failed renews nothing: the forced frame after it still resets.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        d.Frame(in, Shape()); // c
        const auto failed = d.Frame(in, Shape(), false, 1, kGapMs + 1);
        assert(failed.runModel && failed.staleHistory);
        assert(d.Frame(in, Shape()).staleHistory);
    }

    // A scheduled frame handed the chain never resets; the caller resets only if the chain cannot be built.
    {
        Driver d;
        const auto in = Running(3);
        d.Frame(in, Shape()); // M
        d.Frame(in, Shape()); // c
        d.Frame(in, Shape()); // c
        const auto scheduled = d.Frame(in, Shape());
        assert(scheduled.runModel && scheduled.reason == Reason::Scheduled && scheduled.handChain);
        assert(!scheduled.staleHistory);
    }

    ++g_cases;
}

// Smoothing (docs/dlssnr/design/cadence-smoothing.md): the spread's weights and the limiter's step, as numbers.
void Smoothing_WeightsAndStep()
{
    for (uint32_t n = 1; n <= kMaxCadence; ++n)
        assert(SpreadFor(n) == n);
    // Over three frames: two thirds of the previous answer on the model frame, one third on the next, none after.
    assert(FadeWeight(0, 3) == 1.0f - 1.0f / 3.0f && FadeWeight(1, 3) == 1.0f - 2.0f / 3.0f &&
           FadeWeight(2, 3) == 0.0f);
    assert(FadeWeight(0, 2) == 0.5f && FadeWeight(1, 2) == 0.0f);
    assert(FadeWeight(0, 4) == 0.75f && FadeWeight(1, 4) == 0.5f && FadeWeight(2, 4) == 0.25f &&
           FadeWeight(3, 4) == 0.0f);
    assert(FadeWeight(0, 1) == 0.0f && FadeWeight(0, 0) == 0.0f && FadeWeight(7, 4) == 0.0f);
    // Equal steps: the new answer's share rises by 1/S a frame and is whole on frame S-1.
    for (uint32_t n = 2; n <= kMaxCadence; ++n)
        for (uint32_t k = 0; k + 1 < n; ++k)
        {
            const float step = FadeWeight(k, n) - FadeWeight(k + 1, n);
            assert(step > 0.999f / float(n) && step < 1.001f / float(n));
        }

    // 1.5 stops a second: a 60 fps frame may move the edit's light by 0.025 stop; a hitch no more than a 15 fps frame,
    // a frame of nothing (or a clock that went backwards, or NaN) no less than a 500 fps one.
    assert(LimitStep(1.0f / 60.0f) > 0.0249f && LimitStep(1.0f / 60.0f) < 0.0251f);
    assert(LimitStep(1.0f) == kLightRate * kLightMaxSeconds && LimitStep(0.25f) == LimitStep(1.0f));
    assert(LimitStep(0.0f) == kLightRate * kLightMinSeconds && LimitStep(-1.0f) == LimitStep(0.0f));
    assert(LimitStep(std::numeric_limits<float>::quiet_NaN()) == LimitStep(0.0f));
    ++g_cases;
}

// What the scheduler asks of smoothing frame by frame, driven as Dispatch drives it: the fade only on a scheduled
// model frame (never a forced one), its walk on the carried frames after a model frame that faded, the limiter on
// every frame of a running cadence and on none other, and a reset of it after anything that was not one.
void Smoothing_FadesScheduledFramesAndLimitsTheCadenceOnly()
{
    Scheduler sched;
    uint64_t present = 0;
    int64_t ms = 0;
    Inputs in = Running(3);
    in.smoothing = true;
    auto decide = [&](const Inputs& inputs, bool reset = false)
    { return sched.Decide(inputs, Shape(), ++present, ms += 16, reset); };

    // The first frame is forced (nothing stored): the model's answer as it is, no limiter.
    Decision d = decide(in);
    assert(d.runModel && d.reason == Reason::NoEdit && !d.fade && d.fadeWeight == 0.0f && !d.limit);
    sched.Recorded(false);
    assert(!sched.Faded() && !sched.Limited());

    // Carried frames after a model frame that did not fade: no walk, the limiter on, starting over on the first.
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit && d.limitReset);
    sched.Carried(true);
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit && !d.limitReset);
    sched.Carried(true);

    // The scheduled model frame fades in, two thirds of the carried answer still shown, the limiter continuing.
    d = decide(in);
    assert(d.runModel && d.reason == Reason::Scheduled && d.handChain && d.fade && d.limit && !d.limitReset);
    assert(d.fadeWeight == FadeWeight(0, 3));
    sched.Recorded(true);
    assert(sched.Faded() && sched.Limited());

    // Its walk: a third on the next frame, nothing on the last.
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == FadeWeight(1, 3) && d.limit && !d.limitReset);
    sched.Carried(true);
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit);
    sched.Carried(true);

    // A reset forces the next model frame: no fade, no limit, and the fade stored before is gone.
    d = decide(in, true);
    assert(d.runModel && d.reason == Reason::Reset && !d.fade && !d.limit && d.fadeWeight == 0.0f);
    sched.Recorded(false);
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit && d.limitReset);
    sched.Carried(true);
    d = decide(in);
    sched.Carried(true);
    d = decide(in);
    assert(d.fade);
    // The fade could not be shown (Fade or Show failed): recorded without it, so the walk does not happen.
    sched.Recorded(false);
    d = decide(in);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit && d.limitReset);
    sched.Carried(true);

    // A refusal runs the model as it is and forgets both.
    Inputs refused = in;
    refused.hold = true;
    d = decide(refused);
    assert(d.runModel && d.reason == Reason::FrameHold && !d.fade && !d.limit && d.fadeWeight == 0.0f);
    assert(!sched.Faded() && !sched.Limited());

    // Smoothing off: nothing, ever.
    Scheduler plain;
    present = 0;
    Inputs off = Running(3);
    for (int i = 0; i < 12; ++i)
    {
        const Decision e = plain.Decide(off, Shape(), ++present, ms += 16, false);
        assert(!e.fade && e.fadeWeight == 0.0f && !e.limit && !e.limitReset);
        if (e.runModel)
            plain.Recorded(e.fade);
        else
            plain.Carried(e.limit);
    }
    assert(!plain.Faded() && !plain.Limited());

    // Cadence 2: the model frame shows half of each answer, the carried frame the new one whole.
    Scheduler two;
    present = 0;
    Inputs in2 = Running(2);
    in2.smoothing = true;
    two.Decide(in2, Shape(), ++present, ms += 16, false);
    two.Recorded(false);
    two.Decide(in2, Shape(), ++present, ms += 16, false);
    two.Carried(true);
    d = two.Decide(in2, Shape(), ++present, ms += 16, false);
    assert(d.fade && d.fadeWeight == 0.5f);
    two.Recorded(true);
    d = two.Decide(in2, Shape(), ++present, ms += 16, false);
    assert(!d.runModel && d.fadeWeight == 0.0f && d.limit && !d.limitReset);

    // A cadence changed in the middle of a walk: the walk keeps the length it started with, so going from 4 to 2
    // neither drops the remaining three quarters of the fade in one frame nor stretches a walk past the model frame.
    Scheduler four;
    present = 0;
    Inputs in4 = Running(4);
    in4.smoothing = true;
    four.Decide(in4, Shape(), ++present, ms += 16, false);
    four.Recorded(false);
    for (int i = 0; i < 3; ++i)
    {
        four.Decide(in4, Shape(), ++present, ms += 16, false);
        four.Carried(true);
    }
    d = four.Decide(in4, Shape(), ++present, ms += 16, false);
    assert(d.fade && d.fadeWeight == FadeWeight(0, 4));
    four.Recorded(true);
    Inputs nowTwo = Running(2);
    nowTwo.smoothing = true;
    d = four.Decide(nowTwo, Shape(), ++present, ms += 16, false);
    assert(!d.runModel && d.fadeWeight == FadeWeight(1, 4) && d.fadeWeight == 0.5f);
    ++g_cases;
}
} // namespace

int main()
{
    Requested_ClampsToOffOutsideTwoToFour();
    Off_RunsEveryFrameAndKeepsNoSurfaces();
    Cadence_IsAgeBased();
    Cadence_CountsPresentsThePassRanAt_NotPresentIds();
    Cadence_DroppedCallShiftsThePatternAndNeverFlipsIt();
    Decision_HandsTheChainOnlyToAScheduledModelFrame();
    Reset_IsHeldUntilAModelFrameRuns();
    Changes_ForceAModelFrameAndDropTheEdit();
    Settings_HashSeparatesWhatTheModelReads();
    CompositionChanges_DoNotForceAModelFrame();
    Gap_ForcesAModelFrame();
    SecondCallInOnePresent_StandsDownForItAndTheNext();
    FailedModelFrame_LeavesNothingToCarry();
    Refusals_EachRunTheModelWithTheirOwnReason();
    FrameGeneration_RunsWhenOptedIn();
    Refusal_NamesWhatTheUserCanChangeLeast();
    Status_TextsAreDistinctAndCadenceSpecific();
    TapScale_FollowsTheWorkingSize();
    StaleHistory_ResetsWhenHistoryIsOlderThanMotion();
    Smoothing_WeightsAndStep();
    Smoothing_FadesScheduledFramesAndLimitsTheCadenceOnly();
    if (g_cases != 21)
        return 2; // a runner that exercises nothing must fail loudly
    std::printf("PASS %d scheduler cases: off allocates nothing, age in presents the pass ran at, held reset, forced "
                "model frames, every refusal with its reason, smoothing's weights, step and per-frame decisions\n",
                g_cases);
    return 0;
}
