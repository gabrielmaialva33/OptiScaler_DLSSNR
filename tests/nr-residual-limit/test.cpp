// The residual limit (docs/dlssnr/design/residual-limit.md), the composition's own text. run.py slices the bound out of
// dlssnr.hlsl -- the guard, its lift and drop, and the residual limit after them -- and compiles it here twice: whole
// (Bound) and cut before the residual limit (GuardOnly), so the two can be held against each other on the same inputs.
// Then the host's clamp of the configured value, the function the D3D12 and Vulkan paths both send the shader.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <shaders/dlssnr/DlssNr_Common.h>

// HLSL's intrinsics, as the shader means them: clamp is min(max()), which ignores a NaN bound as HLSL's min and max do.
static float max(float a, float b) { return std::fmax(a, b); }
static float clamp(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
static float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
static float lerp(float a, float b, float t) { return a + t * (b - a); }
static float smoothstep(float a, float b, float x)
{
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
static float exp2(float v) { return std::exp2(v); }

#include "bound.inc"

namespace
{
int g_cases = 0;

struct Pixel
{
    float amplified, luma, guard;
};

std::vector<Pixel> Pixels()
{
    std::vector<Pixel> pixels;
    for (float guard : { 1.0f, 1.5f, 2.0f, 4.0f, 8.0f })
        for (float luma : { 0.0f, 1e-4f, 1e-3f, 0.004f, 0.01f, 0.1f, 0.5f, 0.75f, 0.9f, 1.0f, 1.2f, 4.0f })
            for (int i = -48; i <= 48; ++i)
                pixels.push_back({ std::exp2(float(i) / 12.0f), luma, guard }); // 1/16 .. 16, 12 steps a stop
    return pixels;
}

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

void Off_IsTheGuardAlone()
{
    // 0 is off: the composition the shader computed before this existed, to the bit. So is anything that is not above
    // zero -- the shader tests "> 0.0", which a NaN fails.
    for (const Pixel& p : Pixels())
    {
        const float reference = GuardOnly(p.amplified, p.luma, p.guard);
        for (float off : { 0.0f, -0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN() })
            assert(SameBits(Bound(p.amplified, p.luma, p.guard, off), reference));
    }
    ++g_cases;
}

void On_BoundsStopsBothWaysInsideTheGuard()
{
    for (float limit : { 0.05f, 0.25f, 0.5f, 1.0f, 2.0f })
        for (const Pixel& p : Pixels())
        {
            const float guarded = GuardOnly(p.amplified, p.luma, p.guard);
            const float bounded = Bound(p.amplified, p.luma, p.guard, limit);
            // Within the limit, either way.
            assert(std::fabs(std::log2(bounded)) <= limit + 1e-6f); // to float rounding of exp2
            // Narrower only: never further from 1 than the guard left it, never on the other side of 1, and a ratio
            // the guard left inside the limit passes untouched.
            assert(std::fabs(std::log2(bounded)) <= std::fabs(std::log2(guarded)) + 1e-7f);
            assert((bounded - 1.0f) * (guarded - 1.0f) >= 0.0f);
            if (std::fabs(std::log2(guarded)) <= limit)
                assert(SameBits(bounded, guarded));
            // So it stays inside the guard's own floor and ceiling, whatever they are for this pixel.
            assert(bounded <= std::fmax(guarded, 1.0f) && bounded >= std::fmin(guarded, 1.0f));
        }
    ++g_cases;
}

void On_IsSymmetricInStops()
{
    // A pixel the guard lets through at 4x up or 4x down, mid-grey, guard 8: a half-stop limit leaves 2^0.5 either way.
    const float up = Bound(4.0f, 0.3f, 8.0f, 0.5f);
    const float down = Bound(0.25f, 0.3f, 8.0f, 0.5f);
    assert(std::fabs(up - std::exp2(0.5f)) < 1e-6f && std::fabs(down - std::exp2(-0.5f)) < 1e-6f);
    assert(std::fabs(std::log2(up) + std::log2(down)) < 1e-6f);
    ++g_cases;
}

void Host_SendsOnlyWhatTheControlOffers()
{
    assert(DlssNr::ResidualLimitStops(0.0f) == 0.0f && DlssNr::ResidualLimitStops(-1.0f) == 0.0f);
    assert(DlssNr::ResidualLimitStops(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    assert(DlssNr::ResidualLimitStops(0.5f) == 0.5f && DlssNr::ResidualLimitStops(kDlssNrResidualLimitMax) == 2.0f);
    assert(DlssNr::ResidualLimitStops(5.0f) == kDlssNrResidualLimitMax);
    assert(DlssNr::ResidualLimitStops(std::numeric_limits<float>::infinity()) == kDlssNrResidualLimitMax);
    // Zero-initialised constants -- every path that never heard of the key -- are off.
    DlssNrConstants constants {};
    assert(constants.ResidualLimit == 0.0f);
    ++g_cases;
}
} // namespace

int main()
{
    Off_IsTheGuardAlone();
    On_BoundsStopsBothWaysInsideTheGuard();
    On_IsSymmetricInStops();
    Host_SendsOnlyWhatTheControlOffers();
    if (g_cases != 4)
        return 2;
    std::printf("PASS %d residual-limit cases on the shader's own bound: off is the guard to the bit (0, -0, negative, "
                "NaN), on bounds both ways in stops, only narrows, stays inside the guard, symmetric; the host sends "
                "only 0..2\n",
                g_cases);
    return 0;
}
