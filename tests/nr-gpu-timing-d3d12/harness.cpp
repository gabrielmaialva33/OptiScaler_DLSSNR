#include "pch.h"
#include "DlssNr_GpuTiming.h"
#include "DlssNr_Submission.h"
#include <detours/detours.h>
#include <resource_tracking/ResTrack_dx12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
namespace Timing = DlssNr::GpuTiming;

static uint64_t hookInstalls = 0, executedBatches = 0, queryHeaps = 0, readbacks = 0;
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using QueryFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_QUERY_HEAP_DESC*, REFIID, void**);
using ResourceFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                               const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                               const D3D12_CLEAR_VALUE*, REFIID, void**);
static ExecuteFn originalExecute = nullptr;
static void* executeTarget = nullptr;
static QueryFn originalQuery = nullptr;
static ResourceFn originalResource = nullptr;

static void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
static void Hr(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        std::fprintf(stderr, "HRESULT 0x%08lx: %s\n", static_cast<unsigned long>(result), message);
        throw std::runtime_error(message);
    }
}
static void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    ++executedBatches;
    DlssNr::Submission::Batch batch(queue, count, lists);
    originalExecute(queue, count, lists);
    batch.Submitted();
}
static HRESULT STDMETHODCALLTYPE QueryHook(ID3D12Device* device, const D3D12_QUERY_HEAP_DESC* desc, REFIID iid,
                                           void** result)
{
    ++queryHeaps;
    return originalQuery(device, desc, iid, result);
}
static HRESULT STDMETHODCALLTYPE ResourceHook(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
                                              D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc,
                                              D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                              void** result)
{
    if (heap->Type == D3D12_HEAP_TYPE_READBACK)
        ++readbacks;
    return originalResource(device, heap, flags, desc, state, clear, iid, result);
}

IUnknown* ResTrack_Dx12::NrRealObject(IUnknown* object) { return object; }
void* ResTrack_Dx12::NrQueueImplementation() { return executeTarget; }
bool ResTrack_Dx12::EnableNrSubmissionTracking(ID3D12Device* device)
{
    if (originalExecute)
        return true;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc {};
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
        return false;
    executeTarget = (*reinterpret_cast<void***>(queue.Get()))[10];
    originalExecute = reinterpret_cast<ExecuteFn>(executeTarget);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    const auto attach = DetourAttach(reinterpret_cast<PVOID*>(&originalExecute), ExecuteHook);
    const auto commit = DetourTransactionCommit();
    if (attach != NO_ERROR || commit != NO_ERROR)
    {
        originalExecute = nullptr;
        return false;
    }
    ++hookInstalls;
    return true;
}

struct Fixture
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queues[2];
    ComPtr<ID3D12Fence> done, gate;
    ComPtr<ID3D12Resource> source, destination;
    uint64_t doneValue = 0;

    Fixture()
    {
        Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "create hardware D3D12 device");
        D3D12_COMMAND_QUEUE_DESC queueDesc {};
        for (auto& queue : queues)
            Hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "create direct queue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)), "create completion fence");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "create blocking test gate");
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC resource {};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = 1024 * 1024;
        resource.Height = 1;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = 1;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           nullptr, IID_PPV_ARGS(&source)),
           "create copy source");
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                           nullptr, IID_PPV_ARGS(&destination)),
           "create copy destination");
        auto table = *reinterpret_cast<void***>(device.Get());
        originalResource = reinterpret_cast<ResourceFn>(table[27]);
        originalQuery = reinterpret_cast<QueryFn>(table[39]);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        Check(DetourAttach(reinterpret_cast<PVOID*>(&originalResource), ResourceHook) == NO_ERROR,
              "resource probe attach");
        Check(DetourAttach(reinterpret_cast<PVOID*>(&originalQuery), QueryHook) == NO_ERROR, "query probe attach");
        Check(DetourTransactionCommit() == NO_ERROR, "allocation probes commit");
    }
    void Complete(unsigned queue)
    {
        Hr(queues[queue]->Signal(done.Get(), ++doneValue), "signal harness completion");
        const auto start = GetTickCount64();
        while (done->GetCompletedValue() < doneValue)
        {
            Check(GetTickCount64() - start < 10000, "harness GPU completion timeout");
            Sleep(1); // Test orchestration only: production Poll must never wait.
        }
        Check(done->GetCompletedValue() != UINT64_MAX, "device removed during harness completion");
    }
};

struct Recording
{
    ComPtr<ID3D12CommandAllocator> allocator, nextAllocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    explicit Recording(Fixture& fixture)
    {
        Hr(fixture.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
           "allocator");
        Hr(fixture.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nextAllocator)),
           "next allocator");
        Hr(fixture.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list)),
           "command list");
    }
    void Record(Fixture& fixture, uint64_t id, bool enabled)
    {
        Timing::Metadata metadata {};
        metadata.evaluationId = id;
        metadata.presentId = id;
        metadata.settingsGeneration = 1;
        metadata.contractHash = 1;
        metadata.stage = "after";
        std::strcpy(metadata.modelIdentity, "harness-copy-not-NGX");
        {
            Timing::Evaluation sample(fixture.device.Get(), list.Get(), metadata, enabled, 1);
            sample.ModelBegin();
            for (unsigned i = 0; i != 128; ++i)
                list->CopyResource(fixture.destination.Get(), fixture.source.Get());
            sample.ModelEnd();
            sample.Finish(1, true);
        }
        Hr(list->Close(), "close sampled list");
    }
    void Submit(Fixture& fixture, unsigned queue)
    {
        ID3D12CommandList* lists[] = { list.Get() };
        fixture.queues[queue]->ExecuteCommandLists(1, lists);
    }
    void Seal()
    {
        Hr(list->Reset(nextAllocator.Get(), nullptr), "seal sampled epoch by Reset");
        Hr(list->Close(), "close empty next epoch");
    }
};

static uint64_t Reason(const Timing::Snapshot& snapshot, const char* reason)
{
    for (const auto& entry : snapshot.reasons)
        if (std::strcmp(entry.reason, reason) == 0)
            return entry.count;
    return 0;
}

int main(int argc, char** argv)
{
    try
    {
        Fixture fixture;
        const bool negative = argc == 2 && std::strcmp(argv[1], "--zero-coverage") == 0;
        {
            Recording off(fixture);
            off.Record(fixture, 1, false);
            off.Submit(fixture, 0);
            fixture.Complete(0);
            off.Seal();
            Timing::Poll();
            Check(queryHeaps == 0 && readbacks == 0 && hookInstalls == 0 && !DlssNr::Submission::Active(),
                  "OFF allocated timing resources or installed submission tracking");
        }
        if (negative)
            Check(Timing::GetSnapshot().accepted != 0, "ZERO COVERAGE: disabled timing accepted no samples");

        uint64_t baselineAccepted = Timing::GetSnapshot().accepted;
        {
            Recording normal(fixture);
            normal.Record(fixture, 2, true);
            normal.Submit(fixture, 0);
            fixture.Complete(0);
            Timing::Poll();
            Check(Timing::GetSnapshot().accepted == baselineAccepted, "published replayable epoch before Reset");
            normal.Seal();
            Timing::Poll();
            auto snapshot = Timing::GetSnapshot();
            Check(snapshot.accepted == baselineAccepted + 1 && snapshot.valid && snapshot.actualPasses == 1,
                  "ZERO COVERAGE: normal submission did not produce exactly one valid sample");
            Check(snapshot.totalMs > 0 && snapshot.modelMs > 0 && snapshot.otherMs >= 0,
                  "invalid real timestamp durations");
        }
        ++baselineAccepted;
        for (unsigned secondQueue : { 0u, 1u })
        {
            Recording replay(fixture);
            const auto discarded = Timing::GetSnapshot().discarded;
            replay.Record(fixture, 3 + secondQueue, true);
            replay.Submit(fixture, 0);
            fixture.Complete(0);
            replay.Submit(fixture, secondQueue);
            fixture.Complete(secondQueue);
            replay.Seal();
            Timing::Poll();
            const auto snapshot = Timing::GetSnapshot();
            Check(snapshot.accepted == baselineAccepted && snapshot.discarded > discarded,
                  "replay on one/two queues was not rejected");
        }
        {
            Recording waiting(fixture);
            struct ReleaseGate
            {
                ID3D12Fence* gate;
                ~ReleaseGate() { gate->Signal(1); }
            } releaseGate { fixture.gate.Get() };
            Hr(fixture.queues[0]->Wait(fixture.gate.Get(), 1), "block test queue without CPU stall");
            waiting.Record(fixture, 5, true);
            waiting.Submit(fixture, 0);
            waiting.Seal();
            Timing::Poll();
            const auto pending = Timing::GetSnapshot();
            const bool withheld = pending.accepted == baselineAccepted && pending.pending > 0;
            Hr(fixture.gate->Signal(1), "release test queue gate");
            fixture.Complete(0);
            Check(withheld, "Reset authorized timestamp publication while GPU was blocked");
            Timing::Poll();
            Check(Timing::GetSnapshot().accepted == baselineAccepted + 1, "completed delayed sample not accepted");
        }
        const auto result = Timing::GetSnapshot();
        Check(result.accepted >= 2 && result.discarded >= 2 && executedBatches >= 6 && queryHeaps > 0 && readbacks > 0,
              "ZERO COVERAGE: mandatory cases did not execute");
        Check(Reason(result, "replayed-recording") >= 1 && Reason(result, "multiple-queues") >= 1,
              "replay or multiple-queue rejection reason missing");
        std::printf("PASS: OFF, normal, replay same queue, replay second queue, Reset-before-GPU; accepted=%llu "
                    "discarded=%llu batches=%llu heaps=%llu readbacks=%llu\n",
                    result.accepted, result.discarded, executedBatches, queryHeaps, readbacks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
