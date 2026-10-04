#pragma once

// Selection logic for present-time guide snapshot ring.
// Ported from RenoDX (present_path.hpp, commit 9bb6c0f, MIT licence, Carlos Lopez Jr.).
//
// Pure C++ (stdlib only), so tests/nr-present-guides can verify ordering and cross-queue rules on host.

#include <cstddef>
#include <cstdint>

namespace DlssNr::PresentGuides
{
constexpr size_t kRingSize = 3;

struct Slot
{
    uint64_t sequence = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool depthInverted = false;
    bool recorded = false;
    void* userToken = nullptr; // e.g. pointer to submission tracking epoch
};

template <typename CompletedPredicate> class Ring
{
    Slot _slots[kRingSize] {};
    size_t _lastReadIndex = kRingSize; // index of the slot present last read
    uint64_t _nextSequence = 1;

  public:
    // Selects a slot to record a new capture into on the evaluate command list.
    // Never overwrites the slot that present is currently reading or read last.
    size_t AcquireForRecord()
    {
        size_t best = kRingSize;
        uint64_t oldestSeq = UINT64_MAX;

        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (i == _lastReadIndex)
                continue;

            if (!_slots[i].recorded)
                return i;

            if (_slots[i].sequence < oldestSeq)
            {
                oldestSeq = _slots[i].sequence;
                best = i;
            }
        }

        return best != kRingSize ? best : 0;
    }

    void CommitRecord(size_t index, uint32_t w, uint32_t h, bool depthInverted, void* token)
    {
        if (index >= kRingSize)
            return;

        _slots[index].sequence = _nextSequence++;
        _slots[index].width = w;
        _slots[index].height = h;
        _slots[index].depthInverted = depthInverted;
        _slots[index].userToken = token;
        _slots[index].recorded = true;
    }

    // Selects the newest capture whose submission is completed on the CPU.
    // Never performs a GPU cross-queue wait. If no recorded capture is completed,
    // returns kRingSize (caller falls back to neutral zero guides).
    size_t PickForPresent(CompletedPredicate isCompleted)
    {
        size_t newestCompleted = kRingSize;
        uint64_t highestSeq = 0;

        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (!_slots[i].recorded)
                continue;

            if (isCompleted(_slots[i].userToken))
            {
                if (_slots[i].sequence > highestSeq)
                {
                    highestSeq = _slots[i].sequence;
                    newestCompleted = i;
                }
            }
        }

        if (newestCompleted != kRingSize)
            _lastReadIndex = newestCompleted;

        return newestCompleted;
    }

    const Slot& GetSlot(size_t index) const { return _slots[index < kRingSize ? index : 0]; }
    size_t LastReadIndex() const { return _lastReadIndex; }

    void Reset()
    {
        for (auto& s : _slots)
            s = Slot {};
        _lastReadIndex = kRingSize;
        _nextSequence = 1;
    }
};

} // namespace DlssNr::PresentGuides
