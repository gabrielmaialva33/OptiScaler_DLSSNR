#pragma once

#include "DlssNr_SubmissionModel.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace DlssNr::GpuTiming::Detail
{
constexpr size_t SlotCount = 16;
constexpr size_t MaxModels = 3;
constexpr size_t MaxQueries = 2 + 2 * MaxModels;

struct Certificate
{
    bool accepted = false;
    bool terminal = false;
    bool reusable = false;
    uint64_t frequency = 0;
    uint64_t fence = 0;
    size_t queue = Submission::Detail::MaxQueues;
    const char* reason = "pending";
};

// Called under the submission state lock. Frequency and completion belong to the actual queue.
template <typename Completed, typename Frequency>
Certificate Certify(const Submission::Detail::Epoch& epoch, Completed completed, Frequency frequency)
{
    Certificate result;
    if (epoch.unknown)
        return { false, true, false, 0, 0, Submission::Detail::MaxQueues, "unknown-submission" };
    const bool ready = Submission::Detail::IsReady(epoch, completed);
    size_t count = 0;
    for (size_t q = 0; q < epoch.fences.size(); ++q)
    {
        if (!epoch.fences[q])
            continue;
        ++count;
        result.queue = q;
        result.fence = epoch.fences[q];
        if (completed(q) == std::numeric_limits<uint64_t>::max())
            return { false, true, false, 0, 0, q, "device-lost" };
    }
    if (count > 1)
        return { false, true, ready, 0, 0, result.queue, "multiple-queues" };
    if (epoch.executions > 1)
        return { false, true, ready, 0, 0, result.queue, "replayed-recording" };
    if (epoch.pending)
    {
        result.reason = "waiting-notification";
        return result;
    }
    if (!epoch.sealed)
    {
        result.reason = epoch.submitted ? "waiting-reset" : "unsubmitted-recording";
        return result;
    }
    if (!epoch.submitted)
        return { false, true, false, 0, 0, result.queue, "not-submitted" };
    if (!ready)
    {
        result.reason = "waiting-gpu";
        return result;
    }
    if (epoch.executions != 1 || count != 1)
        return { false, true, true, 0, 0, result.queue, "unproven-pairing" };
    result.frequency = frequency(result.queue);
    if (!result.frequency)
        return { false, true, true, 0, result.fence, result.queue, "frequency-unavailable" };
    // The frequency call is a driver call; device removal can be observed after the earlier read.
    const auto finalCompleted = completed(result.queue);
    if (finalCompleted == std::numeric_limits<uint64_t>::max())
        return { false, true, false, 0, result.fence, result.queue, "device-lost" };
    if (finalCompleted < result.fence)
    {
        result.reason = "waiting-gpu";
        return result;
    }
    result.accepted = result.terminal = result.reusable = true;
    result.reason = "confirmed";
    return result;
}

template <typename Completed, typename Frequency>
Certificate CertifyUsage(const Submission::Detail::Usage& usage, Completed completed, Frequency frequency)
{
    const Submission::Detail::Epoch* single = nullptr;
    for (const auto& epoch : usage.epochs)
    {
        if (!epoch)
            continue;
        if (single)
            return { false, true, false, 0, 0, Submission::Detail::MaxQueues, "multiple-recordings" };
        single = epoch.get();
    }
    if (!single)
        return { false, true, false, 0, 0, Submission::Detail::MaxQueues, "empty-recording" };
    return Certify(*single, completed, frequency);
}

struct Durations
{
    double totalMs = 0.0;
    double modelMs = 0.0;
    double outsideModelMs = 0.0;
};

// Layout: total begin, N adjacent model begin/end pairs, total end. Sum explicit gaps.
inline bool Decode(const uint64_t* values, size_t queryCount, size_t models, uint64_t frequency, Durations& result)
{
    result = {};
    if (!values || !frequency || models == 0 || models > MaxModels || queryCount != 2 + models * 2)
        return false;
    for (size_t i = 1; i < queryCount; ++i)
        if (values[i] < values[i - 1])
            return false; // Includes counter wrap; do not guess the counter width.
    uint64_t modelTicks = 0;
    uint64_t outsideTicks = 0;
    for (size_t model = 0; model < models; ++model)
    {
        const size_t begin = 1 + 2 * model;
        modelTicks += values[begin + 1] - values[begin];
        outsideTicks += values[begin] - values[begin - 1];
    }
    outsideTicks += values[queryCount - 1] - values[queryCount - 2];
    const auto totalTicks = values[queryCount - 1] - values[0];
    // Zero total is not useful evidence of GPU execution.
    if (!totalTicks || modelTicks > totalTicks || outsideTicks != totalTicks - modelTicks)
        return false;
    const double millisecondsPerTick = 1000.0 / static_cast<double>(frequency);
    result.totalMs = static_cast<double>(totalTicks) * millisecondsPerTick;
    result.modelMs = static_cast<double>(modelTicks) * millisecondsPerTick;
    result.outsideModelMs = static_cast<double>(outsideTicks) * millisecondsPerTick;
    return true;
}
} // namespace DlssNr::GpuTiming::Detail
