#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <limits>

namespace DlssNr::Submission::Detail
{
// Caller serializes this model. No callbacks into the renderer and no GPU assumptions.
constexpr size_t MaxEpochs = 256;
constexpr size_t MaxUses = 64;
constexpr size_t MaxQueues = 32;

struct Epoch
{
    uintptr_t list = 0;
    bool sealed = false;
    bool submitted = false;
    bool unknown = false;
    unsigned pending = 0;
    std::array<uint64_t, MaxQueues> fences {};
    // Keeps COM identity alive to prevent pointer reuse; absent in host tests.
    std::shared_ptr<void> owner;
};

struct Usage
{
    std::array<std::shared_ptr<Epoch>, MaxUses> epochs {};
};

template <typename Completed> bool IsComplete(const Epoch& e, Completed completed)
{
    if (!e.submitted || e.unknown || e.pending != 0)
        return false;
    for (size_t q = 0; q < MaxQueues; ++q)
    {
        if (e.fences[q] == 0)
            continue;
        const auto value = completed(q);
        if (value == std::numeric_limits<uint64_t>::max() || value < e.fences[q])
            return false;
    }
    return true;
}

template <typename Completed> bool IsReady(const Epoch& e, Completed completed)
{
    // Even a completed, closed list can execute again. Only successful Reset seals it.
    return e.sealed && IsComplete(e, completed);
}

class Model
{
    std::array<std::shared_ptr<Epoch>, MaxEpochs> epochs {};

  public:
    std::shared_ptr<Epoch> Current(uintptr_t list) const
    {
        for (const auto& e : epochs)
            if (e && e->list == list && !e->sealed)
                return e;
        return {};
    }

    template <typename Completed> std::shared_ptr<Epoch> Track(uintptr_t list, Usage& usage, Completed completed)
    {
        size_t freeUse = MaxUses;
        auto current = Current(list);
        if (list == 0 || (current && current->unknown))
            return {};
        for (size_t i = 0; i < MaxUses; ++i)
        {
            auto& e = usage.epochs[i];
            if (e && IsReady(*e, completed))
                e.reset();
            if (current && e == current)
                return current;
            if (!e)
                freeUse = i;
        }
        if (freeUse == MaxUses)
            return {};
        if (!current)
        {
            for (auto& e : epochs)
            {
                if (e && IsReady(*e, completed))
                    e.reset();
                if (!e)
                {
                    e = std::make_shared<Epoch>();
                    e->list = list;
                    current = e;
                    break;
                }
            }
        }
        if (current)
            usage.epochs[freeUse] = current;
        return current;
    }

    std::shared_ptr<Epoch> BeforeExecute(uintptr_t list)
    {
        auto e = Current(list);
        if (e)
            ++e->pending;
        return e;
    }

    static void AfterExecute(const std::shared_ptr<Epoch>& e, size_t queue, uint64_t fence, bool success)
    {
        if (!e)
            return;
        if (!success || queue >= MaxQueues || fence == 0 || fence == std::numeric_limits<uint64_t>::max())
            e->unknown = true;
        else
        {
            e->submitted = true;
            if (e->fences[queue] < fence)
                e->fences[queue] = fence;
        }
        if (e->pending != 0)
            --e->pending;
    }

    static void AfterReset(const std::shared_ptr<Epoch>& e, bool success)
    {
        if (e && success)
            e->sealed = true;
    }
};
} // namespace DlssNr::Submission::Detail
