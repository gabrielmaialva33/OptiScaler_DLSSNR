#include "pch.h"

#include "DlssNr_Dred.h"

#include <Config.h>
#include <State.h>
#include <proxies/D3D12_Proxy.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <format>
#include <iterator>
#include <mutex>
#include <string>

namespace
{
// Whether the runtime took the settings. A device created while this was false has no DRED data to give.
std::atomic<bool> gRecording { false };
std::once_flag gEnableOnce;

// A device factory is configured each time a device comes from it, which is rare, and said once either way.
std::atomic<bool> gFactorySaid { false };
std::atomic<bool> gFactoryRefusalSaid { false };

std::atomic<bool> gReported { false };

// The breadcrumb ring can hold hundreds of lists, nearly all finished long before the removal. Only the
// unfinished ones say anything, and a handful of them is enough to tell our pass from the game's.
constexpr unsigned kMaxListsWalked = 4096;
constexpr unsigned kMaxListsShown = 8;
constexpr uint32_t kOpsBefore = 4;
constexpr uint32_t kOpsAfter = 2;
constexpr unsigned kMaxAllocationsShown = 8;
constexpr uint32_t kMaxContextsRead = 4096;

bool On() { return Config::Instance()->DlssNrDred.value_or_default(); }

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

// D3D12_DRED_DEVICE_STATE by value (ID3D12DeviceRemovedExtendedData2), the same way.
const char* DeviceStateName(uint32_t state)
{
    switch (state)
    {
    case 3:
        return "hung";
    case 6:
        return "fault";
    case 7:
        return "page fault";
    default:
        return "unknown";
    }
}

// The breadcrumb context recorded at an operation (a BeginEvent or SetMarker string: our ScopedEvent spans, the
// game's PIX events), or null.
const D3D12_DRED_BREADCRUMB_CONTEXT* ContextAt(const D3D12_DRED_BREADCRUMB_CONTEXT* contexts, uint32_t count,
                                               uint32_t op)
{
    for (uint32_t c = 0; c < count; ++c)
    {
        if (contexts[c].BreadcrumbIndex == op && contexts[c].pContextString != nullptr)
            return &contexts[c];
    }

    return nullptr;
}

// Node is D3D12_AUTO_BREADCRUMB_NODE, or D3D12_AUTO_BREADCRUMB_NODE1 where the runtime keeps contexts.
template <typename Node> void ReportBreadcrumbNodes(const Node* head)
{
    unsigned walked = 0;
    unsigned unfinished = 0;
    unsigned shown = 0;

    for (auto node = head; node != nullptr && walked < kMaxListsWalked; node = node->pNext, ++walked)
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

        const D3D12_DRED_BREADCRUMB_CONTEXT* contexts = nullptr;
        uint32_t contextCount = 0;

        if constexpr (requires(const Node& n) { n.pBreadcrumbContexts; })
        {
            contexts = node->pBreadcrumbContexts;
            contextCount = contexts != nullptr ? std::min(node->BreadcrumbContextsCount, kMaxContextsRead) : 0;
        }

        // The operations around the stop, with the first one the GPU had not finished marked, and the context
        // recorded at an operation beside it.
        std::string ops;
        const uint32_t from = done > kOpsBefore ? done - kOpsBefore : 0;
        const uint32_t to = std::min(count, done + kOpsAfter + 1);

        for (uint32_t i = from; i < to; ++i)
        {
            ops += std::format("{}{}{}", i == from ? "" : " ", i == done ? ">>" : "",
                               OpName(static_cast<uint32_t>(node->pCommandHistory[i])));

            if (const auto context = ContextAt(contexts, contextCount, i); context != nullptr)
                ops += std::format("(\"{}\")", Narrow(context->pContextString, nullptr));
        }

        // The last event begun at or before the stop: which span of work the GPU was in, when the list has them.
        const D3D12_DRED_BREADCRUMB_CONTEXT* last = nullptr;

        for (uint32_t c = 0; c < contextCount; ++c)
        {
            if (contexts[c].BreadcrumbIndex <= done && contexts[c].pContextString != nullptr &&
                (last == nullptr || contexts[c].BreadcrumbIndex >= last->BreadcrumbIndex))
                last = &contexts[c];
        }

        LOG_ERROR("DLSS-NR DRED: queue \"{}\", list \"{}\": {} of {} operations completed; around the stop: {}{}",
                  Narrow(node->pCommandQueueDebugNameW, node->pCommandQueueDebugNameA),
                  Narrow(node->pCommandListDebugNameW, node->pCommandListDebugNameA), done, count, ops,
                  last != nullptr ? std::format("; last event before the stop: \"{}\" at operation {}",
                                                Narrow(last->pContextString, nullptr), last->BreadcrumbIndex)
                                  : "");
    }

    LOG_ERROR("DLSS-NR DRED: {} command list(s) in the breadcrumb ring, {} unfinished{}", walked, unfinished,
              shown < unfinished ? std::format(", the first {} shown", shown) : "");
}

// With contexts where the runtime has them (ID3D12DeviceRemovedExtendedData1), plain otherwise.
void ReportBreadcrumbs(ID3D12DeviceRemovedExtendedData* dred, ID3D12DeviceRemovedExtendedData1* dred1)
{
    HRESULT result = S_OK;

    if (dred1 != nullptr)
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs {};

        if (result = dred1->GetAutoBreadcrumbsOutput1(&crumbs); SUCCEEDED(result))
            return ReportBreadcrumbNodes(crumbs.pHeadAutoBreadcrumbNode);
    }
    else
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs {};

        if (result = dred->GetAutoBreadcrumbsOutput(&crumbs); SUCCEEDED(result))
            return ReportBreadcrumbNodes(crumbs.pHeadAutoBreadcrumbNode);
    }

    // DXGI_ERROR_UNSUPPORTED: the device was created before the settings, or the system's policy refused them.
    LOG_WARN("DLSS-NR DRED: no breadcrumbs (0x{:08X}{})", (unsigned) result,
             result == DXGI_ERROR_UNSUPPORTED ? ": they were not on for this device" : "");
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

// Both recorders forced on, whatever the system's own DRED policy says, and breadcrumb contexts where the runtime
// has ID3D12DeviceRemovedExtendedDataSettings1; without it, breadcrumbs still come, without event names. Returns
// whether contexts are on.
bool Apply(ID3D12DeviceRemovedExtendedDataSettings* settings)
{
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

    ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;

    if (FAILED(settings->QueryInterface(IID_PPV_ARGS(&settings1))) || settings1 == nullptr)
        return false;

    settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings1->Release();
    return true;
}

// The runtime's own D3D12GetDebugInterface, through D3d12Proxy so a proxy build reaches the real d3d12.dll
// (OptiScaler never hooks it). With no proxy module yet, the link-time import: the fallback
// CreateD3D12DeviceOnAdapter takes for D3D12CreateDevice right after asking here, so the settings land in the
// runtime that then creates the device.
D3d12Proxy::PFN_D3D12GetDebugInterface GetDebugInterface()
{
    if (const auto function = D3d12Proxy::D3D12GetDebugInterface_(); function != nullptr)
        return function;

    return D3d12Proxy::Module() == nullptr ? &D3D12GetDebugInterface : nullptr;
}

void Enable()
{
    const auto getDebugInterface = GetDebugInterface();
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

    const bool contexts = Apply(settings);
    settings->Release();

    gRecording = true;
    LOG_INFO("DLSS-NR DRED: auto-breadcrumbs and page-fault tracking on for D3D12 devices "
             "created from here on{}",
             contexts ? ", with breadcrumb contexts" : "; this runtime keeps no breadcrumb contexts");
}

// The Agility SDK's ID3D12DeviceFactory keeps its own configuration, so a device it makes does not take the
// process-wide settings above; its configuration interface takes the same ones.
void EnableOnFactory(ID3D12DeviceFactory* factory)
{
    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    const HRESULT result =
        factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&settings));

    if (FAILED(result) || settings == nullptr)
    {
        if (!gFactoryRefusalSaid.exchange(true))
            LOG_WARN("DLSS-NR DRED: a D3D12 device factory refused its DRED settings (0x{:08X}); its devices "
                     "rely on the process-wide ones",
                     (unsigned) result);
        return;
    }

    const bool contexts = Apply(settings);
    settings->Release();

    gRecording = true;

    if (!gFactorySaid.exchange(true))
        LOG_INFO("DLSS-NR DRED: on for the devices of a D3D12 device factory too{}",
                 contexts ? ", with breadcrumb contexts" : "");
}
} // namespace

void DlssNr::Dred::EnableBeforeDeviceCreation(ID3D12DeviceFactory* factory)
{
    if (!On())
        return;

    std::call_once(gEnableOnce, Enable);

    if (factory != nullptr)
        EnableOnFactory(factory);
}

void DlssNr::Dred::Report(ID3D12Device* device)
{
    if (device == nullptr || !On())
        return;

    // Callers ask after a failed present, a failed check or a failed fence wait, but a live device has nothing
    // recorded yet, and reporting it would spend the one report a real removal needs.
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

    ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
    ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;

    if (FAILED(dred->QueryInterface(IID_PPV_ARGS(&dred1))))
        dred1 = nullptr;

    if (FAILED(dred->QueryInterface(IID_PPV_ARGS(&dred2))))
        dred2 = nullptr;

    // The state says a hang from a fault, which the reason alone does not always.
    LOG_ERROR("DLSS-NR DRED: device removed (reason 0x{:08X}{}); what the runtime recorded follows", (unsigned) reason,
              dred2 != nullptr
                  ? std::format(", state {}", DeviceStateName(static_cast<uint32_t>(dred2->GetDeviceState())))
                  : "");

    ReportBreadcrumbs(dred, dred1);
    ReportPageFault(dred);

    if (dred2 != nullptr)
        dred2->Release();

    if (dred1 != nullptr)
        dred1->Release();

    dred->Release();
}

void DlssNr::Dred::Name(ID3D12Object* object, const wchar_t* name, int index)
{
    if (object == nullptr || name == nullptr || !On())
        return;

    if (index < 0)
    {
        object->SetName(name);
        return;
    }

    const std::wstring indexed = std::format(L"{} {}", name, index);
    object->SetName(indexed.c_str());
}

DlssNr::Dred::ScopedEvent::ScopedEvent(ID3D12GraphicsCommandList* list, const wchar_t* name)
{
    if (list == nullptr || name == nullptr || !On())
        return;

    // Metadata 0 is PIX's plain wide-string event (PIX_EVENT_UNICODE_VERSION), which DRED keeps as the context.
    list->BeginEvent(0, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
    _list = list;
}

DlssNr::Dred::ScopedEvent::~ScopedEvent()
{
    if (_list != nullptr)
        _list->EndEvent();
}
