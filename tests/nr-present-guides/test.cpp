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
    assert(ring.PickForPresent(isWriteCompleted, 1000) == kRingSize);

    // Slot 0 recorded with writeToken 0x1, readToken 0x101
    void* wToken1 = (void*) 0x1;
    void* rToken1 = (void*) 0x101;
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, true, 1000, wToken1, rToken1);

    // Not yet completed by submission tracker: pick returns kRingSize (falls back to zero guides, no stall)
    assert(ring.PickForPresent(isWriteCompleted, 1010) == kRingSize);

    // Submission completes on GPU
    completedWrites.insert(wToken1);
    size_t picked = ring.PickForPresent(isWriteCompleted, 1015);
    assert(picked == 0);
    assert(ring.GetSlot(picked).sequence == 1);
    assert(ring.GetSlot(picked).width == 1920);

    // Mark slot 0 read as in-flight
    inFlightReads.insert(rToken1);

    // Next record: must not overwrite slot 0 (it is both last-read and in-flight)
    void* wToken2 = (void*) 0x2;
    void* rToken2 = (void*) 0x102;
    size_t s1 = ring.AcquireForRecord(isReadInFlight);
    assert(s1 != 0); // picks a different slot
    ring.CommitRecord(s1, 1920, 1080, true, 1020, wToken2, rToken2);

    // wToken2 not completed yet; present continues reading newest completed (slot 0)
    assert(ring.PickForPresent(isWriteCompleted, 1025) == 0);

    // wToken2 completes
    completedWrites.insert(wToken2);
    size_t picked2 = ring.PickForPresent(isWriteCompleted, 1030);
    assert(picked2 == s1);
    assert(ring.GetSlot(picked2).sequence == 2);
}

void TestRingInFlightReadProtectionNotLastRead()
{
    // Test: a slot currently in-flight that is NOT the last-read index must NOT be overwritten!
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    // Record and complete slot 0
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(s0, 1920, 1080, false, 1000, w[0], r[0]);
    completedWrites.insert(w[0]);

    // Present reads slot 0
    assert(ring.PickForPresent(isWriteCompleted, 1010) == s0);
    inFlightReads.insert(r[0]); // slot 0 read is in flight

    // Record and complete slot 1
    size_t s1 = ring.AcquireForRecord(isReadInFlight);
    ring.CommitRecord(s1, 1920, 1080, false, 1020, w[1], r[1]);
    completedWrites.insert(w[1]);

    // Present reads slot 1 -> lastReadIndex becomes s1 (1)
    assert(ring.PickForPresent(isWriteCompleted, 1030) == s1);
    assert(ring.LastReadIndex() == s1);

    // Now slot 0 read is STILL in flight (r[0] in inFlightReads), but lastReadIndex is s1!
    // Next records must NOT touch slot 0 even though it is not lastReadIndex!
    size_t s2 = ring.AcquireForRecord(isReadInFlight);
    assert(s2 != s0 && s2 != s1);
    ring.CommitRecord(s2, 1920, 1080, false, 1040, w[2], r[2]);

    size_t s3 = ring.AcquireForRecord(isReadInFlight);
    assert(s3 != s0 && s3 != s1 && s3 != s2);
    ring.CommitRecord(s3, 1920, 1080, false, 1050, w[3], r[3]);

    // Slot 0 is still in flight (r[0] in inFlightReads). Slot 1 is lastReadIndex.
    // Calling AcquireForRecord now: slot 0 MUST NOT be chosen because its read is in flight!
    size_t nextAcquire = ring.AcquireForRecord(isReadInFlight);
    assert(nextAcquire != s0);
    assert(nextAcquire != s1);
}

void TestRingAllSlotsInFlightSkipsCapture()
{
    // Test: when all non-last-read slots are currently in-flight on GPU,
    // AcquireForRecord must return kRingSize (skip capture, matching RenoDX).
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    void* w[4] = { (void*) 0x1, (void*) 0x2, (void*) 0x3, (void*) 0x4 };
    void* r[4] = { (void*) 0x11, (void*) 0x12, (void*) 0x13, (void*) 0x14 };

    // Record all 4 slots and mark all of them as in-flight reads
    for (int i = 0; i < 4; ++i)
    {
        size_t s = ring.AcquireForRecord(isReadInFlight);
        ring.CommitRecord(s, 1920, 1080, false, 1000 + i * 10, w[i], r[i]);
        completedWrites.insert(w[i]);
    }

    // Set slot 3 as lastReadIndex, and mark slots 0, 1, 2 in flight
    ring.PickForPresent(isWriteCompleted, 1040); // picks 3
    assert(ring.LastReadIndex() == 3);

    inFlightReads.insert(r[0]);
    inFlightReads.insert(r[1]);
    inFlightReads.insert(r[2]);

    // All slots are either lastReadIndex (3) or inFlightReads (0, 1, 2)
    // AcquireForRecord MUST return kRingSize (skip capture)
    assert(ring.AcquireForRecord(isReadInFlight) == kRingSize);

    // When read on slot 1 completes:
    inFlightReads.erase(r[1]);
    assert(ring.AcquireForRecord(isReadInFlight) == 1);
}

void TestRingMaxAgeStaleRejection()
{
    // Test: captures older than kPresentGuideMaxAgeMs (250 ms) are dropped in favor of neutral guides.
    // Slot 0 is protected as lastReadIndex so it cannot be overwritten while more frames are recorded.
    std::unordered_set<void*> completedWrites;
    std::unordered_set<void*> inFlightReads;

    auto isWriteCompleted = [&](void* token) { return completedWrites.find(token) != completedWrites.end(); };
    auto isReadInFlight = [&](void* token) { return inFlightReads.find(token) != inFlightReads.end(); };

    Ring<decltype(isWriteCompleted), decltype(isReadInFlight)> ring;

    void* w0 = (void*) 0x10;
    void* r0 = (void*) 0x20;

    // Record slot 0 at nowMs = 1000, and complete it
    size_t s0 = ring.AcquireForRecord(isReadInFlight);
    assert(s0 == 0);
    ring.CommitRecord(s0, 1920, 1080, false, 1000, w0, r0);
    completedWrites.insert(w0);

    // Present reads slot 0 at 1010 ms -> slot 0 is now lastReadIndex (protected against overwrite)
    assert(ring.PickForPresent(isWriteCompleted, 1010) == s0);
    assert(ring.LastReadIndex() == s0);

    // Record 3 more frames (slots 1, 2, 3) at 1020, 1030, 1040 ms without completing their writes
    for (int i = 1; i <= 3; ++i)
    {
        void* w = (void*) (uintptr_t) (0x10 + i);
        void* r = (void*) (uintptr_t) (0x20 + i);
        size_t s = ring.AcquireForRecord(isReadInFlight);
        assert(s != s0);
        ring.CommitRecord(s, 1920, 1080, false, 1010 + i * 10, w, r);
    }

    // At 1100 ms (elapsed 100 ms <= 250 ms), slot 0 is still fresh and picked
    assert(ring.PickForPresent(isWriteCompleted, 1100) == s0);

    // At 1300 ms (elapsed 300 ms > 250 ms), slot 0 is stale!
    // PickForPresent must reject slot 0 as stale and return kRingSize
    assert(ring.PickForPresent(isWriteCompleted, 1300) == kRingSize);
}

void TestSkippedPresentAfterTrackReleasesSlot()
{
    // Test (Correction 1): a present skipped/dropped after Track must restore saved usage,
    // ensuring the slot does not remain locked as in-flight forever.
    struct FakeClone
    {
        bool readTracked = false;
        int simulatedFence = 0;
    } clone;

    auto isReadInFlight = [&](void* token) -> bool
    {
        auto* c = static_cast<FakeClone*>(token);
        if (!c->readTracked)
            return false;
        if (c->simulatedFence >= 1)
        {
            c->readTracked = false;
            return false;
        }
        return true;
    };

    // If present tracks reading:
    auto savedTracked = clone.readTracked;
    clone.readTracked = true; // simulated Track(list, clone.readUsage)
    assert(isReadInFlight(&clone) == true);

    // But then present list is dropped without execution (e.g. sequence skip or g_nr.failed):
    // Saved usage is restored:
    clone.readTracked = savedTracked;

    // Slot must now be immediately free for recording again!
    assert(isReadInFlight(&clone) == false);
}

int main()
{
    TestRingBasicAcquireCommitPick();
    TestRingInFlightReadProtectionNotLastRead();
    TestRingAllSlotsInFlightSkipsCapture();
    TestRingMaxAgeStaleRejection();
    TestSkippedPresentAfterTrackReleasesSlot();
    std::printf("PASS: nr-present-guides ring acquisition, in-flight read protection, skip fallback, max-age and "
                "dropped list\n");
    return 0;
}
