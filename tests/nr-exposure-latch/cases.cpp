#include <dlssnr/DlssNr_Exposure.h>

#include <cassert>
#include <cmath>
#include <cstdio>

int main()
{
    using namespace DlssNr::Exposure;

    // Case 1: a normal game with a stable or slowly moving exposure never latches over 1000 readings.
    {
        PlausibilityLatch latch;
        for (int i = 0; i < 1000; ++i)
        {
            // Exposure oscillates gently between ~0.8 and ~1.2, implying white point ~0.83..1.25.
            const float gameExposure = 1.0f + 0.2f * std::sin(static_cast<float>(i) * 0.05f);
            const float impliedWhite = PreExposure(1.0f) / gameExposure * Trim(1.0f);
            const bool tripped = latch.Observe(impliedWhite);
            assert(!tripped);
            assert(!latch.Latched());
        }
        assert(latch.Reason() == LatchReason::None);
        assert(latch.OutOfRangeCount() == 0);
    }

    // Case 2: one wild reading does not latch.
    {
        PlausibilityLatch latch;
        // On a fresh latch, a single extreme out-of-range reading must never latch it.
        const bool tripped = latch.Observe(1000.0f);
        assert(!tripped);
        assert(!latch.Latched());
        assert(latch.OutOfRangeCount() == 1);
        assert(latch.Reason() == LatchReason::None);
    }

    // Case 3: three out-of-range readings latch (non-consecutive).
    {
        PlausibilityLatch latch;
        // Reading 1: out of range high (> 512)
        assert(!latch.Observe(600.0f));
        assert(!latch.Latched());
        assert(latch.OutOfRangeCount() == 1);

        // Interleaved normal readings
        for (int i = 0; i < 5; ++i)
        {
            assert(!latch.Observe(1.0f));
            assert(!latch.Latched());
        }

        // Reading 2: out of range low (< 0.02)
        assert(!latch.Observe(0.01f));
        assert(!latch.Latched());
        assert(latch.OutOfRangeCount() == 2);

        // Interleaved normal reading
        assert(!latch.Observe(2.0f));
        assert(!latch.Latched());

        // Reading 3: out of range high (> 512) -> latches!
        const bool tripped = latch.Observe(1000.0f);
        assert(tripped);
        assert(latch.Latched());
        assert(latch.OutOfRangeCount() == 3);
        assert(latch.Reason() == LatchReason::OutOfRange);

        // Subsequent readings do not re-trip
        assert(!latch.Observe(1.0f));
        assert(latch.Latched());
    }

    // Case 4: an 8.1-stop swing within 120 readings latches, while the same swing spread over 200 readings does not.
    {
        // Part A: 8.1-stop swing within 120 readings latches.
        {
            PlausibilityLatch latch;
            const float start = 1.0f;
            const float stops = 8.1f;
            bool latchedDuring = false;

            for (int i = 0; i < 120; ++i)
            {
                // Smooth exponential progression spanning exactly 8.1 stops from 1.0 to 2^8.1 (~274.37).
                // All values remain within the plausible bounds [0.02, 512.0], so outOfRange is 0.
                const float impliedWhite = start * std::pow(2.0f, stops * static_cast<float>(i) / 119.0f);
                if (latch.Observe(impliedWhite))
                {
                    latchedDuring = true;
                    assert(latch.Reason() == LatchReason::Swinging);
                }
            }

            assert(latchedDuring);
            assert(latch.Latched());
            assert(latch.Reason() == LatchReason::Swinging);
            assert(latch.OutOfRangeCount() == 0);
            assert(latch.StopsSpan() >= 8.0f);
        }

        // Part B: the same 8.1-stop swing spread over 200 readings does not latch.
        {
            PlausibilityLatch latch;
            const float start = 1.0f;
            const float stops = 8.1f;

            for (int i = 0; i < 200; ++i)
            {
                // Over 200 readings, any 120-reading window spans at most 8.1 * 119 / 199 ~= 4.84 stops < 8.
                const float impliedWhite = start * std::pow(2.0f, stops * static_cast<float>(i) / 199.0f);
                const bool tripped = latch.Observe(impliedWhite);
                assert(!tripped);
                assert(!latch.Latched());
            }

            assert(!latch.Latched());
            assert(latch.Reason() == LatchReason::None);
            assert(latch.OutOfRangeCount() == 0);
        }
    }

    // Case 5: Reset() clears everything.
    {
        PlausibilityLatch latch;
        // Trip latch via 3 out-of-range readings.
        assert(!latch.Observe(1000.0f));
        assert(!latch.Observe(1000.0f));
        assert(latch.Observe(1000.0f));
        assert(latch.Latched());

        latch.Reset();
        assert(!latch.Latched());
        assert(latch.Reason() == LatchReason::None);
        assert(latch.OutOfRangeCount() == 0);
        assert(latch.ReadingsSpan() == 0);

        // After reset, it functions normally again.
        assert(!latch.Observe(1.0f));
        assert(!latch.Latched());
    }

    // Case 6: when latched, WhitePoint(...) for source 1 returns the slider fallback, and LiveWanted is false.
    {
        const float slider = 14.0f;
        const float trim = 1.0f;
        const float heldExposure = 0.5f;
        const float preExposure = 1.0f;
        const float anchored = 0.0f;

        // Unlatched source 1: computes preExposure / heldExposure * trim = 1.0 / 0.5 * 1.0 = 2.0.
        const float unlatchedWp = WhitePoint(1, true, slider, trim, heldExposure, preExposure, anchored, false);
        assert(unlatchedWp == 2.0f);
        assert(LiveWanted(1, true, true, false, false));

        // Latched source 1: falls back to the slider value (14.0f).
        const float latchedWp = WhitePoint(1, true, slider, trim, heldExposure, preExposure, anchored, true);
        assert(latchedWp == slider);
        assert(!LiveWanted(1, true, true, false, true));

        // Source 0 and source 2 are unaffected by the latch state.
        assert(WhitePoint(0, true, slider, trim, heldExposure, preExposure, anchored, true) == slider);
        assert(WhitePoint(2, true, slider, trim, heldExposure, preExposure, 25.0f, true) == 25.0f);
    }

    std::printf("PASS: all exposure plausibility latch test cases succeeded\n");
    return 0;
}
