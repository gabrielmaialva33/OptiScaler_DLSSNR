// Behaviour of the production NR log rate limiter, against injected time points. No sleeps: every
// case names the instant it is testing.
#include <dlssnr/DlssNr_LogRate.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using DlssNr::LogRate::Clock;
using DlssNr::LogRate::Drift;
using DlssNr::LogRate::DriftInterval;
using DlssNr::LogRate::Reporter;

namespace
{
int failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

// A report shaped like the composition one: a measured quantity plus fields that only a real change
// moves. Same predicate names the production struct defines, because the Reporter calls them.
struct Report
{
    float measured = 0.0f;
    int setting = 0;
    unsigned width = 0;

    bool SameAs(const Report& other) const { return measured == other.measured && SameShape(other); }
    bool SameShape(const Report& other) const { return setting == other.setting && width == other.width; }
};

constexpr Clock::time_point Origin {};

Clock::time_point At(long long ms) { return Origin + std::chrono::milliseconds(ms); }

void FirstObservationIsAlwaysWritten()
{
    Reporter<Report> r;
    // At the origin itself: a fresh reporter has no window to be inside of.
    Check(r.Observe({ 1.0f, 0, 1920 }, Origin), "first observation written");
    Check(r.Written() != nullptr && r.Written()->measured == 1.0f, "first observation becomes the baseline");

    Reporter<Drift> d;
    Check(d.Observe({ 0.5f, 0.02f, 1e-5f }, Origin), "first scalar written");
}

void IdenticalIsNeverWrittenAndDoesNotMoveTheWindow()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.0f, 0, 1920 }, At(0)), "written at 0");
    for (long long t = 100; t <= 5000; t += 100)
        Check(!r.Observe({ 1.0f, 0, 1920 }, At(t)), "identical sample never written");

    // The window is measured from the last WRITE, so five seconds of identical samples have not
    // pushed it out: a drift now is due immediately.
    Check(r.Observe({ 1.1f, 0, 1920 }, At(5100)), "drift after identical samples is due");
}

void DriftWaitsForTheWindowAndTheBoundaryIsInclusive()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.00f, 0, 1920 }, At(0)), "written at 0");
    Check(!r.Observe({ 1.01f, 0, 1920 }, At(1)), "drift 1 ms after a write is held");
    Check(!r.Observe({ 1.02f, 0, 1920 }, At(999)), "drift just inside the window is held");

    Reporter<Report> exact;
    Check(exact.Observe({ 1.00f, 0, 1920 }, At(0)), "written at 0");
    Check(exact.Observe({ 1.01f, 0, 1920 }, Origin + DriftInterval), "drift exactly at the interval is written");

    Reporter<Report> justUnder;
    Check(justUnder.Observe({ 1.00f, 0, 1920 }, At(0)), "written at 0");
    Check(!justUnder.Observe({ 1.01f, 0, 1920 }, Origin + DriftInterval - Clock::duration(1)),
          "one tick under the interval is held");
}

// The case a "compare against the previous sample" limiter fails. Each step is under any threshold
// worth having; only the distance from the line the log carries is real.
void SlowDriftIsNotStarved()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.00f, 0, 1920 }, At(0)), "written at 0");

    float value = 1.00f;
    for (long long t = 10; t < 1000; t += 10)
    {
        value += 0.01f;
        Check(!r.Observe({ value, 0, 1920 }, At(t)), "each step inside the window is held");
    }

    Check(r.Observe({ value, 0, 1920 }, At(1000)), "accumulated drift is written when the window opens");
    Check(r.Written()->measured == value, "the written line carries the latest value, not the first held one");

    // The same trace through a limiter that adopts every sample it sees. It reports nothing at all,
    // because no single comparison ever saw the value move. That is the failure this design avoids,
    // and it is why the baseline is the last line written rather than the last sample observed.
    float previous = 1.00f;
    float walking = 1.00f;
    auto lastWrite = At(0);
    int naiveWrites = 0;
    for (long long t = 10; t <= 1000; t += 10)
    {
        walking += 0.01f;
        const bool differs = walking != previous;
        previous = walking; // adopting the sample is the defect
        if (differs && At(t) - lastWrite >= DriftInterval)
        {
            lastWrite = At(t);
            ++naiveWrites;
        }
    }
    Check(naiveWrites == 1, "the naive limiter writes once, and it writes the wrong thing");
}

void RealChangeBypassesTheWindow()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.0f, 0, 1920 }, At(0)), "written at 0");
    Check(!r.Observe({ 1.1f, 0, 1920 }, At(1)), "drift is held");
    Check(r.Observe({ 1.1f, 1, 1920 }, At(2)), "a setting change 2 ms after a write is not held");
    Check(r.Observe({ 1.1f, 1, 2560 }, At(3)), "a dimension change is not held either");
    Check(!r.Observe({ 1.2f, 1, 2560 }, At(4)), "drift right after a real change is still held");

    // A real change is a write, so the drift window runs from it.
    Check(!r.Observe({ 1.3f, 1, 2560 }, At(1002)), "window runs from the real change, not the drift");
    Check(r.Observe({ 1.3f, 1, 2560 }, At(1003)), "and opens one interval after it");
}

void SilenceIsRecoveredFrom()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.0f, 0, 1920 }, At(0)), "written at 0");
    Check(!r.Observe({ 1.1f, 0, 1920 }, At(10)), "held");
    // Nothing observed for an hour -- a loading screen, a pause, a menu.
    Check(r.Observe({ 1.2f, 0, 1920 }, At(3600 * 1000)), "the first sample after a long silence is written");
}

void AClockThatDoesNotAdvanceStillPassesRealChanges()
{
    Reporter<Report> r;
    Check(r.Observe({ 1.0f, 0, 1920 }, Origin), "written at the origin");
    for (int i = 1; i < 50; ++i)
        Check(!r.Observe({ 1.0f + static_cast<float>(i), 0, 1920 }, Origin), "frozen clock holds every drift");
    Check(r.Observe({ 1.0f, 7, 1920 }, Origin), "frozen clock still passes a real change");
}

void ScalarToleranceIsRelativeWithAnAbsoluteFloor()
{
    Reporter<Drift> r;
    Check(r.Observe({ 1.0f, 0.02f, 1e-5f }, At(0)), "written at 0");
    // 1% of the new value: inside the 2% tolerance, so the same number as far as the log is concerned.
    Check(!r.Observe({ 1.01f, 0.02f, 1e-5f }, At(10000)), "within tolerance is the same value, window or not");
    Check(r.Observe({ 1.5f, 0.02f, 1e-5f }, At(10000)), "outside tolerance after the window is written");

    // Near zero the relative term collapses and the absolute floor is what answers.
    Reporter<Drift> small;
    Check(small.Observe({ 0.0f, 0.02f, 1e-5f }, At(0)), "written at 0");
    Check(!small.Observe({ 5e-6f, 0.02f, 1e-5f }, At(10000)), "under the absolute floor is the same value");
    Check(small.Observe({ 5e-5f, 0.02f, 1e-5f }, At(10000)), "over the absolute floor is a change");
}

struct Sample
{
    long long ms;
    float whitePoint;
    int shape;
};

std::vector<Sample> LoadTrace(const char* path)
{
    std::vector<Sample> trace;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        Sample s {};
        if (std::sscanf(line.c_str(), "%lld %f %d", &s.ms, &s.whitePoint, &s.shape) == 3)
            trace.push_back(s);
    }
    return trace;
}

// The session that prompted the change, replayed line for line.
void RealSessionIsBoundedAndNeverStale(const char* path)
{
    const auto trace = LoadTrace(path);
    Check(trace.size() == 486, "the recorded session has all 486 composition lines");
    if (trace.empty())
        return;

    const long long span = trace.back().ms - trace.front().ms;
    Check(span > 54000 && span < 55000, "the session spans about 54.7 s");

    // What the code did before: a line whenever any printed field changed.
    Report previous {};
    bool havePrevious = false;
    int before = 0;
    size_t shapeChangeAt = trace.size();
    for (size_t i = 0; i < trace.size(); ++i)
    {
        const Report now { trace[i].whitePoint, trace[i].shape, 0 };
        if (i > 0 && trace[i].shape != trace[i - 1].shape)
            shapeChangeAt = i;
        if (!havePrevious || !now.SameAs(previous))
        {
            previous = now;
            havePrevious = true;
            ++before;
        }
    }
    Check(before == 486, "every one of the 486 lines was a change under the old rule");

    // The session really does contain one: the model size goes 1720x720 -> 3440x1440 mid-run.
    Check(shapeChangeAt < trace.size(), "the session contains a real change to not hold");

    Reporter<Report> r;
    int after = 0;
    long long lastWriteMs = trace.front().ms;
    long long worstStaleness = 0;
    bool shapeChangeWritten = false;
    for (size_t i = 0; i < trace.size(); ++i)
    {
        const Report now { trace[i].whitePoint, trace[i].shape, 0 };
        const bool sameAsLog = r.Written() != nullptr && now.SameAs(*r.Written());
        const bool written = r.Observe(now, At(trace[i].ms));
        if (written)
        {
            ++after;
            lastWriteMs = trace[i].ms;
            if (i == shapeChangeAt)
                shapeChangeWritten = true;
        }
        else if (!sameAsLog)
        {
            // Held. The contract is that a held difference is never older than one interval.
            worstStaleness = std::max(worstStaleness, trace[i].ms - lastWriteMs);
        }
    }

    const long long intervalMs = std::chrono::duration_cast<std::chrono::milliseconds>(DriftInterval).count();
    Check(after >= 1, "the session still reports");
    Check(after <= span / intervalMs + 2, "never more than one line per interval, plus the first");
    Check(worstStaleness < intervalMs, "a held difference is never older than one interval");
    Check(after < before / 8, "the session is reduced by more than eight to one");
    Check(shapeChangeWritten, "the model size change is written on the frame it happens");
    std::printf("trace: %d lines before, %d after, worst staleness %lld ms over %lld ms\n", before, after,
                worstStaleness, span);
}
} // namespace

int main(int argc, char** argv)
{
    static_assert(Clock::is_steady, "the limiter must not read a clock that can step backwards");

    if (argc != 2)
    {
        std::printf("FAIL usage: rate <trace>\n");
        return 1;
    }

    FirstObservationIsAlwaysWritten();
    IdenticalIsNeverWrittenAndDoesNotMoveTheWindow();
    DriftWaitsForTheWindowAndTheBoundaryIsInclusive();
    SlowDriftIsNotStarved();
    RealChangeBypassesTheWindow();
    SilenceIsRecoveredFrom();
    AClockThatDoesNotAdvanceStillPassesRealChanges();
    ScalarToleranceIsRelativeWithAnAbsoluteFloor();
    RealSessionIsBoundedAndNeverStale(argv[1]);

    if (failures != 0)
    {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("PASS: rate limiter behaviour\n");
    return 0;
}
