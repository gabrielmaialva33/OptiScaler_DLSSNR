// Synthesized frame generation's decision logic and the NR/FG motion handoff, compiled from the production
// headers as they are. Neither touches the GPU, so nothing here is faked: the handoff only stores and
// compares pointers, and the policy only does arithmetic on numbers the FG input feeds it.
#include <cassert>
#include <cstdio>
#include <initializer_list>

struct ID3D12Device
{
    int id = 0;
};
struct ID3D12Resource
{
    int id = 0;
};

#include "inputs/FG/Synth_Hud.h"
#include "inputs/FG/Synth_Policy.h"
#include "shaders/synth_motion/SynthMotion_Handoff.h"

using SynthMotion::Handoff::Field;
using SynthMotion::Handoff::Owner;
namespace Handoff = SynthMotion::Handoff;

static void handoffCases()
{
    ID3D12Device device {}, otherDevice {};
    ID3D12Resource nrField {}, fgField {};
    Field field {};

    // D3D11 bridge order: the neural host publishes, frame generation takes it in the same base frame.
    Handoff::BeginBaseFrame();
    Handoff::Publish(Owner::DlssNr, &device, &nrField, 1920, 1080, false);
    assert(Handoff::Take(Owner::FrameGen, &device, 1920, 1080, &field));
    assert(field.motion == &nrField && field.owner == Owner::DlssNr && !field.sceneCut);

    // Never back to the publisher: that is also what protects a route with no transport, where nobody
    // advances the base frame and the publisher would otherwise find its own field every frame.
    assert(!Handoff::Take(Owner::DlssNr, &device, 1920, 1080, &field));

    // Another device or another extent is another picture.
    assert(!Handoff::Take(Owner::FrameGen, &otherDevice, 1920, 1080, &field));
    assert(!Handoff::Take(Owner::FrameGen, &device, 1280, 720, &field));

    // The next base frame does not see this frame's field.
    Handoff::BeginBaseFrame();
    assert(!Handoff::Take(Owner::FrameGen, &device, 1920, 1080, &field));

    // Native D3D12 order: frame generation publishes before the present pass, NR takes it, with the cut.
    Handoff::Publish(Owner::FrameGen, &device, &fgField, 3440, 1440, true);
    assert(Handoff::Take(Owner::DlssNr, &device, 3440, 1440, &field));
    assert(field.motion == &fgField && field.sceneCut);

    // A released estimator withdraws its field; withdrawing someone else's does nothing.
    Handoff::Withdraw(&nrField);
    assert(Handoff::Take(Owner::DlssNr, &device, 3440, 1440, nullptr));
    Handoff::Withdraw(&fgField);
    assert(!Handoff::Take(Owner::DlssNr, &device, 3440, 1440, &field));

    // Nothing is published without an owner, a device, a field or an extent.
    Handoff::BeginBaseFrame();
    Handoff::Publish(Owner::None, &device, &fgField, 64, 64, false);
    Handoff::Publish(Owner::FrameGen, nullptr, &fgField, 64, 64, false);
    Handoff::Publish(Owner::FrameGen, &device, nullptr, 64, 64, false);
    Handoff::Publish(Owner::FrameGen, &device, &fgField, 0, 64, false);
    assert(!Handoff::Take(Owner::DlssNr, &device, 64, 64, &field));

    std::puts("handoff cases passed");
}

static void warmingCases()
{
    ID3D12Device device {}, otherDevice {};

    // D3D11 bridge at start-up: the neural host's estimator recorded but is warming; frame generation sees
    // it in the same base frame, and the host never sees its own announcement.
    Handoff::BeginBaseFrame();
    Handoff::AnnounceWarming(Owner::DlssNr, &device, 3440, 1440);
    assert(Handoff::PeerWarming(Owner::FrameGen, &device, 3440, 1440));
    assert(!Handoff::PeerWarming(Owner::DlssNr, &device, 3440, 1440));

    // Another device or extent is another picture; the next base frame needs a new announcement.
    assert(!Handoff::PeerWarming(Owner::FrameGen, &otherDevice, 3440, 1440));
    assert(!Handoff::PeerWarming(Owner::FrameGen, &device, 1920, 1080));
    Handoff::BeginBaseFrame();
    assert(!Handoff::PeerWarming(Owner::FrameGen, &device, 3440, 1440));

    // Native D3D12: frame generation announces before the present pass.
    Handoff::AnnounceWarming(Owner::FrameGen, &device, 1920, 1080);
    assert(Handoff::PeerWarming(Owner::DlssNr, &device, 1920, 1080));

    // Nothing is announced without an owner, a device or an extent.
    Handoff::BeginBaseFrame();
    Handoff::AnnounceWarming(Owner::None, &device, 64, 64);
    Handoff::AnnounceWarming(Owner::DlssNr, nullptr, 64, 64);
    Handoff::AnnounceWarming(Owner::DlssNr, &device, 0, 64);
    assert(!Handoff::PeerWarming(Owner::FrameGen, &device, 64, 64));
    assert(!Handoff::PeerWarming(Owner::FrameGen, nullptr, 64, 64));

    // A released estimator withdraws its own announcement, not the other consumer's.
    Handoff::BeginBaseFrame();
    Handoff::AnnounceWarming(Owner::DlssNr, &device, 1920, 1080);
    Handoff::WithdrawWarming(Owner::FrameGen);
    assert(Handoff::PeerWarming(Owner::FrameGen, &device, 1920, 1080));
    Handoff::WithdrawWarming(Owner::DlssNr);
    assert(!Handoff::PeerWarming(Owner::FrameGen, &device, 1920, 1080));

    // The wait: only with no estimator of one's own and a warming peer. A frame with no announcement
    // ends it at once (the peer is gone, or never was), and the consumer builds its own.
    Handoff::PeerWait wait {};
    assert(!Handoff::WaitForPeer(wait, false, false, 1000) && wait.sinceMs == -1);
    assert(!Handoff::WaitForPeer(wait, true, true, 1000) && wait.sinceMs == -1);

    // Bounded in fresh frames: at 60 Hz the first frame starts the wait, and kMaxPeerWaitFrames more follow.
    long long now = 1000;
    assert(Handoff::WaitForPeer(wait, false, true, now) && wait.sinceMs == 1000);
    for (uint32_t i = 0; i < Handoff::kMaxPeerWaitFrames; ++i)
        assert(Handoff::WaitForPeer(wait, false, true, now += 16));
    assert(!Handoff::WaitForPeer(wait, false, true, now += 16));

    // A loading screen, a frame every two seconds (Divinity, 2026-09-28): every estimator is reset by
    // staleness on each, so nothing counts, however long it lasts. A bound in milliseconds gave up here.
    wait = {};
    now = 20000;
    for (int i = 0; i < 100; ++i)
        assert(Handoff::WaitForPeer(wait, false, true, now += 2000));
    for (uint32_t i = 0; i < Handoff::kMaxPeerWaitFrames; ++i)
        assert(Handoff::WaitForPeer(wait, false, true, now += 16));
    assert(!Handoff::WaitForPeer(wait, false, true, now += 16));

    // A stale gap mid-wait starts the count over: it restarted the peer's warm-up too.
    wait = {};
    now = 500000;
    assert(Handoff::WaitForPeer(wait, false, true, now));
    for (int i = 0; i < 10; ++i)
        assert(Handoff::WaitForPeer(wait, false, true, now += 16));
    assert(Handoff::WaitForPeer(wait, false, true, now += 1000) && wait.freshFrames == 0);
    for (uint32_t i = 0; i < Handoff::kMaxPeerWaitFrames; ++i)
        assert(Handoff::WaitForPeer(wait, false, true, now += 16));
    assert(!Handoff::WaitForPeer(wait, false, true, now += 16));

    std::puts("warming cases passed");
}

static void fastMotionCases()
{
    SynthFgPolicy policy;
    assert(policy.Decide() == SynthFgPolicy::Action::Generate);

    // Off by default: any motion generates.
    policy.ObserveMotion(500.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Generate);

    policy.Configure(24.0f, 0.0f);
    policy.ObserveMotion(30.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Reset);

    // Between the release level (75%) and the threshold: still fast.
    policy.ObserveMotion(20.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Reset);

    // Two calm samples in a row end it; one is not enough, and a spike in between starts the count over.
    policy.ObserveMotion(10.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Reset);
    policy.ObserveMotion(40.0f, false, false);
    policy.ObserveMotion(10.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Reset);
    policy.ObserveMotion(10.0f, false, false);
    assert(policy.Decide() == SynthFgPolicy::Action::Generate);

    // Turning it off mid-burst stops it at once.
    policy.ObserveMotion(99.0f, false, false);
    assert(policy.Fast());
    policy.Configure(0.0f, 0.0f);
    assert(!policy.Fast() && policy.Decide() == SynthFgPolicy::Action::Generate);

    // A scene cut repeats exactly one frame, whatever the thresholds.
    policy.ObserveMotion(0.0f, false, true);
    assert(policy.Decide() == SynthFgPolicy::Action::Reset);
    assert(policy.Decide() == SynthFgPolicy::Action::Generate);

    std::puts("fast-motion cases passed");
}

static void floorCases()
{
    SynthFgPolicy policy;
    double now = 1000.0;

    // Off by default, however slow.
    for (int i = 0; i < 50; ++i)
        policy.ObserveBaseFrame(now += 100.0);
    assert(policy.Decide() == SynthFgPolicy::Action::Generate);

    // 25 fps against a 30 fps floor: skipped.
    policy.Configure(0.0f, 30.0f);
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 40.0);
    assert(policy.BelowFloor() && policy.Decide() == SynthFgPolicy::Action::Skip);

    // 32 fps is above the floor but inside the 15% hysteresis: still skipped.
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 31.25);
    assert(policy.Decide() == SynthFgPolicy::Action::Skip);

    // 50 fps: generating again.
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 20.0);
    assert(!policy.BelowFloor() && policy.Decide() == SynthFgPolicy::Action::Generate);

    // A pause (a menu, a load) is not a frame rate: it restarts the average instead of skipping.
    policy.ObserveBaseFrame(now += 2000.0);
    assert(policy.Decide() == SynthFgPolicy::Action::Generate && policy.BaseFps() == 0.0);

    // The floor wins over a pending cut: nothing is fed below it.
    policy.ObserveMotion(0.0f, false, true);
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 50.0);
    assert(policy.Decide() == SynthFgPolicy::Action::Skip);

    std::puts("floor cases passed");
}

static void duplicateCases()
{
    // 30 fps content presented at 60: identical and moving frames alternate. Advised once.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < SynthFgPolicy::kWindow; ++i)
        {
            policy.ObserveMotion(i % 2 ? 0.0f : 6.0f, i % 2 == 1, false);
            if (i + 1 < SynthFgPolicy::kWindow)
                assert(!policy.TakeDuplicateAdvice());
        }
        assert(policy.IdenticalInWindow() == SynthFgPolicy::kWindow / 2);
        assert(policy.TakeDuplicateAdvice());
        assert(!policy.TakeDuplicateAdvice());

        // Counts stay exact once the ring wraps.
        for (uint32_t i = 0; i < 3 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(0.0f, i % 2 == 1, false);
        assert(policy.IdenticalInWindow() == SynthFgPolicy::kWindow / 2);
        assert(policy.AlternationsInWindow() == SynthFgPolicy::kWindow - 1);
    }

    // A paused or static game: every sample identical, no moving frames. Not a cadence.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(0.0f, true, false);
        assert(!policy.TakeDuplicateAdvice());
    }

    // A moving game with an occasional identical frame: not a cadence either.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(8.0f, i % 10 == 0, false);
        assert(!policy.TakeDuplicateAdvice());
    }

    // Identical frames in long runs rather than interleaved (a game pausing now and then): not advised.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(8.0f, (i / 15) % 2 == 1, false);
        assert(!policy.TakeDuplicateAdvice());
    }

    std::puts("duplicate cases passed");
}

// The HUD keys (Synth_Hud.h): which fix runs, and what FSR-FG is handed.
static void hudCases()
{
    // Defaults: near depth on, the layer off. Without synthesized motion nothing runs at all, so the input is
    // what it was before the keys existed.
    {
        const auto plan = PlanSynthHud(true, false, false, false);
        assert(!plan.depth && !plan.layer && !plan.detect);
    }

    // With motion, near depth runs, and so does the mask it needs.
    {
        const auto plan = PlanSynthHud(true, false, true, false);
        assert(plan.depth && !plan.layer && plan.detect);
    }

    // Near depth never runs without motion: zero game vectors make it a no-op that would still cost the mask.
    for (bool layer : { false, true })
    {
        for (bool disableUi : { false, true })
            assert(!PlanSynthHud(true, layer, false, disableUi).depth);
    }

    // The layer does not need motion: it holds the HUD however the interpolator moved it.
    {
        const auto plan = PlanSynthHud(false, true, false, false);
        assert(!plan.depth && plan.layer && plan.detect);
    }

    // DisableUI refuses every UI resource, so the layer is not recorded for nobody; near depth is unaffected.
    {
        const auto plan = PlanSynthHud(true, true, true, true);
        assert(plan.depth && !plan.layer && plan.detect);
        assert(!PlanSynthHud(false, true, true, true).detect);
    }

    // The mask is recorded exactly when one of the fixes runs, over every combination.
    for (int bits = 0; bits < 16; ++bits)
    {
        const auto plan = PlanSynthHud(bits & 1, bits & 2, bits & 4, bits & 8);
        assert(plan.detect == (plan.depth || plan.layer));
    }

    // Feed: the mask's depth only for a frame whose mask executed at the presenter's extent; otherwise the
    // constant depth, as before.
    {
        const auto plan = PlanSynthHud(true, true, true, false);
        auto feed = ChooseSynthHudFeed(plan, true, true);
        assert(feed.maskDepth && feed.layer);

        feed = ChooseSynthHudFeed(plan, false, true);
        assert(!feed.maskDepth && !feed.layer);

        // A layer never written yet (native D3D12 writes it after this decision) is not handed to FFX: it would
        // compose memory nothing wrote. The depth does not wait for it.
        feed = ChooseSynthHudFeed(plan, true, false);
        assert(feed.maskDepth && !feed.layer);
    }

    // A fix that is off is never handed over, whatever the mask did.
    {
        const auto feed = ChooseSynthHudFeed(PlanSynthHud(false, false, true, false), true, true);
        assert(!feed.maskDepth && !feed.layer);
    }

    std::puts("hud cases passed");
}

int main()
{
    handoffCases();
    warmingCases();
    fastMotionCases();
    floorCases();
    duplicateCases();
    hudCases();
    std::puts("fg synth policy: handoff, warming, fast-motion, floor, duplicate and hud cases passed");
    return 0;
}
