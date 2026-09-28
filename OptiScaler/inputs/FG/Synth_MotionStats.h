#pragma once

// What the sampled rows of the synthesized motion field say, for SynthFgPolicy. Pure: it reads the rows
// exactly as SynthInputs reads them back from the GPU (R16G16_FLOAT, a pitch per row, every stride-th
// pixel), so the host tests drive it with the production layout (tests/fg-synth-policy). Design:
// dlssnr/design/synthesized-frame-generation.md, "Fast-motion response", "Coherence, not speed".
//
// Speed alone does not decide whether interpolation fails: a uniform pan with accurate vectors
// interpolates at any speed. It fails where neighbouring vectors disagree (parallax, disocclusion, a thin
// foreground over a far background), so besides the magnitudes this measures how many neighbouring
// samples disagree.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SynthMotionStats
{

// The readback layout: kRows rows spread evenly over the height, every kStride-th pixel of each read.
constexpr uint32_t kRows = 16;
constexpr uint32_t kStride = 4;

// Two horizontally neighbouring vectors disagree when they differ by more than the larger of:
// - kPairMinPx: below 2 px a disagreement moves content by at most 1 px at the midpoint frame; it is
//   the estimator's own noise, not a depth edge;
// - kPairRelative of the larger vector: the field is bilinear between 8x8 block centres, so a smooth
//   field (zoom, walking forward) changes by a few percent over the 4 px sample spacing, while a depth
//   edge shows as two steps of half the difference between the blocks (40 px over 10 px: 15 px steps
//   against a 10 px limit).
// Only horizontal pairs: the sampled rows are a sixteenth of the height apart, which is not local, and
// over that distance a smooth field legitimately changes by more than a quarter.
constexpr float kPairMinPx = 2.0f;
constexpr float kPairRelative = 0.25f;

// The median vector is taken over every fourth finite vector (16 px apart, 3440 of them at 3440 wide). It
// describes the dominant motion, which that many samples pin down as well as all of them; its two
// selections over every vector tripled the CPU cost of a readback, on the present thread.
constexpr uint32_t kMedianVectorEvery = 4;

// One readback of the sampled rows.
struct Stats
{
    uint32_t vectors = 0;       // finite vectors read
    uint32_t pairs = 0;         // horizontally neighbouring pairs of finite vectors compared
    float medianPx = 0.0f;      // median |v|, display pixels per base frame (the upper one for an even count)
    float p90Px = 0.0f;         // 90th percentile of |v|, nearest rank
    float medianX = 0.0f;       // the median vector, per component, over every kMedianVectorEvery-th vector
    float medianY = 0.0f;       //
    float medianOfWidth = 0.0f; // medianPx as a fraction of the field's width
    float incoherent = 0.0f;    // share of the pairs that disagree, 0..1
    bool allZero = true;        // every sampled vector exactly zero, negative zero included
};

// Reused between readbacks, so a frame's statistics allocate nothing once warm.
struct Scratch
{
    std::vector<float> magnitudes;
    std::vector<float> xs;
    std::vector<float> ys;
};

inline float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x3ffu;
    uint32_t bits;

    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            // Subnormal: normalise it.
            int e = -1;
            do
            {
                ++e;
                mantissa <<= 1;
            } while ((mantissa & 0x400u) == 0);

            bits = sign | ((uint32_t) (127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
        }
    }
    else if (exponent == 0x1f)
    {
        bits = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }

    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// With the magnitudes already known, as Measure has them.
inline bool Disagree(float ax, float ay, float aMagnitude, float bx, float by, float bMagnitude)
{
    const float limit = std::max(kPairMinPx, kPairRelative * std::max(aMagnitude, bMagnitude));
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy > limit * limit;
}

inline bool Disagree(float ax, float ay, float bx, float by)
{
    return Disagree(ax, ay, std::sqrt(ax * ax + ay * ay), bx, by, std::sqrt(bx * bx + by * by));
}

// rows rows of R16G16_FLOAT starting at base, rowPitch bytes apart, width pixels each; every stride-th
// pixel from x = 0 is read. Non-finite vectors are left out of every statistic but allZero, and break
// the pair on either side of them.
inline Stats Measure(const uint8_t* base, uint32_t rows, size_t rowPitch, uint32_t width, uint32_t stride,
                     Scratch& scratch)
{
    Stats stats {};
    scratch.magnitudes.clear();
    scratch.xs.clear();
    scratch.ys.clear();

    if (base == nullptr || stride == 0)
        return stats;

    uint32_t disagreeing = 0;

    for (uint32_t i = 0; i < rows; ++i)
    {
        const auto* row = reinterpret_cast<const uint16_t*>(base + (size_t) i * rowPitch);
        bool previousValid = false;
        float previousX = 0.0f;
        float previousY = 0.0f;
        float previousMagnitude = 0.0f;

        for (uint32_t x = 0; x < width; x += stride)
        {
            const uint16_t hx = row[2 * (size_t) x];
            const uint16_t hy = row[2 * (size_t) x + 1];

            // Negative zero is zero.
            if (((hx | hy) & 0x7fffu) != 0)
                stats.allZero = false;

            const float fx = HalfToFloat(hx);
            const float fy = HalfToFloat(hy);
            const float magnitude = std::sqrt(fx * fx + fy * fy);

            if (!std::isfinite(magnitude))
            {
                previousValid = false;
                continue;
            }

            scratch.magnitudes.push_back(magnitude);
            if (scratch.magnitudes.size() % kMedianVectorEvery == 1)
            {
                scratch.xs.push_back(fx);
                scratch.ys.push_back(fy);
            }

            if (previousValid)
            {
                ++stats.pairs;
                disagreeing += Disagree(previousX, previousY, previousMagnitude, fx, fy, magnitude) ? 1 : 0;
            }

            previousValid = true;
            previousX = fx;
            previousY = fy;
            previousMagnitude = magnitude;
        }
    }

    const size_t count = scratch.magnitudes.size();
    stats.vectors = (uint32_t) count;

    if (count == 0)
        return stats;

    auto& magnitudes = scratch.magnitudes;
    const auto middle = magnitudes.begin() + count / 2;
    std::nth_element(magnitudes.begin(), middle, magnitudes.end());
    stats.medianPx = *middle;

    // Nearest rank, ceil(0.9 n) - 1, which is never below the median's index: only the upper part needs
    // ordering.
    const auto p90 = magnitudes.begin() + ((9 * count + 9) / 10 - 1);
    std::nth_element(middle, p90, magnitudes.end());
    stats.p90Px = *p90;

    const size_t vectorMiddle = scratch.xs.size() / 2;
    std::nth_element(scratch.xs.begin(), scratch.xs.begin() + vectorMiddle, scratch.xs.end());
    std::nth_element(scratch.ys.begin(), scratch.ys.begin() + vectorMiddle, scratch.ys.end());
    stats.medianX = scratch.xs[vectorMiddle];
    stats.medianY = scratch.ys[vectorMiddle];

    stats.medianOfWidth = width > 0 ? stats.medianPx / (float) width : 0.0f;
    stats.incoherent = stats.pairs > 0 ? (float) disagreeing / (float) stats.pairs : 0.0f;
    return stats;
}

// The samples of one summary window (SynthInputs logs one every 10 s): the mean and the maximum of each
// statistic the policy decides on, so its thresholds can be tuned from the log.
struct Window
{
    uint32_t samples = 0;
    double sumMedianPx = 0.0;
    double sumP90Px = 0.0;
    double sumIncoherent = 0.0;
    float maxMedianPx = 0.0f;
    float maxMedianOfWidth = 0.0f;
    float maxP90Px = 0.0f;
    float maxIncoherent = 0.0f;

    void Add(const Stats& stats)
    {
        ++samples;
        sumMedianPx += stats.medianPx;
        sumP90Px += stats.p90Px;
        sumIncoherent += stats.incoherent;
        maxMedianPx = std::max(maxMedianPx, stats.medianPx);
        maxMedianOfWidth = std::max(maxMedianOfWidth, stats.medianOfWidth);
        maxP90Px = std::max(maxP90Px, stats.p90Px);
        maxIncoherent = std::max(maxIncoherent, stats.incoherent);
    }

    float MeanMedianPx() const { return samples > 0 ? (float) (sumMedianPx / samples) : 0.0f; }
    float MeanP90Px() const { return samples > 0 ? (float) (sumP90Px / samples) : 0.0f; }
    float MeanIncoherent() const { return samples > 0 ? (float) (sumIncoherent / samples) : 0.0f; }
};

} // namespace SynthMotionStats
