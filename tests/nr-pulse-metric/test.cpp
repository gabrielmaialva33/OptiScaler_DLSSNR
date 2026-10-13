// The pulse metric's CPU half (retoque/dlssnr/DlssNr_Pulse.h), the code the renderer runs on every readback: the
// edit's light from a grid, frames paired into steps and filed by kind, windows, resets, the words the menu shows and
// the log's rate. Then a synthetic cadence, so the number is shown to say what it is meant to say.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <dlssnr/DlssNr_Pulse.h>

using namespace DlssNr::Pulse;

namespace
{
int g_cases = 0;

bool Near(double a, double b, double tolerance = 1e-12) { return std::fabs(a - b) <= tolerance; }

std::vector<float> Grid(float out, float in, float pixels = 64.0f)
{
    std::vector<float> grid(kGridFloats, 0.0f);
    for (unsigned i = 0; i < kGrid * kGrid; ++i)
    {
        grid[i * 4 + 0] = out;
        grid[i * 4 + 1] = in;
        grid[i * 4 + 2] = pixels;
    }
    return grid;
}

void EditLight_IsLog2OfTheSums()
{
    assert(Near(EditLight(2.0, 1.0), 1.0));
    assert(Near(EditLight(1.0, 2.0), -1.0));
    assert(Near(EditLight(3.0, 3.0), 0.0));
    // Nothing to compare: a black frame, a black output, a sum that is not finite.
    assert(std::isnan(EditLight(0.0, 1.0)) && std::isnan(EditLight(1.0, 0.0)));
    assert(std::isnan(EditLight(NAN, 1.0)) && std::isnan(EditLight(1.0, INFINITY)));

    // From the grid: the tiles' sums, so a tile twice as bright weighs twice -- a frame-wide mean, not a mean of
    // ratios.
    auto grid = Grid(1.0f, 1.0f);
    grid[0] = 101.0f; // one bright tile, brightened by 100
    grid[1] = 1.0f;
    const double expected = std::log2((256.0 + 100.0) / 256.0);
    assert(Near(EditLight(grid.data()), expected, 1e-9));

    // A tile that read no pixels, or summed to something not finite, is left out rather than poisoning the frame.
    grid = Grid(2.0f, 1.0f);
    grid[2] = 0.0f;
    grid[4] = NAN;
    grid[8 + 1] = INFINITY;
    assert(Near(EditLight(grid.data()), 1.0, 1e-9));
    assert(std::isnan(EditLight(Grid(0.0f, 0.0f).data())));
    assert(std::isnan(EditLight(nullptr)));
    ++g_cases;
}

void KindOf_FollowsTheCadenceInEffect()
{
    assert(KindOf(1, false) == Kind::Every);
    assert(KindOf(1, true) == Kind::Every); // cannot happen, and still is not a carried frame of a cadence
    assert(KindOf(0, false) == Kind::Every);
    assert(KindOf(3, false) == Kind::Model);
    assert(KindOf(3, true) == Kind::Carried);
    ++g_cases;
}

void Tracker_PairsOnlyConsecutiveFrames()
{
    Tracker t;
    // The first frame makes no step; the second does, filed under its own kind.
    assert(!t.Add(1, 0, Kind::Every, 0.10, 1));
    assert(t.Pending() == 0);
    assert(!t.Add(2, 16, Kind::Model, 0.15, 3));
    assert(t.Pending() == 1);

    // A frame that was not measured (seq 3 missing) breaks the pair: 4 makes no step, 5 does.
    assert(!t.Add(4, 48, Kind::Carried, 0.30, 3));
    assert(t.Pending() == 1);
    assert(!t.Add(5, 64, Kind::Carried, 0.31, 3));
    assert(t.Pending() == 2);

    // A gap in time breaks it too, even with consecutive numbers.
    assert(!t.Add(6, 64 + kGapMs + 1, Kind::Carried, 0.50, 3));
    assert(t.Pending() == 2);
    assert(!t.Add(7, 64 + kGapMs + 17, Kind::Carried, 0.51, 3));
    assert(t.Pending() == 3);

    // A frame with nothing to compare breaks the chain on both sides.
    assert(!t.Add(8, 400, Kind::Carried, NAN, 3));
    assert(!t.Add(9, 416, Kind::Carried, 0.52, 3));
    assert(t.Pending() == 3);
    assert(!t.Add(10, 432, Kind::Carried, 0.53, 3));
    assert(t.Pending() == 4);

    // A readback that arrives after a newer one neither makes a step nor breaks the next one.
    assert(!t.Add(9, 433, Kind::Model, 9.0, 3));
    assert(t.Pending() == 4);
    assert(!t.Add(11, 448, Kind::Carried, 0.54, 3));
    assert(t.Pending() == 5);
    ++g_cases;
}

void Tracker_PublishesAWindowPerKWindowSteps()
{
    Tracker t;
    assert(t.Latest().steps == 0);
    assert(t.Latest().Words() == "Light step: measuring...");

    // Cadence 3: the model frame steps +0.03 stop, the two carried frames -0.015 each, so the light returns.
    double light = 0.0;
    uint64_t seq = 1;
    int64_t ms = 0;
    t.Add(seq++, ms, Kind::Model, light, 3);
    unsigned published = 0;
    for (unsigned frame = 1; published == 0; ++frame)
    {
        const bool model = frame % 3 == 0;
        light += model ? 0.03 : -0.015;
        ms += 16;
        if (t.Add(seq++, ms, model ? Kind::Model : Kind::Carried, light, 3))
        {
            ++published;
            assert(frame == kWindow); // exactly one window's worth of steps
        }
    }
    const Reading& r = t.Latest();
    assert(r.steps == kWindow && r.cadence == 3);
    assert(r.Of(Kind::Model).n == kWindow / 3 && r.Of(Kind::Carried).n == 2 * kWindow / 3);
    assert(r.Of(Kind::Every).n == 0);
    assert(Near(r.Of(Kind::Model).MeanAbs(), 0.03, 1e-9) && Near(r.Of(Kind::Model).MeanSigned(), 0.03, 1e-9));
    assert(Near(r.Of(Kind::Carried).MeanAbs(), 0.015, 1e-9) && Near(r.Of(Kind::Carried).MeanSigned(), -0.015, 1e-9));
    assert(Near(r.Of(Kind::Model).maxAbs, 0.03, 1e-9));
    assert(Near(r.Ratio(), 2.0, 1e-9));
    assert(t.Pending() == 0); // the next window starts empty

    const std::string words = r.Words();
    assert(words == "Light step, stops per frame: model frames 0.0300, carried frames 0.0150 (model / carried 2.0x)");
    const std::string line = r.LogLine();
    assert(line.find("DLSS-NR pulse:") == 0);
    assert(line.find("model frames 0.03000 (n=40, signed +0.03000, max 0.0300)") != std::string::npos);
    assert(line.find("carried frames 0.01500 (n=80, signed -0.01500, max 0.0150)") != std::string::npos);
    assert(line.find("model / carried 2.00x") != std::string::npos);
    assert(line.find("cadence 3") != std::string::npos);
    ++g_cases;
}

void Reading_EveryFrameHasNoRatio()
{
    Tracker t;
    double light = 0.0;
    for (uint64_t seq = 1; seq <= kWindow + 1; ++seq)
    {
        light += (seq % 2 == 0) ? 0.004 : -0.004;
        t.Add(seq, int64_t(seq) * 16, Kind::Every, light, 1);
    }
    const Reading& r = t.Latest();
    assert(r.steps == kWindow && r.Of(Kind::Every).n == kWindow);
    assert(r.Ratio() == 0.0);
    assert(r.Words() == "Light step, stops per frame: every frame 0.0040");
    assert(r.LogLine().find("model / carried") == std::string::npos);
    ++g_cases;
}

void Reset_StartsOverWithoutAStepAcrossIt()
{
    Tracker t;
    for (uint64_t seq = 1; seq <= kWindow + 1; ++seq)
        t.Add(seq, int64_t(seq) * 16, Kind::Every, 0.001 * double(seq % 2), 1);
    assert(t.Latest().steps == kWindow);
    t.Add(kWindow + 2, int64_t(kWindow + 2) * 16, Kind::Every, 0.0, 1);
    assert(t.Pending() == 1);

    // Reset forgets the window, the last frame and the reading: the next frame makes no step, even consecutive.
    t.Reset();
    assert(t.Pending() == 0 && t.Latest().steps == 0);
    assert(!t.Add(kWindow + 3, int64_t(kWindow + 3) * 16, Kind::Model, 5.0, 2));
    assert(t.Pending() == 0);
    assert(!t.Add(kWindow + 4, int64_t(kWindow + 4) * 16, Kind::Carried, 5.01, 2));
    assert(t.Pending() == 1);

    // After a reset an older sequence number is accepted as a fresh start (the renderer marks readbacks recorded
    // before the change stale and never feeds them, so this only happens to a frame that belongs to the new shape).
    Tracker u;
    u.Add(50, 0, Kind::Every, 0.0, 1);
    u.Reset();
    assert(!u.Add(10, 16, Kind::Every, 0.0, 1));
    assert(!u.Add(11, 32, Kind::Every, 0.1, 1));
    assert(u.Pending() == 1);
    ++g_cases;
}

void LogGate_FirstThenAtMostEveryTenSeconds()
{
    LogGate gate;
    assert(gate.Due(1000));
    assert(!gate.Due(3000));
    assert(!gate.Due(1000 + kLogEveryMs - 1));
    assert(gate.Due(1000 + kLogEveryMs));
    // What is measured changed: the first reading after it is logged at once.
    gate.Reset();
    assert(gate.Due(1000 + kLogEveryMs + 1));
    assert(!gate.Due(1000 + kLogEveryMs + 2));
    ++g_cases;
}

// A synthetic session: the model re-decides its regional light by a random walk every frame it runs (the drift
// janblade and Skynizz measured), a carried frame shows the last model frame's light, and the scene is still. At
// cadence 1 every frame steps by the drift; at cadence 3 the carried frames step by nothing and the model frame by
// three frames' worth -- the pulse. The metric must read the second as a ratio well above 1 and the first as one
// kind only.
double Drift(unsigned& state)
{
    state = state * 1664525u + 1013904223u;
    return (double((state >> 8) & 0xffff) / 65535.0 - 0.5) * 0.02; // +-0.01 stop
}

Reading Session(uint32_t cadence)
{
    Tracker t;
    unsigned rng = 12345u;
    double model = 0.0, shown = 0.0;
    uint32_t age = 0;
    for (uint64_t seq = 1;; ++seq)
    {
        model += Drift(rng);
        const bool runs = cadence <= 1 || age + 1 >= cadence;
        age = runs ? 0 : age + 1;
        if (runs)
            shown = model;
        const Kind kind = KindOf(cadence, !runs);
        if (t.Add(seq, int64_t(seq) * 16, kind, shown, cadence))
            return t.Latest();
    }
}

void SyntheticCadence_ReadsThePulse()
{
    const Reading every = Session(1);
    assert(every.Of(Kind::Every).n == kWindow && every.Ratio() == 0.0);

    const Reading three = Session(3);
    assert(three.Of(Kind::Model).n == kWindow / 3 && three.Of(Kind::Carried).n == 2 * kWindow / 3);
    assert(three.Of(Kind::Carried).MeanAbs() == 0.0); // a still carried frame shows the last answer exactly
    assert(three.Ratio() == 0.0);                     // no ratio against nothing ...
    assert(three.Of(Kind::Model).MeanAbs() > 1.4 * every.Of(Kind::Every).MeanAbs()); // ... and the pulse is there
    ++g_cases;
}
} // namespace

int main()
{
    EditLight_IsLog2OfTheSums();
    KindOf_FollowsTheCadenceInEffect();
    Tracker_PairsOnlyConsecutiveFrames();
    Tracker_PublishesAWindowPerKWindowSteps();
    Reading_EveryFrameHasNoRatio();
    Reset_StartsOverWithoutAStepAcrossIt();
    LogGate_FirstThenAtMostEveryTenSeconds();
    SyntheticCadence_ReadsThePulse();
    if (g_cases != 8)
        return 2;
    std::printf(
        "PASS %d pulse-metric cases: edit light from the grid, consecutive pairing (gaps, holes, stale and "
        "non-finite frames), windows by kind, reset, words and log line, log rate, a synthetic cadence's pulse\n",
        g_cases);
    return 0;
}
