#include <dlssnr/DlssNr_Exposure.h>

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    using namespace DlssNr::Exposure;

    // A normal game with a stable or slowly moving exposure never latches over 1000 readings.
    {
        PlausibilityLatch latch;
        for (int i = 0; i < 1000; ++i)
        {
            const float gameExposure = 1.0f + 0.2f * std::sin(static_cast<float>(i) * 0.05f);
            assert(!latch.Observe(PreExposure(1.0f) / gameExposure));
        }
        assert(!latch.Latched());
        assert(latch.Reason() == LatchReason::None);
        assert(latch.LowCount() == 0 && latch.HighCount() == 0);
    }

    // Crimson Desert's eye adaptation, as logged on 2026-10-12: the implied white point went from 0.28 to 303
    // within 120 readings during a scene change, and over the session it ranged 0.10..307. The swing test this
    // replaced latched on it; this one must not.
    {
        PlausibilityLatch latch;
        const float logged[] = { 108.85f, 63.87f, 303.6f, 0.2836f, 1.07f, 0.43f, 0.10f, 3.63f, 2.71f, 1.72f,
                                 19.58f, 15.13f, 8.52f, 307.0f, 0.103f, 28.08f, 50.48f, 11.68f };
        for (int round = 0; round < 50; ++round)
            for (float white : logged)
                assert(!latch.Observe(white));
        // Ten-stop sweeps inside the plausible range, up and down, as fast as one reading apart.
        for (int i = 0; i < 2000; ++i)
            assert(!latch.Observe((i & 1) ? 0.11f : 300.0f));
        assert(!latch.Latched());
    }

    // One end alone never latches, however long it lasts: a loading screen or a black frame can push the
    // exposure to an extreme for a while without the value being meaningless.
    {
        PlausibilityLatch high;
        for (int i = 0; i < 500; ++i)
            assert(!high.Observe(2000.0f));
        assert(!high.Latched() && high.HighCount() == 500 && high.LowCount() == 0);

        PlausibilityLatch low;
        for (int i = 0; i < 500; ++i)
            assert(!low.Observe(0.005f));
        assert(!low.Latched() && low.LowCount() == 500 && low.HighCount() == 0);
    }

    // One wild reading at each end does not latch; the second at both ends does.
    {
        PlausibilityLatch latch;
        assert(!latch.Observe(4000.0f));
        for (int i = 0; i < 50; ++i)
            assert(!latch.Observe(1.0f));
        assert(!latch.Observe(0.0086f));
        assert(!latch.Latched());
        assert(!latch.Observe(5000.0f)); // two high, one low
        assert(!latch.Latched());
        assert(latch.Observe(0.004f)); // two at each end
        assert(latch.Latched());
        assert(latch.Reason() == LatchReason::BothEnds);
        assert(latch.LowCount() == 2 && latch.HighCount() == 2);
        assert(latch.Lowest() == 0.004f && latch.Highest() == 5000.0f);
        assert(!latch.Observe(1.0f)); // never trips twice
        assert(latch.Latched());
    }

    // Control, as Skynizz logged it: an exposure jumping between 0.00025 and 116, i.e. implied white points of
    // 4000 and 0.0086, latches within a few readings.
    {
        PlausibilityLatch latch;
        bool tripped = false;
        for (int i = 0; i < 6 && !tripped; ++i)
            tripped = latch.Observe(PreExposure(1.0f) / ((i & 1) ? 116.0f : 0.00025f));
        assert(tripped && latch.Latched());
    }

    // Non-finite and non-positive readings are ignored.
    {
        PlausibilityLatch latch;
        for (int i = 0; i < 10; ++i)
        {
            assert(!latch.Observe(NAN));
            assert(!latch.Observe(INFINITY));
            assert(!latch.Observe(0.0f));
            assert(!latch.Observe(-1.0f));
        }
        assert(latch.LowCount() == 0 && latch.HighCount() == 0 && !latch.Latched());
    }

    // Reset() clears everything.
    {
        PlausibilityLatch latch;
        latch.Observe(4000.0f);
        latch.Observe(4000.0f);
        latch.Observe(0.001f);
        assert(latch.Observe(0.001f));
        latch.Reset();
        assert(!latch.Latched());
        assert(latch.Reason() == LatchReason::None);
        assert(latch.LowCount() == 0 && latch.HighCount() == 0);
        assert(latch.Lowest() == 0.0f && latch.Highest() == 0.0f);
        assert(!latch.Observe(1.0f));
    }

    // When latched, WhitePoint(...) for source 1 returns the slider fallback, and LiveWanted is false.
    {
        const float slider = 14.0f;
        assert(WhitePoint(1, true, slider, 1.0f, 0.5f, 1.0f, 0.0f, false) == 2.0f);
        assert(LiveWanted(1, true, true, false, false));
        assert(WhitePoint(1, true, slider, 1.0f, 0.5f, 1.0f, 0.0f, true) == slider);
        assert(!LiveWanted(1, true, true, false, true));
        // Sources 0 and 2 are unaffected by the latch.
        assert(WhitePoint(0, true, slider, 1.0f, 0.5f, 1.0f, 0.0f, true) == slider);
        assert(WhitePoint(2, true, slider, 1.0f, 0.5f, 1.0f, 25.0f, true) == 25.0f);
    }

    std::printf("PASS: exposure plausibility latch -- both ends to latch, Crimson Desert's adaptation and one-sided "
                "extremes stay unlatched, Control's exposure latches, reset, source-1 fallback\n");
    return 0;
}
