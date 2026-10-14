#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): the arithmetic of the MFG presenter, the fork of FFX's frame
// interpolation swapchain that presents N generated frames per real frame (MfgSwapChain_Dx12.cpp,
// tools/lsfg-study/MFG_PRESENTER.md). Header-only and free of <windows.h>, so tests/lsfg-presenter holds it on the
// host tier.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace LsfgMfg
{

// x4 at most: three generated frames and the real one per present of the game.
constexpr uint32_t kMaxGeneratedFrames = 3;
constexpr uint32_t kMaxFramesPerEntry = kMaxGeneratedFrames + 1;

// The generated frames are double-buffered as sets: present p writes set p % 2 while the presenter may still show
// set (p - 1) % 2 (FFX's two interpolation outputs, FrameInterpolationSwapchainDX12.h:43, as sets of N).
constexpr uint32_t kOutputSets = 2;

// [LSFG] Multiplier: 2, 3 or 4. Anything else is read as 2 (MFG_PRESENTER.md section 5).
inline bool MultiplierValid(int multiplier) { return multiplier >= 2 && multiplier <= 4; }

// Generated frames per real frame for a multiplier: 1, 2 or 3.
inline uint32_t GeneratedFramesFor(int multiplier)
{
    return MultiplierValid(multiplier) ? uint32_t(multiplier - 1) : 1u;
}

// The delta between consecutive presents of one entry, in QPC ticks. FFX gives every present half the averaged real
// frame time, less the variance term and a safety margin (FrameInterpolationSwapchainDX12.cpp:882-886); here the real
// frame time is split over the frames the entry presents, so x2 is FFX's formula exactly and x4 presents every
// quarter frame. An entry with only its real frame waits a whole frame from the previous present. 0 (present at
// once) while the average is not established, as in FFX.
inline int64_t PresentDelta(double averageTicks, double stdDevTicks, uint32_t framesToPresent, double varianceFactor,
                            int64_t safetyMarginTicks)
{
    if (framesToPresent == 0)
        framesToPresent = 1;

    const int64_t conservative = int64_t(averageTicks / double(framesToPresent) - stdDevTicks * varianceFactor);
    return conservative > safetyMarginTicks ? conservative - safetyMarginTicks : 0;
}

// The output set present `presentCount` writes.
inline uint32_t OutputSetFor(uint64_t presentCount) { return uint32_t(presentCount % kOutputSets); }

// The composition fence value after which a set is free again: the present index of the last generated frame shown
// from it. Composition runs in order on one queue, so it covers every frame of the set. With nothing generated it is
// the entry's base, which the previous present already reached.
inline uint64_t SetAvailableAfter(uint64_t entryBase, uint32_t generated) { return entryBase + generated; }

// The presenter's 10 s summary (MFG_PRESENTER.md section 6): real and generated presents, entries by how many
// generated frames they carried, and the QPC interval between consecutive presents against the pacing target.
struct PresentWindow
{
    uint64_t real = 0;
    uint64_t generated = 0;
    uint64_t entriesByGenerated[kMaxFramesPerEntry] {};
    std::vector<double> intervalsMs;
    double targetSumMs = 0.0;
    uint64_t targets = 0;
    uint64_t overTarget = 0; // intervals over 1.5 x their target (a target of 0 never counts)

    void AddEntry(uint32_t generatedFrames) { entriesByGenerated[std::min(generatedFrames, kMaxGeneratedFrames)]++; }

    void AddPresent(bool isGenerated, double intervalMs, double targetMs)
    {
        (isGenerated ? generated : real)++;
        if (intervalMs < 0.0)
            return; // the first present of a window has no predecessor
        intervalsMs.push_back(intervalMs);
        if (targetMs > 0.0)
        {
            targetSumMs += targetMs;
            targets++;
            if (intervalMs > 1.5 * targetMs)
                overTarget++;
        }
    }

    // p in [0, 1]; 0 with no samples. Sorts the samples.
    double Percentile(double p)
    {
        if (intervalsMs.empty())
            return 0.0;
        std::sort(intervalsMs.begin(), intervalsMs.end());
        const size_t index = std::min(intervalsMs.size() - 1, size_t(double(intervalsMs.size()) * p));
        return intervalsMs[index];
    }

    double MeanTargetMs() const { return targets ? targetSumMs / double(targets) : 0.0; }
    double GeneratedPerReal() const { return real ? double(generated) / double(real) : 0.0; }
};

} // namespace LsfgMfg
