#pragma once

#include "DlssNr_PassSettings.h"
#include <chrono>
#include <atomic>

namespace DlssNr::Chain
{
// One explicit recording scope may borrow its own lease; a new recording must wait
// for all previous usage to be Ready, including Reset sealing against future replay.
class RecordingLease;
class RecordingGate
{
    friend class RecordingLease;
    std::atomic<uint64_t> active { 0 };
    uint64_t next = 0;
    uintptr_t list = 0;
};

class RecordingLease
{
    RecordingGate* gate;
    uint64_t token = 0;
    bool tracked = false;

  public:
    // Begin/Validate are serialized by the renderer mutex. End may run at scope exit.
    RecordingLease(RecordingGate& owner, uintptr_t commandList) : gate(&owner)
    {
        if (owner.active.load() != 0)
            return;
        owner.list = commandList;
        token = ++owner.next;
        owner.active.store(token);
    }
    ~RecordingLease()
    {
        if (token)
        {
            auto expected = token;
            gate->active.compare_exchange_strong(expected, 0);
        }
    }
    RecordingLease(const RecordingLease&) = delete;
    RecordingLease& operator=(const RecordingLease&) = delete;
    bool Valid(RecordingGate& owner, uintptr_t list) const
    {
        return gate == &owner && token != 0 && owner.active.load() == token && owner.list == list;
    }
    bool Tracked() const { return tracked; }
    bool MayTrack(bool hasPreviousUsage, bool previousReady) const
    {
        return tracked || !hasPreviousUsage || previousReady;
    }
    void MarkTracked() { tracked = true; }
};

// Portable scheduling/routing contract used by the renderer, independent of NGX.
struct Schedule
{
    DlssNrPassSnapshot candidate {};
    std::chrono::steady_clock::time_point since {};
    uint64_t lastEvaluation = 0, lastCreation = 0;
    std::chrono::steady_clock::time_point lastCreationTime {};
    bool observed = false;

    bool Stable(const DlssNrPassSnapshot& value, std::chrono::steady_clock::time_point now)
    {
        if (!observed || candidate.Count != value.Count || candidate.Individual != value.Individual ||
            candidate.Settings != value.Settings)
        {
            candidate = value;
            since = now;
            observed = true;
            return false;
        }
        return now - since >= std::chrono::milliseconds(500);
    }
    bool Evaluate(uint64_t frame)
    {
        if (!frame || frame <= lastEvaluation)
            return false;
        lastEvaluation = frame;
        return true;
    }
    bool Create(uint64_t frame, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        if (!frame || frame <= lastCreation ||
            (lastCreation && now - lastCreationTime < std::chrono::milliseconds(500)))
            return false;
        lastCreation = frame;
        lastCreationTime = now;
        return true;
    }
};

// The original proxy is never an output. Failed layers leave the last answer selected.
template <class Surface> struct Routing
{
    Surface original, first, second, answer;
    uint32_t completed = 0;
    Routing(Surface proxy, Surface a, Surface b) : original(proxy), first(a), second(b), answer(proxy) {}
    Surface Input() const { return answer; }
    Surface Output() const { return completed % 2 == 0 ? first : second; }
    void Success()
    {
        answer = Output();
        ++completed;
    }
};

template <class Surface> struct ResolveChoice
{
    Surface proxy, answer;
};
template <class Surface>
ResolveChoice<Surface> Resolve(Surface nativeProxy, Surface workingProxy, Surface nativeAnswer, Surface workingAnswer,
                               bool downsampled)
{
    return downsampled ? ResolveChoice<Surface> { nativeProxy, nativeAnswer }
                       : ResolveChoice<Surface> { workingProxy, workingAnswer };
}

inline bool RetirementAllowed(size_t parked) { return parked < 32; }

inline bool Admit(uint64_t budget, uint64_t used, uint64_t measuredModel, uint64_t textureBytes)
{
    // Adapter-reported usage includes opaque NGX allocations. Reserve at least 512 MiB
    // and 20% of the budget, plus 150% of the measured model delta and scratch cost.
    if (!budget || used >= budget)
        return false;
    const uint64_t reserve = std::max<uint64_t>(512ull << 20, budget / 5);
    const uint64_t model = std::max<uint64_t>(measuredModel, 256ull << 20);
    const uint64_t required = reserve + model + model / 2 + textureBytes;
    return required <= budget - used;
}
} // namespace DlssNr::Chain
