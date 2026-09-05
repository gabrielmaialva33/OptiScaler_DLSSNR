#include "OptiScaler/dlssnr/DlssNr_GpuTiming.h"
#include "OptiScaler/dlssnr/DlssNr_Submission.h"
#include "OptiScaler/dlssnr/DlssNr_GpuTimingModel.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace DlssNr;
namespace
{
unsigned heaps = 0, buffers = 0, maps = 0, queries = 0, resolves = 0, tracks = 0;
unsigned heapFrees = 0, bufferFrees = 0;
bool failTrack = false, failMap = false;
uint64_t clockTick = 100;
Submission::Detail::Model submission;
std::array<uint64_t, Submission::Detail::MaxQueues> completed {};
auto completedValue = [](size_t q) { return completed[q]; };
std::array<uint64_t, Submission::Detail::MaxQueues> frequencies { 1000000, 1000000 };
struct Heap : ID3D12QueryHeap
{
    std::array<uint64_t, 8> ticks {};
    std::array<bool, 8> initialized {};
    void Release() override
    {
        ++heapFrees;
        delete this;
    }
};
struct Buffer : ID3D12Resource
{
    std::array<uint64_t, 8> ticks {};
    void Release() override
    {
        ++bufferFrees;
        delete this;
    }
    HRESULT Map(UINT, const D3D12_RANGE* range, void** output) override
    {
        ++maps;
        assert(range->Begin == 0 && range->End <= sizeof(ticks));
        if (failMap)
            return E_FAIL;
        *output = ticks.data();
        return S_OK;
    }
    void Unmap(UINT, const D3D12_RANGE* written) override { assert(written->Begin == 0 && written->End == 0); }
};
struct Device : ID3D12Device
{
    bool failHeap = false, failBuffer = false;
    unsigned references = 1;
    void AddRef() override { ++references; }
    void Release() override
    {
        assert(references > 1);
        --references;
    }
    HRESULT CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* desc, void** output) override
    {
        ++heaps;
        assert(desc->Type == D3D12_QUERY_HEAP_TYPE_TIMESTAMP && desc->Count == 8);
        if (failHeap)
            return E_FAIL;
        *output = new Heap;
        return S_OK;
    }
    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, int, const D3D12_RESOURCE_DESC* desc, int state,
                                    const void*, void** output) override
    {
        ++buffers;
        assert(heap->Type == D3D12_HEAP_TYPE_READBACK && desc->Width == 64 && state == D3D12_RESOURCE_STATE_COPY_DEST);
        if (failBuffer)
            return E_FAIL;
        *output = new Buffer;
        return S_OK;
    }
};
struct List : ID3D12GraphicsCommandList
{
    ID3D12Device* device = nullptr;
    HRESULT GetDevice(void** output) override
    {
        *output = device;
        if (device)
            device->AddRef();
        return device ? S_OK : E_FAIL;
    }
    int GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
    void EndQuery(ID3D12QueryHeap* h, int, UINT query) override
    {
        ++queries;
        assert(query < 8);
        static_cast<Heap*>(h)->ticks[query] = clockTick += 10;
        static_cast<Heap*>(h)->initialized[query] = true;
    }
    void ResolveQueryData(ID3D12QueryHeap* h, int, UINT begin, UINT count, ID3D12Resource* resource,
                          uint64_t offset) override
    {
        ++resolves;
        assert(begin == 0 && count > 0 && count <= 8 && offset == 0);
        for (UINT i = 0; i < count; ++i)
        {
            assert(static_cast<Heap*>(h)->initialized[i]); // Must belong to this recording, not stale heap contents.
            static_cast<Buffer*>(resource)->ticks[i] = static_cast<Heap*>(h)->ticks[i];
        }
        static_cast<Heap*>(h)->initialized.fill(false);
    }
};
std::shared_ptr<Submission::Detail::Epoch> Execute(List& list, size_t queue = 0)
{
    auto epoch = submission.BeforeExecute(reinterpret_cast<uintptr_t>(&list));
    assert(epoch);
    Submission::Detail::Model::AfterExecute(epoch, queue, 10, true);
    return epoch;
}
void Seal(List& list)
{
    auto epoch = submission.Current(reinterpret_cast<uintptr_t>(&list));
    assert(epoch);
    Submission::Detail::Model::AfterReset(epoch, true);
}
void Record(Device& device, List& list, unsigned passes = 1, bool finish = true, unsigned interval = 1)
{
    list.device = &device;
    GpuTiming::Metadata metadata;
    metadata.evaluationId = queries + 1;
    metadata.contractHash = 123;
    char localStage[] = "before";
    metadata.stage = localStage;
    std::strcpy(metadata.modelIdentity, "synthetic-model");
    GpuTiming::Evaluation timing(&device, &list, metadata, true, interval);
    localStage[0] = 'X';
    metadata.contractHash = 999; // The sample must retain its own contract.
    for (unsigned pass = 0; pass < passes; ++pass)
    {
        timing.ModelBegin();
        timing.ModelEnd();
    }
    if (finish)
        timing.FinishOnScopeExit(passes, true);
}

int CadenceCases()
{
    unsigned cases = 0;
    Device device;
    List list;
    assert(GpuTiming::GetSnapshot().eligible == 0);
    ++cases;
    for (unsigned evaluation = 1; evaluation <= 31; ++evaluation)
    {
        const auto before = GpuTiming::GetSnapshot();
        const auto previousQueries = queries;
        Record(device, list, 1, true, 30);
        const auto after = GpuTiming::GetSnapshot();
        assert(after.eligible == evaluation);
        if (evaluation == 1 || evaluation == 31)
        {
            assert(after.attempted == before.attempted + 1 && after.sampled == before.sampled + 1);
            Execute(list);
            completed[0] = 10;
            Seal(list);
            GpuTiming::Poll();
        }
        else
            assert(after.attempted == before.attempted && after.sampled == before.sampled &&
                   queries == previousQueries);
    }
    assert(GpuTiming::GetSnapshot().accepted == 2 && GpuTiming::GetSnapshot().sampleInterval == 30);
    ++cases;
    GpuTiming::SetEnabled(false);
    {
        GpuTiming::Evaluation off(&device, &list, {}, false, 30);
    }
    assert(GpuTiming::GetSnapshot().eligible == 31 && device.references == 1);
    ++cases;
    GpuTiming::SetEnabled(true);
    Record(device, list, 1, true, 30); // Eligible 32: off/on preserves the global evaluation phase.
    assert(GpuTiming::GetSnapshot().eligible == 32 && GpuTiming::GetSnapshot().sampled == 2);
    ++cases;
    for (unsigned evaluation = 33; evaluation <= 35; ++evaluation)
        Record(device, list, 1, true, 5);
    assert(GpuTiming::GetSnapshot().sampled == 2);
    ++cases;
    Record(device, list, 1, true, 5);  // Eligible 36 records at interval 5.
    Record(device, list, 1, true, 30); // Eligible 37 changes current interval without recording.
    Execute(list);
    Seal(list);
    GpuTiming::Poll();
    const auto final = GpuTiming::GetSnapshot();
    assert(final.eligible == 37 && final.attempted == 3 && final.sampled == 3 && final.accepted == 3 &&
           final.interval == 30 && final.sampleInterval == 5);
    ++cases;
    GpuTiming::SetEnabled(false);
    if (cases != 6 || queries != 12 || resolves != 3 || maps != 3)
        return 2;
    std::cout << "PASS " << cases
              << " production cadence cases; interval=30 first/31st, off/on phase, interval changes\n";
    return 0;
}
} // namespace
namespace DlssNr::Submission
{
bool Track(ID3D12GraphicsCommandList* list, Usage& usage)
{
    ++tracks;
    if (failTrack)
        return false;
    return bool(submission.Track(reinterpret_cast<uintptr_t>(list), usage, completedValue));
}
const char* LastRefusal() { return "injected-track-failure"; }
GpuTiming::Detail::Certificate TimingCertificate(const Usage& usage)
{
    return GpuTiming::Detail::CertifyUsage(usage, completedValue, [](size_t q) { return frequencies[q]; });
}
} // namespace DlssNr::Submission
int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--cadence") == 0)
        return CadenceCases();
    unsigned cases = 0;
    std::array<Device, 4> devices;
    std::array<List, 40> lists;
    auto& device = devices[0];
    {
        GpuTiming::Evaluation disabled(&device, &lists[0], {}, false, 1);
        disabled.ModelBegin();
        disabled.ModelEnd();
        disabled.Finish(1, true);
    }
    assert(heaps == 0 && buffers == 0 && tracks == 0 && queries == 0);
    ++cases;
    Record(device, lists[0]);
    GpuTiming::Poll();
    assert(maps == 0);
    ++cases;
    Execute(lists[0]);
    completed[0] = 10;
    GpuTiming::Poll();
    assert(maps == 0);
    ++cases;
    Seal(lists[0]);
    GpuTiming::Poll();
    auto first = GpuTiming::GetSnapshot();
    assert(maps == 1 && first.accepted == 1 && first.valid && first.metadata.contractHash == 123 &&
           std::strcmp(first.metadata.stage, "before") == 0);
    ++cases;

    completed[0] = 0;
    Record(device, lists[1]);
    Execute(lists[1]);
    Seal(lists[1]);
    GpuTiming::Poll();
    assert(maps == 1 && GpuTiming::GetSnapshot().waitingGpu == 1);
    ++cases;
    completed[0] = 10;
    GpuTiming::Poll();
    assert(maps == 2);
    ++cases;
    Record(device, lists[2], 3);
    Execute(lists[2]);
    Seal(lists[2]);
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().actualPasses == 3 && maps == 3);
    ++cases;

    Record(device, lists[3], 1, false);
    Execute(lists[3]);
    Seal(lists[3]);
    GpuTiming::Poll();
    assert(maps == 3 && GpuTiming::GetSnapshot().discarded == 1);
    ++cases;
    Record(device, lists[4]);
    Execute(lists[4]);
    Execute(lists[4]);
    Seal(lists[4]);
    GpuTiming::Poll();
    assert(maps == 3 && GpuTiming::GetSnapshot().discarded == 2);
    ++cases;
    Record(device, lists[5]);
    Execute(lists[5], 0);
    Execute(lists[5], 1);
    completed[1] = 10;
    Seal(lists[5]);
    GpuTiming::Poll();
    assert(maps == 3 && GpuTiming::GetSnapshot().discarded == 3);
    ++cases;
    failMap = true;
    Record(device, lists[6]);
    Execute(lists[6]);
    Seal(lists[6]);
    GpuTiming::Poll();
    failMap = false;
    assert(maps == 4 && GpuTiming::GetSnapshot().discarded == 4);
    ++cases;
    failTrack = true;
    const auto queriesBefore = queries;
    Record(device, lists[7]);
    failTrack = false;
    assert(queries == queriesBefore && GpuTiming::GetSnapshot().discarded == 5);
    ++cases;
    devices[1].failBuffer = true;
    const auto freesBefore = heapFrees;
    Record(devices[1], lists[8]);
    assert(heapFrees > freesBefore && GpuTiming::GetSnapshot().discarded == 6);
    ++cases;
    devices[2].failHeap = true;
    Record(devices[2], lists[9]);
    assert(GpuTiming::GetSnapshot().discarded == 7);
    ++cases;
    Record(device, lists[10], 0);
    Execute(lists[10]);
    Seal(lists[10]);
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().discarded == 8);
    ++cases;
    {
        lists[11].device = &device;
        GpuTiming::Evaluation timing(&device, &lists[11], {}, true, 1);
        timing.ModelBegin(); // Abort while model pair is incomplete.
    }
    Execute(lists[11]);
    Seal(lists[11]);
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().discarded == 9);
    ++cases;
    Record(device, lists[12]);
    Execute(lists[12]);
    Seal(lists[12]);
    {
        GpuTiming::Evaluation disabled(&device, &lists[13], {}, false, 1);
    }
    assert(!GpuTiming::GetSnapshot().enabled && GpuTiming::GetSnapshot().accepted == 4 && device.references == 1);
    ++cases;

    Record(device, lists[14]);
    Record(device, lists[15]);
    Execute(lists[15]);
    Seal(lists[15]);
    GpuTiming::Poll();
    const auto newestId = GpuTiming::GetSnapshot().sampleId;
    assert(GpuTiming::GetSnapshot().accepted == 5);
    ++cases;
    Execute(lists[14]);
    Seal(lists[14]);
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().accepted == 6 && GpuTiming::GetSnapshot().sampleId == newestId);
    ++cases;

    // Unknown/lost or never-observed submission never authorizes releasing/reusing a GPU slot.
    const auto freesBeforeRetained = heapFrees + bufferFrees;
    for (unsigned i = 16; i < 32; ++i)
    {
        Record(device, lists[i]);
        auto epoch = submission.Current(reinterpret_cast<uintptr_t>(&lists[i]));
        assert(epoch);
        epoch->unknown = true;
        GpuTiming::Poll();
    }
    assert(GpuTiming::GetSnapshot().pending == 16 && GpuTiming::GetSnapshot().retained == 16);
    ++cases;
    const auto beforePoolDrop = queries;
    Record(device, lists[32]);
    assert(queries == beforePoolDrop && heapFrees + bufferFrees == freesBeforeRetained);
    ++cases;
    GpuTiming::Poll();
    assert(GpuTiming::GetSnapshot().discarded == 26);
    ++cases;
    if (cases != 22 || queries == 0 || resolves == 0 || maps == 0 || tracks == 0)
        return 2;
    std::cout << "PASS " << cases << " production timing runtime cases; queries=" << queries << " resolves=" << resolves
              << " maps=" << maps << " tracked=" << tracks << '\n';
}
