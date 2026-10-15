// The present-guide identity decision (retoque/dlssnr/DlssNr_PresentGuideIdentity.h) on the production header:
// every rule alone, the first-failing order, narrowing only, the swapchain stamp's null semantics, the registry and
// the frame counter, scenarios on the production ring, a long run that reads only what the rules accept, step 1's
// window and line, the source-change reset, and the status the menu and the status line read.

#include <dlssnr/DlssNr_PresentGuideIdentity.h>

#include <cassert>
#include <cstdio>
#include <random>
#include <set>
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
        static_assert(kLag == 1, "the cases below assume the measured value");
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

    // Rule 6 where it is not enforced (every entry but the game's own Present): a capture past the limit is accepted
    // and marked, one within it is not marked, and the rules after it still refuse.
    {
        Fixture f;
        Stamp s = Good();
        Present p = GoodPresent();
        p.enforceLag = false;
        s.presentEpoch = p.epoch - 3;
        size_t slot = f.Capture(s, 1000);
        Decision d = Decide(slot, f.ring, f.stamps, p);
        CHECK(d.reason == Reason::Accepted && d.slot == slot && d.lagBeyond);

        Fixture g;
        slot = g.Capture(Good(), 1000);
        d = Decide(slot, g.ring, g.stamps, p);
        CHECK(d.reason == Reason::Accepted && !d.lagBeyond);

        Fixture h;
        s.renderWidth = 0;
        slot = h.Capture(s, 1000);
        d = Decide(slot, h.ring, h.stamps, p);
        CHECK(d.reason == Reason::Subrect && d.slot == kRingSize && !d.lagBeyond);

        // A refusal never carries the mark, enforced or not.
        p.enforceLag = true;
        d = Decide(slot, h.ring, h.stamps, p);
        CHECK(d.reason == Reason::Lag && !d.lagBeyond);
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
        { Reason::Lag, lag > kLag && p.enforceLag },
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

    size_t accepted = 0, none = 0, lagBeyond = 0;
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
        p.enforceLag = rng() % 3 != 0;

        const size_t picked = rng() % (kRingSize + 1);
        const Decision d = Decide(picked, f.ring, f.stamps, p);

        CHECK(d.slot == picked || d.slot == kRingSize);
        CHECK((d.slot != kRingSize) == (d.reason == Reason::Accepted));

        // The mark: on an acceptance only, exactly when the capture is past the limit and the limit is not enforced.
        if (d.reason == Reason::Accepted)
        {
            const Stamp& s = f.stamps[picked];
            const uint64_t lag = p.epoch > s.presentEpoch ? p.epoch - s.presentEpoch : 0;
            CHECK(d.lagBeyond == (lag > kLag && !p.enforceLag));
            lagBeyond += d.lagBeyond ? 1 : 0;
        }
        else
        {
            CHECK(!d.lagBeyond);
        }

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
    CHECK(accepted > 0 && none > 0 && lagBeyond > 0);
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

// The production ring, the production tracker, and a present that reads what the rules accept, as RunPresentPass
// does since step 1: the capture it uses is the one rule 5 then compares against.
struct Sim
{
    Ring ring;
    Tracker tracker;
    bool done[kRingSize] {};
    int64_t now = 1000;
    uint64_t lastConsumed = 0;
    uint64_t lastLag = 0;
    bool enforceLag = true;

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
        p.enforceLag = enforceLag;
        const Decision d = tracker.AtPresent(picked, ring, p);
        CHECK(p.epoch == tracker.Epoch());
        if (picked < kRingSize)
            lastLag = p.epoch - tracker.Stamped()[picked].presentEpoch;
        if (d.reason == Reason::Accepted)
            lastConsumed = std::max(lastConsumed, ring.GetSlot(d.slot).sequence);
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

    // A copy that completes late: the next present is offered the used capture again (consumed, so it falls back),
    // the one after it the late capture, two boundaries behind: refused where the lag limit is enforced, used and
    // marked where it is not.
    for (const bool enforce : { true, false })
    {
        Sim s;
        s.enforceLag = enforce;
        s.Capture(D1, 1920, 1080);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Accepted);
        const size_t late = s.Capture(D1, 1920, 1080, false);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::Consumed);
        s.done[late] = true;
        const Decision d = s.PresentFrame(D1, SC1, 1920, 1080);
        CHECK(s.lastLag == 2);
        CHECK(enforce ? d.reason == Reason::Lag : (d.reason == Reason::Accepted && d.lagBeyond));
    }

    // The wrapped entry as measured (9eb4f6e6): each capture completes only after the present of its own frame, so
    // every present is offered the previous frame's, two boundaries behind. Where the limit is enforced none would be
    // used; where it is not (the wrapped entry, step 1) each is used once, marked, and never offered as unused again.
    for (const bool enforce : { true, false })
    {
        Sim s;
        s.enforceLag = enforce;
        size_t previous = s.Capture(D1, 1920, 1080, false);
        CHECK(s.PresentFrame(D1, SC1, 1920, 1080).reason == Reason::NoCapture);
        int used = 0;
        for (int i = 0; i < 100; ++i)
        {
            const size_t next = s.Capture(D1, 1920, 1080, false);
            s.done[previous] = true;
            const Decision d = s.PresentFrame(D1, SC1, 1920, 1080);
            CHECK(s.lastLag == 2);
            CHECK(enforce ? d.reason == Reason::Lag : (d.reason == Reason::Accepted && d.lagBeyond));
            used += d.reason == Reason::Accepted ? 1 : 0;
            previous = next;
        }
        CHECK(used == (enforce ? 0 : 100));
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

// A long run on the production ring, read the way step 1 reads it: a present tracks the slot the rules returned and
// nothing else, and a read stays in flight on the "GPU" for a while. Captures are dropped, completed late or at another
// extent or device, generated presents and portraits come and go, and two entries alternate, one enforcing the lag
// limit. Held throughout:
//   - the answer is the pick or nothing, and a refused pick is never read;
//   - a capture refused for something that cannot get better (its device, extent, output base or render extent, an
//     earlier use, its age, or its lag at an entry that enforces it) is never read later either;
//   - the ring never overwrites a slot whose read is in flight;
//   - the record's books balance.
void LongRun()
{
    std::mt19937_64 rng(7);
    Ring ring;
    Tracker tracker;
    Consumer consumer("sim");
    bool done[kRingSize] {};
    struct Read
    {
        bool inFlight = false;
        int completesAt = 0;
        uint64_t sequence = 0;
    } reads[kRingSize];
    std::set<uint64_t> refusedForGood;
    int64_t now = 1000;
    Clock::time_point t {};
    uint32_t presents = 0, lines = 0, used = 0, refused = 0;
    int tick = 0;

    const auto inFlight = [](void* token) { return token != nullptr && static_cast<Read*>(token)->inFlight; };

    for (int frame = 0; frame < 20000; ++frame)
    {
        // The GPU finishes reads.
        for (auto& read : reads)
            read.inFlight = read.inFlight && read.completesAt > tick;

        const int captures = rng() % 20 == 0 ? 0 : (rng() % 30 == 0 ? 2 : 1);
        for (int c = 0; c < captures; ++c)
        {
            const size_t slot = ring.AcquireForRecord(inFlight);
            if (slot >= kRingSize)
                continue;

            CHECK(!reads[slot].inFlight);
            Stamp s;
            s.device = rng() % 200 == 0 ? D2 : D1;
            s.outputWidth = rng() % 50 == 0 ? 512 : 1920;
            s.outputHeight = s.outputWidth == 512 ? 512 : 1080;
            s.outputBaseX = rng() % 300 == 0 ? 8 : 0;
            s.renderWidth = rng() % 300 == 0 ? 0 : 1280;
            s.renderHeight = 720;
            tracker.StampCapture(slot, s, now);
            done[slot] = rng() % 10 != 0;
            ring.CommitRecord(slot, 1280, 720, false, now, &done[slot], &reads[slot]);
        }

        for (size_t i = 0; i < kRingSize; ++i)
            done[i] = done[i] || rng() % 3 == 0;

        const int fg = rng() % 4 == 0 ? 2 : 1;
        for (int g = 0; g < fg; ++g)
        {
            const size_t picked = ring.PickForPresent([](void* tok) { return *static_cast<bool*>(tok); }, now);
            Present p;
            p.device = D1;
            p.width = 1920;
            p.height = 1080;
            p.swapchain = SC1;
            p.lastConsumed = consumer.LastConsumed();
            p.nowMs = now;
            p.enforceLag = (frame / 500) % 2 == 0; // the boundary for a while, then another entry
            const Decision d = tracker.AtPresent(picked, ring, p);
            CHECK(d.slot == picked || d.slot == kRingSize);

            // The read: the decision's slot, tracked; nothing when refused.
            bool read = false;
            if (d.slot < kRingSize)
            {
                const uint64_t sequence = ring.GetSlot(d.slot).sequence;
                CHECK(refusedForGood.count(sequence) == 0);

                reads[d.slot] = { true, tick + 1 + static_cast<int>(rng() % 3), sequence };
                read = true;
                ++used;
            }
            else if (picked < kRingSize && d.reason != Reason::NoCapture)
            {
                ++refused;
                const bool forGood = d.reason == Reason::Device || d.reason == Reason::Extent ||
                                     d.reason == Reason::OutputBase || d.reason == Reason::Subrect ||
                                     d.reason == Reason::Consumed || d.reason == Reason::Stale;
                if (forGood)
                    refusedForGood.insert(ring.GetSlot(picked).sequence);
            }

            const Outcome outcome = OutcomeOf(d, read);
            CHECK(outcome != Outcome::Refused || !read);
            std::string line;
            const uint64_t before = consumer.LastConsumed();
            lines += consumer.Note(d, picked, ring, tracker.Stamped(), p, outcome, t, &line) ? 1 : 0;
            CHECK(consumer.LastConsumed() >= before);
            CHECK(outcome != Outcome::Used || consumer.LastConsumed() >= ring.GetSlot(d.slot).sequence);
            ++presents;

            const Counts& open = consumer.Open();
            CHECK(open.used + Refusals(open) + open.skipped + open.none == open.presents);
            CHECK(open.enforced <= open.presents && open.lagBeyond <= open.used);

            now += 8;
            t += std::chrono::milliseconds(8);
            ++tick;
        }

        // A slot in flight still holds the capture that was read from it.
        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (reads[i].inFlight)
                CHECK(ring.GetSlot(i).sequence == reads[i].sequence);
        }
    }

    // Every kind of answer happened, so the properties above were tested, not vacuous.
    CHECK(used > 1000 && refused > 100 && !refusedForGood.empty());

    // About 200 s of presents: a line at least every 30 s, and at most one per 2 s.
    const auto seconds = static_cast<uint32_t>(presents * 8 / 1000);
    CHECK(lines >= seconds / 30 && lines <= seconds / 2 + 1);
}

// Step 1's record: what is counted, the line it writes, and how often.
void WindowAndLine()
{
    // The line, from counts set by hand.
    {
        Counts c;
        c.presents = 6;
        c.enforced = 6;
        c.used = 3;
        c.skipped = 1;
        c.none = 1;
        c.refused[static_cast<size_t>(Reason::Lag)] = 1;
        c.lag = { 0, 3, 1, 0, 0 };
        c.frame = { 4, 0, 0 };
        const std::string expected =
            "DLSS-NR present guides, identity applied, D3D12 present: 6 presents in 2.0 s, lag limit enforced on 6 | "
            "3 used the game's depth and motion, 1 refused (lag 1), 1 skipped with no new capture, 1 had none | "
            "skipped but would pass 0 | used past the lag limit where it is not enforced 0 | epoch lag of new captures "
            "0:0 1:3 2:1 3:0 4+:0 | captures per frame at their extent 1:4 2:0 3+:0";
        const std::string line = FormatLine("D3D12 present", c, std::chrono::milliseconds(2049));
        if (line != expected)
            std::fprintf(stderr, "got:      %s\nexpected: %s\n", line.c_str(), expected.c_str());
        CHECK(line == expected);

        c.refused[static_cast<size_t>(Reason::Extent)] = 4;
        c.refused[static_cast<size_t>(Reason::Ambiguous)] = 2;
        c.presents += 6;
        c.enforced = 0;
        c.skippedWouldPass = 1;
        c.lagBeyond = 2;
        c.haveMismatch = true;
        c.mismatch.outputWidth = 3840;
        c.mismatch.outputHeight = 2160;
        c.mismatchWidth = 1920;
        c.mismatchHeight = 1080;
        const std::string second = FormatLine("D3D11 bridge", c, std::chrono::milliseconds(31500));
        const std::string expectedSecond =
            "DLSS-NR present guides, identity applied, D3D11 bridge: 12 presents in 31.5 s, lag limit enforced on 0 | "
            "3 used the game's depth and motion, 7 refused (extent 4, ambiguous 2, lag 1), 1 skipped with no new "
            "capture, 1 had none | skipped but would pass 1 | used past the lag limit where it is not enforced 2 | "
            "epoch lag of new captures 0:0 1:3 2:1 3:0 4+:0 | captures per frame at their extent 1:4 2:0 3+:0 | last "
            "extent refusal: output 3840x2160 at (0,0), backbuffer 1920x1080";
        if (second != expectedSecond)
            std::fprintf(stderr, "got:      %s\nexpected: %s\n", second.c_str(), expectedSecond.c_str());
        CHECK(second == expectedSecond);
    }

    // What Note counts: the four outcomes, refusals by reason, skips the rules would accept, the mark, and a capture's
    // lag and frame count once, at the first present it is offered to. Only a use moves rule 5's record.
    {
        Fixture f;
        Stamp s = Good();
        s.presentEpoch = 9;
        const size_t slot = f.Capture(s, 1000);
        Consumer c("x");
        Present p = GoodPresent();
        Clock::time_point t {};
        std::string line;

        // Refused first (another extent): counted, and the record of uses does not move.
        Present wide = p;
        wide.width = 2560;
        Decision d = Decide(slot, f.ring, f.stamps, wide);
        CHECK(d.reason == Reason::Extent);
        CHECK(!c.Note(d, slot, f.ring, f.stamps, wide, OutcomeOf(d, false), t, &line));
        CHECK(c.LastConsumed() == 0);
        CHECK(c.Open().haveMismatch && c.Open().mismatchWidth == 2560);

        d = Decide(slot, f.ring, f.stamps, p);
        CHECK(OutcomeOf(d, true) == Outcome::Used && OutcomeOf(d, false) == Outcome::None);
        CHECK(!c.Note(d, slot, f.ring, f.stamps, p, Outcome::Used, t, &line));
        CHECK(c.LastConsumed() == f.ring.GetSlot(slot).sequence);
        p.lastConsumed = c.LastConsumed();
        p.epoch = 11;
        d = Decide(slot, f.ring, f.stamps, p);
        CHECK(d.reason == Reason::Consumed && OutcomeOf(d, false) == Outcome::Refused);
        CHECK(!c.Note(d, slot, f.ring, f.stamps, p, Outcome::Skipped, t, &line));
        CHECK(!c.Note({}, kRingSize, f.ring, f.stamps, p, OutcomeOf({}, false), t, &line));

        // An accepted capture whose read could not be tracked is none, and moves no record.
        const uint64_t record = c.LastConsumed();
        Fixture g;
        const size_t other = g.Capture(Good(), 1000);
        const Decision untracked = Decide(other, g.ring, g.stamps, GoodPresent());
        CHECK(untracked.reason == Reason::Accepted && OutcomeOf(untracked, false) == Outcome::None);
        Consumer quiet("y");
        CHECK(!quiet.Note(untracked, other, g.ring, g.stamps, GoodPresent(), Outcome::None, t, &line));
        CHECK(quiet.LastConsumed() == 0 && quiet.Open().none == 1 && quiet.Open().used == 0);
        CHECK(c.LastConsumed() == record);

        // MarkUsed: the record of a use without counting it, never backwards.
        quiet.MarkUsed(5);
        quiet.MarkUsed(3);
        CHECK(quiet.LastConsumed() == 5 && quiet.Open().presents == 1);

        // A use past the limit where it is not enforced is marked.
        Decision marked { slot, Reason::Accepted, true };
        Present relaxed = p;
        relaxed.enforceLag = false;
        CHECK(!c.Note(marked, slot, f.ring, f.stamps, relaxed, Outcome::Used, t, &line));

        const Counts& o = c.Open();
        CHECK(o.presents == 5 && o.enforced == 4 && o.used == 2 && o.skipped == 1 && o.none == 1);
        CHECK(c.LastConsumed() == f.ring.GetSlot(slot).sequence);
        CHECK(o.refused[static_cast<size_t>(Reason::Extent)] == 1 && Refusals(o) == 1);
        CHECK(o.skippedWouldPass == 0 && o.lagBeyond == 1);
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
            return c.Note(d, picked, s.ring, s.tracker.Stamped(), p, OutcomeOf(d, d.slot < kRingSize), t + at, &line);
        };

        using std::chrono::milliseconds;
        CHECK(!present(milliseconds(0), false));
        CHECK(!present(milliseconds(1999), false));
        CHECK(present(milliseconds(2000), false));
        CHECK(line.find(": 3 presents in 2.0 s, lag limit enforced on 3") != std::string::npos);
        CHECK(line.find("| 3 used the game's depth and motion, 0 refused, 0 skipped") != std::string::npos);
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
        CHECK(line.find(" 2 refused (lag 2)") != std::string::npos);

        // An hour of the same: a line every 30 s, no more.
        int lines = 0;
        for (int ms = 34016; ms < 34000 + 3600 * 1000; ms += 16)
            lines += present(milliseconds(ms), true) ? 1 : 0;
        CHECK(lines >= 119 && lines <= 121);

        c.Reset();
        CHECK(c.LastConsumed() == 0 && c.Open().presents == 0);
    }
}

// D3's reset: a single miss, or an alternation, never resets; a change that holds for kSourceStable presents resets
// once, in either direction; the first source to hold is the start and does not.
void SourceChange()
{
    static_assert(kSourceStable == 4, "the cases below assume the starting value");

    SourceHistory h;
    CHECK(h.Established() == -1);

    // The start: the first frame refused (step 0's ambiguous at start-up), then the game's guides. No reset.
    CHECK(!h.Note(false));
    for (int i = 0; i < 10; ++i)
        CHECK(!h.Note(true));
    CHECK(h.Established() == 1);

    // Single misses, and an alternation: no reset, ever.
    for (int i = 0; i < 100; ++i)
    {
        CHECK(!h.Note(false));
        CHECK(!h.Note(true));
    }
    for (int i = 0; i < 100; ++i)
    {
        CHECK(!h.Note(false));
        CHECK(!h.Note(false));
        CHECK(!h.Note(false));
        CHECK(!h.Note(true));
    }
    CHECK(h.Established() == 1);

    // A change that holds: one reset, on its kSourceStable-th frame, then none while it stays.
    CHECK(!h.Note(false));
    CHECK(!h.Note(false));
    CHECK(!h.Note(false));
    CHECK(h.Note(false));
    CHECK(h.Established() == 0);
    for (int i = 0; i < 50; ++i)
        CHECK(!h.Note(false));

    // And back.
    int resets = 0;
    for (int i = 0; i < 50; ++i)
        resets += h.Note(true) ? 1 : 0;
    CHECK(resets == 1 && h.Established() == 1);

    // Reset forgets: the next source to hold is a start again.
    h.Reset();
    for (int i = 0; i < 10; ++i)
        CHECK(!h.Note(false));
    CHECK(h.Established() == 0);

    // A random run: never two resets closer than kSourceStable presents, and every reset is a held change.
    std::mt19937_64 rng(4);
    SourceHistory r;
    int last = -100, total = 0;
    bool run[kSourceStable] {};
    for (int i = 0; i < 100000; ++i)
    {
        const bool game = rng() % 7 != 0 ? (i / 997) % 2 == 0 : rng() % 2 == 0;
        run[i % kSourceStable] = game;
        if (r.Note(game))
        {
            CHECK(i - last >= static_cast<int>(kSourceStable));
            for (const bool b : run)
                CHECK(b == game);
            last = i;
            ++total;
        }
    }
    CHECK(total > 10);
}

// The status: packed and unpacked without the lock, one second at a time, and the words the status line and the menu
// make of it.
void StatusAndText()
{
    // Pack and unpack, saturating.
    {
        Status s;
        s.presents = 61;
        s.used = 60;
        s.refused = 1;
        s.skipped = 3;
        s.top = Reason::Ambiguous;
        const Status u = Unpack(Pack(s));
        CHECK(u.presents == 61 && u.used == 60 && u.refused == 1 && u.skipped == 3 && u.top == Reason::Ambiguous);

        s.presents = 1u << 20;
        s.top = Reason::Subrect;
        const Status big = Unpack(Pack(s));
        CHECK(big.presents == kStatusCountMax && big.used == 60 && big.top == Reason::Subrect);
        CHECK(Unpack(0).presents == 0 && Unpack(0).top == Reason::Accepted);
        for (size_t r = 0; r < kReasons; ++r)
        {
            Status one;
            one.top = static_cast<Reason>(r);
            CHECK(Unpack(Pack(one)).top == one.top);
        }
    }

    // One second at a time: skips apart, refusals by reason, the most frequent named.
    {
        StatusWindow w;
        Clock::time_point t {};
        Status out;
        CHECK(!w.Add(Outcome::Used, Reason::Accepted, t, &out));
        CHECK(!w.Add(Outcome::Refused, Reason::Lag, t + std::chrono::milliseconds(100), &out));
        CHECK(!w.Add(Outcome::Refused, Reason::Extent, t + std::chrono::milliseconds(200), &out));
        CHECK(!w.Add(Outcome::Refused, Reason::Extent, t + std::chrono::milliseconds(300), &out));
        CHECK(!w.Add(Outcome::Skipped, Reason::Consumed, t + std::chrono::milliseconds(400), &out));
        CHECK(!w.Add(Outcome::None, Reason::NoCapture, t + std::chrono::milliseconds(999), &out));
        CHECK(w.Add(Outcome::Used, Reason::Accepted, t + std::chrono::milliseconds(1000), &out));
        CHECK(out.presents == 6 && out.used == 2 && out.refused == 3 && out.skipped == 1);
        CHECK(out.top == Reason::Extent);

        // The next second starts empty.
        CHECK(!w.Add(Outcome::Used, Reason::Accepted, t + std::chrono::milliseconds(1500), &out));
        CHECK(w.Add(Outcome::Used, Reason::Accepted, t + std::chrono::milliseconds(2000), &out));
        CHECK(out.presents == 2 && out.used == 2 && out.refused == 0 && out.top == Reason::Accepted);
    }

    // Which source a second names, and the status line's literal for it.
    {
        Status s;
        s.presents = 10;
        s.used = 1;
        CHECK(SourceOf(s, Fallback::Waiting) == Source::Game);
        s.used = 0;
        CHECK(SourceOf(s, Fallback::Waiting) == Source::Waiting);
        CHECK(SourceOf(s, Fallback::Synthesized) == Source::Synthesized);
        CHECK(SourceOf(s, Fallback::Zero) == Source::Zero);
        Status skipping;
        skipping.skipped = 60;
        CHECK(SourceOf(skipping, Fallback::Zero) == Source::Skipping);
        CHECK(SourceOf(skipping, Fallback::Waiting) == Source::Skipping);

        CHECK(std::string(RouteText(false, Source::Game)) == "present: active, the game's depth and motion");
        CHECK(std::string(RouteText(true, Source::Game)) == "auto: present, the game's depth and motion");
        CHECK(std::string(RouteText(false, Source::Synthesized)) == "present: active, synthesized motion");
        CHECK(std::string(RouteText(false, Source::Zero)) == "present: active, zero guides (no motion)");
        CHECK(std::string(RouteText(false, Source::Waiting)) == "present: waiting for the game's depth and motion");
        CHECK(std::string(RouteText(true, Source::Skipping)) ==
              "auto: present, no new frame from the upscaler (shown as the game drew it)");
        // Never the latch's old claim.
        for (const Source source :
             { Source::Game, Source::Synthesized, Source::Zero, Source::Waiting, Source::Skipping })
        {
            for (const bool autoRoute : { false, true })
                CHECK(std::string(RouteText(autoRoute, source)).find("DLSS temporal inputs") == std::string::npos);
        }
    }

    // The menu's line, row by row of DEVELOPMENT.md section 2.4.
    {
        Status s;
        s.presents = 60;
        s.used = 60;
        CHECK(GuidesLine(s, Fallback::Zero) == "Guides: the game's depth and motion, on every frame");
        s.skipped = 2;
        CHECK(GuidesLine(s, Fallback::Zero) == "Guides: the game's depth and motion, on every frame the pass ran on; "
                                               "2 with no new frame from the upscaler");
        s.skipped = 0;
        s.used = 57;
        s.refused = 2;
        s.top = Reason::Ambiguous;
        CHECK(GuidesLine(s, Fallback::Synthesized) ==
              "Guides: the game's depth and motion on 57 of 60 frames; 2 refused (ambiguous), 1 with none");
        CHECK(GuidesLine(s, Fallback::Waiting) == "Guides: the game's depth and motion on 57 of 60 frames; 2 refused "
                                                  "(ambiguous), 1 with none, shown as the game drew them");
        s.used = 0;
        s.refused = 60;
        s.top = Reason::Extent;
        CHECK(GuidesLine(s, Fallback::Synthesized) ==
              "Guides: synthesized motion; the game's refused on 60 of 60 frames (extent)");
        s.refused = 0;
        s.skipped = 5;
        CHECK(GuidesLine(s, Fallback::Zero) == "Guides: zero (no motion); 5 with no new frame from the upscaler");
        CHECK(GuidesLine(s, Fallback::Waiting) == "Guides: none the rules accept yet; frames are shown as the game "
                                                  "drew them; 5 with no new frame from the upscaler");
        Status skipping;
        skipping.skipped = 60;
        CHECK(GuidesLine(skipping, Fallback::Waiting) ==
              "Guides: no new frame from the upscaler; frames are shown as the game drew them");
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
    SourceChange();
    StatusAndText();
    std::printf("PASS: present-guide identity: %d checks -- every rule alone and in order, rule 6 enforced and not, "
                "narrowing only over 50000 random cases, null swapchain stamps, registry and frame counter, ring "
                "scenarios (FG 2x/4x, late copy, the wrapped entry's lag, portrait, resize, second swapchain), a "
                "20000-frame run that reads only what the rules accept and never overwrites a read in flight, step "
                "1's window and line, the source-change reset, the status and its text\n",
                g_checks);
    return 0;
}
