#pragma once

#include "DlssNr_PassSettings.h"
#include <chrono>

namespace DlssNr::Chain
{
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
        if (!frame || frame <= lastEvaluation) return false;
        lastEvaluation = frame;
        return true;
    }
    bool Create(uint64_t frame, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        if (!frame || frame <= lastCreation || (lastCreation && now - lastCreationTime < std::chrono::milliseconds(500))) return false;
        lastCreation = frame;
        lastCreationTime = now;
        return true;
    }
};

// The original proxy is never an output. Failed layers leave the last answer selected.
template<class Surface> struct Routing
{
    Surface original, first, second, answer;
    uint32_t completed = 0;
    Routing(Surface proxy, Surface a, Surface b) : original(proxy), first(a), second(b), answer(proxy) {}
    Surface Input() const { return answer; }
    Surface Output() const { return completed % 2 == 0 ? first : second; }
    void Success() { answer = Output(); ++completed; }
};

inline bool Admit(uint64_t budget, uint64_t used, uint64_t measuredModel, uint64_t textureBytes)
{
    // Adapter-reported usage includes opaque NGX allocations. Reserve at least 512 MiB
    // and 20% of the budget, plus 150% of the measured model delta and scratch cost.
    if (!budget || used >= budget) return false;
    const uint64_t reserve = std::max<uint64_t>(512ull << 20, budget / 5);
    const uint64_t model = std::max<uint64_t>(measuredModel, 256ull << 20);
    const uint64_t required = reserve + model + model / 2 + textureBytes;
    return required <= budget - used;
}
} // namespace DlssNr::Chain
