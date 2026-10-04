#pragma once

// Selection logic for present-time guide snapshot ring.
// Ported from RenoDX (present_path.hpp:30-33, 96-100, 155-168, 880-1090, commit 9bb6c0f, MIT licence, Carlos Lopez
// Jr.).
//
// Pure C++ (stdlib only), so tests/nr-present-guides can verify ordering, completion, age limits, and cross-queue rules
// on host.

#include <cstddef>
#include <cstdint>

namespace DlssNr::PresentGuides
{
// Four capture sets (RenoDX kPresentGuideSets): the newest, one the game may be copying into,
// one a present on the GPU may still read, and one a present read last.
constexpr size_t kRingSize = 4;
// Drop captures older than 250 ms as stale (RenoDX kPresentGuideMaxAgeMs / present_path.hpp:155-168).
constexpr int64_t kPresentGuideMaxAgeMs = 250;

struct Slot
{
    uint64_t sequence = 0;
    int64_t capturedAtMs = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool depthInverted = false;
    bool recorded = false;
    void* writeToken = nullptr; // pointer to evaluate submission tracking usage
    void* readToken = nullptr;  // pointer to present submission tracking usage
};

class Ring
{
    Slot _slots[kRingSize] {};
    size_t _lastReadIndex = kRingSize; // index of the slot present last read
    uint64_t _nextSequence = 1;

  public:
    // Selects a slot to record a new capture into on the evaluate command list.
    // Skips slots currently being read by in-flight present commands and the slot read last.
    // If all other slots are currently in flight, returns kRingSize (skips capture, matching RenoDX).
    template <typename P> size_t AcquireForRecord(P isReadInFlight)
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

        // When all non-last-read slots have in-flight reads, return kRingSize to skip capture
        return best;
    }

    void CommitRecord(size_t index, uint32_t w, uint32_t h, bool depthInverted, int64_t nowMs, void* writeToken,
                      void* readToken)
    {
        if (index >= kRingSize)
            return;

        _slots[index].sequence = _nextSequence++;
        _slots[index].capturedAtMs = nowMs;
        _slots[index].width = w;
        _slots[index].height = h;
        _slots[index].depthInverted = depthInverted;
        _slots[index].writeToken = writeToken;
        _slots[index].readToken = readToken;
        _slots[index].recorded = true;
    }

    void InvalidateSlot(size_t index)
    {
        if (index < kRingSize)
        {
            _slots[index].recorded = false;
            _slots[index].sequence = 0;
            _slots[index].capturedAtMs = 0;
            _slots[index].writeToken = nullptr;
            _slots[index].readToken = nullptr;
        }
    }

    // Selects the newest capture whose evaluate submission is eligible (either submitted on the same queue,
    // or completed on CPU). Never performs a GPU cross-queue wait. Rejects captures older than kPresentGuideMaxAgeMs.
    // If no recorded capture is eligible and fresh, returns kRingSize (caller uses neutral zero guides).
    template <typename P> size_t PickForPresent(P isEligible, int64_t nowMs)
    {
        size_t newestCompleted = kRingSize;
        uint64_t highestSeq = 0;

        for (size_t i = 0; i < kRingSize; ++i)
        {
            if (!_slots[i].recorded)
                continue;

            // Discard stale captures
            if (nowMs > 0 && _slots[i].capturedAtMs > 0 && (nowMs - _slots[i].capturedAtMs) > kPresentGuideMaxAgeMs)
                continue;

            if (isEligible(_slots[i].writeToken))
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

    uint64_t NewestRecordedSequence() const { return _nextSequence > 1 ? _nextSequence - 1 : 0; }
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
