#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

namespace DlssNr::LogRate
{
// Monotonic on purpose. A wall clock can step backwards over an NTP correction or a suspend, and a
// rate limiter reading one goes silent for as long as the correction.
using Clock = std::chrono::steady_clock;

// One line per second per site for a quantity that only drifts. design/nr-log-rate.md has the
// session that set it: 486 composition lines in 54.7 s, with white-point drift and one model-size
// transition that must still be reported immediately.
constexpr Clock::duration DriftInterval = std::chrono::seconds(1);

// Decides when a report may be written to the log again.
//
// A Report answers two questions about the line the log already carries:
//   SameAs    -- says the same thing, at the precision the line is printed with
//   SameShape -- differs only in a measured quantity; nothing real changed
//
// The remembered report is the last one WRITTEN, never the last one observed. That separation is
// the whole point. Adopting a suppressed sample lets a slow drift starve: every individual step is
// small, the baseline creeps along with the value, and a number that walks from 1.38 to 3.00 over a
// minute is never reported, because no single comparison ever saw it move. So `_written` is private
// and only Adopt() assigns it -- a caller cannot adopt a sample it did not log.
template <typename Report> class Reporter
{
    Report _written {};
    bool _have = false;
    Clock::time_point _lastWrite {};
    Clock::duration _interval;

    bool Adopt(const Report& sample, Clock::time_point now)
    {
        _written = sample;
        _have = true;
        _lastWrite = now;
        return true;
    }

  public:
    explicit Reporter(Clock::duration interval = DriftInterval) : _interval(interval) {}

    // True when the caller should write the line now.
    bool Observe(const Report& sample, Clock::time_point now)
    {
        // A throttle that swallows the first line turns a silent log into evidence of nothing.
        if (!_have)
            return Adopt(sample, now);

        if (sample.SameAs(_written))
            return false;

        // A real change is an event, not a trend, and never waits.
        if (!sample.SameShape(_written))
            return Adopt(sample, now);

        return now - _lastWrite >= _interval ? Adopt(sample, now) : false;
    }

    // The report the log carries, or nullptr before the first line.
    const Report* Written() const { return _have ? &_written : nullptr; }
};

// A single measured number, compared at the tolerance its own site prints with. Every difference is
// drift: a lone scalar has no shape to change. The tolerance is relative to the new value, which is
// what the sites this replaces already did.
struct Drift
{
    float value = 0.0f;
    float relative = 0.0f;
    float absolute = 0.0f;

    bool SameAs(const Drift& other) const
    {
        return std::abs(value - other.value) <= std::max(relative * std::abs(value), absolute);
    }

    bool SameShape(const Drift&) const { return true; }
};
} // namespace DlssNr::LogRate
