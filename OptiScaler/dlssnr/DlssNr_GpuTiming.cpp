#include "pch.h"

#include "DlssNr_GpuTiming.h"
#include "DlssNr_GpuTimingModel.h"
#include "DlssNr_Submission.h"
#include <Logger.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace DlssNr::GpuTiming
{
namespace
{
using Clock = std::chrono::steady_clock;
struct Slot
{
    ID3D12Device* device = nullptr; // Explicitly owned until the slot is safe to release.
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    Submission::Usage usage {};
    Metadata metadata {};
    Clock::time_point recordedAt {};
    uint64_t id = 0;
    uint32_t interval = 1;
    uint32_t queryCount = 0;
    uint32_t models = 0;
    uint32_t actualPasses = 0;
    bool occupied = false;
    bool recording = false;
    bool inModel = false;
    bool malformed = false;
    bool successful = false;
    bool reported = false;
    const char* rejected = nullptr;
};

std::mutex mutex;
std::atomic<bool> everEnabled { false };
// Deliberately raw COM ownership: static destruction must not release GPU-pending objects.
std::array<Slot, Detail::SlotCount> slots {};
Snapshot snapshot {};
Clock::time_point acceptedAt {};
Clock::time_point latestRecordedAt {};
Clock::time_point lastSummary {};
std::array<Clock::time_point, 24> lastDiscardLog {};

const char* StageLabel(const char* value)
{
    // Never retain a caller's temporary string in an asynchronous sample.
    if (value)
        for (const auto* label : { "before", "after", "after-fallback" })
            if (std::strcmp(label, value) == 0)
                return label;
    return "unknown";
}

void Drop(const char* reason, Slot* slot = nullptr)
{
    if (slot && slot->reported)
        return;
    if (slot)
        slot->reported = true;
    ++snapshot.discarded;
    snapshot.reason = reason;
    size_t index = snapshot.reasons.size() - 1;
    for (size_t i = 0; i + 1 < snapshot.reasons.size(); ++i)
    {
        auto& entry = snapshot.reasons[i];
        if (!entry.count || std::strcmp(entry.reason, reason) == 0)
        {
            index = i;
            entry.reason = reason;
            break;
        }
    }
    auto& entry = snapshot.reasons[index];
    if (index + 1 == snapshot.reasons.size())
        entry.reason = "reason-overflow";
    ++entry.count;
    const auto now = Clock::now();
    auto& last = lastDiscardLog[index];
    if (entry.count != 1 && now - last < std::chrono::seconds(5))
        return;
    last = now;
    try
    {
        LOG_INFO("DLSS-NR gpu timing discard: run={} sample={} reason={} reason_count={} discarded={} accepted={} "
                 "pending={} interval={} unit=eligible-evaluations",
                 snapshot.runId, slot ? slot->id : 0, reason, entry.count, snapshot.discarded, snapshot.accepted,
                 snapshot.pending, snapshot.interval);
    }
    catch (...)
    {
        // Diagnostics must not interrupt the rendering path on allocation failure.
    }
}

void ReleaseSafe(Slot& slot)
{
    if (slot.heap)
        slot.heap->Release();
    if (slot.readback)
        slot.readback->Release();
    if (slot.device)
        slot.device->Release();
    slot.heap = nullptr;
    slot.readback = nullptr;
    slot.device = nullptr;
}

bool Allocate(Slot& slot, ID3D12Device* device)
{
    if (slot.device == device && slot.heap && slot.readback)
        return true;
    ReleaseSafe(slot); // Only unoccupied, never-recorded or certified reusable slots reach here.
    D3D12_QUERY_HEAP_DESC query {};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = static_cast<UINT>(Detail::MaxQueries);
    if (FAILED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&slot.heap))))
        return false;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC resource {};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = Detail::MaxQueries * sizeof(uint64_t);
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&slot.readback))))
    {
        ReleaseSafe(slot);
        return false;
    }
    device->AddRef();
    slot.device = device;
    return true;
}

void PollLocked()
{
    snapshot.pending = 0;
    snapshot.waitingReset = snapshot.waitingGpu = snapshot.waitingNotification = snapshot.retained = 0;
    for (auto& slot : slots)
    {
        if (!slot.occupied)
            continue;
        ++snapshot.pending;
        if (slot.recording)
            continue;
        const auto certificate = Submission::TimingCertificate(slot.usage);
        if (!certificate.terminal)
        {
            if (std::strcmp(certificate.reason, "waiting-reset") == 0)
                ++snapshot.waitingReset;
            else if (std::strcmp(certificate.reason, "waiting-gpu") == 0)
                ++snapshot.waitingGpu;
            else
                ++snapshot.waitingNotification;
            continue;
        }
        if (!certificate.reusable)
            ++snapshot.retained;
        if (!certificate.accepted)
            Drop(certificate.reason, &slot);
        else if (!slot.reported)
        {
            if (!slot.successful || slot.malformed || slot.inModel || slot.models != slot.actualPasses)
                Drop("incomplete-evaluation", &slot);
            else
            {
                void* mapped = nullptr;
                const D3D12_RANGE readRange { 0, slot.queryCount * sizeof(uint64_t) };
                const auto result = slot.readback->Map(0, &readRange, &mapped);
                if (FAILED(result) || !mapped)
                {
                    if (SUCCEEDED(result))
                    {
                        const D3D12_RANGE written { 0, 0 };
                        slot.readback->Unmap(0, &written);
                    }
                    Drop("readback-map-failed", &slot);
                }
                else
                {
                    std::array<uint64_t, Detail::MaxQueries> values {};
                    std::memcpy(values.data(), mapped, readRange.End);
                    const D3D12_RANGE written { 0, 0 };
                    slot.readback->Unmap(0, &written);
                    Detail::Durations durations;
                    if (!Detail::Decode(values.data(), slot.queryCount, slot.models, certificate.frequency, durations))
                        Drop("invalid-timestamps", &slot);
                    else
                    {
                        ++snapshot.accepted;
                        if (!snapshot.valid || slot.id > snapshot.sampleId)
                        {
                            snapshot.valid = true;
                            snapshot.sampleId = slot.id;
                            snapshot.sampleInterval = slot.interval;
                            snapshot.metadata = slot.metadata;
                            snapshot.actualPasses = slot.actualPasses;
                            snapshot.totalMs = durations.totalMs;
                            snapshot.modelMs = durations.modelMs;
                            snapshot.otherMs = durations.outsideModelMs;
                            snapshot.reason = "confirmed";
                            acceptedAt = Clock::now();
                            latestRecordedAt = slot.recordedAt;
                        }
                        slot.reported = true;
                        try
                        {
                            LOG_INFO(
                                "DLSS-NR gpu timing confirmed: run={} sample={} evaluation={} present={} stage={} "
                                "contract={} generation={} model_identity={} render={}x{} compose={}x{} model={}x{} "
                                "working_scale={} model_reset={} capture_active={} passes={}/{} dispatch_ms={:.6f} "
                                "model_ms={:.6f} outside_model_ms={:.6f} "
                                "queue={} fence={} frequency_hz={} interval={} unit=eligible-evaluations "
                                "accepted={} discarded={} proof=sealed-single-execution-fence",
                                snapshot.runId, slot.id, slot.metadata.evaluationId, slot.metadata.presentId,
                                slot.metadata.stage, slot.metadata.contractHash, slot.metadata.settingsGeneration,
                                slot.metadata.modelIdentity, slot.metadata.renderWidth, slot.metadata.renderHeight,
                                slot.metadata.outputWidth, slot.metadata.outputHeight, slot.metadata.modelWidth,
                                slot.metadata.modelHeight, slot.metadata.workingScale, slot.metadata.modelReset,
                                slot.metadata.captureActive, slot.actualPasses, slot.metadata.requestedPasses,
                                durations.totalMs, durations.modelMs, durations.outsideModelMs, certificate.queue,
                                certificate.fence, certificate.frequency, slot.interval, snapshot.accepted,
                                snapshot.discarded);
                        }
                        catch (...)
                        {
                            // Diagnostics must not interrupt the rendering path on allocation failure.
                        }
                    }
                }
            }
        }
        if (certificate.reusable)
        {
            slot.usage = {};
            slot.occupied = false;
            if (!snapshot.enabled)
                ReleaseSafe(slot);
            --snapshot.pending;
        }
    }
    const auto now = Clock::now();
    if (now - lastSummary >= std::chrono::seconds(5))
    {
        lastSummary = now;
        try
        {
            LOG_INFO(
                "DLSS-NR gpu timing coverage: run={} eligible={} attempted={} recorded={} accepted={} discarded={} "
                "pending={} waiting_reset={} waiting_gpu={} waiting_notification={} retained={} interval={} "
                "unit=eligible-evaluations enabled={}",
                snapshot.runId, snapshot.eligible, snapshot.attempted, snapshot.sampled, snapshot.accepted,
                snapshot.discarded, snapshot.pending, snapshot.waitingReset, snapshot.waitingGpu,
                snapshot.waitingNotification, snapshot.retained, snapshot.interval, snapshot.enabled);
        }
        catch (...)
        {
            // Diagnostics must not interrupt the rendering path on allocation failure.
        }
    }
}

void Stamp(Slot& slot, ID3D12GraphicsCommandList* list)
{
    if (slot.queryCount >= Detail::MaxQueries)
    {
        slot.malformed = true;
        return;
    }
    list->EndQuery(slot.heap, D3D12_QUERY_TYPE_TIMESTAMP, slot.queryCount++);
}
} // namespace

void Poll()
{
    if (!everEnabled.load(std::memory_order_relaxed))
        return;
    std::lock_guard lock(mutex);
    if (snapshot.sampled)
        PollLocked();
}

void SetEnabled(bool enabled)
{
    if (!enabled && !everEnabled.load(std::memory_order_relaxed))
        return;
    if (enabled)
        everEnabled.store(true, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    snapshot.enabled = enabled;
    if (snapshot.sampled)
        PollLocked();
    if (!enabled)
        for (auto& slot : slots)
            if (!slot.occupied)
                ReleaseSafe(slot);
}

Snapshot GetSnapshot()
{
    std::lock_guard lock(mutex);
    auto result = snapshot;
    if (result.valid)
    {
        result.sampleAgeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - latestRecordedAt).count();
        result.collectedAgeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - acceptedAt).count();
    }
    return result;
}

Evaluation::Evaluation(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const Metadata& metadata,
                       bool enabled, uint32_t interval)
{
    if (!enabled && !everEnabled.load(std::memory_order_relaxed))
        return;
    if (enabled)
        everEnabled.store(true, std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    snapshot.enabled = enabled;
    if (snapshot.sampled)
        PollLocked();
    if (!enabled)
    {
        for (auto& candidate : slots)
            if (!candidate.occupied)
                ReleaseSafe(candidate);
        return;
    }
    if (!snapshot.runId)
        snapshot.runId = static_cast<uint64_t>(Clock::now().time_since_epoch().count()) ^ GetCurrentProcessId();
    interval = std::clamp(interval, 1u, 10000u);
    snapshot.interval = interval;
    ++snapshot.eligible;
    if ((snapshot.eligible - 1) % interval != 0)
        return;
    ++snapshot.attempted;
    if (!device || !commandList || commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        Drop("unsupported-command-list");
        return;
    }
    Slot* available = nullptr;
    for (auto& candidate : slots)
        if (!candidate.occupied)
        {
            available = &candidate;
            break;
        }
    if (!available)
    {
        Drop("pool-full");
        return;
    }
    ID3D12Device* recordingDevice = nullptr;
    if (FAILED(commandList->GetDevice(IID_PPV_ARGS(&recordingDevice))) || !recordingDevice)
    {
        Drop("recording-device-unavailable");
        return;
    }
    const bool allocated = Allocate(*available, recordingDevice);
    recordingDevice->Release();
    if (!allocated)
    {
        Drop("query-allocation-failed");
        return;
    }
    // Independent from NR resource-retirement mode; refusal drops measurement only.
    if (!Submission::Track(commandList, available->usage))
    {
        Drop(Submission::LastRefusal());
        available->usage = {}; // Track may fail after filling an epoch; no query was recorded.
        return;
    }
    available->recordedAt = Clock::now();
    available->metadata = metadata;
    available->metadata.stage = StageLabel(metadata.stage);
    available->metadata.modelIdentity[sizeof(available->metadata.modelIdentity) - 1] = '\0';
    available->id = ++snapshot.sampled;
    available->interval = interval;
    available->queryCount = available->models = available->actualPasses = 0;
    available->occupied = available->recording = true;
    available->inModel = available->malformed = available->successful = available->reported = false;
    available->rejected = nullptr;
    ++snapshot.pending;
    slot = available;
    list = commandList;
    Stamp(*available, list);
}

void Evaluation::SetMetadata(const Metadata& metadata)
{
    if (!slot)
        return;
    auto& current = *static_cast<Slot*>(slot);
    current.metadata = metadata;
    current.metadata.stage = StageLabel(metadata.stage);
    current.metadata.modelIdentity[sizeof(current.metadata.modelIdentity) - 1] = '\0';
}

void Evaluation::Reject(const char* reason)
{
    if (!slot)
        return;
    auto& current = *static_cast<Slot*>(slot);
    current.rejected = "incomplete-evaluation";
    if (reason)
        for (const auto* label : { "metadata-unavailable", "partial-chain" })
            if (std::strcmp(reason, label) == 0)
                current.rejected = label;
}

void Evaluation::FinishOnScopeExit(uint32_t actualPasses, bool success)
{
    finalPasses = actualPasses;
    finalSuccess = success;
}

void Evaluation::ModelBegin()
{
    if (!slot)
        return;
    auto& current = *static_cast<Slot*>(slot);
    if (current.inModel || current.models >= Detail::MaxModels)
    {
        current.malformed = true;
        return;
    }
    current.inModel = true;
    Stamp(current, list);
}

void Evaluation::ModelEnd()
{
    if (!slot)
        return;
    auto& current = *static_cast<Slot*>(slot);
    if (!current.inModel)
    {
        current.malformed = true;
        return;
    }
    Stamp(current, list);
    current.inModel = false;
    ++current.models;
}

void Evaluation::Finish(uint32_t actualPasses, bool success)
{
    if (!slot)
        return;
    auto& current = *static_cast<Slot*>(slot);
    Stamp(current, list);
    // Every resolved timestamp has an EndQuery, including on all early-abort paths.
    list->ResolveQueryData(current.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, current.queryCount, current.readback, 0);
    std::lock_guard lock(mutex);
    current.actualPasses = actualPasses;
    current.successful = success && !current.rejected;
    current.recording = false;
    if (!current.successful)
        Drop(current.rejected ? current.rejected : "incomplete-evaluation", &current);
    slot = nullptr;
    list = nullptr;
}

Evaluation::~Evaluation() { Finish(finalPasses, finalSuccess); }
} // namespace DlssNr::GpuTiming
