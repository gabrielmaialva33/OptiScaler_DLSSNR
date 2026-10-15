#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

// The decision half of the session guard (docs/dlssnr/design/session-guard.md). No Windows types: the file, the
// process probe, the clock, the process identity and the exit watch come in through an Env, so a host suite can
// drive it all with fakes. DlssNr_Session.cpp supplies the real Env.

namespace DlssNr::SessionGuard
{
inline constexpr int64_t kEarlySeconds = 120; // under this, an unclean end is a crash at start
inline constexpr int64_t kLongSeconds = 600;  // this or more, an end without evidence is not blamed on NR
inline constexpr int64_t kBeatSeconds = 30;   // least interval between heartbeat writes
inline constexpr uint32_t kStrikeLimit = 2;   // consecutive strikes that disable
inline constexpr int kEndLockTries = 8;       // how often the end retries the lock, one Pause() apart
inline constexpr size_t kMarkerCapacity = 256;
inline constexpr std::string_view kVersionLine = "retoque-nr-session 2";

struct Marker
{
    bool active = false;
    uint32_t pid = 0;
    uint64_t created = 0; // process creation time, FILETIME ticks
    int64_t first = 0;    // unix seconds of the first NR success
    int64_t beat = 0;     // unix seconds of the last heartbeat, or of the end
    uint32_t streak = 0;  // consecutive unclean sessions
    bool blocked = false; // NR stays off until acknowledged
    bool removed = false; // NR saw its device removed or lost
    bool ended = false;   // the process recorded its own end
    uint32_t code = 0;    // the status it ended with; 0 unless ended
};

// The marker as text, into a fixed buffer: no allocation, so the end of the process can write it. Returns the length,
// or 0 if it would not fit (it always does; the check keeps a future key from writing past the buffer).
inline size_t SerializeTo(char (&out)[kMarkerCapacity], const Marker& m)
{
    size_t n = 0;
    bool fits = true;
    const auto put = [&](std::string_view s)
    {
        if (!fits || s.size() > kMarkerCapacity - n)
        {
            fits = false;
            return;
        }
        std::memcpy(out + n, s.data(), s.size());
        n += s.size();
    };
    const auto number = [&](auto value)
    {
        if (!fits)
            return;
        const auto [end, ec] = std::to_chars(out + n, out + kMarkerCapacity, value);
        if (ec != std::errc())
        {
            fits = false;
            return;
        }
        n = size_t(end - out);
    };

    put(kVersionLine);
    put("\nactive=");
    number(m.active ? 1 : 0);
    put("\npid=");
    number(m.pid);
    put("\ncreated=");
    number(m.created);
    put("\nfirst=");
    number(m.first);
    put("\nbeat=");
    number(m.beat);
    put("\nstreak=");
    number(m.streak);
    put("\nblocked=");
    number(m.blocked ? 1 : 0);
    put("\nremoved=");
    number(m.removed ? 1 : 0);
    put("\nended=");
    number(m.ended ? 1 : 0);
    put("\ncode=");
    number(m.code);
    put("\n");
    return fits ? n : 0;
}

inline std::string Serialize(const Marker& m)
{
    char buffer[kMarkerCapacity];
    return std::string(buffer, SerializeTo(buffer, m));
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

inline bool Flag(std::string_view text, bool& out)
{
    unsigned value = 0;
    if (!Number(text, value) || value > 1)
        return false;
    out = value == 1;
    return true;
}
} // namespace detail

// Exact version line, every key present once, well-formed values, beat not before first, and the end consistent:
// only an active marker records an end, a code only with an end, and an end with code 0 only with a removal (a plain
// normal quit deletes the file instead). Anything else is "no marker": what cannot be read is never held against
// the user.
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

        if (key == "active")
        {
            flag = 1u << 0;
            ok = detail::Flag(value, m.active);
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
            ok = detail::Flag(value, m.blocked);
        }
        else if (key == "removed")
        {
            flag = 1u << 7;
            ok = detail::Flag(value, m.removed);
        }
        else if (key == "ended")
        {
            flag = 1u << 8;
            ok = detail::Flag(value, m.ended);
        }
        else if (key == "code")
        {
            flag = 1u << 9;
            ok = detail::Number(value, m.code);
        }

        if (!ok || (seen & flag) != 0)
            return false;
        seen |= flag;
    }

    if (firstLine || seen != 0x3FFu || m.beat < m.first)
        return false;
    if (m.ended && !m.active)
        return false;
    if (!m.ended && m.code != 0)
        return false;
    if (m.ended && m.code == 0 && !m.removed)
        return false;

    out = m;
    return true;
}

// The status a process ended itself with, by its NTSTATUS severity: warning and error severities (0x80000000 and up)
// are exceptions -- an access violation or illegal instruction reaching the default end, a breakpoint, a crash
// reporter's own code. Below that it is the program's own exit code; 0 is a normal quit. (Fail-fast, which abort() and
// an MSVC program's unhandled C++ exception end in, never reaches the hook: it ends below user mode.)
inline bool IsExceptionStatus(uint32_t status) { return status >= 0x80000000u; }

// How the previous session ended, strongest evidence first.
enum class End
{
    None,          // no verdict about an end (clean, unreadable, live, a standing block)
    Vanished,      // no end was seen: stopped from outside, hung and killed, or died where no code runs
    ExitCode,      // it ended itself with a nonzero exit code
    Crashed,       // it ended itself with an exception status
    DeviceRemoved, // NR saw its device removed or lost, whatever came after
};

// What the probe can say about the process a marker names.
enum class Owner
{
    Dead,    // no such process
    Same,    // alive, same creation time: a second instance of the game
    Reused,  // alive, different creation time: the pid belongs to something else now
    Unknown, // access denied, no creation time: cannot tell
};

// What the probe's answers mean, apart from the calls that produce them (DlssNr_Session.cpp asks OpenProcess,
// GetExitCodeProcess and GetProcessTimes). `noSuchProcess`: OpenProcess failed with ERROR_INVALID_PARAMETER, which is
// what Windows and Wine (STATUS_INVALID_CID) answer for a pid nobody holds; any other failure is an unknown, never a
// verdict.
inline Owner ClassifyProbe(bool opened, bool noSuchProcess, bool exited, bool haveCreation, uint64_t creation,
                           uint64_t expected)
{
    if (!opened)
        return noSuchProcess ? Owner::Dead : Owner::Unknown;
    if (exited)
        return Owner::Dead; // a handle somebody kept to a finished process
    if (!haveCreation)
        return Owner::Unknown;
    return creation == expected ? Owner::Same : Owner::Reused;
}

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
    RepeatedEnd,   // a strike that makes kStrikeLimit in a row
    LongEnd,       // unclean, kLongSeconds or more
    StillBlocked,  // an earlier verdict has not been acknowledged
};

struct Verdict
{
    Action action = Action::None;
    Reason reason = Reason::None;
    End end = End::None;
    uint32_t code = 0; // the status the process recorded at its end; 0 when it recorded none
    int64_t ranSeconds = 0;
    uint32_t streak = 0;
};

// The notice's reason line for a verdict, untranslated (the menu runs it through Tr; the pt-BR pack carries each).
// Empty where the verdict shows nothing: a long end with no evidence is a log line only.
inline const char* ReasonText(const Verdict& v)
{
    if (v.reason == Reason::StillBlocked)
        return "An earlier session ended without closing, and this has not been confirmed since.";
    if (v.reason == Reason::RepeatedEnd)
        return "The last two sessions ended without closing.";

    const bool early = v.reason == Reason::EarlyEnd;
    const bool middle = v.reason == Reason::MiddleEnd;
    const bool late = v.reason == Reason::LongEnd;

    switch (v.end)
    {
    case End::DeviceRemoved:
        if (late)
            return "The graphics device was lost in the last session, long after the model started. If it happens "
                   "again, Neural Rendering will be switched off.";
        return early || middle ? "The graphics device was lost while the model was running in the last session." : "";
    case End::Crashed:
        if (late)
            return "The last session crashed, long after the model started. If the next one does too, Neural "
                   "Rendering will be switched off.";
        return early || middle ? "The last session crashed while the model was running." : "";
    case End::ExitCode:
        if (early)
            return "The last session ended with an error soon after the model started.";
        return middle ? "The last session ended with an error. If it was a crash, this one may too." : "";
    case End::Vanished:
        if (early)
            return "The last session ended without closing, soon after the model started.";
        return middle ? "The last session ended without closing. If it was a crash, this one may too." : "";
    default:
        return "";
    }
}

// How the previous session ended, for the log line.
inline std::string EndText(const Verdict& v)
{
    char text[96];
    switch (v.end)
    {
    case End::Vanished:
        return "no end was seen: stopped from outside, stopped responding, or killed";
    case End::ExitCode:
        std::snprintf(text, sizeof(text), "it ended itself with exit code %u", unsigned(v.code));
        return text;
    case End::Crashed:
        std::snprintf(text, sizeof(text), "it ended with exception 0x%08X", unsigned(v.code));
        return text;
    case End::DeviceRemoved:
        if (v.code == 0)
            return "NR saw the graphics device lost";
        std::snprintf(text, sizeof(text), "NR saw the graphics device lost; it then ended with status 0x%08X",
                      unsigned(v.code));
        return text;
    default:
        return {};
    }
}

// The Env contract (all of it is called under the guard's lock):
//   bool Read(std::string&)                     the marker's text; false when there is none or it cannot be read
//   bool Write(const std::string&)              replace the marker; false when that failed
//   void Remove()                               delete it, best effort
//   bool WatchExit()                            make sure NoteProcessEnd will be called when the process ends itself;
//                                               false when it cannot be (then no marker is ever written)
//   Owner Probe(uint32_t pid, uint64_t created)
//   uint32_t SelfPid(); uint64_t SelfCreated()
//   int64_t Now()                               unix seconds
// and, from the end of the process (inside NtTerminateProcess, or DllMain at exit): no logging, no allocation, no
// waiting on anything another thread may hold:
//   bool RemoveAtExit()                         delete it; false when that failed
//   bool WriteAtExit(const char*, size_t)       replace it from a ready buffer
//   void Pause()                                about a millisecond, between lock retries
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
            // A standing block is applied only if it can still be cleared: a marker that can no longer be written
            // (a read-only file or folder since) would otherwise keep NR off on every launch, and "Enable anyway"
            // could not remove it.
            if (previous.blocked && !_env.Write(Serialize(previous)))
            {
                _verdict.reason = Reason::CannotPersist;
                return _verdict;
            }

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

        // A recorded end is stale by construction: the process said it was ending. Otherwise ask whether it lives.
        if (!previous.ended)
        {
            const Owner owner = _env.Probe(previous.pid, previous.created);
            if (owner == Owner::Same || owner == Owner::Unknown)
            {
                _verdict.reason = Reason::LiveInstance; // not ours to touch: stay idle, write nothing
                return _verdict;
            }
        }

        // Stale: the session that wrote it is gone without having cleared it.
        const End end = HowItEnded(previous);
        const bool evidence = end == End::Crashed || end == End::DeviceRemoved;
        if (end == End::None) // ended=1 with code 0 and no removal is refused by Parse; kept for safety
        {
            _mode = Mode::Armed;
            return _verdict;
        }

        const int64_t ran = std::max<int64_t>(0, previous.beat - previous.first);
        Marker next;
        next.streak = previous.streak + 1;
        _verdict.end = end;
        _verdict.code = previous.code; // 0 unless the process recorded its end
        _verdict.ranSeconds = ran;

        if (ran < kEarlySeconds)
        {
            _verdict.action = Action::Disable;
            _verdict.reason = Reason::EarlyEnd;
        }
        else if (ran < kLongSeconds)
        {
            _verdict.reason = Reason::MiddleEnd;
            _verdict.action = evidence ? Action::Disable : Action::Notice;
        }
        else if (evidence)
        {
            _verdict.reason = Reason::LongEnd;
            _verdict.action = Action::Notice;
        }
        else
        {
            _verdict.reason = Reason::LongEnd; // not blamed: a log line, and the strikes start over
            next.streak = 0;
        }

        if (_verdict.action == Action::Notice && next.streak >= kStrikeLimit)
        {
            _verdict.action = Action::Disable;
            _verdict.reason = Reason::RepeatedEnd;
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
        const Mode seen = _mode;
        if (seen == Mode::Idle || seen == Mode::Ended) // lock-free for a guard that never arms
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode == Mode::Idle || _mode == Mode::Ended)
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
            // The exit watch first: a marker nobody can clear at a normal quit is version 1's false positive.
            if (!_env.WatchExit())
            {
                Move(Mode::Armed, Mode::Idle); // no protection, and no further attempts
                return;
            }

            // The session's clock: NR may stop and start again within one process (a route handing the model to
            // another, a toggle), and how long it has run is counted from its first success in this process.
            if (_sessionFirst == 0)
                _sessionFirst = now;

            _marker = {};
            _marker.active = true;
            _marker.pid = _env.SelfPid();
            _marker.created = _env.SelfCreated();
            _marker.first = std::min(_sessionFirst, now);
            _marker.beat = now;
            _marker.streak = _carriedStreak;
            if (!_env.Write(Serialize(_marker)))
            {
                Move(Mode::Armed, Mode::Idle); // cannot write: no protection, and no further attempts
                return;
            }

            // The end may have given up on the lock while this write held it, and marked the guard Ended without it.
            // Then this marker is one nobody will clear: take it back.
            if (!Move(Mode::Armed, Mode::Running))
                _env.Remove();
            return;
        }

        if (now - _marker.beat < kBeatSeconds)
            return;

        _marker.beat = std::max(now, _marker.first);
        _env.Write(Serialize(_marker)); // a failed beat only makes the end look earlier than it was
    }

    // NR saw its device removed or lost while it owned a marker. Written at once, since what follows may be a crash
    // or a hang that leaves no chance later.
    void NoteDeviceRemoved()
    {
        if (_mode != Mode::Running || _removedNoted.load())
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode != Mode::Running || _marker.removed)
            return;

        _removedNoted = true;
        _marker.removed = true;
        _marker.beat = std::max(_env.Now(), _marker.first);
        _env.Write(Serialize(_marker));
    }

    // NR stopped running (turned off, faulted, or its model freed by a route): a crash from now on is not NR's, so the
    // marker goes. Not the strikes or the session's clock: they belong to the session, which has not ended. And not a
    // marker that holds evidence: a device NR saw lost stays on record for whatever end follows (a Vulkan device is
    // destroyed after it is lost, and that frees the model).
    void NoteOff()
    {
        if (_mode != Mode::Running) // lock-free in the common case: no marker owned
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_mode != Mode::Running || _marker.removed)
            return;

        _env.Remove();
        Move(Mode::Running, Mode::Armed);
    }

    // The process is ending itself with `status` (the hook on NtTerminateProcess, on the thread that ends it). Status 0
    // with no removal is a normal quit: the marker goes. Anything else is recorded for the next launch. Every mode
    // ends in Ended, so no other thread starts a marker in the moment before it is stopped.
    //
    // Never waits for the lock beyond kEndLockTries short pauses: in ExitProcess the caller holds the loader and PEB
    // locks, and a heartbeat write on another thread may be blocked on one of them while holding ours. Then nothing is
    // written and the marker reads as "no end seen" -- a false verdict at worst (a disable under 2 minutes), never a
    // hung exit. The Ended stored without the lock stays: every later transition is a compare-exchange from a mode
    // that is not Ended.
    void NoteProcessEnd(uint32_t status)
    {
        if (_mode == Mode::Ended)
            return;

        std::unique_lock<std::mutex> lock(_mutex, std::defer_lock);
        for (int attempt = 0; !lock.try_lock(); ++attempt)
        {
            if (attempt + 1 >= kEndLockTries)
            {
                _mode = Mode::Ended; // nothing written, but no later frame starts a marker either
                return;
            }
            _env.Pause();
        }

        const Mode mode = _mode;
        _mode = Mode::Ended;
        if (mode != Mode::Running)
            return;

        char text[kMarkerCapacity];
        if (status == 0 && !_marker.removed)
        {
            if (_env.RemoveAtExit())
                return;
            const Marker over; // could not delete: say it is over instead, so it is not read as a crash
            if (const size_t size = SerializeTo(text, over))
                _env.WriteAtExit(text, size);
            return;
        }

        Marker end = _marker;
        end.ended = true;
        end.code = status;
        end.beat = std::max(_env.Now(), end.first);
        if (const size_t size = SerializeTo(text, end))
            _env.WriteAtExit(text, size);
    }

    // DLL_PROCESS_DETACH while the process exits, reached only if the hook never fired (it fires first in ExitProcess):
    // the version-1 fallback. ExitProcess has already stopped every other thread, and one may have stopped while
    // holding the lock, so this never waits for it.
    void NoteOffAtExit()
    {
        if (_mode != Mode::Running)
            return;

        std::unique_lock<std::mutex> lock(_mutex, std::try_to_lock);
        if (!lock.owns_lock() || _mode != Mode::Running)
            return;

        _env.RemoveAtExit();
        Move(Mode::Running, Mode::Armed);
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
        Move(Mode::Idle, Mode::Armed);
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
    bool HasEnded() const { return _mode == Mode::Ended; }

  private:
    // Idle: nothing to do and nothing to write. Armed: may write when NR first succeeds. Running: owns an active
    // marker, so a clean end must clear it. Ended: the process is ending itself; terminal.
    enum class Mode
    {
        Idle,
        Armed,
        Running,
        Ended,
    };

    // Every transition after Begin: never over Ended, which the end may store without the lock.
    bool Move(Mode from, Mode to) { return _mode.compare_exchange_strong(from, to); }

    static End HowItEnded(const Marker& m)
    {
        if (m.removed)
            return End::DeviceRemoved;
        if (!m.ended)
            return End::Vanished;
        if (m.code == 0)
            return End::None;
        return IsExceptionStatus(m.code) ? End::Crashed : End::ExitCode;
    }

    Env& _env;
    mutable std::mutex _mutex;
    std::atomic<Mode> _mode { Mode::Idle };
    std::atomic<bool> _removedNoted { false };
    Marker _marker;
    Verdict _verdict;
    uint32_t _carriedStreak = 0;
    int64_t _sessionFirst = 0; // unix seconds of NR's first success in this process; 0 before it
    bool _blocked = false;
};
} // namespace DlssNr::SessionGuard
