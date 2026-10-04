#include <dlssnr/DlssNr_PresentGuideRing.h>

#include <cassert>
#include <cstdio>
#include <unordered_set>

using namespace DlssNr::PresentGuides;

void TestRingBasicAcquireCommitPick()
{
    std::unordered_set<void*> completedTokens;
    auto isCompleted = [&](void* token) { return completedTokens.find(token) != completedTokens.end(); };

    Ring<decltype(isCompleted)> ring;

    // Initially no slots recorded
    assert(ring.PickForPresent(isCompleted) == kRingSize);

    // Slot 0 recorded with token 0x1
    void* token1 = (void*) 0x1;
    size_t s0 = ring.AcquireForRecord();
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, true, token1);

    // Not yet completed by submission tracker: pick returns kRingSize (falls back to zero guides)
    assert(ring.PickForPresent(isCompleted) == kRingSize);

    // Submission completes on GPU
    completedTokens.insert(token1);
    size_t picked = ring.PickForPresent(isCompleted);
    assert(picked == 0);
    assert(ring.GetSlot(picked).sequence == 1);
    assert(ring.GetSlot(picked).width == 1920);

    // Next record: must not overwrite the slot present is currently reading (slot 0)
    void* token2 = (void*) 0x2;
    size_t s1 = ring.AcquireForRecord();
    assert(s1 != 0); // picks slot 1
    ring.CommitRecord(s1, 1920, 1080, true, token2);

    // Token 2 not completed yet; present continues reading newest completed (slot 0)
    assert(ring.PickForPresent(isCompleted) == 0);

    // Token 2 completes
    completedTokens.insert(token2);
    size_t picked2 = ring.PickForPresent(isCompleted);
    assert(picked2 == s1);
    assert(ring.GetSlot(picked2).sequence == 2);
}

void TestRingCrossQueueNoDeadlockFallback()
{
    // Under DLSS-G, present runs on another queue. If the newest capture is on an uncompleted fence,
    // present must pick the newest completed older slot without waiting on GPU.
    std::unordered_set<void*> completedTokens;
    auto isCompleted = [&](void* token) { return completedTokens.find(token) != completedTokens.end(); };

    Ring<decltype(isCompleted)> ring;

    void* tokenA = (void*) 0xA;
    void* tokenB = (void*) 0xB;
    void* tokenC = (void*) 0xC;

    size_t sA = ring.AcquireForRecord();
    ring.CommitRecord(sA, 3840, 2160, true, tokenA);
    completedTokens.insert(tokenA);
    assert(ring.PickForPresent(isCompleted) == sA);

    size_t sB = ring.AcquireForRecord();
    ring.CommitRecord(sB, 3840, 2160, true, tokenB);
    completedTokens.insert(tokenB);
    assert(ring.PickForPresent(isCompleted) == sB);

    // Frame C in flight on game queue
    size_t sC = ring.AcquireForRecord();
    ring.CommitRecord(sC, 3840, 2160, true, tokenC);
    // Token C is NOT completed yet
    size_t picked = ring.PickForPresent(isCompleted);
    // Picks slot B (newest completed), never stalls
    assert(picked == sB);
}

int main()
{
    TestRingBasicAcquireCommitPick();
    TestRingCrossQueueNoDeadlockFallback();
    std::printf("PASS: nr-present-guides ring acquisition, completion and cross-queue fallback\n");
    return 0;
}
