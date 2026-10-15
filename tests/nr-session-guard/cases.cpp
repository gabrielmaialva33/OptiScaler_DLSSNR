#include <dlssnr/DlssNr_SessionGuard.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace DlssNr::SessionGuard;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

// One directory entry, one clock, one process table, one exit watch.
struct FakeEnv
{
    bool exists = false;
    std::string text;
    bool writable = true;
    bool deletable = true;
    bool watchable = true;
    int writes = 0;
    int removes = 0;
    int exitWrites = 0;
    int exitRemoves = 0;
    int watches = 0;
    int pauses = 0;
    Owner probeAnswer = Owner::Dead;
    uint32_t probedPid = 0;
    uint64_t probedCreated = 0;
    uint32_t selfPid = 7000;
    uint64_t selfCreated = 555;
    int64_t now = 1'000'000;
    std::atomic<bool>* holdWrite = nullptr; // a Write that waits here, holding the guard's lock
    std::atomic<bool>* inWrite = nullptr;

    bool Read(std::string& out)
    {
        if (!exists)
            return false;
        out = text;
        return true;
    }
    bool Write(const std::string& t)
    {
        if (inWrite)
            inWrite->store(true);
        while (holdWrite && holdWrite->load())
            std::this_thread::yield();
        if (!writable || t.empty())
            return false;
        exists = true;
        text = t;
        ++writes;
        return true;
    }
    void Remove()
    {
        ++removes;
        exists = false;
        text.clear();
    }
    bool RemoveAtExit()
    {
        ++exitRemoves;
        if (!deletable)
            return false;
        exists = false;
        text.clear();
        return true;
    }
    bool WriteAtExit(const char* data, size_t size)
    {
        ++exitWrites;
        if (!writable)
            return false;
        exists = true;
        text.assign(data, size);
        return true;
    }
    bool WatchExit()
    {
        ++watches;
        return watchable;
    }
    void Pause() { ++pauses; }
    Owner Probe(uint32_t pid, uint64_t created)
    {
        probedPid = pid;
        probedCreated = created;
        return probeAnswer;
    }
    uint32_t SelfPid() { return selfPid; }
    uint64_t SelfCreated() { return selfCreated; }
    int64_t Now() { return now; }
};

// A marker a previous session left behind.
static void Leave(FakeEnv& env, int64_t ranSeconds, uint32_t streak = 0, bool active = true, bool blocked = false)
{
    Marker m;
    m.active = active;
    m.pid = 4242;
    m.created = 999;
    m.first = 500'000;
    m.beat = m.first + ranSeconds;
    m.streak = streak;
    m.blocked = blocked;
    env.exists = true;
    env.text = Serialize(m);
}

// A marker whose process recorded its end, as NoteProcessEnd writes it.
static void LeaveEnded(FakeEnv& env, int64_t ranSeconds, uint32_t code, bool removed = false, uint32_t streak = 0)
{
    Marker m;
    m.active = true;
    m.pid = 4242;
    m.created = 999;
    m.first = 500'000;
    m.beat = m.first + ranSeconds;
    m.streak = streak;
    m.removed = removed;
    m.ended = true;
    m.code = code;
    env.exists = true;
    env.text = Serialize(m);
}

static Marker Stored(const FakeEnv& env)
{
    Marker m;
    CHECK(env.exists && Parse(env.text, m));
    return m;
}

// The previous session ran `ran` seconds in a guard, then the process ended itself with `status`: the next launch's
// verdict.
static Verdict NextLaunchAfterEnd(int64_t ran, uint32_t status, bool removed = false)
{
    FakeEnv env;
    Guard<FakeEnv> guard(env);
    guard.Begin(true, true);
    guard.NoteEvaluated();
    env.now += ran;
    if (removed)
        guard.NoteDeviceRemoved();
    guard.NoteProcessEnd(status);
    CHECK(guard.HasEnded());

    Guard<FakeEnv> next(env);
    return next.Begin(true, true);
}

int main()
{
    // Round trip, and every way a marker can be unreadable.
    {
        Marker m;
        m.active = true;
        m.pid = 1;
        m.created = 18446744073709551615ull;
        m.first = 5;
        m.beat = 9;
        m.streak = 3;
        m.blocked = false;
        m.removed = true;
        m.ended = true;
        m.code = 0xC0000005u;
        Marker back;
        CHECK(Parse(Serialize(m), back));
        CHECK(back.active && back.pid == 1 && back.created == m.created && back.first == 5 && back.beat == 9 &&
              back.streak == 3 && !back.blocked && back.removed && back.ended && back.code == 0xC0000005u);

        // The widest marker fits the fixed buffer the end of the process writes from.
        Marker widest;
        widest.active = widest.blocked = widest.removed = widest.ended = true;
        widest.pid = 0xFFFFFFFFu;
        widest.created = 18446744073709551615ull;
        widest.first = widest.beat = 9223372036854775807ll;
        widest.streak = 0xFFFFFFFFu;
        widest.code = 0xFFFFFFFFu;
        char buffer[kMarkerCapacity];
        const size_t size = SerializeTo(buffer, widest);
        CHECK(size > 0 && size < kMarkerCapacity);
        CHECK(Parse(std::string(buffer, size), back) && back.code == 0xFFFFFFFFu);

        const std::string good = Serialize(m);
        Marker junk;
        CHECK(!Parse("", junk));
        CHECK(!Parse("garbage", junk));
        CHECK(!Parse("retoque-nr-session 2\nactive=1\n", junk));
        CHECK(!Parse(good.substr(0, good.size() / 2), junk));  // truncated mid-write
        CHECK(!Parse(std::string(good) + "active=0\n", junk)); // key twice
        CHECK(!Parse(good + "extra\n", junk));                 // not key=value
        CHECK(!Parse(good + "pid=-1\n", junk));
        std::string bad = good;
        bad.replace(bad.find("beat=9"), 6, "beat=4"); // beat before first
        CHECK(!Parse(bad, junk));
        bad = good;
        bad.replace(bad.find("active=1"), 8, "active=7");
        CHECK(!Parse(bad, junk));
        bad = good;
        bad.replace(bad.find("streak=3"), 8, "streak=x");
        CHECK(!Parse(bad, junk));
        bad = good;
        bad.replace(bad.find("code=3221225477"), 15, "code=-5");
        CHECK(!Parse(bad, junk));
        std::string crlf;
        for (char c : good)
            crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        CHECK(Parse(crlf, junk)); // a marker opened and saved in an editor still reads

        // A marker from another build's format: version 1 (no end keys) and a future version are none.
        const std::string v1 = "retoque-nr-session 1\nactive=1\npid=4242\ncreated=999\nfirst=500000\nbeat=500040\n"
                               "streak=0\nblocked=0\n";
        CHECK(!Parse(v1, junk));
        std::string v3 = good;
        v3.replace(0, kVersionLine.size(), "retoque-nr-session 3");
        CHECK(!Parse(v3, junk));

        // The end's consistency rules.
        Marker c;
        c.active = false;
        c.ended = true;
        c.code = 5;
        CHECK(!Parse(Serialize(c), junk)); // an end only on an active marker
        c = {};
        c.active = true;
        c.code = 5;
        CHECK(!Parse(Serialize(c), junk)); // a code only with an end
        c.ended = true;
        c.code = 0;
        CHECK(!Parse(Serialize(c), junk)); // a normal quit deletes the file; it is never recorded
        c.removed = true;
        CHECK(Parse(Serialize(c), junk)); // unless NR saw the device lost first
    }

    // Exit statuses: the severity bits decide.
    {
        CHECK(!IsExceptionStatus(0));
        CHECK(!IsExceptionStatus(3));
        CHECK(!IsExceptionStatus(0x7FFFFFFFu));
        CHECK(IsExceptionStatus(0x80000000u));
        CHECK(IsExceptionStatus(0x80000003u)); // breakpoint
        CHECK(IsExceptionStatus(0xC0000005u)); // access violation
        CHECK(IsExceptionStatus(0xC0000409u)); // fail-fast
        CHECK(IsExceptionStatus(0xE06D7363u)); // unhandled C++ exception
        CHECK(IsExceptionStatus(0xFFFFFFFFu)); // exit(-1): not a normal quit either
    }

    // The probe's answers. A pid nobody holds is dead; any other failure to open is unknown, never a verdict; a
    // finished process kept open by a handle is dead; the pid alive with another creation time was reused (the common
    // case under Proton, where each launch's wineserver starts its pid table over); the same creation time is a live
    // second instance.
    {
        CHECK(ClassifyProbe(false, true, false, false, 0, 999) == Owner::Dead);
        CHECK(ClassifyProbe(false, false, false, false, 0, 999) == Owner::Unknown);
        CHECK(ClassifyProbe(true, false, true, false, 0, 999) == Owner::Dead);
        CHECK(ClassifyProbe(true, false, false, false, 0, 999) == Owner::Unknown);
        CHECK(ClassifyProbe(true, false, false, true, 1000, 999) == Owner::Reused);
        CHECK(ClassifyProbe(true, false, false, true, 999, 999) == Owner::Same);
    }

    // Clean previous session: nothing on disk.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::NoMarker && v.end == End::None);
        CHECK(env.writes == 0 && !env.exists); // reading does not write
    }

    // Clean previous session: a marker that says it ended.
    {
        FakeEnv env;
        Leave(env, 0, 0, false);
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::None);
        CHECK(env.probedPid == 0); // never probes an inactive marker
    }

    // A normal quit (the process ends itself with status 0) deletes the marker, and the next launch says nothing,
    // however short the session was: version 1's false positive.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        CHECK(env.exists && env.watches == 1);
        env.now += 90;
        guard.NoteProcessEnd(0);
        CHECK(!env.exists && env.exitRemoves == 1 && env.exitWrites == 0 && guard.HasEnded());
        guard.NoteOffAtExit(); // the detach fallback, after the hook: nothing more
        guard.NoteProcessEnd(0);
        CHECK(env.exitRemoves == 1);

        Guard<FakeEnv> next(env);
        const auto v = next.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::NoMarker);
    }

    // A normal quit whose delete fails: an inactive marker says it is over, and the next launch says nothing.
    {
        FakeEnv env;
        env.deletable = false;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        guard.NoteProcessEnd(0);
        CHECK(env.exitWrites == 1 && !Stored(env).active && !Stored(env).ended);
        Guard<FakeEnv> next(env);
        CHECK(next.Begin(true, true).action == Action::None);
    }

    // After the end, no thread starts a marker: a render thread still running between the hook and the moment
    // NtTerminateProcess(NULL) stops it.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        guard.NoteProcessEnd(0);
        const int writes = env.writes;
        guard.NoteEvaluated();
        env.now += kBeatSeconds;
        guard.NoteEvaluated();
        guard.NoteDeviceRemoved();
        CHECK(env.writes == writes && !env.exists);

        // Armed but never run (NR turned off before the end): no file touched, and still terminal.
        FakeEnv armed;
        Guard<FakeEnv> g2(armed);
        g2.Begin(true, true);
        g2.NoteProcessEnd(0xC0000005u);
        g2.NoteEvaluated();
        CHECK(!armed.exists && armed.writes == 0 && armed.exitWrites == 0 && armed.exitRemoves == 0 &&
              g2.HasEnded());
    }

    // The process ended itself with an exception: a crash, recorded with its code and the time to the end. Disabled
    // under 10 minutes; a notice after; two in a row disable. The recorded end is never probed.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        env.now += 95;
        guard.NoteProcessEnd(0xC0000005u);
        const Marker stored = Stored(env);
        CHECK(stored.active && stored.ended && stored.code == 0xC0000005u && stored.beat - stored.first == 95);

        Guard<FakeEnv> next(env);
        const auto v = next.Begin(true, true);
        CHECK(v.action == Action::Disable && v.reason == Reason::EarlyEnd && v.end == End::Crashed &&
              v.code == 0xC0000005u && v.ranSeconds == 95 && v.streak == 1);
        CHECK(env.probedPid == 0);
        CHECK(!Stored(env).active && Stored(env).blocked);

        const auto middle = NextLaunchAfterEnd(300, 0xC0000409u);
        CHECK(middle.action == Action::Disable && middle.reason == Reason::MiddleEnd && middle.end == End::Crashed);

        const auto late = NextLaunchAfterEnd(kLongSeconds, 0xC0000005u);
        CHECK(late.action == Action::Notice && late.reason == Reason::LongEnd && late.end == End::Crashed &&
              late.streak == 1);

        FakeEnv twice;
        LeaveEnded(twice, kLongSeconds + 10, 0xC0000005u, false, 1);
        Guard<FakeEnv> g2(twice);
        const auto repeated = g2.Begin(true, true);
        CHECK(repeated.action == Action::Disable && repeated.reason == Reason::RepeatedEnd && repeated.streak == 2);
    }

    // The process ended itself with a nonzero exit code: like "no end seen" -- disabled under 2 minutes, a notice in
    // the middle, nothing after 10 minutes -- but the code is carried.
    {
        const auto early = NextLaunchAfterEnd(30, 3);
        CHECK(early.action == Action::Disable && early.reason == Reason::EarlyEnd && early.end == End::ExitCode &&
              early.code == 3);
        const auto middle = NextLaunchAfterEnd(300, 1);
        CHECK(middle.action == Action::Notice && middle.reason == Reason::MiddleEnd && middle.end == End::ExitCode);
        const auto late = NextLaunchAfterEnd(kLongSeconds, 1);
        CHECK(late.action == Action::None && late.reason == Reason::LongEnd && late.end == End::ExitCode &&
              late.streak == 0);
        const auto edge = NextLaunchAfterEnd(30, 0x7FFFFFFFu);
        CHECK(edge.end == End::ExitCode);
        const auto minus = NextLaunchAfterEnd(30, 0xFFFFFFFFu);
        CHECK(minus.end == End::Crashed);
    }

    // NR saw the device lost: written at once, once; the end that follows keeps it, even a normal quit; the next
    // launch disables under 10 minutes and warns after.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        env.now += 40;
        const int writes = env.writes;
        guard.NoteDeviceRemoved();
        CHECK(env.writes == writes + 1 && Stored(env).removed && Stored(env).active && !Stored(env).ended);
        guard.NoteDeviceRemoved(); // every later evaluate reports it again: one write
        CHECK(env.writes == writes + 1);
        guard.NoteProcessEnd(0);
        CHECK(env.exitRemoves == 0 && Stored(env).ended && Stored(env).removed && Stored(env).code == 0);

        Guard<FakeEnv> next(env);
        const auto v = next.Begin(true, true);
        CHECK(v.action == Action::Disable && v.end == End::DeviceRemoved && v.code == 0 && v.ranSeconds == 40);

        const auto middle = NextLaunchAfterEnd(300, 1, true);
        CHECK(middle.action == Action::Disable && middle.end == End::DeviceRemoved && middle.code == 1);
        const auto late = NextLaunchAfterEnd(kLongSeconds, 0, true);
        CHECK(late.action == Action::Notice && late.reason == Reason::LongEnd && late.end == End::DeviceRemoved);

        // Lost, then hung and killed: no end recorded, so the probe decides it is stale, and the removal names it.
        FakeEnv hung;
        Guard<FakeEnv> g3(hung);
        g3.Begin(true, true);
        g3.NoteEvaluated();
        hung.now += 200;
        g3.NoteDeviceRemoved();
        Guard<FakeEnv> g4(hung);
        const auto killed = g4.Begin(true, true);
        CHECK(hung.probedPid == 7000 && killed.end == End::DeviceRemoved && killed.action == Action::Disable);

        // A removal NR sees with no marker owned (guard armed, NR never succeeded) writes nothing.
        FakeEnv idle;
        Guard<FakeEnv> g5(idle);
        g5.Begin(true, true);
        g5.NoteDeviceRemoved();
        CHECK(idle.writes == 0);
    }

    // No end seen (killed from outside, a hang killed, a kernel-level kill) and the pid is dead: disabled, persisted
    // as blocked, and the pid and creation time were what the probe saw. It trips once: the stale marker is consumed,
    // and only the block, which is the point, is shown again.
    {
        FakeEnv env;
        Leave(env, 40);
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::Disable && v.reason == Reason::EarlyEnd && v.end == End::Vanished &&
              v.ranSeconds == 40 && v.streak == 1);
        CHECK(env.probedPid == 4242 && env.probedCreated == 999);
        const Marker stored = Stored(env);
        CHECK(!stored.active && stored.blocked && stored.streak == 1);
        CHECK(guard.Blocked());

        // The block holds across launches until acknowledged: a second launch is disabled again, nothing rewritten,
        // and no end or numbers come with it.
        FakeEnv second = env;
        const int writes = second.writes;
        Guard<FakeEnv> next(second);
        const auto again = next.Begin(true, true);
        CHECK(again.action == Action::Disable && again.reason == Reason::StillBlocked && again.end == End::None);
        CHECK(second.writes == writes);

        // And NR never starts a marker while blocked and off.
        next.NoteOff();
        CHECK(second.removes == 0);

        // Acknowledged: the launch after that has nothing to say.
        next.Acknowledge();
        Guard<FakeEnv> third(second);
        CHECK(third.Begin(true, true).action == Action::None);
    }

    // A middle notice is said once: the next launch, with no new unclean end, is silent.
    {
        FakeEnv env;
        Leave(env, 300);
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::Notice);
        Guard<FakeEnv> next(env);
        CHECK(next.Begin(true, true).action == Action::None && env.probedPid == 4242);
    }

    // Boundary: exactly kEarlySeconds is middle, one less is early; exactly kLongSeconds is long.
    {
        FakeEnv a;
        Leave(a, kEarlySeconds - 1);
        Guard<FakeEnv> ga(a);
        CHECK(ga.Begin(true, true).reason == Reason::EarlyEnd);

        FakeEnv b;
        Leave(b, kEarlySeconds);
        Guard<FakeEnv> gb(b);
        CHECK(gb.Begin(true, true).reason == Reason::MiddleEnd);

        FakeEnv c;
        Leave(c, kLongSeconds - 1);
        Guard<FakeEnv> gc(c);
        CHECK(gc.Begin(true, true).reason == Reason::MiddleEnd);

        FakeEnv d;
        Leave(d, kLongSeconds);
        Guard<FakeEnv> gd(d);
        CHECK(gd.Begin(true, true).reason == Reason::LongEnd);
    }

    // Unclean late exit with no evidence: long is not blamed, and the strikes reset.
    {
        FakeEnv env;
        Leave(env, 3 * 3600, 1);
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::LongEnd && v.end == End::Vanished);
        const Marker stored = Stored(env);
        CHECK(!stored.active && !stored.blocked && stored.streak == 0);
        CHECK(!guard.Blocked());
    }

    // Middle: a notice, one strike, NR stays on; the next unclean middle one disables.
    {
        FakeEnv env;
        Leave(env, 300);
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::Notice && v.reason == Reason::MiddleEnd && v.streak == 1);
        CHECK(!guard.Blocked());

        // The session runs and is cut again after 5 more minutes: the streak was carried into the new marker.
        guard.NoteEvaluated();
        CHECK(Stored(env).streak == 1 && Stored(env).active);
        env.now += 300;
        guard.NoteEvaluated();
        Marker m = Stored(env);
        env.probeAnswer = Owner::Dead;
        m.pid = 4242; // as the next launch would see it
        env.text = Serialize(m);

        Guard<FakeEnv> next(env);
        const auto second = next.Begin(true, true);
        CHECK(second.action == Action::Disable && second.reason == Reason::RepeatedEnd && second.streak == 2);
        CHECK(Stored(env).blocked);
    }

    // DllMain detach, the fallback for an exit the hook did not see: removes the marker like an orderly NoteOff, and
    // does nothing when the guard never ran.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::None);
        guard.NoteOffAtExit(); // armed, never evaluated: no marker to remove
        CHECK(env.exitRemoves == 0);
        guard.NoteEvaluated();
        CHECK(env.exists);
        guard.NoteOffAtExit();
        CHECK(!env.exists && env.exitRemoves == 1);
        guard.NoteOffAtExit(); // already armed again: nothing more
        CHECK(env.exitRemoves == 1);
    }

    // The end finds the lock held: a heartbeat write on another thread, blocked (in ExitProcess, on the PEB lock the
    // ending thread holds). It retries a few times, a pause apart, then gives up: nothing written, never a hung exit.
    // The marker stays as it was, "no end seen", and no later frame starts another.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        std::atomic<bool> hold { true };
        std::atomic<bool> entered { false };
        env.holdWrite = &hold;
        env.inWrite = &entered;
        env.now += kBeatSeconds;
        std::thread beat([&] { guard.NoteEvaluated(); });
        while (!entered.load())
            std::this_thread::yield();
        guard.NoteProcessEnd(0);
        CHECK(env.pauses == kEndLockTries - 1 && env.exitRemoves == 0 && env.exitWrites == 0 && guard.HasEnded());
        hold = false;
        beat.join();
        env.holdWrite = nullptr;
        env.inWrite = nullptr;
        CHECK(Stored(env).active && !Stored(env).ended);
        const int writes = env.writes;
        env.now += kBeatSeconds;
        guard.NoteEvaluated();
        CHECK(env.writes == writes);
    }

    // Two consecutive unclean sessions where the first was clean in between do not add up.
    {
        FakeEnv env;
        Leave(env, 300);
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::Notice);
        guard.NoteEvaluated();
        guard.NoteOff(); // ended in order
        CHECK(!env.exists);

        Leave(env, 300); // a later unclean one starts from zero
        Guard<FakeEnv> next(env);
        CHECK(next.Begin(true, true).action == Action::Notice);
    }

    // Marker from a live second instance: same pid, same creation time. Left alone, nothing written, ever, not even
    // when this process ends.
    {
        FakeEnv env;
        Leave(env, 10);
        env.probeAnswer = Owner::Same;
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::LiveInstance);
        guard.NoteEvaluated();
        guard.NoteDeviceRemoved();
        guard.NoteOff();
        guard.NoteProcessEnd(0xC0000005u);
        CHECK(env.writes == 0 && env.removes == 0 && env.exitWrites == 0 && env.exitRemoves == 0 &&
              env.watches == 0 && env.text == before);
    }

    // The probe cannot tell: same as live, never a verdict.
    {
        FakeEnv env;
        Leave(env, 10);
        env.probeAnswer = Owner::Unknown;
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).reason == Reason::LiveInstance);
        CHECK(env.writes == 0);
    }

    // The pid was reused by another process (different creation time): the marker is stale.
    {
        FakeEnv env;
        Leave(env, 10);
        env.probeAnswer = Owner::Reused;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::Disable && v.reason == Reason::EarlyEnd && v.end == End::Vanished);
    }

    // Unreadable or corrupt marker: no verdict, and the session still gets a fresh marker.
    {
        FakeEnv env;
        env.exists = true;
        env.text = "retoque-nr-session 2\nactive=1\npid=4";
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::Unreadable);
        CHECK(env.probedPid == 0);
        guard.NoteEvaluated();
        CHECK(Stored(env).active && Stored(env).pid == 7000 && Stored(env).created == 555);
    }

    // Read-only directory with a stale marker: the verdict cannot be persisted, so it is not applied. Otherwise
    // the same stale marker would disable NR on every launch.
    {
        FakeEnv env;
        Leave(env, 10);
        env.writable = false;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::CannotPersist);
        CHECK(!guard.Blocked());
        guard.NoteEvaluated(); // the write fails: no throw, no state, no retry storm
        guard.NoteEvaluated();
        CHECK(!guard.OwnsMarker() && env.writes == 0);
        guard.NoteOff();
        guard.NoteProcessEnd(0xC0000005u);
        CHECK(env.removes == 0 && env.exitWrites == 0);
    }

    // Read-only directory, no marker: the first write fails and the guard goes quiet, through the end too.
    {
        FakeEnv env;
        env.writable = false;
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::None);
        guard.NoteEvaluated();
        CHECK(!guard.OwnsMarker() && !env.exists);
        env.writable = true; // it never tries again this session
        guard.NoteEvaluated();
        CHECK(!env.exists);
        guard.NoteProcessEnd(3);
        CHECK(!env.exists && env.exitWrites == 0);
    }

    // A marker the end cannot write (the folder turned read-only mid-session): swallowed, the old marker stays.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        env.writable = false;
        guard.NoteProcessEnd(0xC0000005u);
        CHECK(env.exitWrites == 1 && Stored(env).active && !Stored(env).ended);
    }

    // The exit watch cannot be put in place: no marker is ever written (a marker nobody can clear at a normal quit is
    // version 1's false positive), and it is not asked again.
    {
        FakeEnv env;
        env.watchable = false;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        guard.NoteEvaluated();
        CHECK(env.watches == 1 && env.writes == 0 && !env.exists && !guard.OwnsMarker());
        guard.NoteProcessEnd(0xC0000005u);
        CHECK(env.exitWrites == 0);
    }

    // The watch is asked once per marker started, never on a heartbeat.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        for (int i = 0; i < 5; ++i)
        {
            env.now += kBeatSeconds;
            guard.NoteEvaluated();
        }
        CHECK(env.watches == 1);
        guard.NoteOff();
        guard.NoteEvaluated(); // a new run after a toggle starts a new marker
        CHECK(env.watches == 2);
    }

    // Guard disabled by config: nothing read, written or removed, no watch, and an old marker is left as it is.
    {
        FakeEnv env;
        Leave(env, 10);
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(false, true);
        CHECK(v.action == Action::None && v.reason == Reason::GuardOff);
        guard.NoteEvaluated();
        guard.NoteDeviceRemoved();
        guard.NoteOff();
        guard.NoteProcessEnd(0xC0000005u);
        CHECK(env.writes == 0 && env.removes == 0 && env.exitWrites == 0 && env.exitRemoves == 0 && env.watches == 0 &&
              env.text == before && env.probedPid == 0);
    }

    // NR off at start: no read, no file, no watch, no notice; nothing happens while it stays off, the end included.
    {
        FakeEnv env;
        Leave(env, 10);
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, false);
        CHECK(v.action == Action::None && v.reason == Reason::NrOff);
        guard.NoteOff();
        CHECK(env.writes == 0 && env.removes == 0 && env.text == before && env.probedPid == 0 && env.watches == 0);

        FakeEnv empty;
        Guard<FakeEnv> quiet(empty);
        quiet.Begin(true, false);
        quiet.NoteOff();
        quiet.NoteDeviceRemoved();
        quiet.NoteProcessEnd(0xC0000005u);
        CHECK(!empty.exists && empty.writes == 0 && empty.exitWrites == 0 && empty.exitRemoves == 0 &&
              empty.watches == 0);

        // Turned on later: the first success starts the watch and a marker, with no verdict on the older one.
        guard.NoteEvaluated();
        CHECK(Stored(env).active && Stored(env).pid == 7000 && Stored(env).streak == 0 && env.watches == 1);
        CHECK(guard.Last().action == Action::None);
    }

    // The marker's life: first success, heartbeat throttling, clean off.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        CHECK(!guard.OwnsMarker());
        guard.NoteEvaluated();
        CHECK(guard.OwnsMarker() && env.writes == 1);
        CHECK(Stored(env).first == env.now && Stored(env).beat == env.now);

        for (int i = 0; i < 1000; ++i) // every frame for under kBeatSeconds: no further write
            guard.NoteEvaluated();
        CHECK(env.writes == 1);

        env.now += kBeatSeconds - 1;
        guard.NoteEvaluated();
        CHECK(env.writes == 1);
        env.now += 1;
        guard.NoteEvaluated();
        CHECK(env.writes == 2 && Stored(env).beat == env.now && Stored(env).first == env.now - kBeatSeconds);

        guard.NoteOff();
        CHECK(!env.exists && env.removes == 1 && !guard.OwnsMarker());
        guard.NoteOff(); // the exit path calls it again
        CHECK(env.removes == 1);
    }

    // NR toggled off and on in one session: the second run is a new marker, not a continuation, and a removal noted
    // in the first run does not carry over.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        guard.NoteDeviceRemoved();
        env.now += 500;
        guard.NoteOff();
        env.now += 500;
        guard.NoteEvaluated();
        CHECK(Stored(env).first == env.now && Stored(env).streak == 0 && !Stored(env).removed);
        guard.NoteDeviceRemoved(); // and it can be noted again in the new run
        CHECK(Stored(env).removed);
    }

    // Enable anyway: clears the block and the strikes, and the marker file.
    {
        FakeEnv env;
        Leave(env, 20);
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::Disable);
        guard.Acknowledge();
        CHECK(!guard.Blocked() && guard.Last().action == Action::None && !env.exists);
        guard.NoteEvaluated();
        CHECK(Stored(env).active && Stored(env).streak == 0 && !Stored(env).blocked);

        // And enabling it without the button is the same statement.
        FakeEnv other;
        Leave(other, 20);
        Guard<FakeEnv> g2(other);
        CHECK(g2.Begin(true, true).action == Action::Disable);
        g2.NoteEvaluated();
        CHECK(!g2.Blocked() && Stored(other).active && Stored(other).streak == 0);
    }

    // Acknowledge without a block does nothing.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.Acknowledge();
        CHECK(env.removes == 0 && env.writes == 0);
    }

    // The exact words: the menu's reason line for each verdict the policy can produce, and the log's account of the
    // end. A verdict that shows nothing has no reason line.
    {
        const auto reason = [](End end, Reason why)
        {
            Verdict v;
            v.end = end;
            v.reason = why;
            return std::string(ReasonText(v));
        };
        CHECK(reason(End::Vanished, Reason::EarlyEnd) ==
              "The last session ended without closing, soon after the model started.");
        CHECK(reason(End::Vanished, Reason::MiddleEnd) ==
              "The last session ended without closing. If it was a crash, this one may too.");
        CHECK(reason(End::Vanished, Reason::LongEnd).empty());
        CHECK(reason(End::ExitCode, Reason::EarlyEnd) ==
              "The last session ended with an error soon after the model started.");
        CHECK(reason(End::ExitCode, Reason::MiddleEnd) ==
              "The last session ended with an error. If it was a crash, this one may too.");
        CHECK(reason(End::ExitCode, Reason::LongEnd).empty());
        CHECK(reason(End::Crashed, Reason::EarlyEnd) == "The last session crashed while the model was running.");
        CHECK(reason(End::Crashed, Reason::MiddleEnd) == "The last session crashed while the model was running.");
        CHECK(reason(End::Crashed, Reason::LongEnd) ==
              "The last session crashed, long after the model started. If the next one does too, Neural Rendering "
              "will be switched off.");
        CHECK(reason(End::DeviceRemoved, Reason::EarlyEnd) ==
              "The graphics device was lost while the model was running in the last session.");
        CHECK(reason(End::DeviceRemoved, Reason::LongEnd) ==
              "The graphics device was lost in the last session, long after the model started. If it happens again, "
              "Neural Rendering will be switched off.");
        for (End end : { End::Vanished, End::ExitCode, End::Crashed, End::DeviceRemoved })
            CHECK(reason(end, Reason::RepeatedEnd) == "The last two sessions ended without closing.");
        CHECK(reason(End::None, Reason::StillBlocked) ==
              "An earlier session ended without closing, and this has not been confirmed since.");
        CHECK(reason(End::None, Reason::NoMarker).empty());

        // Every verdict with an action has a reason line to show.
        for (int64_t ran : { 30, 300, 900 })
            for (uint32_t status : { 0u, 3u, 0xC0000005u })
                for (bool removed : { false, true })
                {
                    if (status == 0 && !removed)
                        continue; // a normal quit: no verdict
                    const auto v = NextLaunchAfterEnd(ran, status, removed);
                    CHECK(v.action == Action::None || *ReasonText(v) != '\0');
                }

        Verdict v;
        v.end = End::Vanished;
        CHECK(EndText(v) == "no end was seen: stopped from outside, stopped responding, or killed");
        v.end = End::ExitCode;
        v.code = 3;
        CHECK(EndText(v) == "it ended itself with exit code 3");
        v.end = End::Crashed;
        v.code = 0xC0000005u;
        CHECK(EndText(v) == "it ended with exception 0xC0000005");
        v.end = End::DeviceRemoved;
        v.code = 0;
        CHECK(EndText(v) == "NR saw the graphics device lost");
        v.code = 1;
        CHECK(EndText(v) == "NR saw the graphics device lost; it then ended with status 0x00000001");
        v.end = End::None;
        CHECK(EndText(v).empty());
    }

    std::puts("PASS: session guard policy table");
    return 0;
}
