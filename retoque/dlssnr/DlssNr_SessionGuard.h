#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

// The decision half of the session guard (docs/dlssnr/design/session-guard.md). No Windows types: the file, the
// process probe, the clock and the process identity come in through an Env, so a host suite can drive it all with
// fakes. DlssNr_SessionGuard.cpp supplies the real Env.

namespace DlssNr::SessionGuard
{
inline constexpr int64_t kEarlySeconds = 120; // under this, an unclean end is a crash at start
inline constexpr int64_t kLongSeconds = 600;  // this or more, an unclean end is not blamed on NR
inline constexpr int64_t kBeatSeconds = 30;   // least interval between heartbeat writes
inline constexpr uint32_t kStrikeLimit = 2;   // consecutive unclean middle sessions that disable
inline constexpr std::string_view kVersionLine = "retoque-nr-session 1";

struct Marker
{
    bool active = false;
    uint32_t pid = 0;
    uint64_t created = 0; // process creation time, FILETIME ticks
    int64_t first = 0;    // unix seconds of the first NR success
    int64_t beat = 0;     // unix seconds of the last heartbeat
    uint32_t streak = 0;  // consecutive unclean sessions
    bool blocked = false; // NR stays off until acknowledged
};

inline std::string Serialize(const Marker& m)
{
    std::string text(kVersionLine);
    text += "\nactive=" + std::to_string(m.active ? 1 : 0);
    text += "\npid=" + std::to_string(m.pid);
    text += "\ncreated=" + std::to_string(m.created);
    text += "\nfirst=" + std::to_string(m.first);
    text += "\nbeat=" + std::to_string(m.beat);
    text += "\nstreak=" + std::to_string(m.streak);
    text += "\nblocked=" + std::to_string(m.blocked ? 1 : 0);
    text += "\n";
    return text;
}

namespace detail
{
template <class T> bool Number(std::string_view text, T& out)
{
    if (text.empty())
        return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}
} // namespace detail

// Exact version line, every key present once, well-formed values, beat not before first. Anything else is "no
// marker": what cannot be read is never held against the user.
inline bool Parse(std::string_view text, Marker& out)
{
    Marker m;
    unsigned seen = 0;
    bool firstLine = true;

    while (!text.empty())
    {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view {} : text.substr(eol + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        if (firstLine)
        {
            if (line != kVersionLine)
                return false;
            firstLine = false;
            continue;
        }

        if (line.empty())
            continue;

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return false;

        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        unsigned flag = 0;
        bool ok = false;
        unsigned boolean = 0;

        if (key == "active")
        {
            flag = 1u << 0;
            ok = detail::Number(value, boolean) && boolean <= 1;
            m.active = boolean == 1;
        }
        else if (key == "pid")
        {
            flag = 1u << 1;
            ok = detail::Number(value, m.pid);
        }
        else if (key == "created")
        {
            flag = 1u << 2;
            ok = detail::Number(value, m.created);
        }
        else if (key == "first")
        {
            flag = 1u << 3;
            ok = detail::Number(value, m.first) && m.first >= 0;
        }
        else if (key == "beat")
        {
            flag = 1u << 4;
            ok = detail::Number(value, m.beat) && m.beat >= 0;
        }
        else if (key == "streak")
        {
            flag = 1u << 5;
            ok = detail::Number(value, m.streak);
        }
        else if (key == "blocked")
        {
            flag = 1u << 6;
            ok = detail::Number(value, boolean) && boolean <= 1;
            m.blocked = boolean == 1;
        }

        if (!ok || (seen & flag) != 0)
            return false;
        seen |= flag;
    }

    if (firstLine || seen != 0x7Fu || m.beat < m.first)
        return false;

    out = m;
    return true;
}

// What the probe can say about the process a marker names.
enum class Owner
{
    Dead,    // no such process
    Same,    // alive, same creation time: a second instance of the game
    Reused,  // alive, different creation time: the pid belongs to something else now
    Unknown, // access denied, no creation time: cannot tell
};

enum class Action
{
    None,
    Notice,
    Disable,
};

enum class Reason
{
    None,
    GuardOff, // [DlssNr] SessionGuard=false
    NrOff,    // NR not enabled at start
    NoMarker,
    Unreadable,
    LiveInstance,  // a second instance owns the marker (or the probe could not tell)
    CannotPersist, // the verdict could not be written, so it is not applied
    EarlyEnd,      // unclean, under kEarlySeconds
    MiddleEnd,     // unclean, in between
    RepeatedEnd,   // second consecutive middle strike
    LongEnd,       // unclean, kLongSeconds or more: not blamed
    StillBlocked,  // an earlier verdict has not been acknowledged
};

struct Verdict
{
    Action action = Action::None;
    Reason reason = Reason::None;
    int64_t ranSeconds = 0;
    uint32_t streak = 0;
};

// The Env contract (all of it is called from the guard's own thread of control, under its lock):
//   bool Read(std::string&)           the marker's text; false when there is none or it cannot be read
//   bool Write(const std::string&)    replace the marker; false when that failed
//   void Remove()                     delete it, best effort
//   void RemoveAtExit()               delete it and nothing else: no logging, no allocation, no locks (it runs inside
//                                     DllMain while the process exits)
//   Owner Probe(uint32_t pid, uint64_t created)
//   uint32_t SelfPid(); uint64_t SelfCreated()
//   int64_t Now()                     unix seconds
template <class Env> class Guard
{
  public:
    explicit Guard(Env& env) : _env(env) {}

    // Reads the previous session's marker and decides. The caller applies Action::Disable.
    Verdict Begin(bool guardEnabled, bool nrEnabled)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _verdict = {};
        _mode = Mode::Idle;

        if (!guardEnabled)
        {
            _verdict.reason = Reason::GuardOff;
            return _verdict;
        }

        // NR off at start: nothing is read and nothing written. Armed, so that turning it on later starts a marker
        // at the first success; an older marker is then replaced without a verdict.
        if (!nrEnabled)
        {
            _verdict.reason = Reason::NrOff;
            _mode = Mode::Armed;
            return _verdict;
        }

        std::string text;
        Marker previous;
        if (!_env.Read(text))
        {
            _verdict.reason = Reason::NoMarker;
            _mode = Mode::Armed;
            return _verdict;
        }

        if (!Parse(text, previous))
        {
            _verdict.reason = Reason::Unreadable;
            _mode = Mode::Armed;
            return _verdict;
        }

        _carriedStreak = previous.streak;

        if (!previous.active)
        {
            _mode = Mode::Armed;
            if (previous.blocked)
            {
                _verdict.action = Action::Disable;
                _verdict.reason = Reason::StillBlocked;
                _verdict.streak = previous.streak;
                _blocked = true;
            }
            return _verdict;
        }

        const Owner owner = _env.Probe(previous.pid, previous.created);
        if (owner == Owner::Same || owner == Owner::Unknown)
        {
            _verdict.reason = Reason::LiveInstance; // not ours to touch: stay idle, write nothing
            return _verdict;
        }

        // Stale: the session that wrote it is gone without having cleared it.
        const int64_t ran = std::max<int64_t>(0, previous.beat - previous.first);
        Marker next;
        _verdict.ranSeconds = ran;

        if (ran >= kLongSeconds)
        {
            _verdict.reason = Reason::LongEnd;
            next.streak = 0;
        }
        else if (ran < kEarlySeconds)
        {
            _verdict.action = Action::Disable;
            _verdict.reason = Reason::EarlyEnd;
            next.streak = previous.streak + 1;
        }
        else
        {
            next.streak = previous.streak + 1;
            _verdict.reason = Reason::MiddleEnd;
            _verdict.action = Action::Notice;
            if (next.streak >= kStrikeLimit)
            {
                _verdict.action = Action::Disable;
                _verdict.reason = Reason::RepeatedEnd;
            }
        }

        next.blocked = _verdict.action == Action::Disable;
        _verdict.streak = next.streak;

        // Persist before applying: a directory that cannot be written would show this same stale marker on
        // every launch and disable NR for good.
        if (!_env.Write(Serialize(next)))
        {
            _verdict = {};
            _verdict.reason = Reason::CannotPersist;
            return _verdict;
        }

        _carriedStreak = next.streak;
        _blocked = next.blocked;
        _mode = Mode::Armed;
        return _verdict;
    }

    // An NR evaluate succeeded. Starts the marker on the first call and beats it at most every kBeatSeconds.
    void NoteEvaluated()
    {
        if (_mode == Mode::Idle) // lock-free for a guard that never arms
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode == Mode::Idle)
            return;

        // NR is evaluating while a block stands: the user turned it back on without the button. That is the same
        // statement, so it clears the block and the strikes.
        if (_blocked)
        {
            _blocked = false;
            _carriedStreak = 0;
            _verdict.action = Action::None;
        }

        const int64_t now = _env.Now();

        if (_mode == Mode::Armed)
        {
            _marker = {};
            _marker.active = true;
            _marker.pid = _env.SelfPid();
            _marker.created = _env.SelfCreated();
            _marker.first = _marker.beat = now;
            _marker.streak = _carriedStreak;
            if (!_env.Write(Serialize(_marker)))
            {
                _mode = Mode::Idle; // cannot write: no protection, and no further attempts
                return;
            }
            _mode = Mode::Running;
            return;
        }

        if (now - _marker.beat < kBeatSeconds)
            return;

        _marker.beat = std::max(now, _marker.first);
        _env.Write(Serialize(_marker)); // a failed beat only makes the end look earlier than it was
    }

    // NR stopped running (turned off, faulted) or the process is exiting in order: the session ended clean.
    void NoteOff()
    {
        if (_mode != Mode::Running) // lock-free in the common case: no marker owned
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode != Mode::Running)
            return;

        _env.Remove();
        _carriedStreak = 0;
        _mode = Mode::Armed;
    }

    // The process is exiting (DLL_PROCESS_DETACH with lpReserved set). ExitProcess has already stopped every other
    // thread, and one may have stopped while holding the lock (a heartbeat write), so this never waits for it: when the
    // lock is held the marker stays, and the next launch reads an exit that did not close. That costs at most one false
    // notice; waiting would hang the game's exit.
    void NoteOffAtExit()
    {
        if (_mode != Mode::Running)
            return;

        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (!lock.owns_lock() || _mode != Mode::Running)
            return;

        _env.RemoveAtExit();
        _carriedStreak = 0;
        _mode = Mode::Armed;
    }

    // The user vouched for NR. Clears the block and the strikes; the caller turns NR on.
    void Acknowledge()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_blocked)
            return;

        _blocked = false;
        _carriedStreak = 0;
        _env.Remove();
        _verdict.action = Action::None;
        if (_mode == Mode::Idle)
            _mode = Mode::Armed;
    }

    Verdict Last() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _verdict;
    }

    bool Blocked() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _blocked;
    }

    bool OwnsMarker() const { return _mode == Mode::Running; }

  private:
    // Idle: nothing to do and nothing to write. Armed: may write when NR first succeeds. Running: owns an
    // active marker, so a clean end must clear it.
    enum class Mode
    {
        Idle,
        Armed,
        Running,
    };

    Env& _env;
    mutable std::mutex _mutex;
    std::atomic<Mode> _mode { Mode::Idle };
    Marker _marker;
    Verdict _verdict;
    uint32_t _carriedStreak = 0;
    bool _blocked = false;
};
} // namespace DlssNr::SessionGuard
