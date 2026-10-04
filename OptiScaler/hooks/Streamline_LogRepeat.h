#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// Streamline repeats some of its own complaints every frame, from more than one thread. The one that
// prompted this, "rsync.cpp:660[setDynamicMFGParams] RSYNC: setDynamicMFGParams failed with status 1",
// was 4,893 of the 5,955 lines of a Witcher 3 log (2026-09-29), each a formatted write and a flush.
//
// Nothing is dropped silently. The first line from a site is written as it is; a repeat of the same
// text is written at most once per interval, carrying the count of the repeats folded into it. A new
// site, or new text from an old one, is written at once. A count still pending when the repeats stop
// is not written, because nothing calls back to carry it -- the last written line says how often it
// had been seen up to then.
namespace StreamlineLogRepeat
{
// Monotonic: a wall clock can step back and silence a limiter for as long as the correction.
using Clock = std::chrono::steady_clock;

constexpr Clock::duration Interval = std::chrono::seconds(10);

// Sites remembered at once. Past it, a new site is written every time: the table fails open rather
// than hiding a message nobody has seen before.
constexpr size_t MaxSites = 256;

// Streamline starts every line with bracketed fields that change on each repeat: wall clock, the
// "streamline" tag, the level, the thread, the time since start --
// "[09-59-04][streamline][error][tid:1980][10s:105ms:628us]rsync.cpp:660[...] text". The site is what
// follows them. A line with no leading bracket is its own site.
inline std::string_view Site(std::string_view line)
{
    size_t position = 0;

    while (position < line.size() && line[position] == '[')
    {
        const size_t close = line.find(']', position);

        if (close == std::string_view::npos)
            break;

        position = close + 1;
    }

    return line.substr(position);
}

struct Decision
{
    bool write = false;

    // Repeats of this line that were not written since the last one that was.
    uint64_t folded = 0;
};

class Filter
{
    struct Entry
    {
        Clock::time_point lastWrite {};
        uint64_t pending = 0;
    };

    std::mutex _mutex;
    std::unordered_map<std::string, Entry> _sites;
    Clock::duration _interval;

  public:
    explicit Filter(Clock::duration interval = Interval) : _interval(interval) {}

    Decision Observe(std::string_view line, Clock::time_point now)
    {
        std::string site(Site(line));

        std::lock_guard lock(_mutex);

        const auto found = _sites.find(site);

        if (found == _sites.end())
        {
            if (_sites.size() < MaxSites)
                _sites.emplace(std::move(site), Entry { now, 0 });

            return { true, 0 };
        }

        Entry& entry = found->second;

        if (now - entry.lastWrite < _interval)
        {
            ++entry.pending;
            return { false, 0 };
        }

        const uint64_t folded = entry.pending;
        entry.pending = 0;
        entry.lastWrite = now;

        return { true, folded };
    }

    size_t Sites()
    {
        std::lock_guard lock(_mutex);
        return _sites.size();
    }
};
} // namespace StreamlineLogRepeat
