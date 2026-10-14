#include "pch.h"

#include "DlssNr_Dred.h"

#include <Config.h>
#include <State.h>
#include <proxies/D3D12_Proxy.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <iterator>
#include <mutex>
#include <string>

namespace
{
// Whether the runtime took the settings. A device created while this was false has no DRED data to give.
std::atomic<bool> gRecording { false };
std::once_flag gEnableOnce;
std::atomic<bool> gReported { false };

// The breadcrumb ring can hold hundreds of lists, nearly all finished long before the removal. Only the
// unfinished ones say anything, and a handful of them is enough to tell our pass from the game's.
constexpr unsigned kMaxListsWalked = 4096;
constexpr unsigned kMaxListsShown = 8;
constexpr uint32_t kOpsBefore = 4;
constexpr uint32_t kOpsAfter = 2;
constexpr unsigned kMaxAllocationsShown = 8;

// D3D12_AUTO_BREADCRUMB_OP by value, as d3d12.h of SDK 10.0.26100 numbers it. Written as a table rather
// than a switch on the enumerators so an older SDK, which lacks the late ones, still compiles this; an
// operation past the table prints as its number.
constexpr const char* kOps[] = {
    "SetMarker",
    "BeginEvent",
    "EndEvent",
    "DrawInstanced",
    "DrawIndexedInstanced",
    "ExecuteIndirect",
    "Dispatch",
    "CopyBufferRegion",
    "CopyTextureRegion",
    "CopyResource",
    "CopyTiles",
    "ResolveSubresource",
    "ClearRenderTargetView",
    "ClearUnorderedAccessView",
    "ClearDepthStencilView",
    "ResourceBarrier",
    "ExecuteBundle",
    "Present",
    "ResolveQueryData",
    "BeginSubmission",
    "EndSubmission",
    "DecodeFrame",
    "ProcessFrames",
    "AtomicCopyBufferUINT",
    "AtomicCopyBufferUINT64",
    "ResolveSubresourceRegion",
    "WriteBufferImmediate",
    "DecodeFrame1",
    "SetProtectedResourceSession",
    "DecodeFrame2",
    "ProcessFrames1",
    "BuildRaytracingAccelerationStructure",
    "EmitRaytracingAccelerationStructurePostbuildInfo",
    "CopyRaytracingAccelerationStructure",
    "DispatchRays",
    "InitializeMetaCommand",
    "ExecuteMetaCommand",
    "EstimateMotion",
    "ResolveMotionVectorHeap",
    "SetPipelineState1",
    "InitializeExtensionCommand",
    "ExecuteExtensionCommand",
    "DispatchMesh",
    "EncodeFrame",
    "ResolveEncoderOutputMetadata",
    "Barrier",
    "BeginCommandList",
    "DispatchGraph",
    "SetProgram",
};

std::string OpName(uint32_t op)
{
    if (op < std::size(kOps))
        return kOps[op];

    if (op == 52)
        return "ProcessFrames2";

    return std::format("op{}", op);
}

// D3D12_DRED_ALLOCATION_TYPE by value, the same way. The values start at 19 and have gaps.
std::string AllocationTypeName(uint32_t type)
{
    switch (type)
    {
    case 19:
        return "CommandQueue";
    case 20:
        return "CommandAllocator";
    case 21:
        return "PipelineState";
    case 22:
        return "CommandList";
    case 23:
        return "Fence";
    case 24:
        return "DescriptorHeap";
    case 25:
        return "Heap";
    case 27:
        return "QueryHeap";
    case 28:
        return "CommandSignature";
    case 29:
        return "PipelineLibrary";
    case 34:
        return "Resource";
    case 35:
        return "Pass";
    case 42:
        return "StateObject";
    case 43:
        return "MetaCommand";
    default:
        return std::format("type{}", type);
    }
}

// Debug names come from the game, its middleware and the driver. They are copied out bounded and with
// anything unprintable replaced, so a hostile or corrupt name cannot run past the log line.
std::string Narrow(const wchar_t* wide, const char* narrow)
{
    constexpr int kMax = 96;
    std::string out;

    if (wide != nullptr)
    {
        for (int i = 0; i < kMax && wide[i] != 0; ++i)
            out.push_back(wide[i] >= 32 && wide[i] < 127 ? static_cast<char>(wide[i]) : '?');
    }
    else if (narrow != nullptr)
    {
        for (int i = 0; i < kMax && narrow[i] != 0; ++i)
            out.push_back(narrow[i] >= 32 && narrow[i] < 127 ? narrow[i] : '?');
    }

    return out.empty() ? "(unnamed)" : out;
}

void ReportBreadcrumbs(ID3D12DeviceRemovedExtendedData* dred)
{
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs {};

    if (const HRESULT result = dred->GetAutoBreadcrumbsOutput(&crumbs); FAILED(result))
    {
        LOG_WARN("DLSS-NR DRED: no breadcrumbs (0x{:08X})", (unsigned) result);
        return;
    }

    unsigned walked = 0;
    unsigned unfinished = 0;
    unsigned shown = 0;

    for (auto node = crumbs.pHeadAutoBreadcrumbNode; node != nullptr && walked < kMaxListsWalked;
         node = node->pNext, ++walked)
    {
        const uint32_t count = node->BreadcrumbCount;
        const uint32_t done = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;

        // Every operation completed: the GPU was past this list when the device went.
        if (done >= count)
            continue;

        ++unfinished;

        if (shown >= kMaxListsShown || node->pCommandHistory == nullptr)
            continue;

        ++shown;

        // The operations around the stop, with the first one the GPU had not finished marked.
        std::string ops;
        const uint32_t from = done > kOpsBefore ? done - kOpsBefore : 0;
        const uint32_t to = std::min(count, done + kOpsAfter + 1);

        for (uint32_t i = from; i < to; ++i)
            ops += std::format("{}{}{}", i == from ? "" : " ", i == done ? ">>" : "",
                               OpName(static_cast<uint32_t>(node->pCommandHistory[i])));

        LOG_ERROR("DLSS-NR DRED: queue \"{}\", list \"{}\": {} of {} operations completed; around the stop: {}",
                  Narrow(node->pCommandQueueDebugNameW, node->pCommandQueueDebugNameA),
                  Narrow(node->pCommandListDebugNameW, node->pCommandListDebugNameA), done, count, ops);
    }

    LOG_ERROR("DLSS-NR DRED: {} command list(s) in the breadcrumb ring, {} unfinished{}", walked, unfinished,
              shown < unfinished ? std::format(", the first {} shown", shown) : "");
}

void ReportAllocations(const char* what, const D3D12_DRED_ALLOCATION_NODE* node)
{
    unsigned total = 0;
    std::string list;

    for (; node != nullptr && total < kMaxListsWalked; node = node->pNext, ++total)
    {
        if (total < kMaxAllocationsShown)
            list += std::format("{}{} \"{}\"", total == 0 ? "" : ", ", AllocationTypeName(node->AllocationType),
                                Narrow(node->ObjectNameW, node->ObjectNameA));
    }

    if (total == 0)
        LOG_ERROR("DLSS-NR DRED: {}: none", what);
    else
        LOG_ERROR("DLSS-NR DRED: {}: {}{}", what, list,
                  total > kMaxAllocationsShown ? std::format(" (and {} more)", total - kMaxAllocationsShown) : "");
}

void ReportPageFault(ID3D12DeviceRemovedExtendedData* dred)
{
    D3D12_DRED_PAGE_FAULT_OUTPUT fault {};

    if (const HRESULT result = dred->GetPageFaultAllocationOutput(&fault); FAILED(result))
    {
        LOG_WARN("DLSS-NR DRED: no page-fault data (0x{:08X})", (unsigned) result);
        return;
    }

    if (fault.PageFaultVA == 0)
    {
        LOG_ERROR("DLSS-NR DRED: no page fault recorded, so the removal was not a bad GPU address");
        return;
    }

    LOG_ERROR("DLSS-NR DRED: page fault at GPU virtual address 0x{:X}", (uint64_t) fault.PageFaultVA);
    ReportAllocations("allocations live at that address", fault.pHeadExistingAllocationNode);
    ReportAllocations("allocations freed there recently", fault.pHeadRecentFreedAllocationNode);
}

void Enable()
{
    const auto getDebugInterface = D3d12Proxy::D3D12GetDebugInterface_();
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;

    const HRESULT result = getDebugInterface != nullptr ? getDebugInterface(IID_PPV_ARGS(&settings)) : E_NOINTERFACE;

    if (FAILED(result) || settings == nullptr)
    {
        if (State::Instance().isRunningOnLinux)
            LOG_WARN("DLSS-NR DRED: not available (0x{:08X}): vkd3d-proton does not implement it. "
                     "Under Proton, VKD3D_CONFIG=breadcrumbs is the equivalent and reports into "
                     "the vkd3d log",
                     (unsigned) result);
        else
            LOG_WARN("DLSS-NR DRED: D3D12 refused the settings interface (0x{:08X}); a device "
                     "removal will be reported without breadcrumbs",
                     (unsigned) result);
        return;
    }

    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->Release();

    gRecording = true;
    LOG_INFO("DLSS-NR DRED: auto-breadcrumbs and page-fault tracking on for D3D12 devices "
             "created from here on");
}
} // namespace

void DlssNr::Dred::EnableBeforeDeviceCreation()
{
    if (!Config::Instance()->DlssNrDred.value_or_default())
        return;

    std::call_once(gEnableOnce, Enable);
}

void DlssNr::Dred::Report(ID3D12Device* device)
{
    if (device == nullptr || !Config::Instance()->DlssNrDred.value_or_default())
        return;

    // Callers ask after a failed present or a failed check, but a live device has nothing recorded yet,
    // and reporting it would spend the one report a real removal needs.
    const HRESULT reason = device->GetDeviceRemovedReason();

    if (SUCCEEDED(reason) || gReported.exchange(true))
        return;

    ID3D12DeviceRemovedExtendedData* dred = nullptr;

    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr)
    {
        LOG_WARN("DLSS-NR DRED: the removed device has no DRED data{}",
                 gRecording ? "" : " (the recorders were not on when it was created)");
        return;
    }

    LOG_ERROR("DLSS-NR DRED: device removed (reason 0x{:08X}); what the runtime recorded follows", (unsigned) reason);

    ReportBreadcrumbs(dred);
    ReportPageFault(dred);

    dred->Release();
}
