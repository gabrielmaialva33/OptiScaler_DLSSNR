#pragma once

#include <d3d12.h>
#include <cstdint>
#include <array>

namespace DlssNr::GpuTiming
{
struct Metadata
{
    uint64_t evaluationId = 0;
    uint64_t presentId = 0;
    uint64_t settingsGeneration = 0;
    uint64_t contractHash = 0;
    char modelIdentity[128] {};
    const char* stage = "unknown"; // Canonicalized to static before/after/after-fallback/unknown labels.
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint32_t modelWidth = 0, modelHeight = 0;
    uint32_t requestedPasses = 1;
    float workingScale = 1.0f;
    bool modelReset = false;
    bool captureActive = false;
};

struct DiscardCount
{
    const char* reason = "unused";
    uint64_t count = 0;
};

struct Snapshot
{
    bool enabled = false;
    uint64_t runId = 0;
    uint64_t sampleAgeMs = 0;
    uint64_t collectedAgeMs = 0;
    std::array<DiscardCount, 24> reasons {};
    bool valid = false;
    uint64_t eligible = 0, attempted = 0, sampled = 0, accepted = 0, discarded = 0;
    uint32_t pending = 0;
    uint32_t waitingReset = 0, waitingGpu = 0, waitingNotification = 0, retained = 0;
    uint32_t interval = 1;
    uint32_t sampleInterval = 1;
    uint32_t actualPasses = 0;
    uint64_t sampleId = 0;
    double totalMs = 0.0, modelMs = 0.0, otherMs = 0.0;
    Metadata metadata {};
    const char* reason = "disabled";
};

// Collection never waits for GPU completion. Pending resources survive disable and teardown.
void Poll();
void SetEnabled(bool enabled);
Snapshot GetSnapshot();

class Evaluation
{
    void* slot = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    uint32_t finalPasses = 0;
    bool finalSuccess = false;

  public:
    Evaluation(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const Metadata& metadata, bool enabled,
               uint32_t interval);
    ~Evaluation();
    Evaluation(const Evaluation&) = delete;
    Evaluation& operator=(const Evaluation&) = delete;
    void SetMetadata(const Metadata& metadata);
    void Reject(const char* reason);
    void FinishOnScopeExit(uint32_t actualPasses, bool success);
    void ModelBegin();
    void ModelEnd();
    void Finish(uint32_t actualPasses, bool success);
};
} // namespace DlssNr::GpuTiming
