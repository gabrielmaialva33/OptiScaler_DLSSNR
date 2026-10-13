// The HDR present conversion's maths (docs/dlssnr/design/hdr-present-conversion.md), the shader's own text. run.py
// slices the block between the markers in dlssnr_present_hdr.hlsl and compiles it here, inside namespace hlsl with
// HLSL's intrinsics as the shader means them, beside the float64 reference in dlssnr/DlssNr_PresentHdr.h. FP16 is
// emulated with _Float16 (IEEE binary16, round to nearest even), the format of the work texture.
//
// What this proves: the decode, the encode and the bypass rule as written, on the CPU, in float32. What it does not:
// the GPU's pow (DXIL's exp2/log2 are approximations), the UNORM load and store, and the dispatch. The wine-tier test
// the design names (tests/nr-hdr-d3d12) is where those are measured; it does not exist yet.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include <dlssnr/DlssNr_PresentHdr.h>

namespace hlsl
{
using uint = unsigned int;

struct float3
{
    float x, y, z;
    float3() = default;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};

inline float3 operator+(float3 a, float3 b) { return float3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline float3 operator-(float3 a, float3 b) { return float3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline float3 operator*(float s, float3 v) { return float3(s * v.x, s * v.y, s * v.z); }

// HLSL's min and max ignore a NaN operand, as fmin and fmax do; saturate of NaN is 0.
inline float max(float a, float b) { return std::fmax(a, b); }
inline float3 max(float3 v, float s) { return float3(max(v.x, s), max(v.y, s), max(v.z, s)); }
inline float abs(float v) { return std::fabs(v); }
inline float3 abs(float3 v) { return float3(abs(v.x), abs(v.y), abs(v.z)); }
inline float saturate(float v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }
inline float pow(float a, float b) { return std::pow(a, b); }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline bool isfinite(float v) { return std::isfinite(v); }

#include "conversion.inc"
} // namespace hlsl

namespace
{
using hlsl::float3;
namespace ref = DlssNr::PresentHdr;

int g_cases = 0;

float Half(float v) { return static_cast<float>(static_cast<_Float16>(v)); }
float3 Half(float3 v) { return float3(Half(v.x), Half(v.y), Half(v.z)); }
bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }
bool SameBits(float3 a, float3 b) { return SameBits(a.x, b.x) && SameBits(a.y, b.y) && SameBits(a.z, b.z); }
int Code(float unorm) { return static_cast<int>(std::lrint(unorm * 1023.0f)); }
float Unorm(int code) { return static_cast<float>(code) / 1023.0f; }

// A deterministic LCG, so a failure names a texel that can be reproduced.
uint32_t g_seed = 0x2408u;
int NextCode()
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<int>((g_seed >> 8) % 1024u);
}

// What the composition can do to a texel it did not change: nothing, or divide by the white point and multiply back in
// float32, then store FP16 again.
std::vector<float3> UntouchedVariants(float3 work)
{
    std::vector<float3> variants { work };
    for (float white : { 1.0f, ref::kSdrWhiteScRgb, ref::kSdrWhiteScRgb * 0.37f, ref::kSdrWhiteScRgb * 3.1f })
        variants.push_back(Half(float3(work.x / white * white, work.y / white * white, work.z / white * white)));
    return variants;
}

void PqTransfer_MatchesTheReference()
{
    double worst = 0.0;
    for (int k = 0; k <= 1023; ++k)
    {
        const double reference = ref::PqToScRgb(k / 1023.0);
        const double shader = hlsl::PqToScRgb(Unorm(k));
        if (reference > 1e-6)
            worst = std::fmax(worst, std::fabs(shader - reference) / reference);
        else
            assert(std::fabs(shader - reference) < 1e-9);
    }
    // Measured 5.1e-5 at the top codes (the 1/m2 power flattens the code, the 1/m1 power amplifies what float32 lost):
    // under FP16's own rounding (2^-11, 4.9e-4) and far under a PQ code step (0.9%).
    assert(worst < 1e-4);
    std::printf("  PQ decode, float32 against float64: worst relative error %.2e over all 1024 codes\n", worst);
    ++g_cases;
}

void PqRoundTrip_EveryCodeIsItself()
{
    // In float32, and through the FP16 work texture: the encode of a decoded code rounds back to that code, every one
    // of the 1024, with no bypass to help. This is the precision the bypass is not needed for on a neutral pixel.
    for (int k = 0; k <= 1023; ++k)
    {
        const float light = hlsl::PqToScRgb(Unorm(k));
        assert(Code(hlsl::ScRgbToPq(light)) == k);
        assert(Code(hlsl::ScRgbToPq(Half(light))) == k);
        assert(Code(static_cast<float>(ref::ScRgbToPq(ref::PqToScRgb(k / 1023.0)))) == k);
    }
    std::printf("  PQ round trip: all 1024 codes return as themselves in float64, float32 and through FP16\n");
    ++g_cases;
}

void PqStep_IsCoarserThanTheBypassAndTheStabilizerUnit()
{
    // The finest relative step between neighbouring codes, from code 64 up. The bypass slack must sit well inside it
    // (so the bypass cannot hide a step on a neutral pixel), and the stabilizer's HDR10 unit must not exceed it (so a
    // creep step is never coarser than one code).
    double finest = 1.0;
    int at = 0;
    for (int k = 64; k < 1023; ++k)
    {
        const double low = ref::PqToScRgb(k / 1023.0);
        const double step = (ref::PqToScRgb((k + 1) / 1023.0) - low) / low;
        if (step < finest)
        {
            finest = step;
            at = k;
        }
    }
    assert(finest > 4.0 * ref::kBypassEps);
    assert(ref::kPqQuantRel <= finest);
    std::printf("  finest PQ step from code 64: %.4f%% at code %d; bypass slack %.4f%%, stabilizer unit %.4f%%\n",
                finest * 100.0, at, ref::kBypassEps * 100.0, ref::kPqQuantRel * 100.0);
    ++g_cases;
}

void Hdr10Decode_MatchesTheReferenceAndKeepsNegatives()
{
    // Pure Rec.2020 primaries leave Rec.709: their decode has negative channels, and they are kept, not clamped.
    for (int channel = 0; channel < 3; ++channel)
    {
        float3 code(0.0f, 0.0f, 0.0f);
        (channel == 0 ? code.x : channel == 1 ? code.y : code.z) = Unorm(768);
        const float3 light = hlsl::DecodeOriginal(code, hlsl::kProfileHdr10);
        assert(light.x < 0.0f || light.y < 0.0f || light.z < 0.0f);

        const double codes[3] = { code.x, code.y, code.z };
        double reference[3] {};
        ref::DecodeHdr10(codes, reference);
        const float got[3] = { light.x, light.y, light.z };
        for (int c = 0; c < 3; ++c)
            assert(std::fabs(got[c] - reference[c]) <= 1e-4 * std::fmax(1.0, std::fabs(reference[c])));
    }
    ++g_cases;
}

void Hdr10Bypass_UntouchedTexelsReturnBitForBit()
{
    // Every code on each channel alone and on all three, a grid of primaries and secondaries, and random texels: decode,
    // store to FP16, let the composition leave it alone (or divide and multiply by the white point), encode with the
    // original as reference. The original comes back bit for bit, so the UNORM store writes the game's own code.
    std::vector<float3> originals;
    for (int k = 0; k <= 1023; ++k)
    {
        originals.push_back(float3(Unorm(k), 0.0f, 0.0f));
        originals.push_back(float3(0.0f, Unorm(k), 0.0f));
        originals.push_back(float3(0.0f, 0.0f, Unorm(k)));
        originals.push_back(float3(Unorm(k), Unorm(k), Unorm(k)));
    }
    const int levels[] = { 0, 1, 2, 16, 64, 128, 256, 384, 512, 594, 640, 768, 896, 1023 };
    for (int r : levels)
        for (int g : levels)
            for (int b : levels)
                originals.push_back(float3(Unorm(r), Unorm(g), Unorm(b)));
    for (int i = 0; i < 200000; ++i)
        originals.push_back(float3(Unorm(NextCode()), Unorm(NextCode()), Unorm(NextCode())));

    size_t texels = 0, wrongWithout = 0;
    int worstWithout = 0;
    for (const float3& original : originals)
    {
        const float3 work = Half(hlsl::DecodeOriginal(original, hlsl::kProfileHdr10));
        for (const float3& processed : UntouchedVariants(work))
        {
            assert(SameBits(hlsl::EncodeTexel(processed, original, hlsl::kProfileHdr10), original));
            ++texels;
        }

        // The same texel re-encoded from the work texture with no reference: what the bypass is for.
        const float3 direct = hlsl::EncodeHdr10(work);
        const int moved = std::max({ std::abs(Code(direct.x) - Code(original.x)),
                                     std::abs(Code(direct.y) - Code(original.y)),
                                     std::abs(Code(direct.z) - Code(original.z)) });
        wrongWithout += moved != 0 ? 1 : 0;
        worstWithout = std::max(worstWithout, moved);
    }
    // Saturated Rec.2020 colours lose a channel near zero to FP16's rounding of the Rec.709 values they cancel from.
    assert(wrongWithout > 0);
    std::printf("  HDR10 untouched: %zu texels return bit for bit with the reference; without it %.1f%% of %zu "
                "originals would move, by up to %d codes\n",
                texels, 100.0 * wrongWithout / originals.size(), originals.size(), worstWithout);
    ++g_cases;
}

void Hdr10Bypass_HidesNoCodeInsideRec709()
{
    // The bypass accepts a texel within its slack of the original; for a colour inside Rec.709 nothing in that slack is
    // a code away. Outside Rec.709 a Rec.2020 channel near zero is the difference of large Rec.709 values, and the
    // slack -- 0.2% of the pixel's light -- can be many PQ codes in that dark channel; reported, not bounded here.
    int worstInside = 0, worstOutside = 0;
    size_t inside = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const int codes[3] = { NextCode(), NextCode(), NextCode() };
        const double unorm[3] = { codes[0] / 1023.0, codes[1] / 1023.0, codes[2] / 1023.0 };
        double decoded[3] {};
        ref::DecodeHdr10(unorm, decoded);
        const bool inGamut = decoded[0] >= 0.0 && decoded[1] >= 0.0 && decoded[2] >= 0.0;
        inside += inGamut ? 1 : 0;

        for (int corner = 0; corner < 8; ++corner)
        {
            double processed[3] {};
            for (int c = 0; c < 3; ++c)
            {
                const double slack = ref::kBypassEps * std::fmax(std::fabs(decoded[c]), double(ref::kBypassFloor));
                processed[c] = decoded[c] + ((corner >> c) & 1 ? 0.999 : -0.999) * slack;
            }
            assert(ref::KeepsOriginal(processed, decoded));
            double encoded[3] {};
            ref::EncodeHdr10(processed, encoded);
            int moved = 0;
            for (int c = 0; c < 3; ++c)
                moved = std::max(moved, std::abs(static_cast<int>(std::lrint(encoded[c] * 1023.0)) - codes[c]));
            (inGamut ? worstInside : worstOutside) = std::max(inGamut ? worstInside : worstOutside, moved);
        }
    }
    assert(inside > 1000 && worstInside == 0);
    std::printf("  bypass slack inside Rec.709 (%zu texels): no code; outside: up to %d codes in a channel near zero\n",
                inside, worstOutside);
    ++g_cases;
}

void Hdr10Encode_AnEditIsWithinOneCodeOfTheReference()
{
    // A texel the composition did change is re-encoded from FP16: within one code of the float64 encode of the same
    // edit, and the bypass does not take it.
    int worst = 0;
    for (int i = 0; i < 50000; ++i)
    {
        const float3 original(Unorm(NextCode()), Unorm(NextCode()), Unorm(NextCode()));
        const float3 decoded = hlsl::DecodeOriginal(original, hlsl::kProfileHdr10);
        const float3 edited = Half(1.25f * Half(decoded) + float3(0.01f, 0.0f, 0.02f));
        const float3 written = hlsl::EncodeTexel(edited, original, hlsl::kProfileHdr10);
        if (SameBits(written, original))
            continue; // a black texel the small edit stays inside the floor of; nothing to compare

        const double light[3] = { edited.x, edited.y, edited.z };
        double reference[3] {};
        ref::EncodeHdr10(light, reference);
        const float got[3] = { written.x, written.y, written.z };
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(Code(got[c]) - static_cast<int>(std::lrint(reference[c] * 1023.0))));
    }
    assert(worst <= 1);
    std::printf("  HDR10 edited: within %d code of the float64 encode of the same edit\n", worst);
    ++g_cases;
}

void ScRgb_EveryHalfComesBackAsItself()
{
    // scRGB's decode is a copy and its encode stores the work texture: every finite half, negatives, subnormals and
    // values far above 1 included, returns bit for bit, untouched or through the white point's divide and multiply. A
    // NaN or Inf is black to the model and comes back as itself.
    size_t finite = 0, special = 0;
    for (uint32_t bits = 0; bits <= 0xFFFFu; ++bits)
    {
        const uint16_t half16 = static_cast<uint16_t>(bits);
        _Float16 h;
        std::memcpy(&h, &half16, sizeof(h));
        const float value = static_cast<float>(h);
        const float3 original(value, -0.0f, 1.0f);
        const float3 decoded = hlsl::DecodeOriginal(original, hlsl::kProfileScRgb);
        if (!std::isfinite(value))
        {
            assert(decoded.x == 0.0f);
            assert(SameBits(hlsl::EncodeTexel(Half(decoded), original, hlsl::kProfileScRgb), original));
            ++special;
            continue;
        }
        assert(SameBits(decoded, original));
        for (const float3& processed : UntouchedVariants(Half(decoded)))
            assert(SameBits(hlsl::EncodeTexel(processed, original, hlsl::kProfileScRgb), original));
        ++finite;
    }
    assert(finite == 63488 && special == 2048);
    ++g_cases;
}

void NonFiniteProcessed_KeepsTheOriginal()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float3 hdr10(Unorm(700), Unorm(300), Unorm(100));
    const float3 scrgb(3.5f, -0.25f, 140.0f);
    for (const float3& processed : { float3(nan, 0.0f, 0.0f), float3(0.0f, inf, 0.0f), float3(0.0f, 0.0f, -inf) })
    {
        assert(SameBits(hlsl::EncodeTexel(processed, hdr10, hlsl::kProfileHdr10), hdr10));
        assert(SameBits(hlsl::EncodeTexel(processed, scrgb, hlsl::kProfileScRgb), scrgb));
    }
    // And a changed scRGB texel is stored as it is, negatives and values above 1 included.
    const float3 changed(4.0f, -0.5f, 200.0f);
    assert(SameBits(hlsl::EncodeTexel(changed, scrgb, hlsl::kProfileScRgb), changed));
    ++g_cases;
}
} // namespace

int main()
{
    PqTransfer_MatchesTheReference();
    PqRoundTrip_EveryCodeIsItself();
    PqStep_IsCoarserThanTheBypassAndTheStabilizerUnit();
    Hdr10Decode_MatchesTheReferenceAndKeepsNegatives();
    Hdr10Bypass_UntouchedTexelsReturnBitForBit();
    Hdr10Bypass_HidesNoCodeInsideRec709();
    Hdr10Encode_AnEditIsWithinOneCodeOfTheReference();
    ScRgb_EveryHalfComesBackAsItself();
    NonFiniteProcessed_KeepsTheOriginal();

    if (g_cases != 9)
        return 2; // a runner that exercises nothing must fail loudly
    std::printf("hdr math: %d cases passed\n", g_cases);
    return 0;
}
