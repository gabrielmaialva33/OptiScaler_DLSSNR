#pragma once

// A second swapchain on a window the D3D11-to-D3D12 bridge already presents to.
//
// The bridge puts the game's D3D11 swapchain on a hidden window and shows its frames through a D3D12
// flip-model swapchain of its own, the presenter, in the game's window. DXGI allows one flip-model
// swapchain per window. A game that created a second swapchain on its window before releasing the first,
// which the blt model it was written for allows, had the second presenter refused with E_ACCESSDENIED while
// the first one lived, and fell back to a plain D3D11 swapchain with no bridge, and so no neural pass, for the
// rest of the session (Metal: Hellsinger on Rafael's RTX 3060 under Windows, 2026-10-13). Proton does not
// refuse it, so none of the D3D11 titles run here ever showed it.
//
// So the new swapchain takes the window over: the bridge still presenting to it gives its presenter up, behind
// the same drain a release makes first, and the creation is made once more. The old wrapper stays a valid COM
// object for the game, which usually releases it right after, and shows nothing more.
// docs/dlssnr/design/nr-dx11-bridge-host.md, "A second swapchain on the same window".
//
// The pieces that need no device, so tests/bridge-window can run them: who presents to which window, the gate
// that keeps a takeover and the game's own calls on that wrapper apart, and the order of the steps.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Dx11wDx12::Window
{

// What CreateSwapChainForHwnd returns for a second flip-model swapchain on a window, spelled here so the header
// needs no Windows header.
inline constexpr int32_t kAccessDenied = static_cast<int32_t>(0x80070005u);

// One bridge's presenter, as the game's calls on that wrapper and a takeover see it. Neither ever waits for the
// other: a call in flight means no takeover, and once a takeover holds the presenter every call finds it gone.
// Waiting either way could deadlock: the game's Present can wait on its window thread inside DXGI, and the
// takeover runs inside a swapchain creation that may be on that thread.
class PresenterGate
{
  public:
    // One of the game's calls, for as long as it lasts. False: the presenter is gone, or going.
    class Use
    {
      public:
        explicit Use(PresenterGate& gate) : _gate(gate), _entered(gate._Enter()) {}
        ~Use()
        {
            if (_entered)
                _gate._Leave();
        }

        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;

        explicit operator bool() const { return _entered; }

      private:
        PresenterGate& _gate;
        bool _entered;
    };

    // The takeover's half: closed only when no call is in flight. False when one is, or when it already is.
    bool TryClose()
    {
        int expected = 0;
        return _state.compare_exchange_strong(expected, kClosed);
    }

    // A takeover that could not finish gives the presenter back.
    void Reopen()
    {
        int expected = kClosed;
        _state.compare_exchange_strong(expected, 0);
    }

    bool Closed() const { return _state.load() == kClosed; }

  private:
    static constexpr int kClosed = -1;

    bool _Enter()
    {
        int state = _state.load();
        while (state != kClosed)
        {
            if (_state.compare_exchange_weak(state, state + 1))
                return true;
        }
        return false;
    }

    void _Leave() { _state.fetch_sub(1); }

    // Calls in flight, or kClosed.
    std::atomic<int> _state { 0 };
};

// Live bridges by the window they present to, in creation order.
//
// Find hands out a bridge only through acquire, which runs under the registry's lock. Remove takes the same lock,
// and a bridge removes itself before its memory goes, so the bridge acquire is handed cannot be freed while it
// decides. It must take its reference there -- one only if the count is not already zero, since a count at zero
// is a wrapper on its way out or quarantined -- and answer false for a bridge it will not take.
template <typename Bridge> class Registry
{
  public:
    void Add(void* window, Bridge* bridge)
    {
        if (window == nullptr || bridge == nullptr)
            return;

        std::lock_guard lock(_mutex);
        _entries.push_back({ window, bridge });
    }

    void Remove(Bridge* bridge)
    {
        std::lock_guard lock(_mutex);
        for (auto it = _entries.begin(); it != _entries.end();)
        {
            if (it->bridge == bridge)
                it = _entries.erase(it);
            else
                ++it;
        }
    }

    // The newest bridge on the window that acquire accepts, holding the reference acquire took; null if none.
    template <typename Acquire> Bridge* Find(void* window, Acquire&& acquire)
    {
        if (window == nullptr)
            return nullptr;

        std::lock_guard lock(_mutex);
        for (auto it = _entries.rbegin(); it != _entries.rend(); ++it)
        {
            if (it->window == window && acquire(it->bridge))
                return it->bridge;
        }
        return nullptr;
    }

    size_t Count(void* window) const
    {
        std::lock_guard lock(_mutex);
        size_t count = 0;
        for (const auto& entry : _entries)
            count += entry.window == window ? 1 : 0;
        return count;
    }

  private:
    struct Entry
    {
        void* window;
        Bridge* bridge;
    };

    mutable std::mutex _mutex;
    std::vector<Entry> _entries;
};

// What the bridge on the window answered.
enum class Surrender
{
    Surrendered,     // presenter given up; nothing of this wrapper presents again
    Busy,            // a call of the game's was in flight on it; nothing changed
    FrameGeneration, // it presents through frame generation's swapchain, which its backend owns; nothing changed
    Undrained,       // its GPU work could not be proved finished, so nothing was freed; nothing changed
};

enum class Outcome
{
    NotApplicable,   // the creation did not fail with E_ACCESSDENIED: handled as it always was
    NoBridge,        // E_ACCESSDENIED, and no live bridge of ours presents to the window
    Busy,            // see Surrender
    FrameGeneration, // see Surrender
    Undrained,       // see Surrender
    TakenOver,       // the window was taken over and the creation made once more; result is that attempt's
};

inline const char* Describe(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::NotApplicable:
        return "not a second swapchain on the window";
    case Outcome::NoBridge:
        return "no live bridge of ours presents to that window, so there is nothing to take over";
    case Outcome::Busy:
        return "the bridge there was inside a call of the game's on another thread";
    case Outcome::FrameGeneration:
        return "the bridge there presents through frame generation's swapchain, which is not taken over";
    case Outcome::Undrained:
        return "the bridge there could not prove its GPU work finished, so its presenter is kept";
    case Outcome::TakenOver:
        return "the window was taken over";
    }
    return "unknown";
}

struct Attempt
{
    Outcome outcome = Outcome::NotApplicable;
    int32_t result = 0; // the creation's result: the retry's after a takeover, the one handed in otherwise
};

// The steps a creation site runs after its presenter's creation returned result:
//   only on E_ACCESSDENIED, find the newest live bridge on the window (acquire takes a reference);
//   ask it to give its presenter up (surrender);
//   drop that reference (release), whatever it answered and before anything else happens;
//   and only if it gave the presenter up, make the creation once more (create), once.
template <typename Bridge, typename Acquire, typename SurrenderFn, typename ReleaseFn, typename CreateFn>
Attempt TakeOverAndRetry(int32_t result, Registry<Bridge>& registry, void* window, Acquire&& acquire,
                         SurrenderFn&& surrender, ReleaseFn&& release, CreateFn&& create)
{
    if (result != kAccessDenied)
        return { Outcome::NotApplicable, result };

    Bridge* bridge = registry.Find(window, acquire);
    if (bridge == nullptr)
        return { Outcome::NoBridge, result };

    const Surrender answer = surrender(bridge);
    release(bridge);

    switch (answer)
    {
    case Surrender::Surrendered:
        return { Outcome::TakenOver, static_cast<int32_t>(create()) };
    case Surrender::Busy:
        return { Outcome::Busy, result };
    case Surrender::FrameGeneration:
        return { Outcome::FrameGeneration, result };
    case Surrender::Undrained:
        return { Outcome::Undrained, result };
    }
    return { Outcome::Undrained, result };
}

} // namespace Dx11wDx12::Window
