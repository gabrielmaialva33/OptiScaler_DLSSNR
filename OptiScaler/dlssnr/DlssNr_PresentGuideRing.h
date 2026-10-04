#pragma once

// Selection logic for present-time guide snapshot ring.
// Ported from RenoDX (present_path.hpp:30-33, 96-100, 155-168, commit 9bb6c0f, MIT licence, Carlos Lopez Jr.).
//
// Pure C++ (stdlib only), so tests/nr-present-guides can verify ordering, completion and cross-queue rules on host.

#include <cstddef>
#include <cstdint>

namespace DlssNr::PresentGuides
{
// Four capture sets (RenoDX kPresentGuideSets): the newest, one the game may be copying into,
// one a present on the GPU may still read, and one a present read last.
constexpr size_t kRingSize = 4;

struct Slot
{
    uint64_t sequence = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool depthInverted = false;
    bool recorded = false;
    void* writeToken = nullptr; // e.g. pointer to evaluate submission tracking usage
    void* readToken = nullptr;  // e.g. pointer to present submission tracking usage
};

template <typename WriteCompletedPredicate, typename ReadInFlightPredicate> class Ring
{
    Slot _slots[kRingSize] {};
    size_t _lastReadIndex = kRingSize; // index of the slot present last read
    uint64_t _nextSequence = 1;

  public:
    // Selects a slot to record a new capture into on the evaluate command list.
    // Skips slots currently being read by in-flight present commands or the slot read last.
    size_t AcquireForRecord(ReadInFlightPredicate isReadInFlight)
    {
        size_t best = kRingSize;
        uint64_t oldestSeq = UINT64_MAX;

        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (i == _lastReadIndex)
                continue;

            // Never overwrite a slot whose present-time read is still in flight on the GPU
            if (isReadInFlight(_slots[i].readToken))
                continue;

            if (!_slots[i].recorded)
                return i;

            if (_slots[i].sequence < oldestSeq)
            {
                oldestSeq = _slots[i].sequence;
                best = i;
            }
        }

        // If all other slots have in-flight reads, pick oldest recorded
        if (best == kRingSize)
        {
            for (size_t i = 0; i < kRingSize; ++i)
            {
                if (i != _lastReadIndex && _slots[i].sequence < oldestSeq)
                {
                    oldestSeq = _slots[i].sequence;
                    best = i;
                }
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
        _slots[index].writeToken = token;
        _slots[index].recorded = true;
    }

    void InvalidateSlot(size_t index)
    {
        if (index < kRingSize)
        {
            _slots[index].recorded = false;
            _slots[index].sequence = 0;
            _slots[index].writeToken = nullptr;
        }
    }

    // Selects the newest capture whose submission is completed on the CPU.
    // Never performs a GPU cross-queue wait. If no recorded capture is completed,
    // returns kRingSize (caller falls back to neutral zero guides).
    size_t PickForPresent(WriteCompletedPredicate isWriteCompleted, void* presentReadToken)
    {
        size_t newestCompleted = kRingSize;
        uint64_t highestSeq = 0;

        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (!_slots[i].recorded)
                continue;

            if (isWriteCompleted(_slots[i].writeToken))
            {
                if (_slots[i].sequence > highestSeq)
                {
                    highestSeq = _slots[i].sequence;
                    newestCompleted = i;
                }
            }
        }

        if (newestCompleted != kRingSize)
        {
            _lastReadIndex = newestCompleted;
            _slots[newestCompleted].readToken = presentReadToken;
        }

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
