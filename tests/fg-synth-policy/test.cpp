// Synthesized frame generation's decision logic, the statistics it decides on, and the NR/FG motion handoff,
// compiled from the production headers as they are. None of it touches the GPU, so nothing here is faked:
// the handoff only stores and compares pointers, the statistics read rows of halves laid out as the
// readback lays them out, and the policy only does arithmetic on what the statistics say.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

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
using SynthMotionStats::Stats;

constexpr auto Generate = SynthFgPolicy::Action::Generate;
constexpr auto Reset = SynthFgPolicy::Action::Reset;
constexpr auto Skip = SynthFgPolicy::Action::Skip;

// A motion sample as the policy sees it, for the cases about the policy rather than the rows: every
// magnitude at the median, 1920 pixels wide.
static Stats Sample(float medianPx, bool allZero = false, float incoherent = 0.0f)
{
    Stats stats {};
    stats.medianPx = medianPx;
    stats.p90Px = medianPx;
    stats.medianX = medianPx;
    stats.medianOfWidth = medianPx / 1920.0f;
    stats.incoherent = incoherent;
    stats.allZero = allZero;
    return stats;
}

// Only what these cases need: zero, infinity, NaN, and normal numbers a half holds exactly.
static uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exponent = (bits >> 23) & 0xffu;
    const uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xff)
        return (uint16_t) (sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));

    if (exponent == 0 && mantissa == 0)
        return (uint16_t) sign;

    const int halfExponent = (int) exponent - 127 + 15;
    assert(halfExponent > 0 && halfExponent < 0x1f && (mantissa & 0x1fffu) == 0);
    return (uint16_t) (sign | ((uint32_t) halfExponent << 10) | (mantissa >> 13));
}

// The readback as SynthInputs lays it out: SynthMotionStats::kRows rows of R16G16_FLOAT, each on a pitch
// aligned to 512 bytes (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT). The padding past the width holds a
// moving vector, so reading it would show.
struct Readback
{
    uint32_t width;
    size_t pitch;
    std::vector<uint16_t> halves;

    explicit Readback(uint32_t w)
        : width(w), pitch((w * 4 + 511) / 512 * 512), halves(SynthMotionStats::kRows * pitch / 2, FloatToHalf(7.0f))
    {
    }

    void Set(uint32_t row, uint32_t x, float vx, float vy)
    {
        halves[row * pitch / 2 + 2 * x] = FloatToHalf(vx);
        halves[row * pitch / 2 + 2 * x + 1] = FloatToHalf(vy);
    }

    // field(row, x) -> {vx, vy}, for every pixel of every row, as the copy writes them.
    template <typename Field> Readback& Fill(Field field)
    {
        for (uint32_t row = 0; row < SynthMotionStats::kRows; ++row)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const auto [vx, vy] = field(row, x);
                Set(row, x, vx, vy);
            }
        }

        return *this;
    }

    Stats Measure(SynthMotionStats::Scratch& scratch) const
    {
        return SynthMotionStats::Measure(reinterpret_cast<const uint8_t*>(halves.data()), SynthMotionStats::kRows,
                                         pitch, width, SynthMotionStats::kStride, scratch);
    }
};

// A uniform pan, and 8x8 blocks moving alternately right and left by amplitude (the estimator's block size).
static std::pair<float, float> Pan40(uint32_t, uint32_t) { return { 40.0f, 0.0f }; }
static auto Alternating(float amplitude)
{
    return [amplitude](uint32_t, uint32_t x)
    { return std::pair<float, float> { (x / 8) % 2 ? -amplitude : amplitude, 0.0f }; };
}

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

static void statsCases()
{
    using namespace SynthMotionStats;
    Scratch scratch;

    // A uniform pan: every statistic at the pan, nothing incoherent. 1920 wide: 480 samples, 479 pairs a row.
    {
        const Stats s = Readback(1920).Fill(Pan40).Measure(scratch);
        assert(s.vectors == kRows * 480 && s.pairs == kRows * 479);
        assert(s.medianPx == 40.0f && s.p90Px == 40.0f && s.medianX == 40.0f && s.medianY == 0.0f);
        assert(s.medianOfWidth == 40.0f / 1920.0f && s.incoherent == 0.0f && !s.allZero);
    }

    // A still frame, negative zeros included. The width is not a multiple of the stride (1002: x = 0..1000,
    // 251 a row) and the pitch is 4096 bytes: the moving padding past it is never read.
    {
        Readback readback(1002);
        readback.Fill([](uint32_t, uint32_t x) { return std::pair<float, float> { x % 3 ? 0.0f : -0.0f, -0.0f }; });
        const Stats s = readback.Measure(scratch);
        assert(readback.pitch == 4096);
        assert(s.vectors == kRows * 251 && s.pairs == kRows * 250);
        assert(s.allZero && s.medianPx == 0.0f && s.p90Px == 0.0f && s.incoherent == 0.0f);
    }

    // Percentiles by nearest rank: magnitudes 0..9 in equal numbers give a median of 5 and a p90 of 8.
    // Neighbours differ by 1 px (under the 2 px floor) except where 9 wraps to 0, 47 times in a row's 479
    // pairs.
    {
        const Stats s =
            Readback(1920)
                .Fill([](uint32_t, uint32_t x) { return std::pair<float, float> { 0.0f, (float) ((x / 4) % 10) }; })
                .Measure(scratch);
        assert(s.medianPx == 5.0f && s.p90Px == 8.0f);
        assert(s.incoherent == 47.0f / 479.0f);
    }

    // The median vector, per component, over every fourth vector: -12 px in the top 6 rows and 20 px in the
    // other 10 give 20; -5 and 3 in alternate rows, equal in number, give the upper one.
    {
        const Stats s =
            Readback(1920)
                .Fill([](uint32_t row, uint32_t)
                      { return std::pair<float, float> { row < 6 ? -12.0f : 20.0f, row % 2 ? 3.0f : -5.0f }; })
                .Measure(scratch);
        assert(s.medianX == 20.0f && s.medianY == 3.0f);
    }

    // The pair rule: a 2 px floor, and above it a quarter of the larger vector, in any direction.
    assert(!Disagree(0.0f, 0.0f, 1.5f, 0.0f) && Disagree(0.0f, 0.0f, 3.0f, 0.0f));
    assert(!Disagree(40.0f, 0.0f, 32.0f, 0.0f) && Disagree(40.0f, 0.0f, 25.0f, 0.0f));
    assert(!Disagree(0.0f, 40.0f, 0.0f, 30.0f) && Disagree(0.0f, 40.0f, 0.0f, 29.0f));
    assert(Disagree(30.0f, 0.0f, -30.0f, 0.0f) && Disagree(40.0f, 0.0f, 0.0f, 40.0f));

    // Alternating 8x8 blocks: every block boundary disagrees, 239 of a row's 479 pairs.
    {
        const Stats s = Readback(1920).Fill(Alternating(30.0f)).Measure(scratch);
        assert(s.medianPx == 30.0f && s.incoherent == 239.0f / 479.0f);
    }

    // Non-finite vectors are left out of everything but allZero, and break the pairs on both sides.
    {
        Readback readback(1920);
        readback.Fill(Pan40);
        readback.Set(0, 8, INFINITY, 0.0f); // row 0, sample 2: pairs (1,2) and (2,3)
        readback.Set(1, 0, NAN, NAN);       // row 1, sample 0: pair (0,1)
        const Stats s = readback.Measure(scratch);
        assert(s.vectors == kRows * 480 - 2 && s.pairs == kRows * 479 - 3);
        assert(s.medianPx == 40.0f && s.incoherent == 0.0f && !s.allZero);

        Readback still(1920);
        still.Fill([](uint32_t, uint32_t) { return std::pair<float, float> { 0.0f, 0.0f }; });
        still.Set(3, 40, INFINITY, 0.0f);
        assert(!still.Measure(scratch).allZero);
    }

    // No rows, no stride: nothing measured.
    assert(Measure(nullptr, kRows, 512, 64, kStride, scratch).vectors == 0);
    {
        const Readback readback(64);
        const Stats s =
            Measure(reinterpret_cast<const uint8_t*>(readback.halves.data()), kRows, readback.pitch, 64, 0, scratch);
        assert(s.vectors == 0 && s.pairs == 0 && s.incoherent == 0.0f);
    }

    // The summary window: mean and maximum per statistic, and empty after a reset.
    {
        Window window;
        window.Add(Sample(10.0f, false, 0.1f));
        window.Add(Sample(30.0f, false, 0.5f));
        assert(window.samples == 2 && window.MeanMedianPx() == 20.0f && window.maxMedianPx == 30.0f);
        assert(window.maxMedianOfWidth == 30.0f / 1920.0f && window.MeanP90Px() == 20.0f && window.maxP90Px == 30.0f);
        assert(std::fabs(window.MeanIncoherent() - 0.3f) < 1e-6f && window.maxIncoherent == 0.5f);
        window = {};
        assert(window.samples == 0 && window.MeanMedianPx() == 0.0f && window.maxIncoherent == 0.0f);
    }

    std::puts("stats cases passed");
}

// The cap alone: SynthesizedFastMotion as it always behaved, now named the cap.
static void fastMotionCases()
{
    SynthFgPolicy policy;
    assert(policy.Decide() == Generate);

    // Off by default: any motion generates.
    policy.ObserveMotion(Sample(500.0f), false);
    assert(policy.Decide() == Generate);

    policy.Configure(24.0f, 0.0f, 0.0f);
    policy.ObserveMotion(Sample(30.0f), false);
    assert(policy.Decide() == Reset);

    // Between the release level (75%) and the threshold: still fast.
    policy.ObserveMotion(Sample(20.0f), false);
    assert(policy.Decide() == Reset);

    // Two calm samples in a row end it; one is not enough, and a spike in between starts the count over.
    policy.ObserveMotion(Sample(10.0f), false);
    assert(policy.Decide() == Reset);
    policy.ObserveMotion(Sample(40.0f), false);
    policy.ObserveMotion(Sample(10.0f), false);
    assert(policy.Decide() == Reset);
    policy.ObserveMotion(Sample(10.0f), false);
    assert(policy.Decide() == Generate);

    // Exactly at the release level is not below it: not calm, as before.
    policy.ObserveMotion(Sample(30.0f), false);
    policy.ObserveMotion(Sample(18.0f), false);
    policy.ObserveMotion(Sample(18.0f), false);
    assert(policy.Decide() == Reset);

    // Turning it off mid-burst stops it at once.
    policy.ObserveMotion(Sample(99.0f), false);
    assert(policy.Fast());
    policy.Configure(0.0f, 0.0f, 0.0f);
    assert(!policy.Fast() && policy.Decide() == Generate);

    // A scene cut repeats exactly one frame, whatever the thresholds.
    policy.ObserveMotion(Sample(0.0f), true);
    assert(policy.Decide() == Reset);
    assert(policy.Decide() == Generate);

    std::puts("fast-motion cases passed");
}

// The incoherence rule (synthesized-frame-generation.md, "Coherence, not speed"), on rows measured from
// the readback layout.
static void coherenceCases()
{
    SynthMotionStats::Scratch scratch;
    const Stats pan = Readback(1920).Fill(Pan40).Measure(scratch);                 // 40 px, 2.1% of the width
    const Stats blocks = Readback(1920).Fill(Alternating(30.0f)).Measure(scratch); // +-30 px blocks, 1.6%
    const Stats small = Readback(1920).Fill(Alternating(8.0f)).Measure(scratch);   // +-8 px blocks, 0.4%
    assert(pan.incoherent == 0.0f && blocks.incoherent > 0.49f && small.incoherent > 0.49f);

    // A coherent 40 px pan interpolates, however long it lasts. The rule before this one repeated every
    // one of these frames at Generation Zero's 12 px (the cap case below keeps that behaviour).
    {
        SynthFgPolicy policy;
        policy.Configure(0.0f, 0.0f, 0.25f);
        for (int i = 0; i < 100; ++i)
        {
            policy.ObserveMotion(pan, false);
            assert(policy.Decide() == Generate);
        }
    }

    // Incoherent and large: repeated.
    {
        SynthFgPolicy policy;
        policy.Configure(0.0f, 0.0f, 0.25f);
        policy.ObserveMotion(blocks, false);
        assert(policy.Fast() && policy.Decide() == Reset);
    }

    // Incoherent but under 1% of the width: interpolated. A halo of a few pixels costs less than the ten
    // frames a Reset degrades.
    {
        SynthFgPolicy policy;
        policy.Configure(0.0f, 0.0f, 0.25f);
        for (int i = 0; i < 10; ++i)
            policy.ObserveMotion(small, false);
        assert(policy.Decide() == Generate);
    }

    // The cap still forces a repeat when set and exceeded, however coherent: alone, beside the rule, and at
    // Generation Zero's old 12 px, so an existing ini behaves as it did.
    {
        SynthFgPolicy policy;
        policy.Configure(24.0f, 0.0f, 0.0f);
        policy.ObserveMotion(pan, false);
        assert(policy.Decide() == Reset);

        policy.Configure(24.0f, 0.0f, 0.25f);
        policy.ObserveMotion(pan, false);
        assert(policy.Decide() == Reset);

        policy.Configure(12.0f, 0.0f, 0.0f);
        policy.ObserveMotion(pan, false);
        assert(policy.Decide() == Reset);
    }

    // Both off, the default: nothing repeats, incoherent or not.
    {
        SynthFgPolicy policy;
        policy.Configure(0.0f, 0.0f, 0.0f);
        policy.ObserveMotion(blocks, false);
        policy.ObserveMotion(Sample(500.0f, false, 1.0f), false);
        assert(!policy.FastMotionConfigured() && policy.Decide() == Generate);
    }

    // Hysteresis: held while either level stays at 75% of its threshold or above, ended by two calm
    // samples in a row.
    {
        SynthFgPolicy policy;
        policy.Configure(0.0f, 0.0f, 0.25f);
        policy.ObserveMotion(Sample(30.0f, false, 0.4f), false); // 1.6% of the width, 40% incoherent
        assert(policy.Decide() == Reset);
        policy.ObserveMotion(Sample(30.0f, false, 0.2f), false); // 20%: under 25%, over its release 18.75%
        assert(policy.Decide() == Reset);
        policy.ObserveMotion(Sample(16.0f, false, 0.4f), false); // 0.83%: under 1%, over its release 0.75%
        assert(policy.Decide() == Reset);
        policy.ObserveMotion(pan, false); // calm: coherent
        assert(policy.Decide() == Reset);
        policy.ObserveMotion(Sample(30.0f, false, 0.2f), false); // held again: the count starts over
        policy.ObserveMotion(pan, false);
        assert(policy.Decide() == Reset);
        policy.ObserveMotion(Sample(12.0f, false, 0.9f), false); // calm: 0.63% of the width
        assert(policy.Decide() == Generate);
    }

    // A threshold change decides afresh: a trigger switched off while it holds the response stops at once.
    {
        SynthFgPolicy policy;
        policy.Configure(24.0f, 0.0f, 0.25f);
        policy.ObserveMotion(pan, false); // over the cap
        assert(policy.Decide() == Reset);
        policy.Configure(0.0f, 0.0f, 0.25f);
        assert(policy.Decide() == Generate);
        policy.ObserveMotion(pan, false);
        assert(policy.Decide() == Generate);

        policy.ObserveMotion(blocks, false);
        assert(policy.Decide() == Reset);
        policy.Configure(0.0f, 0.0f, 0.0f);
        assert(!policy.Fast() && policy.Decide() == Generate);

        // Configured with the same values every frame, nothing is reset.
        policy.Configure(0.0f, 0.0f, 0.25f);
        policy.ObserveMotion(blocks, false);
        policy.Configure(0.0f, 0.0f, 0.25f);
        assert(policy.Decide() == Reset);
    }

    std::puts("coherence cases passed");
}

static void floorCases()
{
    SynthFgPolicy policy;
    double now = 1000.0;

    // Off by default, however slow.
    for (int i = 0; i < 50; ++i)
        policy.ObserveBaseFrame(now += 100.0);
    assert(policy.Decide() == Generate);

    // 25 fps against a 30 fps floor: skipped.
    policy.Configure(0.0f, 30.0f, 0.0f);
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 40.0);
    assert(policy.BelowFloor() && policy.Decide() == Skip);

    // 32 fps is above the floor but inside the 15% hysteresis: still skipped.
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 31.25);
    assert(policy.Decide() == Skip);

    // 50 fps: generating again.
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 20.0);
    assert(!policy.BelowFloor() && policy.Decide() == Generate);

    // A pause (a menu, a load) is not a frame rate: it restarts the average instead of skipping.
    policy.ObserveBaseFrame(now += 2000.0);
    assert(policy.Decide() == Generate && policy.BaseFps() == 0.0);

    // The floor wins over a pending cut: nothing is fed below it.
    policy.ObserveMotion(Sample(0.0f), true);
    for (int i = 0; i < 100; ++i)
        policy.ObserveBaseFrame(now += 50.0);
    assert(policy.Decide() == Skip);

    std::puts("floor cases passed");
}

static void duplicateCases()
{
    // 30 fps content presented at 60: identical and moving frames alternate. Advised once.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < SynthFgPolicy::kWindow; ++i)
        {
            policy.ObserveMotion(Sample(i % 2 ? 0.0f : 6.0f, i % 2 == 1), false);
            if (i + 1 < SynthFgPolicy::kWindow)
                assert(!policy.TakeDuplicateAdvice());
        }
        assert(policy.IdenticalInWindow() == SynthFgPolicy::kWindow / 2);
        assert(policy.TakeDuplicateAdvice());
        assert(!policy.TakeDuplicateAdvice());

        // Counts stay exact once the ring wraps.
        for (uint32_t i = 0; i < 3 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(Sample(0.0f, i % 2 == 1), false);
        assert(policy.IdenticalInWindow() == SynthFgPolicy::kWindow / 2);
        assert(policy.AlternationsInWindow() == SynthFgPolicy::kWindow - 1);
    }

    // A paused or static game: every sample identical, no moving frames. Not a cadence.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(Sample(0.0f, true), false);
        assert(!policy.TakeDuplicateAdvice());
    }

    // A moving game with an occasional identical frame: not a cadence either.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(Sample(8.0f, i % 10 == 0), false);
        assert(!policy.TakeDuplicateAdvice());
    }

    // Identical frames in long runs rather than interleaved (a game pausing now and then): not advised.
    {
        SynthFgPolicy policy;
        for (uint32_t i = 0; i < 2 * SynthFgPolicy::kWindow; ++i)
            policy.ObserveMotion(Sample(8.0f, (i / 15) % 2 == 1), false);
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
    statsCases();
    fastMotionCases();
    coherenceCases();
    floorCases();
    duplicateCases();
    hudCases();
    std::puts("fg synth policy: handoff, warming, stats, fast-motion, coherence, floor, duplicate and hud cases "
              "passed");
    return 0;
}
