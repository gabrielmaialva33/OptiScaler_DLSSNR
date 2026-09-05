#include "OptiScaler/dlssnr/DlssNr_GpuTimingModel.h"
#include <array>
#include <cassert>
#include <iostream>
#include <cstring>

using namespace DlssNr;
int main()
{
    unsigned cases = 0;
    std::array<uint64_t, Submission::Detail::MaxQueues> completed {};
    const auto done = [&](size_t queue) { return completed[queue]; };
    const auto frequency = [](size_t) { return uint64_t(1000000); };
    Submission::Detail::Model model;
    Submission::Detail::Usage usage;
    auto epoch = model.Track(1, usage, done);
    assert(epoch);
    auto certificate = [&] { return GpuTiming::Detail::Certify(*epoch, done, frequency); };
    assert(!certificate().terminal);
    ++cases;
    auto before = model.BeforeExecute(1);
    assert(before == epoch && !certificate().terminal);
    ++cases;
    Submission::Detail::Model::AfterExecute(before, 0, 10, true);
    completed[0] = 10;
    assert(!certificate().terminal);
    ++cases; // Completed but replay still legal.
    Submission::Detail::Model::AfterReset(epoch, false);
    assert(!certificate().terminal);
    ++cases;
    Submission::Detail::Model::AfterReset(epoch, true);
    assert(certificate().accepted && certificate().reusable && certificate().frequency == 1000000);
    ++cases;
    completed[0] = 9;
    assert(!certificate().terminal);
    ++cases;
    completed[0] = UINT64_MAX;
    assert(certificate().terminal && !certificate().accepted && !certificate().reusable);
    ++cases;
    completed[0] = 10;
    epoch->executions = 2;
    assert(!certificate().accepted && certificate().reusable &&
           std::strcmp(certificate().reason, "replayed-recording") == 0);
    ++cases;
    epoch->fences[1] = 3;
    completed[1] = 3;
    assert(!certificate().accepted && std::strcmp(certificate().reason, "multiple-queues") == 0);
    ++cases;
    epoch->fences[1] = 0;
    epoch->executions = 1;
    assert(!GpuTiming::Detail::Certify(*epoch, done, [](size_t) { return uint64_t(0); }).accepted);
    ++cases;
    epoch->unknown = true;
    assert(certificate().terminal && !certificate().reusable);
    ++cases;
    epoch->unknown = false;
    epoch->pending = 1;
    assert(!certificate().terminal);
    ++cases;
    epoch->pending = 0;
    Submission::Detail::Epoch neverSubmitted;
    neverSubmitted.sealed = true;
    auto notSubmitted = GpuTiming::Detail::Certify(neverSubmitted, done, frequency);
    assert(notSubmitted.terminal && !notSubmitted.reusable && !notSubmitted.accepted);
    ++cases;

    Submission::Detail::Usage empty;
    assert(!GpuTiming::Detail::CertifyUsage(empty, done, frequency).accepted);
    ++cases;
    auto multiple = usage;
    multiple.epochs[0] = std::make_shared<Submission::Detail::Epoch>();
    multiple.epochs[1] = epoch;
    assert(!GpuTiming::Detail::CertifyUsage(multiple, done, frequency).accepted);
    ++cases;
    auto lostDuringFrequency = [&](size_t)
    {
        completed[0] = UINT64_MAX;
        return uint64_t(1000000);
    };
    const auto lost = GpuTiming::Detail::Certify(*epoch, done, lostDuringFrequency);
    assert(lost.terminal && !lost.accepted && !lost.reusable);
    ++cases;
    completed[0] = 10;

    auto saturated = std::make_shared<Submission::Detail::Epoch>();
    saturated->executions = UINT64_MAX;
    Submission::Detail::Model::CountExecution(saturated);
    assert(saturated->unknown && saturated->executions == UINT64_MAX);
    ++cases;
    Submission::Detail::Usage duplicateUsage;
    auto duplicate = model.Track(42, duplicateUsage, done);
    auto duplicateBefore = model.BeforeExecute(42);
    Submission::Detail::Model::CountExecution(duplicateBefore); // Duplicate list in one Execute batch.
    Submission::Detail::Model::AfterExecute(duplicateBefore, 0, 10, true);
    Submission::Detail::Model::AfterReset(duplicate, true);
    assert(duplicate->pending == 0 && duplicate->executions == 2 &&
           !GpuTiming::Detail::Certify(*duplicate, done, frequency).accepted);
    ++cases;

    GpuTiming::Detail::Durations result;
    const uint64_t one[] { 100, 110, 160, 180 };
    assert(GpuTiming::Detail::Decode(one, 4, 1, 1000, result));
    assert(result.totalMs == 80 && result.modelMs == 50 && result.outsideModelMs == 30);
    ++cases;
    const uint64_t three[] { 100, 110, 120, 130, 150, 160, 190, 200 };
    assert(GpuTiming::Detail::Decode(three, 8, 3, 1000, result));
    assert(result.totalMs == 100 && result.modelMs == 60 && result.outsideModelMs == 40);
    ++cases;
    const uint64_t unordered[] { 100, 160, 110, 180 };
    assert(!GpuTiming::Detail::Decode(unordered, 4, 1, 1000, result));
    ++cases;
    const uint64_t equal[] { 100, 100, 100, 100 };
    assert(!GpuTiming::Detail::Decode(equal, 4, 1, 1000, result));
    ++cases;
    assert(!GpuTiming::Detail::Decode(one, 4, 1, 0, result));
    ++cases;
    assert(!GpuTiming::Detail::Decode(one, 3, 1, 1000, result));
    ++cases;
    assert(!GpuTiming::Detail::Decode(one, 4, 0, 1000, result));
    ++cases;
    const uint64_t large[] { UINT64_MAX - 100, UINT64_MAX - 90, UINT64_MAX - 30, UINT64_MAX - 10 };
    assert(GpuTiming::Detail::Decode(large, 4, 1, 1000, result) && result.totalMs == 90);
    ++cases;

    for (uintptr_t iteration = 2; iteration < 3002; ++iteration)
    {
        Submission::Detail::Usage local;
        auto e = model.Track(iteration, local, done);
        assert(e);
        auto submitted = model.BeforeExecute(iteration);
        Submission::Detail::Model::AfterReset(e, true); // Reset races notification, old epoch preserved.
        Submission::Detail::Model::AfterExecute(submitted, 0, 10, true);
        assert(GpuTiming::Detail::Certify(*e, done, frequency).accepted);
    }
    ++cases;
    if (cases != 27)
        return 2; // A runner that exercises nothing must fail loudly.
    std::cout << "PASS " << cases << " timing model cases; 3000 confirmed epoch cycles\n";
}
