#include "pch.h"

#include "Dred.h"

#include <Logger.h>
#include <Util.h>
#include <proxies/D3D12_Proxy.h>

#include <magic_enum.hpp>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Dred
{

namespace
{

// How much of a dump goes to the log. A game's own lists can carry thousands of operations and DRED keeps every list
// still in flight, so the summary of each node is bounded, and the operation history is a window around where the GPU
// stopped rather than the whole of it.
constexpr size_t kMaxNodes = 64;
constexpr UINT kOpsBefore = 12;
constexpr UINT kOpsAfter = 4;
constexpr size_t kMaxAllocations = 32;

std::mutex g_mutex;
std::vector<const void*> g_reported;  // devices already dumped
std::vector<const void*> g_factories; // factories already configured
bool g_settingsApplied = false;       // the global settings took

// Kept for the process's life. The settings are the runtime's global state, and holding the interface makes sure
// nothing about releasing it can matter.
ID3D12DeviceRemovedExtendedDataSettings* g_settings = nullptr;

std::string NameOf(const char* narrow, const wchar_t* wide)
{
    if (narrow != nullptr && narrow[0] != '\0')
        return narrow;

    if (wide != nullptr && wide[0] != L'\0')
        return wstring_to_string(wide);

    return "(unnamed)";
}

template <typename E> std::string EnumName(E value, std::string_view prefix)
{
    auto name = magic_enum::enum_name(value);
    if (name.empty())
        return std::to_string(static_cast<long long>(value));

    if (name.starts_with(prefix))
        name.remove_prefix(prefix.size());

    return std::string(name);
}

std::string OpName(D3D12_AUTO_BREADCRUMB_OP op) { return EnumName(op, "D3D12_AUTO_BREADCRUMB_OP_"); }

std::string AllocationName(D3D12_DRED_ALLOCATION_TYPE type) { return EnumName(type, "D3D12_DRED_ALLOCATION_TYPE_"); }

const char* DeviceStateName(D3D12_DRED_DEVICE_STATE state)
{
    switch (state)
    {
    case D3D12_DRED_DEVICE_STATE_HUNG:
        return "HUNG";
    case D3D12_DRED_DEVICE_STATE_FAULT:
        return "FAULT";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT:
        return "PAGEFAULT";
    default:
        return "UNKNOWN";
    }
}

// Forced on, whatever the system's own DRED policy says. Contexts need ID3D12DeviceRemovedExtendedDataSettings1
// (Windows 10 2004 and later); without it, breadcrumbs still come, unnamed by event.
bool Apply(ID3D12DeviceRemovedExtendedDataSettings* settings, const char* through)
{
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

    bool contexts = false;
    ID3D12DeviceRemovedExtendedDataSettings1* settings1 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings1))) && settings1 != nullptr)
    {
        settings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        settings1->Release();
        contexts = true;
    }

    LOG_INFO("DRED: auto-breadcrumbs and page-fault reporting forced on through {}{}", through,
             contexts ? ", breadcrumb contexts too"
                      : "; this runtime has no breadcrumb contexts (no ID3D12DeviceRemovedExtendedDataSettings1)");
    return true;
}

// The real d3d12.dll's D3D12GetDebugInterface. OptiScaler never hooks it, so the proxy's pointer is the system's own
// function even when OptiScaler is itself loaded as d3d12.dll (its _dll is then the system copy). With no proxy module
// at all, the link-time import, the same fallback CreateD3D12DeviceOnAdapter takes for D3D12CreateDevice.
D3d12Proxy::PFN_D3D12GetDebugInterface RealGetDebugInterface()
{
    if (auto function = D3d12Proxy::D3D12GetDebugInterface_(); function != nullptr)
        return function;

    if (D3d12Proxy::Module() != nullptr)
    {
        const auto address = KernelBaseProxy::GetProcAddress_()(D3d12Proxy::Module(), "D3D12GetDebugInterface");
        return reinterpret_cast<D3d12Proxy::PFN_D3D12GetDebugInterface>(address);
    }

    return &D3D12GetDebugInterface;
}

void ApplyGlobal()
{
    const auto getDebugInterface = RealGetDebugInterface();
    if (getDebugInterface == nullptr)
    {
        LOG_WARN("DRED: d3d12.dll has no D3D12GetDebugInterface; devices are created without DRED");
        return;
    }

    const HRESULT result = getDebugInterface(IID_PPV_ARGS(&g_settings));
    if (FAILED(result) || g_settings == nullptr)
    {
        g_settings = nullptr;
        LOG_WARN("DRED: D3D12GetDebugInterface(ID3D12DeviceRemovedExtendedDataSettings) failed: {:X}. This runtime has "
                 "no DRED (an older Windows, or vkd3d-proton); devices are created without it",
                 (UINT) result);
        return;
    }

    g_settingsApplied = Apply(g_settings, "D3D12GetDebugInterface");
}

void ApplyFactory(ID3D12DeviceFactory* factory)
{
    {
        std::lock_guard lock(g_mutex);
        if (std::find(g_factories.begin(), g_factories.end(), factory) != g_factories.end())
            return;
        g_factories.push_back(factory);
    }

    ID3D12DeviceRemovedExtendedDataSettings* settings = nullptr;
    const HRESULT result =
        factory->GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData, IID_PPV_ARGS(&settings));
    if (FAILED(result) || settings == nullptr)
    {
        LOG_WARN("DRED: the device factory {:X} has no DRED settings ({:X}); its devices rely on the global ones",
                 (size_t) factory, (UINT) result);
        return;
    }

    // Kept, as the global settings are.
    Apply(settings, "the device factory's configuration interface");
}

// Every node DRED kept, one line each, and for the ones the GPU stopped inside, the operations around where it
// stopped. A node's pLastBreadcrumbValue is how many of its operations completed: operation `last` is the first that
// did not. 0 < last < count is a list the GPU was inside; last == 0 a list it had not finished the first operation of,
// which is the one that matters for each queue's first pending list and noise after it.
template <typename Node> void DumpBreadcrumbs(const Node* head)
{
    size_t total = 0;
    for (auto* node = head; node != nullptr; node = node->pNext)
        ++total;

    LOG_ERROR("DRED breadcrumbs: {} command list(s) kept", total);

    std::vector<const void*> queuesWithPending;
    size_t index = 0;
    for (auto* node = head; node != nullptr && index < kMaxNodes; node = node->pNext, ++index)
    {
        const UINT count = node->BreadcrumbCount;
        const UINT last = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
        const bool inside = last > 0 && last < count;
        const bool pending = last == 0 && count > 0;
        const char* state = count == 0 ? "empty" : last >= count ? "completed" : inside ? "STOPPED INSIDE" : "pending";

        LOG_ERROR("DRED node {}: queue '{}', list '{}': {} of {} operations completed, {}", index,
                  NameOf(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW),
                  NameOf(node->pCommandListDebugNameA, node->pCommandListDebugNameW), std::min(last, count), count,
                  state);

        bool firstPending = false;
        if (pending && std::find(queuesWithPending.begin(), queuesWithPending.end(), node->pCommandQueue) ==
                           queuesWithPending.end())
        {
            queuesWithPending.push_back(node->pCommandQueue);
            firstPending = true;
        }

        if ((!inside && !firstPending) || node->pCommandHistory == nullptr)
            continue;

        const UINT begin = last > kOpsBefore ? last - kOpsBefore : 0;
        const UINT end = std::min(count, last + kOpsAfter + 1);

        // The event names, where this runtime keeps them: the nearest one before the window says what the window is
        // inside of, and every one in it is printed beside its operation.
        const D3D12_DRED_BREADCRUMB_CONTEXT* contexts = nullptr;
        UINT contextCount = 0;
        if constexpr (requires(const Node& n) { n.pBreadcrumbContexts; })
        {
            contexts = node->pBreadcrumbContexts;
            contextCount = contexts != nullptr ? node->BreadcrumbContextsCount : 0;
        }

        const D3D12_DRED_BREADCRUMB_CONTEXT* enclosing = nullptr;
        for (UINT c = 0; c < contextCount; ++c)
        {
            if (contexts[c].BreadcrumbIndex < begin && contexts[c].pContextString != nullptr &&
                (enclosing == nullptr || contexts[c].BreadcrumbIndex >= enclosing->BreadcrumbIndex))
            {
                enclosing = &contexts[c];
            }
        }

        if (enclosing != nullptr)
            LOG_ERROR("DRED node {}:   last context before op {}: '{}' at op {}", index, begin,
                      wstring_to_string(enclosing->pContextString), enclosing->BreadcrumbIndex);

        for (UINT op = begin; op < end; ++op)
        {
            std::string context;
            for (UINT c = 0; c < contextCount; ++c)
            {
                if (contexts[c].BreadcrumbIndex == op && contexts[c].pContextString != nullptr)
                {
                    context += context.empty() ? "  [" : "; ";
                    context += wstring_to_string(contexts[c].pContextString);
                }
            }
            if (!context.empty())
                context += "]";

            LOG_ERROR("DRED node {}:   op {}: {}{}{}", index, op, OpName(node->pCommandHistory[op]),
                      op == last ? "  <-- first not completed" : "", context);
        }
    }

    if (total > kMaxNodes)
        LOG_ERROR("DRED breadcrumbs: {} more command list(s) not shown", total - kMaxNodes);
}

template <typename Node> void DumpAllocations(const char* what, const Node* head)
{
    size_t count = 0;
    for (auto* node = head; node != nullptr; node = node->pNext, ++count)
    {
        if (count < kMaxAllocations)
            LOG_ERROR("DRED page fault: {} allocation '{}' ({})", what, NameOf(node->ObjectNameA, node->ObjectNameW),
                      AllocationName(node->AllocationType));
    }

    if (count == 0)
        LOG_ERROR("DRED page fault: no {} allocation at that address", what);
    else if (count > kMaxAllocations)
        LOG_ERROR("DRED page fault: {} more {} allocation(s) not shown", count - kMaxAllocations, what);
}

template <typename Output> void DumpPageFault(const Output& output)
{
    if (output.PageFaultVA == 0)
    {
        LOG_ERROR("DRED page fault: none recorded (a hang, or a fault that was not a page fault)");
        return;
    }

    LOG_ERROR("DRED page fault: GPU virtual address 0x{:016X}", (UINT64) output.PageFaultVA);
    DumpAllocations("existing", output.pHeadExistingAllocationNode);
    DumpAllocations("recently freed", output.pHeadRecentFreedAllocationNode);
}

} // namespace

bool Enabled()
{
    static const bool enabled = []
    {
        wchar_t value[8] {};
        const DWORD length = GetEnvironmentVariableW(L"RETOQUE_DRED", value, static_cast<DWORD>(std::size(value)));
        if (length > 0 && length < std::size(value) && std::wcscmp(value, L"1") == 0)
        {
            LOG_INFO("DRED: on (RETOQUE_DRED=1): D3D12 devices are created with DRED, and a removed one is "
                     "described in this log");
            return true;
        }

        std::error_code ec;
        if (std::filesystem::exists(Util::DllPath().parent_path() / L"retoque_dred", ec))
        {
            LOG_INFO("DRED: on (retoque_dred marker beside the DLL): D3D12 devices are created with DRED, and a "
                     "removed one is described in this log");
            return true;
        }

        return false;
    }();

    return enabled;
}

void BeforeDeviceCreation(ID3D12DeviceFactory* factory)
{
    if (!Enabled())
        return;

    static std::once_flag once;
    std::call_once(once, ApplyGlobal);

    if (factory != nullptr)
        ApplyFactory(factory);
}

void ReportRemoval(ID3D12Device* device, const char* where)
{
    if (!Enabled() || device == nullptr)
        return;

    const HRESULT reason = device->GetDeviceRemovedReason();
    if (SUCCEEDED(reason))
        return;

    {
        std::lock_guard lock(g_mutex);
        if (std::find(g_reported.begin(), g_reported.end(), device) != g_reported.end())
            return;
        g_reported.push_back(device);
    }

    LOG_ERROR("DRED: device {:X} removed with {:X}, first seen at {}; what DRED kept follows", (size_t) device,
              (UINT) reason, where != nullptr ? where : "(unknown)");

    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))) || dred == nullptr)
    {
        LOG_ERROR("DRED: the device offers no ID3D12DeviceRemovedExtendedData; nothing was kept (the settings were {})",
                  g_settingsApplied ? "applied" : "never applied");
        return;
    }

    ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
    ID3D12DeviceRemovedExtendedData2* dred2 = nullptr;
    if (FAILED(dred->QueryInterface(IID_PPV_ARGS(&dred1))))
        dred1 = nullptr;
    if (FAILED(dred->QueryInterface(IID_PPV_ARGS(&dred2))))
        dred2 = nullptr;

    if (dred2 != nullptr)
        LOG_ERROR("DRED: device state {}", DeviceStateName(dred2->GetDeviceState()));

    // Breadcrumbs: with contexts where the runtime has them (ID3D12DeviceRemovedExtendedData1), plain otherwise.
    // DXGI_ERROR_UNSUPPORTED means they were never enabled for this device -- created before the settings, or a
    // policy that refused them.
    HRESULT result = S_OK;
    if (dred1 != nullptr)
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 output {};
        result = dred1->GetAutoBreadcrumbsOutput1(&output);
        if (SUCCEEDED(result))
            DumpBreadcrumbs(output.pHeadAutoBreadcrumbNode);
    }
    else
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT output {};
        result = dred->GetAutoBreadcrumbsOutput(&output);
        if (SUCCEEDED(result))
            DumpBreadcrumbs(output.pHeadAutoBreadcrumbNode);
    }

    if (FAILED(result))
        LOG_ERROR("DRED breadcrumbs: none ({:X}{})", (UINT) result,
                  result == DXGI_ERROR_UNSUPPORTED ? ": not enabled for this device" : "");

    // The page fault, with the allocations at that address and the ones freed from it shortly before.
    if (dred2 != nullptr)
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 output {};
        result = dred2->GetPageFaultAllocationOutput2(&output);
        if (SUCCEEDED(result))
            DumpPageFault(output);
    }
    else if (dred1 != nullptr)
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 output {};
        result = dred1->GetPageFaultAllocationOutput1(&output);
        if (SUCCEEDED(result))
            DumpPageFault(output);
    }
    else
    {
        D3D12_DRED_PAGE_FAULT_OUTPUT output {};
        result = dred->GetPageFaultAllocationOutput(&output);
        if (SUCCEEDED(result))
            DumpPageFault(output);
    }

    if (FAILED(result))
        LOG_ERROR("DRED page fault: no data ({:X}{})", (UINT) result,
                  result == DXGI_ERROR_UNSUPPORTED ? ": not enabled for this device" : "");

    LOG_ERROR("DRED: end of the report for device {:X}", (size_t) device);

    if (dred2 != nullptr)
        dred2->Release();
    if (dred1 != nullptr)
        dred1->Release();
    dred->Release();
}

void Name(ID3D12Object* object, const wchar_t* name, int index)
{
    if (!Enabled() || object == nullptr || name == nullptr)
        return;

    if (index < 0)
    {
        object->SetName(name);
        return;
    }

    std::wstring indexed(name);
    indexed += L' ';
    indexed += std::to_wstring(index);
    object->SetName(indexed.c_str());
}

ScopedEvent::ScopedEvent(ID3D12GraphicsCommandList* list, const wchar_t* name)
{
    if (!Enabled() || list == nullptr || name == nullptr)
        return;

    // Metadata 0 is PIX's plain wide-string event (PIX_EVENT_UNICODE_VERSION), which DRED records as the context.
    list->BeginEvent(0, name, static_cast<UINT>((std::wcslen(name) + 1) * sizeof(wchar_t)));
    _list = list;
}

ScopedEvent::~ScopedEvent()
{
    if (_list != nullptr)
        _list->EndEvent();
}

} // namespace Dred
