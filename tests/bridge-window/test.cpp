// The window takeover's device-free half: retoque/with_dx12/Dx11wDx12_Window.h, compiled as production builds it.
#include <with_dx12/Dx11wDx12_Window.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Dx11wDx12::Window;

namespace
{

constexpr int32_t kOk = 0;
constexpr int32_t kFail = static_cast<int32_t>(0x80004005u);             // E_FAIL
constexpr int32_t kNoInterface = static_cast<int32_t>(0x80004002u);      // E_NOINTERFACE, what FG answered in the log
constexpr int32_t kInvalidCall = static_cast<int32_t>(0x887A0001u);      // DXGI_ERROR_INVALID_CALL
constexpr int32_t kDxgiAccessDenied = static_cast<int32_t>(0x887A002Bu); // DXGI_ERROR_ACCESS_DENIED: another code

void* const kWindow = reinterpret_cast<void*>(0x150888);
void* const kOtherWindow = reinterpret_cast<void*>(0x2a0b1c);

// A bridge as the takeover sees it: a COM count, a gate, and whether it still has its presenter.
struct Bridge
{
    int refs = 1;
    PresenterGate gate;
    bool presenter = true;
    int releases = 0;
};

// What Dx11wDx12SC::_AcquireForTakeover does: passes over a bridge that has given its presenter up, never revives a
// count at zero.
bool Acquire(Bridge* bridge)
{
    if (bridge->gate.Closed() || bridge->refs <= 0)
        return false;
    ++bridge->refs;
    return true;
}

void Release(Bridge* bridge)
{
    assert(bridge->refs > 0 && "released a reference nobody took");
    --bridge->refs;
    ++bridge->releases;
}

void registryCases()
{
    Registry<Bridge> registry;
    Bridge first, second, elsewhere;

    // Nothing registered, and the null window and bridge are not entries.
    assert(registry.Find(kWindow, Acquire) == nullptr);
    registry.Add(nullptr, &first);
    registry.Add(kWindow, nullptr);
    assert(registry.Count(nullptr) == 0 && registry.Count(kWindow) == 0);
    assert(registry.Find(nullptr, Acquire) == nullptr);

    registry.Add(kWindow, &first);
    registry.Add(kOtherWindow, &elsewhere);
    assert(registry.Count(kWindow) == 1 && registry.Count(kOtherWindow) == 1);

    // Found by its window and nothing else, with the reference acquire took.
    assert(registry.Find(kWindow, Acquire) == &first && first.refs == 2);
    Release(&first);
    assert(elsewhere.refs == 1);

    // The newest on the window first.
    registry.Add(kWindow, &second);
    assert(registry.Find(kWindow, Acquire) == &second && second.refs == 2 && first.refs == 1);
    Release(&second);

    // The newest gave its presenter up: the next newest that still presents is the one found.
    assert(second.gate.TryClose());
    assert(registry.Find(kWindow, Acquire) == &first && second.refs == 1);
    Release(&first);

    // A count at zero (on its way out, or quarantined) is never revived, and the search goes on past it.
    first.refs = 0;
    assert(registry.Find(kWindow, Acquire) == nullptr);
    assert(first.refs == 0 && second.refs == 1);
    first.refs = 1;

    // Remove takes every entry of that bridge, and only of that bridge.
    registry.Add(kWindow, &first);
    assert(registry.Count(kWindow) == 3);
    registry.Remove(&first);
    assert(registry.Count(kWindow) == 1 && registry.Count(kOtherWindow) == 1);
    registry.Remove(&second);
    registry.Remove(&second);
    assert(registry.Count(kWindow) == 0);
    assert(registry.Find(kWindow, Acquire) == nullptr);
    registry.Remove(&elsewhere);
    assert(registry.Count(kOtherWindow) == 0);
}

// acquire runs under the lock Remove takes, which is what keeps the bridge it is handed alive while it decides: the
// destructor removes the bridge first, so it cannot free that memory until acquire has returned.
void registryLockCase()
{
    Registry<Bridge> registry;
    Bridge bridge;
    registry.Add(kWindow, &bridge);

    std::atomic<bool> inside { false }, decided { false }, removed { false };
    std::thread finder(
        [&]()
        {
            registry.Find(kWindow,
                          [&](Bridge* candidate)
                          {
                              inside = true;
                              std::this_thread::sleep_for(std::chrono::milliseconds(50));
                              decided = true;
                              return Acquire(candidate);
                          });
        });

    while (!inside)
        std::this_thread::yield();

    registry.Remove(&bridge);
    removed = true;
    // Remove returned only once acquire had: it waited on the same lock.
    assert(decided);
    finder.join();
    assert(removed && registry.Count(kWindow) == 0);
    Release(&bridge);
}

void gateCases()
{
    PresenterGate gate;
    assert(!gate.Closed());

    // The game's calls nest and overlap; a takeover is refused while any is in flight.
    {
        PresenterGate::Use present(gate);
        assert(present);
        {
            PresenterGate::Use resize(gate);
            assert(resize);
            assert(!gate.TryClose());
        }
        assert(!gate.TryClose());
    }

    // None in flight: closed, and every call after it is refused.
    assert(gate.TryClose());
    assert(gate.Closed());
    assert(!gate.TryClose());
    {
        PresenterGate::Use present(gate);
        assert(!present);
    }
    // A refused Use leaves nothing behind for its destructor to take back.
    assert(gate.Closed());

    // A takeover that could not finish gives the presenter back.
    gate.Reopen();
    assert(!gate.Closed());
    {
        PresenterGate::Use present(gate);
        assert(present);
    }

    // Reopen never opens a gate a call holds, nor counts below zero.
    {
        PresenterGate::Use present(gate);
        gate.Reopen();
        assert(!gate.TryClose());
    }
    assert(gate.TryClose());
}

// Many of the game's calls on two threads, a takeover trying on a third: when it closes the gate, no call is inside,
// and none gets in after.
void gateRaceCase()
{
    PresenterGate gate;
    std::atomic<int> inside { 0 };
    std::atomic<bool> closed { false }, violation { false };

    const auto caller = [&]()
    {
        for (int i = 0; i < 200000; ++i)
        {
            PresenterGate::Use use(gate);
            if (!use)
                continue;
            if (closed.load())
                violation = true;
            inside.fetch_add(1);
            inside.fetch_sub(1);
        }
    };

    std::thread a(caller), b(caller);
    std::thread taker(
        [&]()
        {
            while (!gate.TryClose())
                std::this_thread::yield();
            if (inside.load() != 0)
                violation = true;
            closed = true;
        });

    a.join();
    b.join();
    taker.join();
    assert(closed && gate.Closed() && !violation);
}

struct Calls
{
    std::vector<std::string> order;
    int surrenders = 0, creates = 0;
};

void sequenceCases()
{
    // Anything but E_ACCESSDENIED is handled as it always was: nothing found, asked, released or created.
    for (const int32_t result : { kOk, kFail, kNoInterface, kInvalidCall, kDxgiAccessDenied })
    {
        Registry<Bridge> registry;
        Bridge bridge;
        registry.Add(kWindow, &bridge);
        Calls calls;
        const auto attempt = TakeOverAndRetry(
            result, registry, kWindow, Acquire,
            [&](Bridge*)
            {
                ++calls.surrenders;
                return Surrender::Surrendered;
            },
            Release,
            [&]()
            {
                ++calls.creates;
                return kOk;
            });
        assert(attempt.outcome == Outcome::NotApplicable && attempt.result == result);
        assert(calls.surrenders == 0 && calls.creates == 0 && bridge.refs == 1 && bridge.releases == 0);
    }

    // E_ACCESSDENIED with no bridge of ours on that window: nothing to take over, and nothing created again.
    {
        Registry<Bridge> registry;
        Bridge elsewhere;
        registry.Add(kOtherWindow, &elsewhere);
        int creates = 0;
        const auto attempt = TakeOverAndRetry(
            kAccessDenied, registry, kWindow, Acquire, [](Bridge*) { return Surrender::Surrendered; }, Release,
            [&]()
            {
                ++creates;
                return kOk;
            });
        assert(attempt.outcome == Outcome::NoBridge && attempt.result == kAccessDenied);
        assert(creates == 0 && elsewhere.refs == 1);
    }

    // Each refusal: the reference taken is dropped once, nothing is created again, and the result is the original.
    const std::pair<Surrender, Outcome> refusals[] = { { Surrender::Busy, Outcome::Busy },
                                                       { Surrender::FrameGeneration, Outcome::FrameGeneration },
                                                       { Surrender::Undrained, Outcome::Undrained } };
    for (const auto& [answer, outcome] : refusals)
    {
        Registry<Bridge> registry;
        Bridge bridge;
        registry.Add(kWindow, &bridge);
        int creates = 0;
        const auto attempt = TakeOverAndRetry(
            kAccessDenied, registry, kWindow, Acquire,
            [&](Bridge* found)
            {
                assert(found == &bridge && found->refs == 2 && "asked without a reference held");
                return answer;
            },
            Release,
            [&]()
            {
                ++creates;
                return kOk;
            });
        assert(attempt.outcome == outcome && attempt.result == kAccessDenied);
        assert(creates == 0 && bridge.refs == 1 && bridge.releases == 1);
    }

    // Taken over: asked once, released once and before the creation is made again, created once, and the result is
    // the retry's, success or not.
    for (const int32_t retry : { kOk, kAccessDenied, kFail })
    {
        Registry<Bridge> registry;
        Bridge bridge;
        registry.Add(kWindow, &bridge);
        Calls calls;
        const auto attempt = TakeOverAndRetry(
            kAccessDenied, registry, kWindow, Acquire,
            [&](Bridge* found)
            {
                calls.order.push_back("surrender");
                ++calls.surrenders;
                assert(found->gate.TryClose());
                found->presenter = false;
                return Surrender::Surrendered;
            },
            [&](Bridge* found)
            {
                calls.order.push_back("release");
                Release(found);
            },
            [&]()
            {
                calls.order.push_back("create");
                ++calls.creates;
                return retry;
            });
        assert(attempt.outcome == Outcome::TakenOver && attempt.result == retry);
        assert((calls.order == std::vector<std::string> { "surrender", "release", "create" }));
        assert(bridge.refs == 1 && bridge.releases == 1 && !bridge.presenter && bridge.gate.Closed());

        // A third swapchain on the window finds nothing left to take: the bridge that gave its presenter up is
        // passed over.
        const auto again = TakeOverAndRetry(
            kAccessDenied, registry, kWindow, Acquire, [](Bridge*) { return Surrender::Surrendered; }, Release,
            [&]()
            {
                ++calls.creates;
                return kOk;
            });
        assert(again.outcome == Outcome::NoBridge && calls.creates == 1 && bridge.refs == 1);
    }

    // A count at zero on the window -- a wrapper quarantined after an unproven teardown, whose presenter may still be
    // bound -- is not taken.
    {
        Registry<Bridge> registry;
        Bridge quarantined;
        quarantined.refs = 0;
        registry.Add(kWindow, &quarantined);
        const auto attempt = TakeOverAndRetry(
            kAccessDenied, registry, kWindow, Acquire, [](Bridge*) { return Surrender::Surrendered; }, Release,
            []() { return kOk; });
        assert(attempt.outcome == Outcome::NoBridge && quarantined.refs == 0 && quarantined.releases == 0);
    }

    // Every outcome says something of its own for the log.
    std::set<std::string> lines;
    for (const auto outcome : { Outcome::NotApplicable, Outcome::NoBridge, Outcome::Busy, Outcome::FrameGeneration,
                                Outcome::Undrained, Outcome::TakenOver })
    {
        const char* line = Describe(outcome);
        assert(line != nullptr && *line != '\0');
        lines.insert(line);
    }
    assert(lines.size() == 6);
    assert(kAccessDenied == static_cast<int32_t>(0x80070005u));
}

} // namespace

int main()
{
    registryCases();
    registryLockCase();
    gateCases();
    gateRaceCase();
    sequenceCases();
    std::puts("bridge window: registry, its lock, the presenter gate, its race and the takeover sequence passed");
    return 0;
}
