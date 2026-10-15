#include <dlssnr/DlssNr_Exposure.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Shader
{
struct int3
{
    int3(int, int, int) {}
};
struct ExposureTexture
{
    float r = 1.0f;
    ExposureTexture Load(int3) const { return *this; }
} gExposure;
unsigned gUseGameExposure = 1;
float gExposurePreMul = 1.0f;
float gWhitePoint = 7.0f;
inline float max(float a, float b) { return std::max(a, b); }
inline float clamp(float a, float b, float c) { return std::clamp(a, b, c); }
#include "white_point.h"
} // namespace Shader

int main()
{
    using namespace DlssNr::Exposure;

    // Keep the CPU fallback's own policy, including single-sided extremes and sanitization.
    assert(WhitePoint(1, true, 7.0f, 1.0f, 1000.0f, 1.0f, 0.0f) == 0.01f);
    assert(WhitePoint(1, true, 7.0f, 1.0f, 0.00001f, 1.0f, 0.0f) == 4096.0f);
    assert(WhitePoint(1, true, 7.0f, NAN, 0.5f, NAN, 0.0f) == 2.0f);
    assert(WhitePoint(1, true, 7.0f, 1.0f, NAN, 1.0f, 0.0f) == 7.0f);

    using Shader::gExposure;
    using Shader::gExposurePreMul;
    using Shader::gUseGameExposure;
    using Shader::gWhitePoint;
    for (float sample : { NAN, INFINITY, -INFINITY, -1.0f, 0.0f, 1e-6f, 1e6f })
    {
        gExposure.r = sample;
        assert(Shader::WhitePoint() == gWhitePoint);
    }
    gExposure.r = 1.0f;
    for (float ratio : { NAN, INFINITY, -INFINITY, -1.0f, 0.0f, 0.009f, 4097.0f, std::nextafter(0.01f, 0.0f),
                         std::nextafter(4096.0f, INFINITY) })
    {
        gExposurePreMul = ratio;
        assert(Shader::WhitePoint() == gWhitePoint);
    }
    // Overflow during division also falls through.
    gExposure.r = 0.5f;
    gExposurePreMul = std::numeric_limits<float>::max();
    assert(Shader::WhitePoint() == gWhitePoint);
    gExposure.r = 1.0f;
    for (float ratio : { 0.01f, 0.1f, 1.0f, 307.0f, 4096.0f })
    {
        gExposurePreMul = ratio;
#ifdef VK_MODE
        assert(Shader::WhitePoint() == gWhitePoint);
#else
        assert(Shader::WhitePoint() == ratio);
#endif
    }
    gUseGameExposure = 0;
    assert(Shader::WhitePoint() == gWhitePoint);
    gWhitePoint = 0.0f;
    assert(Shader::WhitePoint() == 1e-4f);

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
        const float logged[] = { 108.85f, 63.87f, 303.6f, 0.2836f, 1.07f,  0.43f,  0.10f,  3.63f,  2.71f,
                                 1.72f,   19.58f, 15.13f, 8.52f,   307.0f, 0.103f, 28.08f, 50.48f, 11.68f };
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
