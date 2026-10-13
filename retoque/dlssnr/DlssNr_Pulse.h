#pragma once

// The pulse metric ([DlssNr] PulseMetric, docs/dlssnr/design/pulse-metric.md): how far the picture's light moves
// from one frame to the next because of the edit, split by the kind of frame, so model cadence's pulse -- a step
// on the frame the model runs again -- is a number rather than an impression.
//
// Per measured frame the GPU sums the luminance of the pass's output and of the game's frame under it over a grid
// of tiles (dlssnr_pulse.hlsl); the edit's light is log2 of their ratio, in stops; the light step is its change
// from the previous frame. Taking the edit's light rather than the output's alone cancels the scene's own change
// of light to first order (a pan into a brighter room, the game's exposure), and on a still camera the two are the
// same number.
//
// Pure C++ (stdlib only): tests/nr-pulse-metric drives it on the host. The renderer feeds it readbacks in the order
// they were recorded and publishes what it returns; nothing here knows about a device.

#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace DlssNr::Pulse
{
// The GPU's grid: kGrid x kGrid tiles, each (luminance out, luminance in, pixels measured, 0) as RGBA32F.
constexpr unsigned kGrid = 16;
constexpr unsigned kGridFloats = kGrid * kGrid * 4;

// Steps per published reading: about two seconds at 60 fps, so a reading follows a setting within a breath and
// still averages over many cadence cycles (40 at cadence 3).
constexpr unsigned kWindow = 120;

// No step is taken across a gap this long: the pass did not run in between, so the frame before it shows another
// moment. The cadence uses the same.
constexpr int64_t kGapMs = 250;

// One INFO line at most this often. A window completes every two seconds; the log needs one line now and then,
// not thirty a minute.
constexpr int64_t kLogEveryMs = 10000;

enum class Kind : uint8_t
{
    Every,   // the model ran and the cadence is not in effect: the model every frame
    Model,   // the model ran on a frame of a running cadence
    Carried, // the cadence carried the last model frame's edit
};
constexpr unsigned kKinds = 3;

// A frame's kind from what the cadence decided for it: the cadence in effect (1 whenever a refusal runs the model
// every frame) and whether the frame was carried.
constexpr Kind KindOf(uint32_t cadenceInEffect, bool carried)
{
    return cadenceInEffect <= 1 ? Kind::Every : (carried ? Kind::Carried : Kind::Model);
}

// The edit's light, in stops, from the summed luminances. NaN where there is nothing to compare -- a black frame,
// a black output, a sum that is not finite -- so the frame breaks the chain of steps instead of making one.
inline double EditLight(double sumOut, double sumIn)
{
    if (!std::isfinite(sumOut) || !std::isfinite(sumIn) || !(sumOut > 0.0) || !(sumIn > 0.0))
        return NAN;
    return std::log2(sumOut / sumIn);
}

// The same from the GPU's grid. A tile is weighed by nothing but its sums: every tile measured the same share of
// its pixels, and the sums are what a frame-wide mean is made of.
inline double EditLight(const float* grid)
{
    if (grid == nullptr)
        return NAN;

    double sumOut = 0.0;
    double sumIn = 0.0;
    for (unsigned i = 0; i < kGrid * kGrid; ++i)
    {
        const float* tile = grid + i * 4;
        if (!(tile[2] > 0.0f) || !std::isfinite(tile[0]) || !std::isfinite(tile[1]))
            continue;
        sumOut += tile[0];
        sumIn += tile[1];
    }
    return EditLight(sumOut, sumIn);
}

struct Bucket
{
    unsigned n = 0;
    double sumAbs = 0.0;
    double sumSigned = 0.0;
    double maxAbs = 0.0;

    void Add(double step)
    {
        ++n;
        sumAbs += std::fabs(step);
        sumSigned += step;
        maxAbs = std::fmax(maxAbs, std::fabs(step));
    }

    double MeanAbs() const { return n > 0 ? sumAbs / n : 0.0; }
    double MeanSigned() const { return n > 0 ? sumSigned / n : 0.0; }
};

// One completed window.
struct Reading
{
    Bucket kinds[kKinds] {};
    unsigned steps = 0;   // 0: nothing published yet
    uint32_t cadence = 1; // the cadence the window was measured at, as configured

    const Bucket& Of(Kind kind) const { return kinds[static_cast<unsigned>(kind)]; }

    // The model frames' step over the carried frames': the pulse, as a factor. 0 when either is missing.
    double Ratio() const
    {
        const Bucket& model = Of(Kind::Model);
        const Bucket& carried = Of(Kind::Carried);
        if (model.n == 0 || carried.n == 0 || !(carried.MeanAbs() > 0.0))
            return 0.0;
        return model.MeanAbs() / carried.MeanAbs();
    }

    // The menu's line. English, as the other measurement read-outs are.
    std::string Words() const
    {
        if (steps == 0)
            return "Light step: measuring...";

        std::string parts;
        auto add = [&parts](const char* name, const Bucket& bucket)
        {
            if (bucket.n == 0)
                return;
            if (!parts.empty())
                parts += ", ";
            parts += std::format("{} {:.4f}", name, bucket.MeanAbs());
        };
        add("model frames", Of(Kind::Model));
        add("carried frames", Of(Kind::Carried));
        add("every frame", Of(Kind::Every));

        std::string line = "Light step, stops per frame: " + parts;
        if (const double ratio = Ratio(); ratio > 0.0)
            line += std::format(" (model / carried {:.1f}x)", ratio);
        return line;
    }

    // The log's line: every number, so a session can be read without the menu.
    std::string LogLine() const
    {
        std::string line = std::format("DLSS-NR pulse: light step over {} frame pairs at cadence {} (mean |change| of "
                                       "log2 output light / frame light, stops)",
                                       steps, cadence);
        auto add = [&line](const char* name, const Bucket& bucket)
        {
            if (bucket.n == 0)
                return;
            line += std::format("; {} {:.5f} (n={}, signed {:+.5f}, max {:.4f})", name, bucket.MeanAbs(), bucket.n,
                                bucket.MeanSigned(), bucket.maxAbs);
        };
        add("model frames", Of(Kind::Model));
        add("carried frames", Of(Kind::Carried));
        add("every frame", Of(Kind::Every));
        if (const double ratio = Ratio(); ratio > 0.0)
            line += std::format("; model / carried {:.2f}x", ratio);
        return line;
    }
};

// Pairs consecutive measured frames into steps and publishes a reading per kWindow steps.
//
// A step needs both frames: the one before must carry the sequence number just below this one's (the renderer
// counts every present at which the pass ran, measured or not, so a frame it could not measure breaks the pair),
// and must be under kGapMs old. A step is filed under the kind of the later frame: the step INTO a model frame is
// the pulse.
class Tracker
{
    Reading _window {};
    Reading _latest {};
    uint64_t _lastSeq = 0;
    int64_t _lastMs = 0;
    double _lastLight = 0.0;
    bool _haveLast = false;
    bool _haveAny = false; // a frame has been fed since the last Reset, so a stale older one can be told apart

  public:
    // Whatever changed what is measured -- the cadence, its smoothing, the frame's size or format -- starts over:
    // the partial window, the last frame and the published reading are dropped, so no step spans the change and no
    // reading taken before it is shown as if it described what is on screen now.
    void Reset()
    {
        _window = {};
        _latest = {};
        _haveLast = false;
        _haveAny = false;
    }

    // One measured frame. Returns true when it completed a window, which Latest() now holds.
    bool Add(uint64_t seq, int64_t ms, Kind kind, double light, uint32_t cadence)
    {
        // A readback that arrives after a newer one belongs to the past: it neither makes a step nor breaks one.
        if (_haveAny && seq <= _lastSeq)
            return false;
        _haveAny = true;

        if (!std::isfinite(light))
        {
            _haveLast = false;
            _lastSeq = seq;
            return false;
        }

        const bool paired = _haveLast && seq == _lastSeq + 1 && ms >= _lastMs && ms - _lastMs <= kGapMs;
        const double previous = _lastLight;

        _haveLast = true;
        _lastSeq = seq;
        _lastMs = ms;
        _lastLight = light;

        if (!paired)
            return false;

        _window.kinds[static_cast<unsigned>(kind)].Add(light - previous);
        _window.cadence = cadence;
        if (++_window.steps < kWindow)
            return false;

        _latest = _window;
        _window = {};
        return true;
    }

    const Reading& Latest() const { return _latest; }
    unsigned Pending() const { return _window.steps; }
};

// When a completed reading is logged: the first one after the metric started or what it measures changed, then at
// most once per kLogEveryMs.
class LogGate
{
    int64_t _lastMs = 0;
    bool _fresh = true;

  public:
    void Reset() { _fresh = true; }

    bool Due(int64_t nowMs)
    {
        if (!_fresh && nowMs - _lastMs < kLogEveryMs)
            return false;
        _fresh = false;
        _lastMs = nowMs;
        return true;
    }
};
} // namespace DlssNr::Pulse
