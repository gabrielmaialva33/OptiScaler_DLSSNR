#include <dlssnr/DlssNr_PresentGuideRing.h>

#include <cassert>
#include <cstdio>
#include <unordered_set>

using namespace DlssNr::PresentGuides;

void TestRingBasicAcquireCommitPick()
{
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    // Initially no slots recorded
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == kRingSize);

    // Slot 0 recorded with token 0x1
    void* token1 = (void*) 0x1;
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, true, token1);

    // Not yet completed by submission tracker: pick returns kRingSize (falls back to zero guides, no stall)
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == kRingSize);

    // Submission completes on GPU
    completedWrites.insert(token1);
    void* presentToken1 = (void*) 0x101;
    size_t picked = ring.PickForPresent(isWriteCompleted, presentToken1);
    assert(picked == 0);
    assert(ring.GetSlot(picked).sequence == 1);
    assert(ring.GetSlot(picked).width == 1920);

    // Mark slot 0 read as in-flight
    inFlightReads.insert(presentToken1);

    // Next record: must not overwrite slot 0 (it is both last-read and in-flight)
    void* token2 = (void*) 0x2;
    size_t s1 = ring.AcquireForRecord(isReadInFlight);
    assert(s1 != 0); // picks a different slot
    ring.CommitRecord(s1, 1920, 1080, true, token2);

    // Token 2 not completed yet; present continues reading newest completed (slot 0)
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == 0);

    // Token 2 completes
    completedWrites.insert(token2);
    size_t picked2 = ring.PickForPresent(isWriteCompleted, nullptr);
    assert(picked2 == s1);
    assert(ring.GetSlot(picked2).sequence == 2);
}

void TestRingCrossQueueNoDeadlockFallback()
{
    // Under DLSS-G, present runs on another queue. If the newest capture is on an uncompleted fence,
    // present must pick the newest completed older slot without waiting on GPU, or fall back to kRingSize.
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    void* tokenA = (void*) 0xA;
    void* tokenB = (void*) 0xB;
    void* tokenC = (void*) 0xC;

    size_t sA = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(sA, 3840, 2160, true, tokenA);
    completedWrites.insert(tokenA);
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == sA);

    size_t sB = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(sB, 3840, 2160, true, tokenB);
    completedWrites.insert(tokenB);
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == sB);

    // Frame C in flight on game queue
    size_t sC = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(sC, 3840, 2160, true, tokenC);
    // Token C is NOT completed yet
    size_t picked = ring.PickForPresent(isWriteCompleted, nullptr);
    // Must pick slot B (newest completed), never stall and never return uncompleted C
    assert(picked == sB);

    // Invalidation test: if slot B is invalidated, should pick A
    ring.InvalidateSlot(sB);
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == sA);

    // If both A and B invalidated and C not completed, returns kRingSize (neutral guides)
    ring.InvalidateSlot(sA);
    assert(ring.PickForPresent(isWriteCompleted, nullptr) == kRingSize);
}

void TestRingInFlightReadProtection()
{
    // Ensure AcquireForRecord skips slots whose present read is still in flight on GPU
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    void* w0 = (void*) 0x10;
    void* r0 = (void*) 0x20;
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(s0, 1920, 1080, false, w0);
    completedWrites.insert(w0);

    ring.PickForPresent(isWriteCompleted, r0);
    inFlightReads.insert(r0); // present is reading slot 0

    // Record next 3 slots
    for (int i = 1; i < 4; ++i)
    {
        size_t s = ring.AcquireForRecord(isReadInFlight);
        assert(s != s0); // must never pick s0 while read is in flight
        ring.CommitRecord(s, 1920, 1080, false, (void*) (uintptr_t) (0x10 + i));
    }
}

int main()
{
    TestRingBasicAcquireCommitPick();
    TestRingCrossQueueNoDeadlockFallback();
    TestRingInFlightReadProtection();
    std::printf("PASS: nr-present-guides ring acquisition, completion, in-flight read protection and fallback\n");
    return 0;
}
