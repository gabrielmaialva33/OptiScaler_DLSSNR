#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// The write half of the MFG unlock, separated from the search so it can be tested on the host.
//
// MfgUnlock first finds every site a patch needs and records, per site, the bytes it found and the bytes
// it wants. Only then does it write, and it writes a group of sites as one unit: either every site in the
// group ends up holding its replacement, read back and compared, or every site that was touched is put
// back to the bytes it held before. Without that, a gate pair could end up half applied -- the provider
// advertising five generated frames while its validator still accepts one -- which is worse than no
// unlock at all, because the game asks for a count the provider then refuses.
//
// No Windows here. The caller supplies Read and Write; the production ones are a memcpy and the
// VirtualProtect write in MfgUnlock.cpp, and tests/mfg-pattern drives this with a buffer that can be
// told to fail any given write.
namespace MfgPatch
{
// Longest replacement any patch writes: the six-byte jl/je rewrites, and the fourteen-byte .target
// directive of the kernel swap.
constexpr size_t kMaxSiteBytes = 16;

struct Site
{
    uintptr_t at = 0;
    uint8_t size = 0;
    std::array<uint8_t, kMaxSiteBytes> original {};
    std::array<uint8_t, kMaxSiteBytes> replacement {};

    // A site whose replacement equals what is already there -- a count that is already five -- is kept
    // in the plan, so the outcome still reports it, but it is never written.
    bool NoOp() const { return std::memcmp(original.data(), replacement.data(), size) == 0; }
};

// Records a site from the bytes currently at `at`. Returns false when the replacement does not fit.
template <typename Ops>
bool AddSite(std::vector<Site>& plan, Ops& ops, uintptr_t at, const uint8_t* replacement, size_t size)
{
    if (size == 0 || size > kMaxSiteBytes)
        return false;

    Site site;
    site.at = at;
    site.size = static_cast<uint8_t>(size);

    if (!ops.Read(at, site.original.data(), size))
        return false;

    std::memcpy(site.replacement.data(), replacement, size);
    plan.push_back(site);
    return true;
}

enum class Outcome
{
    Committed,      // every site holds its replacement, read back
    AlreadyApplied, // every site already held its replacement; nothing written
    Stale,          // a site no longer held the bytes it was planned from; nothing written
    RolledBack,     // a write or the read-back failed; every touched site holds its original again
    RollbackFailed, // and putting a site back failed too: the module is in an unknown state
};

namespace detail
{
template <typename Ops> bool Holds(Ops& ops, const Site& site, const std::array<uint8_t, kMaxSiteBytes>& expected)
{
    std::array<uint8_t, kMaxSiteBytes> now {};
    return ops.Read(site.at, now.data(), site.size) && std::memcmp(now.data(), expected.data(), site.size) == 0;
}

// Puts back every site in [0, count). Every one is attempted even after a failure, so one stuck page
// does not leave the others patched.
template <typename Ops> bool Restore(const std::vector<Site>& plan, size_t count, Ops& ops)
{
    bool restored = true;

    for (size_t i = 0; i < count; ++i)
    {
        const Site& site = plan[i];

        if (site.NoOp())
            continue;

        if (!ops.Write(site.at, site.original.data(), site.size) || !Holds(ops, site, site.original))
            restored = false;
    }

    return restored;
}
} // namespace detail

template <typename Ops> Outcome Commit(const std::vector<Site>& plan, Ops& ops)
{
    // Re-read before writing anything. The plan was made under the same lock, so this is about another
    // module -- an overlay or a second unlocker -- having written in between, not about our own threads.
    for (const Site& site : plan)
    {
        if (!detail::Holds(ops, site, site.original))
            return Outcome::Stale;
    }

    bool wroteAny = false;

    for (size_t i = 0; i < plan.size(); ++i)
    {
        const Site& site = plan[i];

        if (site.NoOp())
            continue;

        // A failed write may still have changed some of its bytes, so site i is restored with the rest.
        if (!ops.Write(site.at, site.replacement.data(), site.size))
            return detail::Restore(plan, i + 1, ops) ? Outcome::RolledBack : Outcome::RollbackFailed;

        wroteAny = true;
    }

    for (const Site& site : plan)
    {
        if (!detail::Holds(ops, site, site.replacement))
            return detail::Restore(plan, plan.size(), ops) ? Outcome::RolledBack : Outcome::RollbackFailed;
    }

    return wroteAny ? Outcome::Committed : Outcome::AlreadyApplied;
}

inline const char* OutcomeName(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::Committed:
        return "committed";
    case Outcome::AlreadyApplied:
        return "already applied";
    case Outcome::Stale:
        return "stale";
    case Outcome::RolledBack:
        return "rolled back";
    case Outcome::RollbackFailed:
        return "ROLLBACK FAILED";
    }

    return "?";
}
} // namespace MfgPatch
