#pragma once

// HDR10 and scRGB at present (docs/dlssnr/design/hdr-present-conversion.md): the numbers the present hosts and the
// conversion shader share, and a float64 reference of the shader's maths. Standard library only, so tests/nr-hdr-math
// compiles it as it is.
//
// The working representation is linear Rec.709 light in scRGB units (1.0 = 80 nits), in FP16. HDR10 is PQ (SMPTE ST
// 2084), which is absolute: a code of 1.0 is 10,000 nits, 125 scRGB units. The shader,
// shaders/dlssnr/precompile/dlssnr_present_hdr.hlsl, carries the same literals; nr-hdr-math compares the two texts
// and runs the shader's own block against the reference below, so a number changed on one side alone fails there.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::PresentHdr
{
// The shader's profile and mode switches (Constants::Profile and ::Mode, gProfile and gMode).
inline constexpr uint32_t kProfileHdr10 = 1; // R10G10B10A2_UNORM, PQ, Rec.2020
inline constexpr uint32_t kProfileScRgb = 2; // R16G16B16A16_FLOAT, linear, Rec.709
inline constexpr uint32_t kModeDecode = 0;   // the original into the FP16 work texture
inline constexpr uint32_t kModeEncode = 1;   // the work texture back into the swapchain's format, original as reference

// ST 2084, exactly: every one of these is a dyadic fraction, so the decimal is the number.
inline constexpr double kPqM1 = 2610.0 / 16384.0;        // 0.1593017578125
inline constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0; // 78.84375
inline constexpr double kPqC1 = 3424.0 / 4096.0;         // 0.8359375
inline constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;  // 18.8515625
inline constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;  // 18.6875
inline constexpr double kPqPeakNits = 10000.0;
inline constexpr double kScRgbUnitNits = 80.0;
inline constexpr double kPqPeakScRgb = kPqPeakNits / kScRgbUnitNits; // 125

// BT.2408 fallback when the target's DISPLAYCONFIG_SDR_WHITE_LEVEL cannot be used. This changes what the model
// sees, not the reference reconstruction: composition divides by the same white that it multiplies back.
inline constexpr float kSdrWhiteNits = 203.0f;
inline constexpr float kSdrWhiteScRgb = kSdrWhiteNits / 80.0f;
struct WhiteLevel
{
    float nits = kSdrWhiteNits;
    bool fromDisplay = false;
};
inline WhiteLevel SelectSdrWhite(uint32_t level, bool queried)
{
    // Windows reports thousandths of an 80-nit unit. The supported SDR slider range is 80..480 nits;
    // reject failed/stub/implausible answers rather than normalizing the model against zero or garbage.
    if (!queried || level < 1000 || level > 6000)
        return {};
    return { static_cast<float>(level) * 0.08f, true };
}
inline const char* WhiteSource(WhiteLevel white)
{
    return white.fromDisplay ? "Windows SDR white" : "203-nit fallback";
}
class WhiteCache
{
    uintptr_t _monitor = 0;
    uint64_t _at = 0;
    bool _known = false;
    WhiteLevel _value;

  public:
    bool NeedsRefresh(uintptr_t monitor, uint64_t now) const
    {
        return !_known || monitor != _monitor || now < _at || now - _at >= 5000;
    }
    void Store(uintptr_t monitor, uint64_t now, WhiteLevel value)
    {
        _monitor = monitor;
        _at = now;
        _known = true;
        _value = value;
    }
    WhiteLevel Value() const { return _value; }
};

// Rec.2020 to Rec.709 and back, linear light, D65 white, row-major (out = M * in). tests/nr-hdr-math derives both
// from the primaries and holds these, and the shader's, to the derivation.
inline constexpr double kRec2020To709[9] = {
    1.660491002108,  -0.587641138789, -0.072849863320, //
    -0.124550474522, 1.132899897126,  -0.008349422604, //
    -0.018150763355, -0.100578898008, 1.118729661363,
};
inline constexpr double kRec709To2020[9] = {
    0.627403895935, 0.329283038378, 0.043313065687, //
    0.069097289358, 0.919540395075, 0.011362315566, //
    0.016391438875, 0.088013307877, 0.895595253248,
};

// The encode's reference bypass (design section 4.2, rule 2): a texel whose every channel is within kBypassEps of the
// decoded original, relative to max(|original|, kBypassFloor), is written back as the original code. Four times FP16's
// largest rounding error (2^-11; two ulps at the bottom of a binade, one at the top): the FP16 store of the decode and
// the composition's divide and multiply by the white point move an untouched value by less, and from code 64 up no
// HDR10 code is within 0.9% of the next (nr-hdr-math measures both).
inline constexpr float kBypassEps = 1.0f / 512.0f;     // 2^-9
inline constexpr float kBypassFloor = 1.0f / 16384.0f; // 2^-14, FP16's smallest normal

// One PQ code, relative to the light it encodes, where codes are finest -- 0.90%, near code 800, over codes 64 to 1023
// -- rounded down to a power of two. The output stabilizer's minimum creep step on an HDR10 frame (design section 5.6),
// so the step is that of the format the frame goes out in, not of the FP16 work texture, and never coarser than one
// code anywhere in that range. scRGB goes out in FP16 and keeps the stabilizer's own FP16 step.
inline constexpr float kPqQuantRel = 1.0f / 128.0f;

// --- float64 reference of the shader's maths ---------------------------------------------------------------------

// One PQ code (0..1) to scRGB units, and back. Out-of-range input is clamped as the shader's saturate clamps it.
inline double PqToScRgb(double code)
{
    const double e = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / kPqM2);
    return kPqPeakScRgb * std::pow(std::max(e - kPqC1, 0.0) / (kPqC2 - kPqC3 * e), 1.0 / kPqM1);
}

inline double ScRgbToPq(double value)
{
    const double y = std::pow(std::clamp(value / kPqPeakScRgb, 0.0, 1.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

inline void Apply(const double (&m)[9], const double in[3], double out[3])
{
    for (int r = 0; r < 3; ++r)
        out[r] = m[3 * r] * in[0] + m[3 * r + 1] * in[1] + m[3 * r + 2] * in[2];
}

// An HDR10 texel's codes (0..1) to linear Rec.709 in scRGB units. Negative where Rec.2020 leaves Rec.709; kept.
inline void DecodeHdr10(const double code[3], double out[3])
{
    const double rec2020[3] = { PqToScRgb(code[0]), PqToScRgb(code[1]), PqToScRgb(code[2]) };
    Apply(kRec2020To709, rec2020, out);
}

// Linear Rec.709 in scRGB units to HDR10 codes (0..1, before the UNORM store rounds them). Rec.2020 light outside
// 0..125 units (0..10,000 nits) is clamped to it, channel by channel.
inline void EncodeHdr10(const double light[3], double out[3])
{
    double rec2020[3] {};
    Apply(kRec709To2020, light, rec2020);
    for (int c = 0; c < 3; ++c)
        out[c] = ScRgbToPq(rec2020[c]);
}

// The bypass rule on one texel: processed and decoded are the working-space values, decoded without FP16's rounding.
inline bool KeepsOriginal(const double processed[3], const double decoded[3])
{
    for (int c = 0; c < 3; ++c)
    {
        if (!std::isfinite(processed[c]))
            return true;
    }
    for (int c = 0; c < 3; ++c)
    {
        const double slack = kBypassEps * std::max(std::fabs(decoded[c]), static_cast<double>(kBypassFloor));
        if (!(std::fabs(processed[c] - decoded[c]) <= slack))
            return false;
    }
    return true;
}
} // namespace DlssNr::PresentHdr
