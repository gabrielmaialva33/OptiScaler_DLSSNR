// tests/lsfg-presenter: the arithmetic of the MFG presenter (retoque/lsfg/presenter/MfgPacing.h) and what [LSFG]
// Multiplier does in a running process (MfgLatch.h), compiled unmodified.
// tools/lsfg-study/MFG_PRESENTER.md sections 1.4, 3.2, 3.3, 5 and 6.

#include <lsfg/presenter/MfgLatch.h>
#include <lsfg/presenter/MfgPacing.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
        }                                                                                                              \
    } while (0)

using namespace LsfgMfg;

// FrameInterpolationSwapchainDX12.cpp:882-884, as FFX computes it for its one generated frame.
static int64_t FfxDelta(double avg, double stdDev, double varianceFactor, int64_t margin)
{
    const int64_t conservativeAvg = int64_t(avg * 0.5 - stdDev * varianceFactor);
    return conservativeAvg > margin ? (conservativeAvg - margin) : 0;
}

static void Multipliers()
{
    CHECK(GeneratedFramesFor(2) == 1);
    CHECK(GeneratedFramesFor(3) == 2);
    CHECK(GeneratedFramesFor(4) == 3);
    for (int bad : { -1, 0, 1, 5, 8, 1000 })
    {
        CHECK(GeneratedFramesFor(bad) == 1);
        CHECK(!MultiplierValid(bad));
    }
    CHECK(MultiplierValid(2) && MultiplierValid(3) && MultiplierValid(4));
    CHECK(kMaxGeneratedFrames == 3 && kMaxFramesPerEntry == 4);
    // ffxDispatchDescFrameGeneration::outputs has four slots: the widest request fits.
    CHECK(GeneratedFramesFor(4) <= 4);
}

static void Deltas()
{
    const int64_t qpc = 10'000'000; // QPC ticks per second on Windows

    // x2 is FFX's formula exactly, over a spread of frame times, deviations, factors and margins.
    for (double fps : { 20.0, 30.0, 59.94, 60.0, 82.5, 120.0, 240.0 })
    {
        for (double stdDevMs : { 0.0, 0.05, 0.4, 2.0, 9.0 })
        {
            for (double vf : { 0.0, 0.1, 0.3, 1.0 })
            {
                for (double marginMs : { 0.0, 0.01, 0.1, 1.0 })
                {
                    const double avg = double(qpc) / fps;
                    const double sd = stdDevMs * double(qpc) / 1000.0;
                    const int64_t margin = int64_t(double(qpc) * marginMs / 1000.0);
                    CHECK(PresentDelta(avg, sd, 2, vf, margin) == FfxDelta(avg, sd, vf, margin));

                    // Every entry fits inside one real frame: its presents never outrun the next real frame.
                    for (uint32_t frames = 1; frames <= kMaxFramesPerEntry; ++frames)
                    {
                        const int64_t d = PresentDelta(avg, sd, frames, vf, margin);
                        CHECK(d >= 0);
                        CHECK(double(d) * frames <= avg + 1.0);
                        // and with no deviation or margin, it is the even split
                        if (stdDevMs == 0.0 && marginMs == 0.0)
                            CHECK(std::llabs(d - int64_t(avg / frames)) <= 1);
                    }
                }
            }
        }
    }

    // x3 and x4 at 30 fps: a third and a quarter of 33.3 ms.
    const double avg30 = double(qpc) / 30.0;
    CHECK(std::llabs(PresentDelta(avg30, 0.0, 3, 0.3, 0) - int64_t(avg30 / 3)) <= 1);
    CHECK(std::llabs(PresentDelta(avg30, 0.0, 4, 0.3, 0) - int64_t(avg30 / 4)) <= 1);

    // An entry with its real frame only waits a whole frame from the previous present.
    CHECK(std::llabs(PresentDelta(avg30, 0.0, 1, 0.3, 0) - int64_t(avg30)) <= 1);

    // No average yet (FFX's SimpleMovingAverage returns 0 for its first 10 frames): present at once.
    for (uint32_t frames = 0; frames <= kMaxFramesPerEntry; ++frames)
        CHECK(PresentDelta(0.0, 0.0, frames, 0.3, 100) == 0);

    // Zero frames is read as one, never a division by zero.
    CHECK(PresentDelta(avg30, 0.0, 0, 0.0, 0) == PresentDelta(avg30, 0.0, 1, 0.0, 0));

    // A deviation larger than the share clamps at 0 rather than going negative.
    CHECK(PresentDelta(avg30, avg30, 4, 1.0, 0) == 0);
}

static void OutputSets()
{
    // Present p writes set p % 2: consecutive presents never write the same set.
    for (uint64_t p = 0; p < 64; ++p)
    {
        CHECK(OutputSetFor(p) < kOutputSets);
        CHECK(OutputSetFor(p) != OutputSetFor(p + 1));
        CHECK(OutputSetFor(p) == OutputSetFor(p + 2));
    }

    // FFX's availability for its one output is base + doPresent (FrameInterpolationSwapchainDX12.cpp:2014).
    CHECK(SetAvailableAfter(100, 1) == 101);
    CHECK(SetAvailableAfter(100, 0) == 100);
    // With three generated frames the set is free after the third one's composition, before the real frame's.
    CHECK(SetAvailableAfter(100, 3) == 103);
}

// The presenter's timing model (FrameInterpolationSwapchainDX12.cpp presenterThread/interpolationThread with the
// widened delta): entries arrive every real frame; each present happens at max(arrival, previous present + delta).
// At a steady rate every frame of an entry is shown before the next entry arrives (no entry is overwritten), and the
// presented rate is (N + 1) x the real rate.
static void Schedule()
{
    for (uint32_t n = 1; n <= kMaxGeneratedFrames; ++n)
    {
        for (double fps : { 24.0, 30.0, 60.0, 82.5 })
        {
            const double frame = 1.0 / fps;
            const double vf = 0.3, margin = 0.00001; // OptiScaler's FGFPTVarianceFactor / SafetyMarginInMs defaults
            double previousPresent = 0.0;
            int presents = 0;
            double first = -1.0, last = 0.0;
            double maxGap = 0.0, minGap = 1e9;
            bool overrun = false;

            for (int entry = 1; entry <= 300; ++entry)
            {
                const double arrival = entry * frame;
                // after 10 entries the moving average is established (stdDev 0 at a steady rate)
                const double delta =
                    entry > 10 ? double(PresentDelta(frame * 1e7, 0.0, n + 1, vf, int64_t(margin * 1e7))) / 1e7 : 0.0;

                for (uint32_t k = 0; k <= n; ++k)
                {
                    double t = previousPresent + delta;
                    if (k == 0 && t < arrival)
                        t = arrival;
                    if (entry > 20 && presents > 0)
                    {
                        maxGap = std::max(maxGap, t - previousPresent);
                        minGap = std::min(minGap, t - previousPresent);
                    }
                    previousPresent = t;
                    if (first < 0.0)
                        first = t;
                    last = t;
                    ++presents;
                }

                if (previousPresent > arrival + frame)
                    overrun = true;
            }

            CHECK(!overrun);
            const double rate = double(presents - 1) / (last - first);
            CHECK(std::fabs(rate / (fps * (n + 1)) - 1.0) < 0.05);
            // Steady state: every gap is the even split, within the margin's slack.
            CHECK(maxGap - minGap < 0.0002);
            CHECK(std::fabs(minGap - frame / (n + 1)) < 0.0002);
        }
    }
}

static void Window()
{
    PresentWindow w;
    CHECK(w.Percentile(0.5) == 0.0);
    CHECK(w.GeneratedPerReal() == 0.0);
    CHECK(w.MeanTargetMs() == 0.0);

    // 100 entries of three generated frames and the real one at x4, 30 fps: 8.33 ms apart.
    for (int e = 0; e < 100; ++e)
    {
        w.AddEntry(3);
        for (int k = 0; k < 4; ++k)
            w.AddPresent(k < 3, e == 0 && k == 0 ? -1.0 : 8.33, 8.3);
    }
    CHECK(w.real == 100 && w.generated == 300);
    CHECK(w.entriesByGenerated[3] == 100 && w.entriesByGenerated[0] == 0);
    CHECK(std::fabs(w.GeneratedPerReal() - 3.0) < 1e-9);
    CHECK(w.intervalsMs.size() == 399); // the first present of the window has no interval
    CHECK(std::fabs(w.Percentile(0.5) - 8.33) < 1e-9);
    CHECK(w.overTarget == 0);
    CHECK(std::fabs(w.MeanTargetMs() - 8.3) < 1e-9);

    // A late present counts over 1.5 x target; a zero target never does.
    w.AddPresent(false, 13.0, 8.3);
    w.AddPresent(true, 50.0, 0.0);
    CHECK(w.overTarget == 1);
    CHECK(w.Percentile(0.99) >= 8.33);
    CHECK(w.Percentile(1.0) == 50.0);

    // Entries beyond three generated frames are counted in the last bucket, never out of bounds.
    w.AddEntry(7);
    CHECK(w.entriesByGenerated[3] == 101);
}

// The interpolation lists of one entry: generated frame k is complete at the end of list k, frames past the last list
// at the end of the last one, all of them at the end of the only one.
static void Lists()
{
    for (uint32_t k = 0; k < 8; ++k)
    {
        CHECK(ListForFrame(k, 0) == 0);
        CHECK(ListForFrame(k, 1) == 0);
    }
    CHECK(ListForFrame(0, 3) == 0 && ListForFrame(1, 3) == 1 && ListForFrame(2, 3) == 2);
    CHECK(ListForFrame(0, 2) == 0 && ListForFrame(1, 2) == 1 && ListForFrame(2, 2) == 1);
    for (uint32_t lists = 1; lists <= kMaxGeneratedFrames; ++lists)
        for (uint32_t k = 0; k < kMaxGeneratedFrames; ++k)
        {
            CHECK(ListForFrame(k, lists) < lists);
            CHECK(ListForFrame(k, lists) <= k);
            // display order is submission order: a later frame never completes in an earlier list
            if (k > 0)
                CHECK(ListForFrame(k, lists) >= ListForFrame(k - 1, lists));
        }
    CHECK(kTimestampsPerEntry == kMaxGeneratedFrames + 1);

    // The generator places frame k on ListForFrame(k, handed) and reports how many lists it recorded into; the
    // presenter maps frame k over that count. Both pick the same list, and the count is min(generated, handed).
    for (uint32_t handed = 1; handed <= kMaxGeneratedFrames; ++handed)
        for (uint32_t generated = 1; generated <= kMaxGeneratedFrames; ++generated)
        {
            uint32_t used = 0;
            for (uint32_t k = 0; k < generated; ++k)
                used = std::max(used, ListForFrame(k, handed) + 1);
            CHECK(used == std::min(generated, handed));
            for (uint32_t k = 0; k < generated; ++k)
                CHECK(ListForFrame(k, used) == ListForFrame(k, handed));
        }
}

static void Ready()
{
    const uint64_t f = 1'000'000'000; // ns ticks
    double ms[kMaxGeneratedFrames] {};
    uint64_t last = 0;

    // Three lists: begin at 10 ms, the lists end 0.6, 1.0 and 1.4 ms later.
    uint64_t three[kTimestampsPerEntry] { 10'000'000, 10'600'000, 11'000'000, 11'400'000 };
    CHECK(ReadyTimes(three, 3, 3, f, last, ms));
    CHECK(std::fabs(ms[0] - 0.6) < 1e-9 && std::fabs(ms[1] - 1.0) < 1e-9 && std::fabs(ms[2] - 1.4) < 1e-9);
    CHECK(last == 10'000'000);

    // The same slot read again (its resolve not landed for the next entry): its start is not newer, skipped.
    ms[0] = -1.0;
    CHECK(!ReadyTimes(three, 3, 3, f, last, ms));
    CHECK(ms[0] == -1.0 && last == 10'000'000);

    // One list for every frame (FISC's model): all three are done when it ends.
    uint64_t one[kTimestampsPerEntry] { 20'000'000, 21'400'000, 0, 0 };
    CHECK(ReadyTimes(one, 1, 3, f, last, ms));
    CHECK(std::fabs(ms[0] - 1.4) < 1e-9 && std::fabs(ms[1] - 1.4) < 1e-9 && std::fabs(ms[2] - 1.4) < 1e-9);

    // FFX interpolating: one frame, one list; the unwritten ends are never read.
    uint64_t ffx[kTimestampsPerEntry] { 30'000'000, 30'700'000, 1, 1 };
    CHECK(ReadyTimes(ffx, 1, 1, f, last, ms));
    CHECK(std::fabs(ms[0] - 0.7) < 1e-9);

    // Refused: no frequency, no lists, no frames, too many, an end before the start, a second-long span, a zero start.
    uint64_t later[kTimestampsPerEntry] { 40'000'000, 40'500'000, 40'900'000, 41'300'000 };
    CHECK(!ReadyTimes(later, 3, 3, 0, last, ms));
    CHECK(!ReadyTimes(later, 0, 3, f, last, ms));
    CHECK(!ReadyTimes(later, 3, 0, f, last, ms));
    CHECK(!ReadyTimes(later, 4, 3, f, last, ms));
    CHECK(!ReadyTimes(later, 3, 4, f, last, ms));
    CHECK(!ReadyTimes(nullptr, 3, 3, f, last, ms));
    uint64_t backwards[kTimestampsPerEntry] { 50'000'000, 50'500'000, 49'000'000, 51'000'000 };
    CHECK(!ReadyTimes(backwards, 3, 3, f, last, ms));
    uint64_t huge[kTimestampsPerEntry] { 60'000'000, 60'500'000, 60'900'000, 1'061'000'000 };
    CHECK(!ReadyTimes(huge, 3, 3, f, last, ms));
    uint64_t zero[kTimestampsPerEntry] { 0, 1, 2, 3 };
    uint64_t none = 0;
    CHECK(!ReadyTimes(zero, 3, 3, f, none, ms));
    CHECK(last == 30'000'000); // nothing refused moved it
    CHECK(ReadyTimes(later, 3, 3, f, last, ms));
    CHECK(last == 40'000'000);
}

static void Latency()
{
    PresentWindow w;
    CHECK(w.ListsPerEntry() == 0.0);
    for (int e = 0; e < 10; ++e)
    {
        w.AddLists(3);
        for (uint32_t k = 0; k < 3; ++k)
        {
            w.AddReady(k, 0.5 + 0.4 * k);
            w.AddShown(true, k, 1.0 + 8.0 * k);
        }
        w.AddShown(false, 3, 25.0);
    }
    w.AddLists(1); // a held frame: its one list
    CHECK(std::fabs(w.ListsPerEntry() - 31.0 / 11.0) < 1e-9);
    CHECK(w.readyMs[0].size() == 10 && w.readyMs[2].size() == 10);
    CHECK(std::fabs(PresentWindow::PercentileOf(w.readyMs[1], 0.5) - 0.9) < 1e-9);
    CHECK(std::fabs(PresentWindow::PercentileOf(w.shownGeneratedMs[2], 0.5) - 17.0) < 1e-9);
    CHECK(w.shownRealMs.size() == 10 && PresentWindow::PercentileOf(w.shownRealMs, 0.99) == 25.0);
    // out of range frames are dropped, never out of bounds
    w.AddReady(kMaxGeneratedFrames, 1.0);
    w.AddShown(true, kMaxGeneratedFrames, 1.0);
    CHECK(w.readyMs[2].size() == 10 && w.shownGeneratedMs[2].size() == 10);
    std::vector<double> empty;
    CHECK(PresentWindow::PercentileOf(empty, 0.5) == 0.0);
}

// The latch the router takes at the first FG swapchain (LsfgPresenter.cpp, Wanted: Enabled && Multiplier > 2).
static Latch LatchAtStart(bool enabled, int multiplier)
{
    return enabled && MultiplierOrDefault(multiplier) > 2 ? Latch::Ours : Latch::Ffx;
}

// MFG_PRESENTER.md 1.4 and 5: the menu's matrix. Started at 2 (or with LSFG off), FFX's swapchain shows x2 for the
// session and 3 or 4 wait for a restart; started at 3 or 4, ours shows whatever is chosen from the next real frame,
// 2 included (N = 1). Undecided (no FG swapchain yet), the latch reads the choice.
static void Latches()
{
    const int multipliers[] = { 2, 3, 4 };

    struct Row
    {
        int start;
        int choice;
        bool live;
        int active;
    };
    const Row matrix[] = {
        { 2, 2, true, 2 }, { 2, 3, false, 2 }, { 2, 4, false, 2 }, //
        { 3, 2, true, 2 }, { 3, 3, true, 3 },  { 3, 4, true, 4 },  //
        { 4, 2, true, 2 }, { 4, 3, true, 3 },  { 4, 4, true, 4 },
    };
    for (const auto& row : matrix)
    {
        const Latch latch = LatchAtStart(true, row.start);
        CHECK(latch == (row.start > 2 ? Latch::Ours : Latch::Ffx));
        CHECK(AppliesLive(latch, row.choice) == row.live);
        CHECK(ActiveMultiplier(latch, row.choice) == row.active);
        // What the process started with always runs as it was chosen.
        CHECK(AppliesLive(latch, row.start) && ActiveMultiplier(latch, row.start) == row.start);
    }

    // LSFG off at start: FFX's swapchain whatever the multiplier, so 3 and 4 wait for a restart once it is enabled.
    for (int start : multipliers)
    {
        CHECK(LatchAtStart(false, start) == Latch::Ffx);
        CHECK(!AppliesLive(LatchAtStart(false, start), 3) && !AppliesLive(LatchAtStart(false, start), 4));
    }

    for (int m : multipliers)
    {
        CHECK(AppliesLive(Latch::Undecided, m) && ActiveMultiplier(Latch::Undecided, m) == m);
        CHECK(ActiveMultiplier(Latch::Ffx, m) == 2);
        CHECK(MultiplierOrDefault(m) == m);
    }

    // Anything but 2, 3 or 4 is read as 2 (Config.cpp drops it at load; the code reads it as 2 anyway).
    for (int bad : { -1, 0, 1, 5, 8, 1000 })
    {
        CHECK(MultiplierOrDefault(bad) == 2);
        for (Latch latch : { Latch::Undecided, Latch::Ffx, Latch::Ours })
        {
            CHECK(AppliesLive(latch, bad));
            CHECK(ActiveMultiplier(latch, bad) == 2);
        }
        CHECK(GeneratedFramesLive(true, bad) == 1);
        CHECK(LatchAtStart(true, bad) == Latch::Ffx);
    }

    // What our swapchain is asked for at the next real frame: N = M - 1 with LSFG on, one with it off (FFX writes
    // outputs[0] only). With LSFG on it is what the menu calls active, so the row and the presenter agree.
    for (int m : multipliers)
    {
        CHECK(GeneratedFramesLive(true, m) == uint32_t(m - 1));
        CHECK(GeneratedFramesLive(false, m) == 1);
        CHECK(GeneratedFramesLive(true, m) + 1 == uint32_t(ActiveMultiplier(Latch::Ours, m)));
        CHECK(GeneratedFramesLive(true, m) >= 1 && GeneratedFramesLive(true, m) <= kMaxGeneratedFrames);
    }

    // N = 1 through our swapchain paces as FFX's x2 does: two presents per entry, the same delta bit for bit.
    const int64_t qpc = 10'000'000;
    for (double fps : { 24.0, 30.0, 60.0, 82.5 })
    {
        const double avg = double(qpc) / fps;
        const uint32_t frames = GeneratedFramesLive(true, 2) + 1;
        CHECK(PresentDelta(avg, avg * 0.01, frames, 0.3, qpc / 10000) == FfxDelta(avg, avg * 0.01, 0.3, qpc / 10000));
    }
}

int main()
{
    Multipliers();
    Deltas();
    OutputSets();
    Schedule();
    Window();
    Lists();
    Ready();
    Latency();
    Latches();

    std::printf("lsfg-presenter: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
