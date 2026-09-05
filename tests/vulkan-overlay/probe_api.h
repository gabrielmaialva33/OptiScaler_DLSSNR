#pragma once
#include <cstdint>

// Test-only ABI. No production source or solution references this header.
struct VkLifetimeStats
{
    uint64_t abi = 1;
    uint64_t createCalls = 0;
    uint64_t destroyCalls = 0;
    uint64_t teardownCalls = 0;
    uint64_t presentCalls = 0;
    uint64_t overlaySubmits = 0;
    uint64_t allocations = 0;
    uint64_t releases = 0;
    uint64_t liveBytes = 0;
    uint64_t peakBytes = 0;
    uint64_t objectsCreated = 0;
    uint64_t objectsDestroyed = 0;
    uint64_t drainFailures = 0;
    uint64_t partialFailures = 0;
    uint64_t ready = 0;
    uint64_t imageCount = 0;
};
using GetStats = void (*)(VkLifetimeStats*);
using ArmFailure = void (*)(unsigned);
using Control = void (*)();
