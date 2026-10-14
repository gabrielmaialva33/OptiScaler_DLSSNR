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

// The interpolation list at whose end generated frame `frame` is complete, when its entry executed `lists` of them
// (MFG_PRESENTER.md 4.2): frame k in list k, any frame past the last list in the last one; one list holds them all.
// The generator places frame k with the same function over the lists it was handed (Lsfg::FfxDispatch), so it uses
// min(generated, handed) of them, and the presenter's mapping over that count picks the same list.
inline uint32_t ListForFrame(uint32_t frame, uint32_t lists) { return lists == 0 ? 0 : std::min(frame, lists - 1); }

// GPU timestamps of one entry's interpolation lists: [0] the start of its first list, [1 + i] the end of list i.
constexpr uint32_t kTimestampsPerEntry = 1 + kMaxGeneratedFrames;

// Generated frame k of an entry is complete `readyMs[k]` after its first interpolation list started, k < generated.
// False, and nothing written, unless the slot holds this entry's values: a slot whose resolve has not landed still
// holds an older entry's, whose start is not after `lastBegin` (the newest start taken, updated on success); an end
// before its start or an absurd span is dropped too.
inline bool ReadyTimes(const uint64_t* stamps, uint32_t lists, uint32_t generated, uint64_t frequency,
                       uint64_t& lastBegin, double* readyMs)
{
    if (stamps == nullptr || frequency == 0 || lists == 0 || lists > kMaxGeneratedFrames || generated == 0 ||
        generated > kMaxGeneratedFrames)
        return false;

    const uint64_t begin = stamps[0];
    if (begin == 0 || begin <= lastBegin)
        return false;

    double ms[kMaxGeneratedFrames] {};
    for (uint32_t k = 0; k < generated; ++k)
    {
        const uint64_t end = stamps[1 + ListForFrame(k, lists)];
        if (end < begin)
            return false;
        ms[k] = double(end - begin) * 1000.0 / double(frequency);
        if (ms[k] >= 1000.0)
            return false;
    }

    lastBegin = begin;
    for (uint32_t k = 0; k < generated; ++k)
        readyMs[k] = ms[k];
    return true;
}

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

    // Latency (MFG_PRESENTER.md 6): generated frame k complete on the GPU after its entry's first interpolation list
    // started; generated frame k and the real frame presented after the game handed its frame over (QPC); and how
    // many interpolation lists each entry executed.
    std::vector<double> readyMs[kMaxGeneratedFrames];
    std::vector<double> shownGeneratedMs[kMaxGeneratedFrames];
    std::vector<double> shownRealMs;
    uint64_t interpolationLists = 0;
    uint64_t interpolatedEntries = 0;

    void AddEntry(uint32_t generatedFrames) { entriesByGenerated[std::min(generatedFrames, kMaxGeneratedFrames)]++; }

    void AddLists(uint32_t lists)
    {
        interpolationLists += lists;
        interpolatedEntries++;
    }

    void AddReady(uint32_t frame, double ms)
    {
        if (frame < kMaxGeneratedFrames)
            readyMs[frame].push_back(ms);
    }

    void AddShown(bool isGenerated, uint32_t frame, double ms)
    {
        if (!isGenerated)
            shownRealMs.push_back(ms);
        else if (frame < kMaxGeneratedFrames)
            shownGeneratedMs[frame].push_back(ms);
    }

    double ListsPerEntry() const
    {
        return interpolatedEntries ? double(interpolationLists) / double(interpolatedEntries) : 0.0;
    }

    // p in [0, 1]; 0 with no samples. Sorts the samples.
    static double PercentileOf(std::vector<double>& samples, double p)
    {
        if (samples.empty())
            return 0.0;
        std::sort(samples.begin(), samples.end());
        const size_t index = std::min(samples.size() - 1, size_t(double(samples.size()) * p));
        return samples[index];
    }

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

    // The present intervals' percentile.
    double Percentile(double p) { return PercentileOf(intervalsMs, p); }

    double MeanTargetMs() const { return targets ? targetSumMs / double(targets) : 0.0; }
    double GeneratedPerReal() const { return real ? double(generated) / double(real) : 0.0; }
};

} // namespace LsfgMfg
