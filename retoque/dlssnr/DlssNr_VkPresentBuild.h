#pragma once

// The Vulkan present host's model, built off the present path, and the host's stall watchdog
// (docs/dlssnr/design/nr-vulkan-present-host.md, "Off the present path").
//
// - The build's state machine: what a frame does with the model, given what is built and what is building.
// - The activity table the watchdog reads: every host entry point that can hold or wait for a host lock records its
//   thread, when it entered, its stage and the locks it holds, with atomics only, so the watchdog never takes a lock.
// - TrackedMutex: a lock that records its owner thread, so a stalled waiter's line names the holder.
//
// Standard library only, so tests/nr-vk-present-host drives all of it on the host.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace DlssNr::VkPresentHost
{
// --- The build ---------------------------------------------------------------------------------------------------

// What a product is built for: the frame, the model's working size, the queue family its GPU work ran on (its
// images and feature are used there without an ownership transfer), and whether it carries the synthesized-motion
// estimator's images ([DlssNr] SynthMotion, stage 5: turning it on or off is a rebuild, frames carried meanwhile).
struct BuildKey
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    uint32_t family = UINT32_MAX;
    bool synth = false;

    bool operator==(const BuildKey&) const = default;
};

enum class BuildState : uint8_t
{
    None,    // no job
    Queued,  // posted to the builder thread
    Running, // the builder thread is on it
    Done,    // its product is ready to adopt
    Failed,  // it failed: adopted as the model's latched failure
};

enum class BuildStep : uint8_t
{
    Use,     // the product is built for this frame: run the model
    Request, // retire the product and request a job for this frame's key
    Wait,    // a job is queued or running: carry the frame unchanged (model building)
    Adopt,   // a job is over: adopt its product or its failure, then decide again
    Off,     // the model is off on this device (a latched failure)
};

// At an eligible frame, before any model call. `built` is whether a product exists and `builtKey` what it was built
// for. A request while a job runs is not queued behind it: the job finishes and is adopted, and the next frame asks
// again if the key moved.
inline BuildStep DecideBuild(bool failed, BuildState job, bool built, const BuildKey& builtKey, const BuildKey& want)
{
    if (failed)
        return BuildStep::Off;
    if (job == BuildState::Queued || job == BuildState::Running)
        return BuildStep::Wait;
    if (job == BuildState::Done || job == BuildState::Failed)
        return BuildStep::Adopt;
    if (built && builtKey == want)
        return BuildStep::Use;
    return BuildStep::Request;
}

inline const char* BuildStateName(BuildState state)
{
    switch (state)
    {
    case BuildState::Queued:
        return "queued";
    case BuildState::Running:
        return "running";
    case BuildState::Done:
        return "done";
    case BuildState::Failed:
        return "failed";
    default:
        return "none";
    }
}

// The longest the builder waits for its own GPU work (the feature's initialisation, the guides' clear), and for the
// work queue's locks (in short tries: it never holds the present lock while it waits for the submit lock). The longest
// vkDestroyDevice waits for a job under way -- longer than those two together, so a merely slow GPU never orphans
// one -- and then, once it has orphaned it, for the builder to step out of the device (every wait of the builder's is
// sliced and checks the orphan flag; only an opaque NGX call can keep it longer).
inline constexpr uint64_t kBuildGpuWaitMs = 10000;
inline constexpr uint64_t kBuildQueueLockWaitMs = 2000;
inline constexpr uint64_t kBuildDestroyWaitMs = 15000;
inline constexpr uint64_t kBuildOrphanWaitMs = 5000;
inline constexpr uint64_t kBuildSliceMs = 50;

// --- Threads, locks and the activity table ---------------------------------------------------------------------

// The thread's id as the log names it (Windows' GetCurrentThreadId in production; the host sets it per thread before
// the thread's first activity). 0 until set: the table then names "thread ?".
inline thread_local uint32_t t_threadTag = 0;
inline uint32_t ThreadTag() { return t_threadTag; }
inline void SetThreadTag(uint32_t tag) { t_threadTag = tag; }

// A lock that records which thread holds it (0: nobody), for the watchdog's lines. Recursion is counted by the holder
// only, so the count needs no atomic. Meets Lockable and TimedLockable for the Mutex given.
template <typename Mutex> class TrackedMutex
{
  public:
    void lock()
    {
        _mutex.lock();
        Acquired();
    }

    bool try_lock()
    {
        if (!_mutex.try_lock())
            return false;
        Acquired();
        return true;
    }

    template <typename Rep, typename Period> bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout)
    {
        if (!_mutex.try_lock_for(timeout))
            return false;
        Acquired();
        return true;
    }

    void unlock()
    {
        if (--_depth == 0)
            _owner.store(0, std::memory_order_release);
        _mutex.unlock();
    }

    // The thread holding it, or 0. Read without the lock: a hint for a log line, never a decision.
    uint32_t Owner() const { return _owner.load(std::memory_order_acquire); }

  private:
    void Acquired()
    {
        if (_depth++ == 0)
            _owner.store(ThreadTag() != 0 ? ThreadTag() : 1u, std::memory_order_release);
    }

    Mutex _mutex;
    std::atomic<uint32_t> _owner { 0 };
    uint32_t _depth = 0;
};

// What a host lock is, for an activity's "holds" and "waiting for" (bits).
enum HostLock : uint32_t
{
    kLockNone = 0,
    kLockPresent = 1u << 0,         // the presenting queue's present lock
    kLockTransport = 1u << 1,       // g_transportMutex
    kLockBorrowedPresent = 1u << 2, // a borrowed queue's present lock
    kLockBorrowedSubmit = 1u << 3,  // a borrowed queue's submit lock
    kLockSubmit = 1u << 4,          // the queue-call hooks' submit lock
    kLockRoute = 1u << 5,           // g_vkMutex, the after-upscale route's
};

inline std::string HostLockNames(uint32_t locks)
{
    static constexpr std::pair<uint32_t, const char*> kNames[] = {
        { kLockPresent, "the present lock" },
        { kLockTransport, "the transport lock" },
        { kLockBorrowedPresent, "a borrowed present lock" },
        { kLockBorrowedSubmit, "a borrowed submit lock" },
        { kLockSubmit, "the submit lock" },
        { kLockRoute, "the after-upscale route's lock" },
    };
    std::string out;
    for (const auto& [bit, name] : kNames)
    {
        if ((locks & bit) != 0)
            out += (out.empty() ? "" : ", ") + std::string(name);
    }
    return out;
}

// One activity in the table. Written by its own thread, read by the watchdog; every field atomic. `entry` and
// `stage` point at string literals only, so a pointer read from another thread is always a valid string.
struct Activity
{
    std::atomic<uint32_t> thread { 0 };             // 0: the slot is free
    std::atomic<uint64_t> serial { 0 };             // which occupancy of the slot, so a report is per occupancy
    std::atomic<uint64_t> enteredMs { UINT64_MAX }; // monotonic; UINT64_MAX while the slot is free or being taken
    std::atomic<uint64_t> stallMs { 0 };            // how long inside before it is a stall
    std::atomic<const char*> entry { nullptr };     // "vkQueuePresentKHR", "model build", ...
    std::atomic<const char*> stage { nullptr };     // what it is doing now
    std::atomic<uint64_t> object { 0 };             // the queue, swapchain or device it concerns, when one
    std::atomic<uint32_t> holds { 0 };              // HostLock bits
    std::atomic<uint32_t> waitingFor { 0 };         // HostLock bit, when its stage is a wait for one
    std::atomic<uint32_t> waitingOwner { 0 };       // ... and the thread that held it when the wait began
};

inline constexpr size_t kActivitySlots = 64;
inline constexpr uint64_t kStallMs = 2000;        // inside the host this long: logged
inline constexpr uint64_t kStallRepeatMs = 10000; // ... and again every this long while it lasts
inline constexpr uint64_t kBuildStallMs = 5000;   // the model's build holds no host lock: a longer allowance

class ActivityTable
{
  public:
    // A free slot for this thread's new activity, or kActivitySlots when the table is full (the activity then goes
    // unrecorded; nothing waits for a slot).
    size_t Enter(const char* entry, uint64_t object, uint64_t nowMs, uint64_t stallMs = kStallMs)
    {
        const uint32_t tag = ThreadTag() != 0 ? ThreadTag() : 1u;
        for (size_t i = 0; i < kActivitySlots; ++i)
        {
            uint32_t expected = 0;
            Activity& a = _slots[i];
            if (a.thread.load(std::memory_order_relaxed) != 0 ||
                !a.thread.compare_exchange_strong(expected, tag, std::memory_order_acq_rel))
                continue;
            a.entry.store(entry, std::memory_order_relaxed);
            a.stallMs.store(stallMs, std::memory_order_relaxed);
            a.stage.store("entered", std::memory_order_relaxed);
            a.object.store(object, std::memory_order_relaxed);
            a.holds.store(0, std::memory_order_relaxed);
            a.waitingFor.store(0, std::memory_order_relaxed);
            a.waitingOwner.store(0, std::memory_order_relaxed);
            a.serial.store(++_serials, std::memory_order_relaxed);
            a.enteredMs.store(nowMs, std::memory_order_release); // last: until then the watchdog skips the slot
            return i;
        }
        return kActivitySlots;
    }

    void Stage(size_t slot, const char* stage, uint32_t waitingFor = kLockNone, uint32_t waitingOwner = 0)
    {
        if (slot >= kActivitySlots)
            return;
        _slots[slot].waitingFor.store(waitingFor, std::memory_order_relaxed);
        _slots[slot].waitingOwner.store(waitingOwner, std::memory_order_relaxed);
        _slots[slot].stage.store(stage, std::memory_order_release);
    }

    void Hold(size_t slot, uint32_t lock, bool held)
    {
        if (slot >= kActivitySlots)
            return;
        if (held)
            _slots[slot].holds.fetch_or(lock, std::memory_order_acq_rel);
        else
            _slots[slot].holds.fetch_and(~lock, std::memory_order_acq_rel);
    }

    void Leave(size_t slot)
    {
        if (slot >= kActivitySlots)
            return;
        _slots[slot].enteredMs.store(UINT64_MAX, std::memory_order_relaxed);
        _slots[slot].stage.store(nullptr, std::memory_order_relaxed);
        _slots[slot].thread.store(0, std::memory_order_release);
    }

    // The watchdog's pass: one line per activity inside the host for its allowance (kStallMs, or kBuildStallMs for the
    // build), once per occupancy, then every kStallRepeatMs while it lasts. Reads only atomics.
    std::vector<std::string> Check(uint64_t nowMs)
    {
        std::vector<std::string> lines;
        for (size_t i = 0; i < kActivitySlots; ++i)
        {
            Activity& a = _slots[i];
            const uint32_t thread = a.thread.load(std::memory_order_acquire);
            if (thread == 0)
                continue;
            const uint64_t entered = a.enteredMs.load(std::memory_order_acquire);
            const uint64_t serial = a.serial.load(std::memory_order_relaxed);
            if (entered == UINT64_MAX || nowMs < entered || nowMs - entered < a.stallMs.load(std::memory_order_relaxed))
                continue;
            if (_reportedSerial[i] == serial && nowMs - _reportedMs[i] < kStallRepeatMs)
                continue;
            _reportedSerial[i] = serial;
            _reportedMs[i] = nowMs;
            lines.push_back(Describe(a, thread, nowMs, true));
        }
        return lines;
    }

    // What the thread `tag` is doing in the host, for a waiter's line (empty when it is in none).
    std::string Find(uint32_t tag, uint64_t nowMs)
    {
        // A thread inside a nested entry point has several: its innermost is the latest occupancy.
        Activity* innermost = nullptr;
        uint64_t latest = 0;
        for (Activity& a : _slots)
        {
            if (tag != 0 && a.thread.load(std::memory_order_acquire) == tag &&
                a.serial.load(std::memory_order_relaxed) >= latest)
            {
                latest = a.serial.load(std::memory_order_relaxed);
                innermost = &a;
            }
        }
        return innermost != nullptr ? Describe(*innermost, tag, nowMs, false) : std::string();
    }

  private:
    std::string Describe(Activity& a, uint32_t thread, uint64_t nowMs, bool full)
    {
        const char* entry = a.entry.load(std::memory_order_relaxed);
        const char* stage = a.stage.load(std::memory_order_acquire);
        const uint64_t entered = a.enteredMs.load(std::memory_order_acquire);
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f",
                      entered != UINT64_MAX && nowMs >= entered ? (nowMs - entered) / 1000.0 : 0.0);

        std::string line = std::string(full ? "thread " + std::to_string(thread) + " " : "") + "in " +
                           (entry != nullptr ? entry : "?") + " for " + seconds +
                           " s: " + (stage != nullptr ? stage : "leaving");
        if (!full)
            return line;

        const uint64_t object = a.object.load(std::memory_order_relaxed);
        if (object != 0)
        {
            char hex[32];
            std::snprintf(hex, sizeof(hex), " (0x%llX)", static_cast<unsigned long long>(object));
            line.insert(line.find(" for "), hex);
        }

        const uint32_t waiting = a.waitingFor.load(std::memory_order_relaxed);
        const uint32_t holder = a.waitingOwner.load(std::memory_order_relaxed);
        if (waiting != 0 && holder != 0 && holder != thread)
        {
            const std::string holderDoing = Find(holder, nowMs);
            line += " (" + HostLockNames(waiting) + " is held by thread " + std::to_string(holder) +
                    (holderDoing.empty() ? std::string(", outside the host's entry points") : ", " + holderDoing) + ")";
        }

        const uint32_t holds = a.holds.load(std::memory_order_relaxed);
        line += holds != 0 ? "; holds " + HostLockNames(holds) : "; holds no host lock";
        return line;
    }

    std::array<Activity, kActivitySlots> _slots;
    std::array<uint64_t, kActivitySlots> _reportedSerial {};
    std::array<uint64_t, kActivitySlots> _reportedMs {};
    std::atomic<uint64_t> _serials { 0 };
};

// The process's table, made on first use: only the host's hooks and its builder ever use it, so with the host off it
// never exists.
inline ActivityTable& Activities()
{
    static ActivityTable table;
    return table;
}

inline uint64_t ActivityNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// One activity for the scope of a host entry point.
class ActivityScope
{
  public:
    ActivityScope(const char* entry, uint64_t object = 0, uint64_t stallMs = kStallMs)
        : _slot(Activities().Enter(entry, object, ActivityNowMs(), stallMs))
    {
    }
    ~ActivityScope() { Activities().Leave(_slot); }
    ActivityScope(const ActivityScope&) = delete;
    ActivityScope& operator=(const ActivityScope&) = delete;

    void Stage(const char* stage, uint32_t waitingFor = kLockNone, uint32_t waitingOwner = 0)
    {
        Activities().Stage(_slot, stage, waitingFor, waitingOwner);
    }
    void Hold(uint32_t lock, bool held) { Activities().Hold(_slot, lock, held); }

  private:
    size_t _slot;
};

// The innermost activity of this thread, for code that cannot be handed its scope (a helper several entry points
// share). Set by ActivityScope's users through CurrentActivity; null outside every entry point.
inline thread_local ActivityScope* t_activity = nullptr;

class CurrentActivity
{
  public:
    explicit CurrentActivity(ActivityScope& scope) : _saved(t_activity) { t_activity = &scope; }
    ~CurrentActivity() { t_activity = _saved; }
    CurrentActivity(const CurrentActivity&) = delete;
    CurrentActivity& operator=(const CurrentActivity&) = delete;

  private:
    ActivityScope* _saved;
};

inline void ActivityStage(const char* stage, uint32_t waitingFor = kLockNone, uint32_t waitingOwner = 0)
{
    if (t_activity != nullptr)
        t_activity->Stage(stage, waitingFor, waitingOwner);
}

inline void ActivityHold(uint32_t lock, bool held)
{
    if (t_activity != nullptr)
        t_activity->Hold(lock, held);
}
} // namespace DlssNr::VkPresentHost
