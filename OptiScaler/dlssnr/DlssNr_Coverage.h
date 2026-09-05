#pragma once

#include <chrono>
#include <cstdint>

namespace DlssNr::Detail
{
// CPU observations only: NGX accepting a call and recording a resolve do not prove GPU completion.
struct CoverageSample
{
    bool modelCalled = false, modelOk = false, applied = false, fallback = false;
    uint64_t present = 0;
    uint32_t width = 0, height = 0, modelWidth = 0, modelHeight = 0;
    const char* reason = "no composition recorded (model unavailable, building, skipped or failed)";

    void ModelResult(int result, uint32_t w, uint32_t h)
    {
        modelCalled = true;
        modelOk = result == 1;
        modelWidth = w;
        modelHeight = h;
    }
};

struct CoverageTotals
{
    uint64_t calls = 0, modelOk = 0, modelFailed = 0, applied = 0, skipped = 0, fallback = 0;
    uint64_t appliedPresents = 0, skippedPresents = 0;
};

// Externally serialized. Each outcome counts only advancing, nonzero present ids. Buckets may
// overlap: one present can contain both skipped and applied evaluations. Never sum them as frames.
class StageCoverage
{
    uint64_t _appliedPresent = 0, _skippedPresent = 0;
    std::chrono::steady_clock::time_point _lastLog {};

  public:
    CoverageTotals totals;

    bool Record(const CoverageSample& sample, std::chrono::steady_clock::time_point now)
    {
        const bool firstApplied = sample.applied && totals.applied == 0;
        const bool firstFailure = sample.modelCalled && !sample.modelOk && totals.modelFailed == 0;
        ++totals.calls;
        totals.modelOk += sample.modelCalled && sample.modelOk;
        totals.modelFailed += sample.modelCalled && !sample.modelOk;
        totals.applied += sample.applied;
        totals.fallback += !sample.applied && sample.fallback;
        totals.skipped += !sample.applied && !sample.fallback;
        auto& last = sample.applied ? _appliedPresent : _skippedPresent;
        if (sample.present > last)
        {
            last = sample.present;
            ++(sample.applied ? totals.appliedPresents : totals.skippedPresents);
        }
        if (totals.calls == 1 || firstApplied || firstFailure || now - _lastLog >= std::chrono::seconds(5))
        {
            _lastLog = now;
            return true;
        }
        return false;
    }
};
} // namespace DlssNr::Detail
