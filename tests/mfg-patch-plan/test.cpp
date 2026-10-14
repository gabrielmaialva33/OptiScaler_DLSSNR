// Host test for the MFG unlock's write half. See README.md.
//
// MfgPatchPlan.h is included from production unchanged. Its Commit writes a group of sites as one unit
// and undoes every touched site when any write or read-back fails; here the "module" is a byte buffer
// and the ops can be told to fail, or to land the wrong bytes, at any given write.

#include "MfgPatchPlan.h"

#include <cstdio>
#include <vector>

namespace
{
int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

// A buffer standing in for a mapped module. Writes are counted from one; the counters include the
// writes Commit makes while rolling back.
struct FakeMemory
{
    std::vector<uint8_t> bytes;

    int writes = 0;
    int failWrite = 0;   // this write returns false, after mangling its first byte
    int garbleWrite = 0; // this write returns true but lands the wrong bytes
    int failAllFrom = 0; // every write from this one on returns false

    uintptr_t At(size_t offset) { return reinterpret_cast<uintptr_t>(bytes.data()) + offset; }

    bool Read(uintptr_t address, uint8_t* out, size_t count)
    {
        std::memcpy(out, reinterpret_cast<const void*>(address), count);
        return true;
    }

    bool Write(uintptr_t address, const uint8_t* source, size_t count)
    {
        ++writes;
        auto* target = reinterpret_cast<uint8_t*>(address);

        if (writes == failWrite || (failAllFrom != 0 && writes >= failAllFrom))
        {
            // A write that fails part way: the first byte changed, the rest did not.
            target[0] ^= 0xFF;
            return false;
        }

        std::memcpy(target, source, count);

        if (writes == garbleWrite)
            target[count - 1] ^= 0x5A;

        return true;
    }
};

FakeMemory Module()
{
    FakeMemory memory;
    for (int i = 0; i < 64; ++i)
        memory.bytes.push_back(static_cast<uint8_t>(0x10 + i));
    return memory;
}

// Two gates in the shape the unlock plans them: a six-byte branch at 8 and a one-byte count at 40.
std::vector<MfgPatch::Site> TwoGates(FakeMemory& memory)
{
    std::vector<MfgPatch::Site> plan;
    const uint8_t nop[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 };
    const uint8_t five[] = { 0x05 };
    MfgPatch::AddSite(plan, memory, memory.At(8), nop, sizeof(nop));
    MfgPatch::AddSite(plan, memory, memory.At(40), five, sizeof(five));
    return plan;
}
} // namespace

int main()
{
    std::printf("commit\n");
    {
        auto memory = Module();
        const auto pristine = memory.bytes;
        const auto plan = TwoGates(memory);

        check(plan.size() == 2, "both sites planned");
        check(memory.bytes == pristine, "planning writes nothing");
        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::Committed, "two good writes commit");
        check(memory.bytes[8] == 0x0F && memory.bytes[13] == 0x90 && memory.bytes[40] == 0x05,
              "both sites hold their replacements");
        check(memory.bytes[7] == pristine[7] && memory.bytes[14] == pristine[14] && memory.bytes[41] == pristine[41],
              "the bytes around them are untouched");
    }

    std::printf("a site that already holds its replacement\n");
    {
        auto memory = Module();
        memory.bytes[40] = 0x05;
        std::vector<MfgPatch::Site> plan;
        const uint8_t five[] = { 0x05 };
        MfgPatch::AddSite(plan, memory, memory.At(40), five, sizeof(five));

        check(plan[0].NoOp(), "a count already at five is a no-op site");
        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::AlreadyApplied,
              "nothing to write is AlreadyApplied");
        check(memory.writes == 0, "and nothing was written");
    }

    std::printf("a write that fails part way through the group\n");
    {
        auto memory = Module();
        const auto pristine = memory.bytes;
        const auto plan = TwoGates(memory);
        memory.failWrite = 2;

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::RolledBack, "the second write failing rolls back");
        check(memory.bytes == pristine, "the first gate and the half-written second are both restored");
    }

    std::printf("a write that lands the wrong bytes\n");
    {
        auto memory = Module();
        const auto pristine = memory.bytes;
        const auto plan = TwoGates(memory);
        memory.garbleWrite = 1;

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::RolledBack, "a failed read-back rolls back");
        check(memory.bytes == pristine, "every site holds its original again");
    }

    std::printf("a module that changed between plan and commit\n");
    {
        auto memory = Module();
        const auto plan = TwoGates(memory);
        memory.bytes[41] = 0xEE; // outside every site: not a change Commit should care about
        memory.bytes[40] = 0x03; // inside one: someone else wrote here

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::Stale, "a changed site is Stale");
        check(memory.writes == 0, "and nothing was written");
    }

    std::printf("a rollback that cannot complete\n");
    {
        auto memory = Module();
        const auto pristine = memory.bytes;
        const auto plan = TwoGates(memory);
        // Write 1 lands, write 2 fails, and so does every restore after it.
        memory.failAllFrom = 2;

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::RollbackFailed,
              "a failed restore is RollbackFailed");
        check(memory.bytes != pristine, "and the module is left changed, which is what that outcome says");
    }

    std::printf("a third write failing restores the two before it\n");
    {
        auto memory = Module();
        const auto pristine = memory.bytes;
        std::vector<MfgPatch::Site> plan;
        const uint8_t a[] = { 0xAA };
        const uint8_t b[] = { 0xBB };
        const uint8_t c[] = { 0xCC };
        MfgPatch::AddSite(plan, memory, memory.At(2), a, 1);
        MfgPatch::AddSite(plan, memory, memory.At(20), b, 1);
        MfgPatch::AddSite(plan, memory, memory.At(50), c, 1);
        // Writes 1-2 land and 3 fails; the restores are writes 4-6 and all land.
        memory.failWrite = 3;

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::RolledBack, "the group rolls back");
        check(memory.bytes == pristine, "all three sites restored");
    }

    std::printf("one stuck restore does not stop the others\n");
    {
        // The restore of the first site (write 4) fails; the restores of the other two must still run.
        struct StuckFirstRestore : FakeMemory
        {
            bool Write(uintptr_t address, const uint8_t* source, size_t count)
            {
                if (writes + 1 == 4)
                {
                    ++writes;
                    return false;
                }

                return FakeMemory::Write(address, source, count);
            }
        } memory;
        memory.bytes = Module().bytes;

        std::vector<MfgPatch::Site> plan;
        const uint8_t a[] = { 0xAA };
        const uint8_t b[] = { 0xBB };
        const uint8_t c[] = { 0xCC };
        MfgPatch::AddSite(plan, memory, memory.At(2), a, 1);
        MfgPatch::AddSite(plan, memory, memory.At(20), b, 1);
        MfgPatch::AddSite(plan, memory, memory.At(50), c, 1);
        memory.failWrite = 3;

        check(MfgPatch::Commit(plan, memory) == MfgPatch::Outcome::RollbackFailed,
              "a stuck restore of the first site is RollbackFailed");
        check(memory.bytes[20] == static_cast<uint8_t>(0x10 + 20) &&
                  memory.bytes[50] == static_cast<uint8_t>(0x10 + 50),
              "the sites after it were still restored");
    }

    std::printf("plan limits\n");
    {
        auto memory = Module();
        std::vector<MfgPatch::Site> plan;
        uint8_t big[MfgPatch::kMaxSiteBytes + 1] = {};
        check(!MfgPatch::AddSite(plan, memory, memory.At(0), big, 0), "an empty site is refused");
        check(!MfgPatch::AddSite(plan, memory, memory.At(0), big, sizeof(big)),
              "a site longer than the cap is refused");
        check(MfgPatch::AddSite(plan, memory, memory.At(0), big, MfgPatch::kMaxSiteBytes),
              "the fourteen-byte .target rewrite fits under the cap");
        check(plan.size() == 1, "only the valid site was recorded");
    }

    if (g_failures != 0)
    {
        std::printf("FAIL: %d check(s) failed\n", g_failures);
        return 1;
    }

    std::printf("PASS: MFG patch groups commit whole or roll back whole\n");
    return 0;
}
