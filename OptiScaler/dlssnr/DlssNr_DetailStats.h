#pragma once

// "Measure detail": pure CPU reduction and statistics aggregation for in-game detail, flicker,
// colour and shadow metrics. Ported from janblade (OptiScaler descendants-scan, commit 28daeebf, GPL-3.0).
//
// Pure C++ (stdlib only), so tests/nr-detail-measure can exercise it completely on the host.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace DlssNr::DetailStats
{
constexpr unsigned kGridTiles = 64;
constexpr unsigned kGridColumns = 4;                               // 4 texels per tile (256x64 texels total)
constexpr unsigned kGridRowFloats = kGridTiles * kGridColumns * 4; // 1024 floats per row
constexpr unsigned kMeasureEvaluations = 60;                       // ~1 second of frames at 60 fps

// One measured evaluation reduced over the whole frame.
struct Stats
{
    float detailRaw = 0.0f;    // output: mean |4-neighbour Laplacian|
    float detailBand = 0.0f;   // output: mean |difference of Gaussians| (sigma ~1.2 px vs ~3 px)
    float inputBand = 0.0f;    // input: same band filter on game's frame before NR
    float outputChange = 0.0f; // mean |output - previous output|
    float inputChange = 0.0f;  // mean |input - previous input|
    float shoulder = 0.0f;     // share of model input on the highlight shoulder
    float floor = 0.0f;        // share of model input in the shadow floor
    float chromaIn = 0.0f;     // OkLab chroma of input
    float chromaOut = 0.0f;    // OkLab chroma of output
    float colourShift = 0.0f;  // |output - input| in OkLab a-b plane
    float warmth = 0.0f;       // OkLab b output minus input (+ is warmer/yellow)
    float shadowShare = 0.0f;  // share of pixels in shadows (< ~2% white)
    float shadowIn = 0.0f;     // input shadow luma sum / measured pixels
    float shadowOut = 0.0f;    // output shadow luma sum / measured pixels
    float crushed = 0.0f;      // share of pixels crushed to under half their level

    float AddedBand() const { return detailBand - inputBand; }
    float Flicker() const { return std::max(outputChange - inputChange, 0.0f); }
    float Saturation() const { return chromaIn > 1e-6f ? (chromaOut / chromaIn - 1.0f) : 0.0f; }
    float ShadowDarkening() const { return shadowIn > 1e-6f ? (1.0f - shadowOut / shadowIn) : 0.0f; }
};

inline bool Finite(const Stats& s)
{
    return std::isfinite(s.detailRaw) && std::isfinite(s.detailBand) && std::isfinite(s.inputBand) &&
           std::isfinite(s.outputChange) && std::isfinite(s.inputChange) && std::isfinite(s.shoulder) &&
           std::isfinite(s.floor) && std::isfinite(s.chromaIn) && std::isfinite(s.chromaOut) &&
           std::isfinite(s.colourShift) && std::isfinite(s.warmth) && std::isfinite(s.shadowShare) &&
           std::isfinite(s.shadowIn) && std::isfinite(s.shadowOut) && std::isfinite(s.crushed);
}

// Converts a 256x64 RGBA32F grid into a single frame Stats instance.
inline Stats ReduceGrid(const float* grid)
{
    if (grid == nullptr)
    {
        Stats s {};
        s.detailBand = NAN;
        return s;
    }

    double sum[15] = {};
    double pixels = 0.0;

    for (unsigned y = 0; y < kGridTiles; ++y)
    {
        const float* row = grid + (size_t) y * kGridRowFloats;

        for (unsigned x = 0; x < kGridTiles; ++x)
        {
            const float* a = row + x * 4;
            const float* b = row + (x + kGridTiles) * 4;
            const float* c = row + (x + 2 * kGridTiles) * 4;
            const float* d = row + (x + 3 * kGridTiles) * 4;
            const double n = b[3]; // pixels measured in tile

            if (!(n > 0.0) || !std::isfinite(n))
                continue;

            sum[0] += a[0] * n;
            sum[1] += a[1] * n;
            sum[2] += a[2] * n;
            sum[3] += a[3] * n;
            sum[4] += b[0] * n;
            sum[5] += b[1] * n;
            sum[6] += b[2] * n;
            for (int i = 0; i < 4; ++i)
            {
                sum[7 + i] += c[i] * n;
                sum[11 + i] += d[i] * n;
            }
            pixels += n;
        }
    }

    Stats s {};
    if (pixels <= 0.0)
    {
        s.detailBand = NAN;
        return s;
    }

    s.detailRaw = (float) (sum[0] / pixels);
    s.detailBand = (float) (sum[1] / pixels);
    s.inputBand = (float) (sum[2] / pixels);
    s.outputChange = (float) (sum[3] / pixels);
    s.inputChange = (float) (sum[4] / pixels);
    s.shoulder = (float) (sum[5] / pixels);
    s.floor = (float) (sum[6] / pixels);
    s.chromaIn = (float) (sum[7] / pixels);
    s.chromaOut = (float) (sum[8] / pixels);
    s.colourShift = (float) (sum[9] / pixels);
    s.warmth = (float) (sum[10] / pixels);
    s.shadowShare = (float) (sum[11] / pixels);
    s.shadowIn = (float) (sum[12] / pixels);
    s.shadowOut = (float) (sum[13] / pixels);
    s.crushed = (float) (sum[14] / pixels);
    return s;
}

// Aggregated results over a completed ~60 evaluate run.
struct Measurement
{
    unsigned samples = 0;
    float detailAdded = 0.0f;
    float detailOut = 0.0f;
    float detailIn = 0.0f;
    float detailRaw = 0.0f;
    float flicker = 0.0f;
    float flickerOut = 0.0f;
    float flickerIn = 0.0f;
    float saturation = 0.0f;
    float warmth = 0.0f;
    float shadowDarkening = 0.0f;
    float crushed = 0.0f;
    uint32_t width = 0;
    uint32_t height = 0;

    std::string DetailWords() const
    {
        if (detailIn <= 1e-6f)
            return "Detail: no input detail to compare";
        const float pct = (detailAdded / detailIn) * 100.0f;
        if (std::fabs(pct) < 1.0f)
            return "Detail: unchanged from game frame";
        if (pct > 0.0f)
            return std::format("Detail: adds +{:.1f}% fine detail over the game's frame", pct);
        return std::format("Detail: {:.1f}% softer than the game's frame", pct);
    }

    std::string FlickerWords() const
    {
        if (flickerIn <= 1e-6f)
            return std::format("Flicker: {:.5f} residual motion on a still frame", flicker);
        const float ratio = flickerOut / flickerIn;
        if (std::fabs(ratio - 1.0f) < 0.05f)
            return "Flicker: 1.0x (matches game's natural frame variation)";
        return std::format("Flicker: {:.2f}x the game's frame-to-frame change", ratio);
    }

    std::string ColourWords() const
    {
        std::string sat;
        const float satPct = saturation * 100.0f;
        if (std::fabs(satPct) < 1.0f)
            sat = "neutral saturation";
        else if (satPct > 0.0f)
            sat = std::format("+{:.1f}% more saturated", satPct);
        else
            sat = std::format("{:.1f}% less saturated", satPct);

        std::string warm;
        if (std::fabs(warmth) < 0.001f)
            warm = "neutral tone";
        else if (warmth > 0.0f)
            warm = "warmer";
        else
            warm = "cooler";

        return std::format("Colour: {}, {}", sat, warm);
    }

    std::string ShadowWords() const
    {
        std::string dark;
        const float darkPct = shadowDarkening * 100.0f;
        if (std::fabs(darkPct) < 1.0f)
            dark = "shadows intact";
        else if (darkPct > 0.0f)
            dark = std::format("shadows darkened by {:.1f}%", darkPct);
        else
            dark = std::format("shadows lifted by {:.1f}%", -darkPct);

        const float crushPct = crushed * 100.0f;
        if (crushPct >= 0.05f)
            return std::format("Shadows: {}, {:.1f}% crushed to black", dark, crushPct);
        return std::format("Shadows: {}, no crushing", dark);
    }

    std::string Compare(const Measurement& prev) const
    {
        if (prev.samples == 0)
            return {};
        if (width != prev.width || height != prev.height)
            return "vs previous: resolution changed (not directly comparable)";

        const float dDetail = detailAdded - prev.detailAdded;
        const float dFlicker = flicker - prev.flicker;
        const float dSat = (saturation - prev.saturation) * 100.0f;
        return std::format("vs previous: detail {:+.1f}%, flicker {:+.5f}, saturation {:+.1f}%",
                           prev.detailIn > 1e-6f ? (dDetail / prev.detailIn) * 100.0f : 0.0f, dFlicker, dSat);
    }
};

// Aggregator that accumulates Stats instances from GPU readbacks.
class Accumulator
{
    std::vector<Stats> _samples;

  public:
    void Reset() { _samples.clear(); }
    void Add(const Stats& s)
    {
        if (Finite(s))
            _samples.push_back(s);
    }

    size_t Count() const { return _samples.size(); }

    Measurement Finish(uint32_t width, uint32_t height) const
    {
        Measurement m {};
        m.width = width;
        m.height = height;
        m.samples = static_cast<unsigned>(_samples.size());
        if (_samples.empty())
            return m;

        double sumDetailOut = 0, sumDetailIn = 0, sumRaw = 0;
        double sumFlickerOut = 0, sumFlickerIn = 0;
        double sumSat = 0, sumWarmth = 0, sumDark = 0, sumCrush = 0;

        for (const auto& s : _samples)
        {
            sumDetailOut += s.detailBand;
            sumDetailIn += s.inputBand;
            sumRaw += s.detailRaw;
            sumFlickerOut += s.outputChange;
            sumFlickerIn += s.inputChange;
            sumSat += s.Saturation();
            sumWarmth += s.warmth;
            sumDark += s.ShadowDarkening();
            sumCrush += s.crushed;
        }

        const double inv = 1.0 / _samples.size();
        m.detailOut = static_cast<float>(sumDetailOut * inv);
        m.detailIn = static_cast<float>(sumDetailIn * inv);
        m.detailAdded = m.detailOut - m.detailIn;
        m.detailRaw = static_cast<float>(sumRaw * inv);
        m.flickerOut = static_cast<float>(sumFlickerOut * inv);
        m.flickerIn = static_cast<float>(sumFlickerIn * inv);
        m.flicker = std::max(m.flickerOut - m.flickerIn, 0.0f);
        m.saturation = static_cast<float>(sumSat * inv);
        m.warmth = static_cast<float>(sumWarmth * inv);
        m.shadowDarkening = static_cast<float>(sumDark * inv);
        m.crushed = static_cast<float>(sumCrush * inv);
        return m;
    }
};
} // namespace DlssNr::DetailStats
