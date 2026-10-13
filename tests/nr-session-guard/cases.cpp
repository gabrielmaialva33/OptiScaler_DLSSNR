#include <dlssnr/DlssNr_SessionGuard.h>

#include <cstdio>
#include <cstdlib>
#include <string>

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

// One directory entry, one clock, one process table.
struct FakeEnv
{
    bool exists = false;
    std::string text;
    bool writable = true;
    int writes = 0;
    int removes = 0;
    Owner probeAnswer = Owner::Dead;
    uint32_t probedPid = 0;
    uint64_t probedCreated = 0;
    uint32_t selfPid = 7000;
    uint64_t selfCreated = 555;
    int64_t now = 1'000'000;

    bool Read(std::string& out)
    {
        if (!exists)
            return false;
        out = text;
        return true;
    }
    bool Write(const std::string& t)
    {
        if (!writable)
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
    void RemoveAtExit() { Remove(); }
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

static Marker Stored(const FakeEnv& env)
{
    Marker m;
    CHECK(env.exists && Parse(env.text, m));
    return m;
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
        Marker back;
        CHECK(Parse(Serialize(m), back));
        CHECK(back.active && back.pid == 1 && back.created == m.created && back.first == 5 && back.beat == 9 &&
              back.streak == 3 && !back.blocked);

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
        std::string crlf;
        for (char c : good)
            crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
        CHECK(Parse(crlf, junk)); // a marker opened and saved in an editor still reads
    }

    // Clean previous session: nothing on disk.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::NoMarker);
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

    // Unclean early crash: disabled, persisted as blocked, and the pid and creation time were what the probe saw.
    {
        FakeEnv env;
        Leave(env, 40);
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::Disable && v.reason == Reason::EarlyEnd && v.ranSeconds == 40 && v.streak == 1);
        CHECK(env.probedPid == 4242 && env.probedCreated == 999);
        const Marker stored = Stored(env);
        CHECK(!stored.active && stored.blocked && stored.streak == 1);
        CHECK(guard.Blocked());

        // The block holds across launches until acknowledged: a second launch is disabled again, nothing rewritten.
        FakeEnv second = env;
        const int writes = second.writes;
        Guard<FakeEnv> next(second);
        const auto again = next.Begin(true, true);
        CHECK(again.action == Action::Disable && again.reason == Reason::StillBlocked);
        CHECK(second.writes == writes);

        // And NR never starts a marker while blocked and off.
        next.NoteOff();
        CHECK(second.removes == 0);
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

    // Unclean late exit: long is not blamed, and the strikes reset.
    {
        FakeEnv env;
        Leave(env, 3 * 3600, 1);
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::LongEnd);
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

    // Process exit (DllMain detach): the at-exit form removes the marker like an orderly NoteOff, and does nothing when
    // the guard never ran.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        CHECK(guard.Begin(true, true).action == Action::None);
        guard.NoteOffAtExit(); // armed, never evaluated: no marker to remove
        CHECK(env.removes == 0);
        guard.NoteEvaluated();
        CHECK(env.exists);
        guard.NoteOffAtExit();
        CHECK(!env.exists && env.removes == 1);
        guard.NoteOffAtExit(); // already armed again: nothing more
        CHECK(env.removes == 1);
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

    // Marker from a live second instance: same pid, same creation time. Left alone, nothing written, ever.
    {
        FakeEnv env;
        Leave(env, 10);
        env.probeAnswer = Owner::Same;
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, true);
        CHECK(v.action == Action::None && v.reason == Reason::LiveInstance);
        guard.NoteEvaluated();
        guard.NoteOff();
        CHECK(env.writes == 0 && env.removes == 0 && env.text == before);
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
        CHECK(v.action == Action::Disable && v.reason == Reason::EarlyEnd);
    }

    // Unreadable or corrupt marker: no verdict, and the session still gets a fresh marker.
    {
        FakeEnv env;
        env.exists = true;
        env.text = "retoque-nr-session 1\nactive=1\npid=4";
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
        CHECK(env.removes == 0);
    }

    // Read-only directory, no marker: the first write fails and the guard goes quiet.
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
    }

    // Guard disabled by config: nothing read, written or removed, and an old marker is left as it is.
    {
        FakeEnv env;
        Leave(env, 10);
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(false, true);
        CHECK(v.action == Action::None && v.reason == Reason::GuardOff);
        guard.NoteEvaluated();
        guard.NoteOff();
        CHECK(env.writes == 0 && env.removes == 0 && env.text == before && env.probedPid == 0);
    }

    // NR off at start: no read, no file, no notice; nothing happens while it stays off.
    {
        FakeEnv env;
        Leave(env, 10);
        const std::string before = env.text;
        Guard<FakeEnv> guard(env);
        const auto v = guard.Begin(true, false);
        CHECK(v.action == Action::None && v.reason == Reason::NrOff);
        guard.NoteOff();
        CHECK(env.writes == 0 && env.removes == 0 && env.text == before && env.probedPid == 0);

        FakeEnv empty;
        Guard<FakeEnv> quiet(empty);
        quiet.Begin(true, false);
        quiet.NoteOff();
        CHECK(!empty.exists && empty.writes == 0);

        // Turned on later: the first success starts a marker, with no verdict on the older one.
        guard.NoteEvaluated();
        CHECK(Stored(env).active && Stored(env).pid == 7000 && Stored(env).streak == 0);
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
        {
            env.now += 0;
            guard.NoteEvaluated();
        }
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

    // NR toggled off and on in one session: the second run is a new marker, not a continuation.
    {
        FakeEnv env;
        Guard<FakeEnv> guard(env);
        guard.Begin(true, true);
        guard.NoteEvaluated();
        env.now += 500;
        guard.NoteOff();
        env.now += 500;
        guard.NoteEvaluated();
        CHECK(Stored(env).first == env.now && Stored(env).streak == 0);
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

    std::puts("PASS: session guard policy table");
    return 0;
}
