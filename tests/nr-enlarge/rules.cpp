// Transfer 2's rules, from the production header: what the resolve is sent and why, the carrier's
// arithmetic, the menu's table, when the private SR's history resets, and where its guides come from.
#include <dlssnr/DlssNr_Enlarge.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

using namespace DlssNr::Enlarge;

namespace
{
int cases = 0;

// A frame where Transfer 2 can run: 3440x1440 with the model at half size, after the upscaler, real
// guides, no debug view, the SR built and past its creation.
Inputs Runnable()
{
    Inputs in;
    in.configured = kTransferDlss;
    in.width = 3440;
    in.height = 1440;
    in.workWidth = 1720;
    in.workHeight = 720;
    in.state = State::Ready;
    return in;
}

void Expect(const Inputs& in, uint32_t transfer, Why why, bool keep, bool build, bool evaluate)
{
    const Decision d = Decide(in);
    if (d.transfer != transfer || d.why != why || d.keep != keep || d.build != build || d.evaluate != evaluate)
    {
        std::fprintf(stderr, "Decide: got transfer %u %s keep %d build %d evaluate %d, wanted %u %s %d %d %d\n",
                     d.transfer, Token(d.why), d.keep, d.build, d.evaluate, transfer, Token(why), keep, build,
                     evaluate);
        assert(false);
    }
    ++cases;
}

void DecisionTable()
{
    // Every value that is not 2 goes through as it always did, with nothing kept or built, whatever else is
    // true: 0, 1 and 3 are the shipped modes, 4 and 99 run Classic in the shader as before.
    for (uint32_t configured : { 0u, 1u, 3u, 4u, 99u })
    {
        for (State state : { State::Missing, State::Waiting, State::Ready, State::Failed })
        {
            Inputs in = Runnable();
            in.configured = configured;
            in.state = state;
            Expect(in, configured, Why::NotSelected, false, false, false);
            in.vulkan = true;
            in.workWidth = in.width;
            in.workHeight = in.height;
            Expect(in, configured, Why::NotSelected, false, false, false);
        }
    }

    // The ordinary path: built and ready runs DLSS; missing builds it and sends 1; built but not yet past its
    // creation waits on 1; failed sends 1 and lets it go.
    Inputs in = Runnable();
    Expect(in, 2, Why::Running, true, false, true);
    in.state = State::Missing;
    Expect(in, 1, Why::WarmingUp, true, true, false);
    in.state = State::Waiting;
    Expect(in, 1, Why::WarmingUp, true, false, false);
    in.state = State::Failed;
    Expect(in, 1, Why::Failed, false, false, false);

    // Native Vulkan never builds one.
    in = Runnable();
    in.vulkan = true;
    Expect(in, 1, Why::Vulkan, false, false, false);

    // The driver proxy and peripheral compression send 1 and let go, each with its own reason, in every state:
    // the log must not read as Transfer having been changed. Any other Transfer goes through untouched under
    // either -- 3 stays sharp and 0 Classic under the periphery, as its design note says.
    for (State state : { State::Missing, State::Waiting, State::Ready, State::Failed })
    {
        in = Runnable();
        in.state = state;
        in.proxy = true;
        Expect(in, 1, Why::Proxy, false, false, false);
        in.periphery = true;
        Expect(in, 1, Why::Proxy, false, false, false);
        in.proxy = false;
        Expect(in, 1, Why::Periphery, false, false, false);
    }
    for (uint32_t configured : { 0u, 1u, 3u })
    {
        in = Runnable();
        in.configured = configured;
        in.periphery = true;
        Expect(in, configured, Why::NotSelected, false, false, false);
        in.periphery = false;
        in.proxy = true;
        Expect(in, configured, Why::NotSelected, false, false, false);
    }

    // At and above the frame's size there is nothing to enlarge, and nothing is kept: this is the case
    // wilsjo2's build created and released every frame (supersampling). Equal size, 2x, and one axis above.
    for (auto [w, h] : { std::pair { 3440u, 1440u }, std::pair { 6880u, 2880u }, std::pair { 3441u, 1440u },
                         std::pair { 1720u, 1441u } })
    {
        in = Runnable();
        in.workWidth = w;
        in.workHeight = h;
        Expect(in, 1, Why::FullSize, false, false, false);
    }

    // Below a third of the frame is past DLSS's own range (Ultra Performance), and is let go: the pass's floor of
    // a quarter, and one axis short. A third exactly runs.
    for (auto [w, h] : { std::pair { 860u, 360u }, std::pair { 1146u, 480u }, std::pair { 1720u, 479u } })
    {
        in = Runnable();
        in.workWidth = w;
        in.workHeight = h;
        Expect(in, 1, Why::TooSmall, false, false, false);
    }
    in = Runnable();
    in.workWidth = 1147;
    in.workHeight = 480;
    Expect(in, 2, Why::Running, true, false, true);
    assert(WithinDlssRange(3440, 1440, 1147, 480) && !WithinDlssRange(3440, 1440, 1146, 480));
    ++cases;

    // Below on one axis and equal on the other is still below.
    in = Runnable();
    in.workWidth = 3440;
    in.workHeight = 1439;
    Expect(in, 2, Why::Running, true, false, true);
    assert(BelowFrame(3440, 1440, 860, 360) && !BelowFrame(64, 64, 64, 64) && !BelowFrame(100, 100, 50, 101));
    ++cases;

    // A frame that cannot use it keeps an existing one idle and builds none. Before the upscaler, zero guides,
    // guides the build cannot bring to the working size, and the debug view that shows the model's own answer.
    struct Idle
    {
        void (*apply)(Inputs&);
        Why why;
    };
    const Idle idle[] = {
        { [](Inputs& i) { i.beforeUpscale = true; }, Why::BeforeUpscale },
        { [](Inputs& i) { i.realGuides = false; }, Why::NoGuides },
        { [](Inputs& i) { i.guidesUsable = false; }, Why::GuidesUnmatched },
        { [](Inputs& i) { i.debugView = 2; }, Why::ModelView },
    };
    for (const auto& c : idle)
    {
        for (State state : { State::Missing, State::Waiting, State::Ready })
        {
            in = Runnable();
            in.state = state;
            c.apply(in);
            Expect(in, 1, c.why, true, false, false);
        }
        in = Runnable();
        in.state = State::Failed;
        c.apply(in);
        Expect(in, 1, c.why, false, false, false);
    }

    // The other debug views do not stop it: proxy and difference show this transfer's own numbers.
    for (uint32_t view : { 0u, 1u, 3u })
    {
        in = Runnable();
        in.debugView = view;
        Expect(in, 2, Why::Running, true, false, true);
    }

    // Order: Vulkan before size, size before the per-frame reasons.
    in = Runnable();
    in.vulkan = true;
    in.workWidth = in.width;
    in.workHeight = in.height;
    in.beforeUpscale = true;
    Expect(in, 1, Why::Vulkan, false, false, false);
    in.vulkan = false;
    Expect(in, 1, Why::FullSize, false, false, false);
}

void VulkanAndMenu()
{
    assert(VulkanTransfer(2) == 1);
    for (uint32_t v : { 0u, 1u, 3u, 4u, 99u })
        assert(VulkanTransfer(v) == v);

    // Index 2 is still Transfer 3, as it was before 2 existed; 2 is appended. Unknown values show as Classic.
    assert(MenuIndex(0) == 0 && MenuIndex(1) == 1 && MenuIndex(3) == 2 && MenuIndex(2) == 3);
    assert(MenuIndex(4) == 0 && MenuIndex(99) == 0);
    for (int i = 0; i < kMenuEntries; ++i)
        assert(MenuIndex(MenuTransfer(i)) == i);
    assert(MenuTransfer(-1) == 0 && MenuTransfer(kMenuEntries) == 0);
    cases += 2;
}

void HistoryAndMotion()
{
    // Consecutive frames keep the history; anything else resets it.
    assert(!ResetHistory(false, false, false, 11, 10));
    assert(ResetHistory(true, false, false, 11, 10));  // the model reset
    assert(ResetHistory(false, true, false, 11, 10));  // the game reset
    assert(ResetHistory(false, false, true, 11, 10));  // frame hold toggled
    assert(ResetHistory(false, false, false, 1, 0));   // the first evaluation of a feature
    assert(ResetHistory(false, false, false, 12, 10)); // an NR frame went by without the SR
    assert(ResetHistory(false, false, false, 10, 10)); // the counter did not move
    assert(ResetHistory(false, false, false, 9, 10));  // or went back
    ++cases;

    // The model's own rule, and 0 while held. Cyberpunk at DLSS Quality, half scale: low-resolution vectors
    // measured against the 2293 render width, handed at 1720.
    assert(MotionScale(2293.0f, 1720, 2293, false) == 1720.0f);
    assert(MotionScale(2293.0f, 1720, 2293, true) == 0.0f);
    assert(MotionScale(-1.0f, 960, 1920, false) == -0.5f);
    assert(MotionScale(3.0f, 960, 0, false) == 3.0f);
    ++cases;

    // A present when the counter runs; two NR frames when it does not; never the creation frame itself.
    assert(!CreationCrossed(100, 100, 7, 7) && CreationCrossed(100, 101, 7, 7));
    assert(!CreationCrossed(0, 0, 7, 8) && CreationCrossed(0, 0, 7, 9));
    assert(!CreationCrossed(0, 5, 7, 8) && CreationCrossed(0, 5, 7, 9));
    ++cases;
}

void GuideSources()
{
    // The model's matched pair wins whatever the regions are.
    assert(Guides(true, false, 1720, 720, 3, 4, 2293, 960, 0, 0, 3440, 1440) == GuideSource::Matched);
    // The game's own when both regions already are the working size at the origin.
    assert(Guides(false, false, 1720, 720, 0, 0, 1720, 720, 0, 0, 1720, 720) == GuideSource::Game);
    // Otherwise the resample, when the build has it: a base off the origin, a smaller region (Ultra
    // Performance under a half-size model), or a larger one the model's own match did not take.
    assert(Guides(false, true, 1720, 720, 8, 0, 1720, 720, 0, 0, 1720, 720) == GuideSource::Resample);
    assert(Guides(false, true, 1720, 720, 0, 0, 1147, 480, 0, 0, 1147, 480) == GuideSource::Resample);
    assert(Guides(false, true, 1720, 720, 0, 0, 2293, 960, 0, 0, 2293, 960) == GuideSource::Resample);
    assert(Guides(false, false, 1720, 720, 0, 0, 1147, 480, 0, 0, 1147, 480) == GuideSource::None);
    assert(Guides(false, true, 1720, 720, 0, 0, 0, 480, 0, 0, 1147, 480) == GuideSource::None);
    ++cases;
}

// IEEE binary16, round to nearest even, as an RGBA16F texture stores what the carrier pass writes.
float Half(float value) { return static_cast<float>(static_cast<_Float16>(value)); }

void Carrier()
{
    // No edit is exactly neutral, through FP16 and back.
    assert(EncodeCarrier(0.0f) == 0.5f && Half(0.5f) == 0.5f && DecodeCarrier(Half(EncodeCarrier(0.0f))) == 0.0f);
    assert(EncodeCarrier(-0.0f) == 0.5f);

    // Non-finite in, neutral out: a NaN edit is no edit, and a NaN or infinite carrier decodes to nothing.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    assert(EncodeCarrier(nan) == 0.5f && EncodeCarrier(inf) == 0.5f);
    assert(DecodeCarrier(nan) == 0.0f && DecodeCarrier(inf) == 0.0f && DecodeCarrier(-inf) == 0.0f);

    // The carrier stays inside (0, 1) for any finite edit, monotonic and odd around 0.5.
    float previous = 0.0f;
    for (float d = -64.0f; d <= 64.0f; d += 0.03125f)
    {
        const float c = EncodeCarrier(d);
        assert(c > 0.0f && c < 1.0f && c >= previous);
        assert(std::fabs((c - 0.5f) + (EncodeCarrier(-d) - 0.5f)) < 1e-6f);
        previous = c;
    }

    // The clamp: DLSS can ring past the carrier's range, and the inverse has poles at 0 and 1. At the clamp an
    // edit is (1/64) * 0.999 / 0.001, 15.6, and nothing beyond it is reachable.
    const float bound = kCarrierScale * kCarrierClamp / (1.0f - kCarrierClamp);
    assert(std::fabs(bound - 15.609375f) < 0.01f);
    for (float c : { 1.0f, 1.5f, 40.0f })
    {
        assert(DecodeCarrier(c) == DecodeCarrier(1.0f));
        assert(std::isfinite(DecodeCarrier(c)) && std::fabs(DecodeCarrier(c) - bound) < 0.01f);
        assert(DecodeCarrier(1.0f - c) == -DecodeCarrier(c));
    }

    // The round trip in float is exact to rounding, across six decades.
    for (float d = 1e-5f; d < 10.0f; d *= 1.37f)
    {
        for (float s : { 1.0f, -1.0f })
        {
            const float back = DecodeCarrier(EncodeCarrier(s * d));
            assert(std::fabs(back - s * d) <= 1e-3f * d + 1e-7f);
        }
    }

    // Through FP16, which is what the carrier texture and DLSS's output store. With the 1/64 scale an edit from
    // a ten-thousandth to one comes back within 8%. With a unit scale -- the speckles wilsjo2 measured -- an
    // edit of 0.0003 sits a third of an FP16 step above 0.5 and comes back as no edit at all; at 1/64 it lands
    // nineteen steps above and comes back within 4%.
    auto unitEncode = [](float d) { return 0.5f + 0.5f * d / (1.0f + std::fabs(d)); };
    auto unitDecode = [](float c)
    {
        const float s = std::fmax(-kCarrierClamp, std::fmin(kCarrierClamp, 2.0f * c - 1.0f));
        return s / (1.0f - std::fabs(s));
    };
    float worstScaled = 0.0f;
    for (float d = 1e-4f; d <= 1.0f; d *= 1.05f)
    {
        for (float s : { 1.0f, -1.0f })
        {
            const float err = std::fabs(DecodeCarrier(Half(EncodeCarrier(s * d))) - s * d) / d;
            worstScaled = std::fmax(worstScaled, err);
        }
    }
    assert(worstScaled < 0.08f);
    assert(unitDecode(Half(unitEncode(3e-4f))) == 0.0f);
    assert(std::fabs(DecodeCarrier(Half(EncodeCarrier(3e-4f))) - 3e-4f) / 3e-4f < 0.04f);
    cases += 5;
}
} // namespace

int main()
{
    DecisionTable();
    VulkanAndMenu();
    HistoryAndMotion();
    GuideSources();
    Carrier();
    std::printf("PASS: %d Transfer 2 rule cases: decision table (every transfer, state and reason, Vulkan, "
                "supersampling), menu table, history, motion, guides, carrier through FP16\n",
                cases);
    return 0;
}
