// The present-guide identity decision (retoque/dlssnr/DlssNr_PresentGuideIdentity.h) on the production header:
// every rule alone, the first-failing order, narrowing only, the swapchain stamp's null semantics, the registry and
// the frame counter, scenarios on the production ring, and step 0's window and line.

#include <dlssnr/DlssNr_PresentGuideIdentity.h>

#include <cassert>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace DlssNr::PresentGuideIdentity;
using DlssNr::PresentGuides::kPresentGuideMaxAgeMs;
using DlssNr::PresentGuides::Ring;

namespace
{
int g_identities[8];
const void* const D1 = &g_identities[0];
const void* const D2 = &g_identities[1];
const void* const SC1 = &g_identities[2];
const void* const SC2 = &g_identities[3];
const void* const SC3 = &g_identities[4];

int g_checks = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                                 \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

bool Never(void*) { return false; }

// A ring and its stamps, filled the way CaptureTemporal fills them: stamp, then commit.
struct Fixture
{
    Ring ring;
    Stamps stamps {};

    size_t Capture(const Stamp& stamp, int64_t atMs)
    {
        const size_t slot = ring.AcquireForRecord(Never);
        CHECK(slot < kRingSize);
        stamps[slot] = stamp;
        ring.CommitRecord(slot, 100, 100, false, atMs, nullptr, nullptr);
        return slot;
    }
};

Stamp Good()
{
    Stamp s;
    s.device = D1;
    s.outputWidth = 1920;
    s.outputHeight = 1080;
    s.renderWidth = 1280;
    s.renderHeight = 720;
    s.presentEpoch = 9;
    s.evalsSincePresent = 1;
    s.swapchain = SC1;
    return s;
}

Present GoodPresent()
{
    Present p;
    p.device = D1;
    p.width = 1920;
    p.height = 1080;
    p.swapchain = SC1;
    p.swapchainsOnDevice = 1;
    p.epoch = 10;
    p.lastConsumed = 0;
    p.nowMs = 1010;
    return p;
}

Reason DecideOne(const Stamp& stamp, const Present& present)
{
    Fixture f;
    const size_t slot = f.Capture(stamp, 1000);
    const Decision d = Decide(slot, f.ring, f.stamps, present);
    CHECK(d.slot == (d.reason == Reason::Accepted ? slot : kRingSize));
    return d.reason;
}

void EveryRuleAlone()
{
    CHECK(DecideOne(Good(), GoodPresent()) == Reason::Accepted);

    // Rule 1: same device; unknown on either side is not a match.
    {
        Stamp s = Good();
        s.device = D2;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Device);
        s.device = nullptr;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Device);
        Present p = GoodPresent();
        p.device = nullptr;
        CHECK(DecideOne(s, p) == Reason::Device);
    }

    // Rule 2: the output's extent is the backbuffer's, and its base is the origin. An unknown output is not a match,
    // even against an unknown backbuffer.
    {
        Stamp s = Good();
        s.outputWidth = 2560;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Extent);
        s = Good();
        s.outputHeight = 1440;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Extent);
        s = Good();
        s.outputWidth = s.outputHeight = 0;
        Present p = GoodPresent();
        p.width = p.height = 0;
        CHECK(DecideOne(s, p) == Reason::Extent);
        s = Good();
        s.outputBaseX = 8;
        CHECK(DecideOne(s, GoodPresent()) == Reason::OutputBase);
        s = Good();
        s.outputBaseY = 1;
        CHECK(DecideOne(s, GoodPresent()) == Reason::OutputBase);
    }

    // Rule 3, a stamp: it must be the presenting swapchain, however many there are.
    {
        Stamp s = Good();
        s.swapchain = SC2;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Swapchain);
        Present p = GoodPresent();
        p.swapchainsOnDevice = 3;
        CHECK(DecideOne(Good(), p) == Reason::Accepted);
        p.swapchain = nullptr;
        CHECK(DecideOne(Good(), p) == Reason::Swapchain);
    }

    // Rule 3, no stamp: "unknown" matches only a present that sees exactly one swapchain on the device.
    {
        Stamp s = Good();
        s.swapchain = nullptr;
        Present p = GoodPresent();
        p.swapchainsOnDevice = 1;
        CHECK(DecideOne(s, p) == Reason::Accepted);
        p.swapchainsOnDevice = 2;
        CHECK(DecideOne(s, p) == Reason::Swapchain);
        p.swapchainsOnDevice = 0;
        CHECK(DecideOne(s, p) == Reason::Swapchain);
    }

    // Rule 4: the only capture of its frame at its extent.
    {
        Stamp s = Good();
        s.evalsSincePresent = 2;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Ambiguous);
        s.evalsSincePresent = kOrdinalUnknown;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Ambiguous);

        // A newer capture of the same frame and extent still in the ring, not yet eligible: ambiguous.
        Fixture f;
        const size_t picked = f.Capture(Good(), 1000);
        Stamp second = Good();
        second.evalsSincePresent = 2;
        f.Capture(second, 1001);
        CHECK(Decide(picked, f.ring, f.stamps, GoodPresent()).reason == Reason::Ambiguous);
        CHECK(FrameCaptures(picked, f.ring, f.stamps) == 2);

        // A newer capture at another extent, or of another frame, or on another device: not ambiguous.
        for (int variant = 0; variant < 3; ++variant)
        {
            Fixture g;
            const size_t p = g.Capture(Good(), 1000);
            Stamp other = Good();
            if (variant == 0)
                other.outputWidth = 512;
            if (variant == 1)
                other.presentEpoch = 10;
            if (variant == 2)
                other.device = D2;
            g.Capture(other, 1001);
            CHECK(Decide(p, g.ring, g.stamps, GoodPresent()).reason == Reason::Accepted);
            CHECK(FrameCaptures(p, g.ring, g.stamps) == 1);
        }
    }

    // Rule 5: a capture an earlier present already read.
    {
        Fixture f;
        const size_t slot = f.Capture(Good(), 1000);
        Present p = GoodPresent();
        p.lastConsumed = f.ring.GetSlot(slot).sequence;
        CHECK(Decide(slot, f.ring, f.stamps, p).reason == Reason::Consumed);
        p.lastConsumed = f.ring.GetSlot(slot).sequence - 1;
        CHECK(Decide(slot, f.ring, f.stamps, p).reason == Reason::Accepted);
    }

    // Rule 6: no more than kLag boundaries behind; a stamp ahead of the present is not "behind".
    {
        static_assert(kLag == 1, "the cases below assume the note's starting value");
        Stamp s = Good();
        Present p = GoodPresent();
        s.presentEpoch = p.epoch - 2;
        CHECK(DecideOne(s, p) == Reason::Lag);
        s.presentEpoch = p.epoch - 1;
        CHECK(DecideOne(s, p) == Reason::Accepted);
        s.presentEpoch = p.epoch;
        CHECK(DecideOne(s, p) == Reason::Accepted);
        s.presentEpoch = p.epoch + 1;
        CHECK(DecideOne(s, p) == Reason::Accepted);
    }

    // Rule 7: the ring's age limit, with the present's clock; a clock of zero does not judge age.
    {
        Present p = GoodPresent();
        p.nowMs = 1000 + kPresentGuideMaxAgeMs + 1;
        CHECK(DecideOne(Good(), p) == Reason::Stale);
        p.nowMs = 1000 + kPresentGuideMaxAgeMs;
        CHECK(DecideOne(Good(), p) == Reason::Accepted);
        p.nowMs = 0;
        CHECK(DecideOne(Good(), p) == Reason::Accepted);
    }

    // Rule 8: a real render extent, inside the output.
    {
        Stamp s = Good();
        s.renderWidth = 0;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Subrect);
        s = Good();
        s.renderHeight = 0;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Subrect);
        s = Good();
        s.renderWidth = 1921;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Subrect);
        s = Good();
        s.renderHeight = 1081;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Subrect);
        s = Good();
        s.renderWidth = 1920;
        s.renderHeight = 1080;
        CHECK(DecideOne(s, GoodPresent()) == Reason::Accepted);
    }

    // No pick, or a pick the ring does not hold: no capture, which is not an identity refusal.
    {
        Fixture f;
        CHECK(Decide(kRingSize, f.ring, f.stamps, GoodPresent()).reason == Reason::NoCapture);
        CHECK(Decide(kRingSize + 3, f.ring, f.stamps, GoodPresent()).slot == kRingSize);
        CHECK(Decide(0, f.ring, f.stamps, GoodPresent()).reason == Reason::NoCapture);
    }
}

// A capture that fails every rule reports the first; mending them one at a time walks the table in order.
void FirstFailingOrder()
{
    Fixture f;
    Stamp s;
    s.device = D2;
    s.outputWidth = 2560;
    s.outputHeight = 1440;
    s.outputBaseX = 4;
    s.outputBaseY = 4;
    s.swapchain = SC2;
    s.evalsSincePresent = 2;
    s.presentEpoch = 1;
    s.renderWidth = 0;
    s.renderHeight = 0;
    const size_t slot = f.Capture(s, 1000);

    Present p = GoodPresent();
    p.lastConsumed = f.ring.GetSlot(slot).sequence;
    p.nowMs = 2000;

    Stamp& st = f.stamps[slot];
    const auto now = [&] { return Decide(slot, f.ring, f.stamps, p).reason; };

    CHECK(now() == Reason::Device);
    st.device = D1;
    CHECK(now() == Reason::Extent);
    st.outputWidth = 1920;
    st.outputHeight = 1080;
    CHECK(now() == Reason::OutputBase);
    st.outputBaseX = st.outputBaseY = 0;
    CHECK(now() == Reason::Swapchain);
    st.swapchain = SC1;
    CHECK(now() == Reason::Ambiguous);
    st.evalsSincePresent = 1;
    CHECK(now() == Reason::Consumed);
    p.lastConsumed = 0;
    CHECK(now() == Reason::Lag);
    st.presentEpoch = 9;
    CHECK(now() == Reason::Stale);
    p.nowMs = 1010;
    CHECK(now() == Reason::Subrect);
    st.renderWidth = 1280;
    st.renderHeight = 720;
    CHECK(now() == Reason::Accepted);
}

// An oracle written as a table, independent of Decide's control flow: the rules that fail, in table order.
std::vector<Reason> Failing(size_t picked, const Ring& ring, const Stamps& stamps, const Present& p)
{
    std::vector<Reason> out;
    const auto& slot = ring.GetSlot(picked);
    const Stamp& s = stamps[picked];
    const uint64_t lag = p.epoch > s.presentEpoch ? p.epoch - s.presentEpoch : 0;

    const std::pair<Reason, bool> rules[] = {
        { Reason::Device, s.device == nullptr || s.device != p.device },
        { Reason::Extent,
          s.outputWidth == 0 || s.outputHeight == 0 || s.outputWidth != p.width || s.outputHeight != p.height },
        { Reason::OutputBase, s.outputBaseX != 0 || s.outputBaseY != 0 },
        { Reason::Swapchain, s.swapchain != nullptr ? s.swapchain != p.swapchain : p.swapchainsOnDevice != 1 },
        { Reason::Ambiguous, FrameCaptures(picked, ring, stamps) != 1 },
        { Reason::Consumed, slot.sequence <= p.lastConsumed },
        { Reason::Lag, lag > kLag },
        { Reason::Stale, p.nowMs > 0 && slot.capturedAtMs > 0 && p.nowMs - slot.capturedAtMs > kPresentGuideMaxAgeMs },
        { Reason::Subrect, s.renderWidth == 0 || s.renderHeight == 0 || s.renderWidth > s.outputWidth ||
                               s.renderHeight > s.outputHeight },
    };

    for (const auto& [reason, fails] : rules)
    {
        if (fails)
            out.push_back(reason);
    }

    return out;
}

// Random rings, stamps and presents: each field starts from a matching value and is replaced from a small pool now
// and then, so that every rule both holds and fails often. The answer is always the pick or nothing, accepted exactly
// when no rule fails, and otherwise the first failing rule.
void NarrowingOnly()
{
    std::mt19937_64 rng(20261013);
    const void* devices[] = { nullptr, D1, D2 };
    const void* chains[] = { nullptr, SC1, SC2 };
    const uint32_t widths[] = { 0, 1280, 1920, 2560 };
    const uint32_t heights[] = { 0, 720, 1080, 1440 };
    const auto sometimes = [&rng](auto good, auto& pool)
    { return rng() % 8 == 0 ? pool[rng() % std::size(pool)] : good; };

    size_t accepted = 0, none = 0;
    size_t reasons[kReasons] {};

    for (int round = 0; round < 50000; ++round)
    {
        Fixture f;
        int64_t t = 1000;
        const int captures = static_cast<int>(rng() % 7);

        for (int c = 0; c < captures; ++c)
        {
            const Stamp good = Good();
            Stamp s;
            s.device = sometimes(good.device, devices);
            s.outputWidth = sometimes(good.outputWidth, widths);
            s.outputHeight = sometimes(good.outputHeight, heights);
            s.outputBaseX = rng() % 12 == 0 ? 8 : 0;
            s.outputBaseY = rng() % 12 == 0 ? 2 : 0;
            s.renderWidth = sometimes(good.renderWidth, widths);
            s.renderHeight = sometimes(good.renderHeight, heights);
            s.presentEpoch = rng() % 6 == 0 ? 5 + rng() % 6 : good.presentEpoch;
            s.evalsSincePresent = rng() % 8 == 0 ? 1 + static_cast<uint32_t>(rng() % 3) : 1;
            s.swapchain = sometimes(good.swapchain, chains);
            t += static_cast<int64_t>(rng() % 60);
            const size_t slot = f.Capture(s, t);

            if (rng() % 9 == 0)
                f.ring.InvalidateSlot(slot);
        }

        const Present good = GoodPresent();
        Present p;
        p.device = sometimes(good.device, devices);
        p.width = sometimes(good.width, widths);
        p.height = sometimes(good.height, heights);
        p.swapchain = sometimes(good.swapchain, chains);
        p.swapchainsOnDevice = rng() % 6 == 0 ? static_cast<uint32_t>(rng() % 3) : 1;
        p.epoch = good.epoch;
        p.lastConsumed = rng() % 6 == 0 ? rng() % 5 : 0;
        p.nowMs = rng() % 8 == 0 ? 0 : t + static_cast<int64_t>(rng() % 6 == 0 ? 300 : 10);

        const size_t picked = rng() % (kRingSize + 1);
        const Decision d = Decide(picked, f.ring, f.stamps, p);

        CHECK(d.slot == picked || d.slot == kRingSize);
        CHECK((d.slot != kRingSize) == (d.reason == Reason::Accepted));

        if (picked >= kRingSize || !f.ring.GetSlot(picked).recorded)
        {
            CHECK(d.reason == Reason::NoCapture);
            ++none;
            continue;
        }

        const auto failing = Failing(picked, f.ring, f.stamps, p);
        CHECK(failing.empty() == (d.reason == Reason::Accepted));
        if (!failing.empty())
            CHECK(failing.front() == d.reason);

        accepted += d.reason == Reason::Accepted ? 1 : 0;
        ++reasons[static_cast<size_t>(d.reason)];
    }

    // Every reason was the answer somewhere, so the order was exercised, not just the happy path.
    CHECK(accepted > 0 && none > 0);
    for (size_t r = static_cast<size_t>(Reason::Device); r < kReasons; ++r)
        CHECK(reasons[r] > 0);
}

void RegistryAndCounter()
{
    // Presenters: per device, live for kPresenterLiveMs after the last present, sole or not.
    {
        Presenters r;
        CHECK(r.Count(D1, 0) == 0 && r.Sole(D1, 0) == nullptr);
        r.Note(D1, SC1, 0);
        CHECK(r.Count(D1, 0) == 1 && r.Sole(D1, 0) == SC1);
        CHECK(r.Count(D2, 0) == 0 && r.Sole(D2, 0) == nullptr);
        r.Note(D1, SC2, 100);
        CHECK(r.Count(D1, 100) == 2 && r.Sole(D1, 100) == nullptr);
        r.Note(D1, SC2, 100);
        CHECK(r.Count(D1, 100) == 2);
        CHECK(r.Count(D1, kPresenterLiveMs) == 2);
        CHECK(r.Count(D1, kPresenterLiveMs + 1) == 1 && r.Sole(D1, kPresenterLiveMs + 1) == SC2);
        r.Note(D1, SC1, 2000);
        CHECK(r.Count(D1, 2000) == 1 && r.Sole(D1, 2000) == SC1);
        r.Note(D1, nullptr, 2000);
        CHECK(r.Count(D1, 2000) == 1);

        // More presenters than entries: the count is capped, never below two once two are live.
        Presenters full;
        int extra[6];
        for (int i = 0; i < 6; ++i)
            full.Note(D2, &extra[i], i);
        CHECK(full.Count(D2, 6) == kPresenters && full.Sole(D2, 6) == nullptr);

        // Full and all live: the one that presented longest ago makes room, not the first entry.
        Presenters order;
        int chains[5];
        order.Note(D1, &chains[0], 0);
        order.Note(D2, &chains[1], 1);
        order.Note(D2, &chains[2], 2);
        order.Note(D2, &chains[3], 3);
        order.Note(D1, &chains[0], 4); // the first entry presents again, so it is the newest
        order.Note(D2, &chains[4], 5); // replaces chains[1], presented at 1
        CHECK(order.Count(D1, 5) == 1 && order.Sole(D1, 5) == &chains[0]);
        CHECK(order.Count(D2, 5) == 3);
        r.Reset();
        CHECK(r.Count(D1, 2000) == 0);
    }

    // FrameCounter: the ordinal per device and output extent, restarting at every boundary; full means unknown.
    {
        FrameCounter c;
        CHECK(c.Next(D1, 1920, 1080, 1) == 1);
        CHECK(c.Next(D1, 1920, 1080, 1) == 2);
        CHECK(c.Next(D1, 512, 512, 1) == 1);
        CHECK(c.Next(D2, 1920, 1080, 1) == 1);
        CHECK(c.Next(D1, 1920, 1080, 1) == 3);
        CHECK(c.Next(D1, 1920, 1080, 2) == 1);
        CHECK(c.Next(D1, 1, 1, 2) == 1);
        CHECK(c.Next(D1, 2, 2, 2) == 1);
        CHECK(c.Next(D1, 3, 3, 2) == 1);
        CHECK(c.Next(D1, 4, 4, 2) == kOrdinalUnknown);
        CHECK(c.Next(D1, 1920, 1080, 2) == 2);
        c.Reset();
        CHECK(c.Next(D1, 4, 4, 2) == 1);
    }

    // RenderExtent: the stated subrect, else the depth extent; half a subrect stays as stated.
    {
        const auto a = RenderExtent(0, 0, 1280, 720);
        CHECK(a.width == 1280 && a.height == 720);
        const auto b = RenderExtent(1100, 600, 1280, 720);
        CHECK(b.width == 1100 && b.height == 600);
        const auto c = RenderExtent(1100, 0, 1280, 720);
        CHECK(c.width == 1100 && c.height == 0);
    }
}

// The production ring, the production tracker, and a present that reads the way RunPresentPass reads today.
struct Sim
{
    Ring ring;
    Tracker tracker;
    bool done[kRingSize] {};
    int64_t now = 1000;
    uint64_t lastConsumed = 0;
    uint64_t lastLag = 0;

    size_t Capture(const void* device, uint32_t w, uint32_t h, bool completed = true)
    {
        const size_t slot = ring.AcquireForRecord(Never);
        CHECK(slot < kRingSize);
        Stamp s;
        s.device = device;
        s.outputWidth = w;
        s.outputHeight = h;
        const auto render = RenderExtent(0, 0, w * 2 / 3, h * 2 / 3);
        s.renderWidth = render.width;
        s.renderHeight = render.height;
        tracker.StampCapture(slot, s, now);
        done[slot] = completed;
        ring.CommitRecord(slot, w * 2 / 3, h * 2 / 3, false, now, &done[slot], nullptr);
        return slot;
    }

    Decision PresentFrame(const void* device, const void* chain, uint32_t w, uint32_t h)
    {
        const size_t picked = ring.PickForPresent([](void* t) { return *static_cast<bool*>(t); }, now);
        Present p;
        p.device = device;
        p.width = w;
        p.height = h;
        p.swapchain = chain;
        p.lastConsumed = lastConsumed;
        p.nowMs = now;
        const Decision d = tracker.AtPresent(picked, ring, p);
        CHECK(p.epoch == tracker.Epoch());
        if (picked < kRingSize)
        {
            lastLag = p.epoch - tracker.Stamped()[picked].presentEpoch;
            lastConsumed = std::max(lastConsumed, ring.GetSlot(picked).sequence);
        }
        now += 16;
        return d;
    }
};

void Scenarios()
{
    // One thread, one capture per present: every frame accepted, one boundary behind.
    {
        Sim s;
        for (int i = 0; i < 200; ++i)
        {
            s.Capture(D1, 1920, 1080);
            CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
            CHECK(s.lastLag == 1);
        }
    }

    // The first capture before any present: no swapchain to stamp, and a present that is alone on its device.
    {
        Sim s;
        const size_t first = s.Capture(D1, 1920, 1080);
        CHECK(s.tracker.Stamped()[first].swapchain == nullptr);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
        const size_t second = s.Capture(D1, 1920, 1080);
        CHECK(s.tracker.Stamped()[second].swapchain == SC1);
    }

    // Frame generation at 2x and 4x: a generated frame has no capture of its own, so rule 5 refuses it.
    for (int factor : { 2, 4 })
    {
        Sim s;
        for (int i = 0; i < 50; ++i)
        {
            s.Capture(D1, 1920, 1080);
            CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
            for (int g = 1; g < factor; ++g)
                CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Consumed);
        }
    }

    // A copy that completes late: the next present is offered the read capture again (consumed), the one after it
    // the late capture, two boundaries behind (lag).
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
        const size_t late = s.Capture(D1, 1920, 1080, false);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Consumed);
        s.done[late] = true;
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Lag);
        CHECK(s.lastLag == 2);
    }

    // A portrait, then the main view at another extent: the newest is the main view and it is accepted.
    {
        Sim s;
        s.Capture(D1, 512, 512);
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
    }

    // The main view, then a portrait: today the newest wins and is the portrait; identity refuses it on extent.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        s.Capture(D1, 512, 512);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Extent);
    }

    // Two evaluates at one extent in one frame: refused, not guessed; whichever is picked.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Ambiguous);

        Sim t;
        t.Capture(D1, 1920, 1080);
        t.Capture(D1, 1920, 1080, false); // the newer is not eligible yet, so the older is picked
        CHECK(t.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Ambiguous);

        // The next frame, one capture again: accepted.
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
    }

    // A resize between the capture and the present.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 2560, 1440).reason == Reason::Extent);
    }

    // A capture on another device.
    {
        Sim s;
        s.Capture(D2, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Device);
    }

    // A second swapchain presenting on the device: captures carry no stamp, and a present that sees two refuses.
    // Once the second stops presenting for longer than kPresenterLiveMs, the game's chain is alone again.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC2, 640, 480).reason == Reason::Extent); // the tool window's own present
        const size_t slot = s.Capture(D1, 1920, 1080);
        CHECK(s.tracker.Stamped()[slot].swapchain == nullptr);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Swapchain);
        s.now += kPresenterLiveMs + 1;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
    }

    // A swapchain recreated: a capture stamped for the old chain does not match the new one.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
        const size_t slot = s.Capture(D1, 1920, 1080);
        CHECK(s.tracker.Stamped()[slot].swapchain == SC1);
        CHECK(s.PresentFrame(D1, SC3, 1920, 1080).reason == Reason::Swapchain);
    }

    // Reset forgets the stamps, the presenters and the boundary count.
    {
        Sim s;
        s.Capture(D1, 1920, 1080);
        s.PresentFrame(D1, SC1, 1920, 1080);
        s.tracker.Reset();
        CHECK(s.tracker.Epoch() == 0 && s.tracker.Stamped()[0].device == nullptr);
    }
}

// A long run on the production ring with drops, late copies, generated frames and the odd portrait: the answer is
// always the pick or nothing, and the step-0 record's books balance.
void LongRun()
{
    std::mt19937_64 rng(7);
    Sim s;
    Consumer consumer("sim");
    Clock::time_point t {};
    uint32_t presents = 0, lines = 0;

    for (int frame = 0; frame < 20000; ++frame)
    {
        if (rng() % 20 != 0)
            s.Capture(D1, 1920, 1080, rng() % 10 != 0);
        if (rng() % 50 == 0)
            s.Capture(D1, 512, 512);
        for (size_t i = 0; i < kRingSize; ++i)
            s.done[i] = s.done[i] || rng() % 3 == 0;

        const int fg = rng() % 4 == 0 ? 2 : 1;
        for (int g = 0; g < fg; ++g)
        {
            const size_t picked = s.ring.PickForPresent([](void* tok) { return *static_cast<bool*>(tok); }, s.now);
            Present p;
            p.device = D1;
            p.width = 1920;
            p.height = 1080;
            p.swapchain = SC1;
            p.lastConsumed = consumer.LastConsumed();
            p.nowMs = s.now;
            const Decision d = s.tracker.AtPresent(picked, s.ring, p);
            CHECK(d.slot == picked || d.slot == kRingSize);

            const Today today = picked < kRingSize ? Today::Read : Today::None;
            std::string line;
            const uint64_t before = consumer.LastConsumed();
            lines += consumer.Note(d, picked, s.ring, s.tracker.Stamped(), p, today, t, &line) ? 1 : 0;
            CHECK(consumer.LastConsumed() >= before);
            ++presents;

            const Counts& open = consumer.Open();
            CHECK(open.read + open.skipped + open.none == open.presents);
            uint32_t reasons = 0;
            for (uint32_t n : open.readReasons)
                reasons += n;
            CHECK(reasons == open.read);

            s.now += 8;
            t += std::chrono::milliseconds(8);
        }
    }

    // About 200 s of presents: a line at least every 30 s, and at most one per 2 s.
    const auto seconds = static_cast<uint32_t>(presents * 8 / 1000);
    CHECK(lines >= seconds / 30 && lines <= seconds / 2 + 1);
}

// Step 0's record: what is counted, the line it writes, and how often.
void WindowAndLine()
{
    // The line, from counts set by hand.
    {
        Counts c;
        c.presents = 5;
        c.read = 3;
        c.skipped = 1;
        c.none = 1;
        c.readReasons[static_cast<size_t>(Reason::Accepted)] = 2;
        c.readReasons[static_cast<size_t>(Reason::Lag)] = 1;
        c.lag = { 0, 2, 1, 0, 0 };
        c.frame = { 3, 0, 0 };
        const std::string expected =
            "DLSS-NR present guides, identity check (log only, nothing refused), D3D12 present: 5 presents in 2.0 s | "
            "today 3 read a capture, 1 skipped with no new capture, 1 had none | of those read, 2 would pass, 1 would "
            "be refused (lag 1) | skipped but would pass 0 | epoch lag of new captures 0:0 1:2 2:1 3:0 4+:0 | "
            "captures per frame at their extent 1:3 2:0 3+:0";
        const std::string line = FormatLine("D3D12 present", c, std::chrono::milliseconds(2049));
        if (line != expected)
            std::fprintf(stderr, "got:      %s\nexpected: %s\n", line.c_str(), expected.c_str());
        CHECK(line == expected);

        c.readReasons[static_cast<size_t>(Reason::Extent)] = 4;
        c.readReasons[static_cast<size_t>(Reason::Ambiguous)] = 2;
        c.read += 6;
        c.presents += 6;
        c.skippedWouldPass = 1;
        c.haveMismatch = true;
        c.mismatch.outputWidth = 3840;
        c.mismatch.outputHeight = 2160;
        c.mismatch.outputBaseX = 0;
        c.mismatch.outputBaseY = 0;
        c.mismatchWidth = 1920;
        c.mismatchHeight = 1080;
        const std::string second = FormatLine("D3D11 bridge", c, std::chrono::milliseconds(31500));
        CHECK(
            second ==
            "DLSS-NR present guides, identity check (log only, nothing refused), D3D11 bridge: 11 presents in 31.5 s | "
            "today 9 read a capture, 1 skipped with no new capture, 1 had none | of those read, 2 would pass, 7 would "
            "be refused (extent 4, ambiguous 2, lag 1) | skipped but would pass 1 | epoch lag of new captures 0:0 "
            "1:2 2:1 3:0 4+:0 | captures per frame at their extent 1:3 2:0 3+:0 | last extent refusal: output "
            "3840x2160 at (0,0), backbuffer 1920x1080");
    }

    // What Note counts: today's three outcomes, the decision over reads, would-pass skips, and a capture's lag and
    // frame count once, at the first present it is offered to.
    {
        Fixture f;
        Stamp s = Good();
        s.presentEpoch = 9;
        const size_t slot = f.Capture(s, 1000);
        Consumer c("x");
        Present p = GoodPresent();
        Clock::time_point t {};
        std::string line;

        Decision d = Decide(slot, f.ring, f.stamps, p);
        CHECK(!c.Note(d, slot, f.ring, f.stamps, p, Today::Read, t, &line));
        CHECK(c.LastConsumed() == f.ring.GetSlot(slot).sequence);
        p.lastConsumed = c.LastConsumed();
        p.epoch = 11;
        d = Decide(slot, f.ring, f.stamps, p);
        CHECK(d.reason == Reason::Consumed);
        CHECK(!c.Note(d, slot, f.ring, f.stamps, p, Today::Skipped, t, &line));
        CHECK(!c.Note({}, kRingSize, f.ring, f.stamps, p, Today::None, t, &line));

        const Counts& o = c.Open();
        CHECK(o.presents == 3 && o.read == 1 && o.skipped == 1 && o.none == 1);
        CHECK(o.readReasons[static_cast<size_t>(Reason::Accepted)] == 1);
        CHECK(o.skippedWouldPass == 0);
        CHECK(o.lag[1] == 1 && o.lag[2] == 0); // counted once, at lag 1, not again at lag 2
        CHECK(o.frame[0] == 1);
    }

    // The rate: nothing inside kWindow; the first window at kWindow; a window like the last one written waits for
    // kSteadyWindow and carries its whole interval; a different one is written at kWindow. One fresh capture per
    // present, as a steady game makes them.
    {
        Sim s;
        Consumer c("x");
        Clock::time_point t {};
        std::string line;
        const auto present = [&](Clock::duration at, bool forceLag)
        {
            s.Capture(D1, 1920, 1080);
            const size_t picked = s.ring.PickForPresent([](void* tok) { return *static_cast<bool*>(tok); }, s.now);
            Present p;
            p.device = D1;
            p.width = 1920;
            p.height = 1080;
            p.swapchain = SC1;
            p.lastConsumed = c.LastConsumed();
            p.nowMs = s.now;
            Decision d = s.tracker.AtPresent(picked, s.ring, p);
            CHECK(d.reason == Reason::Accepted);
            if (forceLag)
                d = { kRingSize, Reason::Lag }; // a refusal, to change what the window looks like
            s.now += 16;
            return c.Note(d, picked, s.ring, s.tracker.Stamped(), p, Today::Read, t + at, &line);
        };

        using std::chrono::milliseconds;
        CHECK(!present(milliseconds(0), false));
        CHECK(!present(milliseconds(1999), false));
        CHECK(present(milliseconds(2000), false));
        CHECK(line.find(": 3 presents in 2.0 s") != std::string::npos);
        CHECK(line.find("of those read, 3 would pass, 0 would be refused |") != std::string::npos);
        CHECK(line.find("epoch lag of new captures 0:0 1:3 2:0") != std::string::npos);

        // Steady: the same buckets in use. Held until 30 s after the last line, then the whole interval at once.
        int held = 0;
        for (int ms = 2016; ms < 32000; ms += 16)
            held += present(milliseconds(ms), false) ? 0 : 1;
        CHECK(held == (32000 - 2016) / 16);
        CHECK(present(milliseconds(32000), false));
        CHECK(line.find(": 1875 presents in 30.0 s") != std::string::npos);

        // A refusal appears: a new bucket, written once kWindow has passed, not kSteadyWindow.
        CHECK(!present(milliseconds(33000), true));
        CHECK(present(milliseconds(34000), true));
        CHECK(line.find("would be refused (lag 2)") != std::string::npos);

        // An hour of the same: a line every 30 s, no more.
        int lines = 0;
        for (int ms = 34016; ms < 34000 + 3600 * 1000; ms += 16)
            lines += present(milliseconds(ms), true) ? 1 : 0;
        CHECK(lines >= 119 && lines <= 121);

        c.Reset();
        CHECK(c.LastConsumed() == 0 && c.Open().presents == 0);
    }
}
} // namespace

int main()
{
    EveryRuleAlone();
    FirstFailingOrder();
    NarrowingOnly();
    RegistryAndCounter();
    Scenarios();
    LongRun();
    WindowAndLine();
    std::printf("PASS: present-guide identity: %d checks -- every rule alone and in order, narrowing only over 50000 "
                "random cases, null swapchain stamps, registry and frame counter, ring scenarios (FG 2x/4x, late copy, "
                "portrait, resize, second swapchain), step 0's window and line\n",
                g_checks);
    return 0;
}
