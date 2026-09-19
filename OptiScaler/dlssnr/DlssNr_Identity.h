#pragma once

// What the neural snippet is actually being handed, said the same way by everyone who hands it.
//
// Feature 18 refuses to be created in a game and agrees to be created in this project's loopback
// harness, on the same files, the same core, the same forwarder and the same model. Every parameter
// either matched or was proved irrelevant, and every structural difference anyone could name was
// reproduced in the harness without reproducing the refusal. What is left is a difference nobody has
// thought to name, which is not something guessing finds.
//
// So both sides emit the same report and the two are diffed. One implementation, header-only, taking
// a line sink: production writes it to the log, the harness writes it to stdout, and neither can
// drift from the other because there is nothing to keep in sync.
//
// Every line is "NRID <key>=<value>" so a diff is mechanical rather than a reading exercise. Values
// that are pointers or handles are deliberately included even though they differ every run: their
// job is to show that two lines describe different objects, not to be compared literally.

#include <d3d12.h>
#include <dxgi1_4.h>

#include <tlhelp32.h>

#include <cstdio>
#include <cstring>

namespace DlssNr::Identity
{

// Where a line goes. Production passes a lambda over LOG_INFO; the harness passes one over its own
// Say(). Nothing here knows which.
using Sink = void (*)(void* context, const char* line);

namespace Detail
{

inline void Emit(Sink sink, void* context, const char* format, ...)
{
    char body[512] {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(body, sizeof(body), format, args);
    va_end(args);

    char line[576] {};
    std::snprintf(line, sizeof(line), "NRID %s", body);
    sink(context, line);
}

inline const char* FeatureLevelName(D3D_FEATURE_LEVEL level)
{
    switch (level)
    {
    case D3D_FEATURE_LEVEL_11_0:
        return "11_0";
    case D3D_FEATURE_LEVEL_11_1:
        return "11_1";
    case D3D_FEATURE_LEVEL_12_0:
        return "12_0";
    case D3D_FEATURE_LEVEL_12_1:
        return "12_1";
    case D3D_FEATURE_LEVEL_12_2:
        return "12_2";
    default:
        return "other";
    }
}

} // namespace Detail

// The adapter the device sits on, found from the device's own LUID rather than from whatever the
// caller thinks it used. The two have disagreed before.
inline void ReportAdapter(Sink sink, void* context, ID3D12Device* device)
{
    if (device == nullptr)
        return;

    const LUID luid = device->GetAdapterLuid();
    Detail::Emit(sink, context, "device.luid=%08lX%08lX", (unsigned long) luid.HighPart, (unsigned long) luid.LowPart);

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || factory == nullptr)
    {
        Detail::Emit(sink, context, "adapter=<no factory>");
        return;
    }

    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter != nullptr)
    {
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        char name[128] {};
        std::snprintf(name, sizeof(name), "%ls", desc.Description);
        Detail::Emit(sink, context, "adapter.name=%s", name);
        Detail::Emit(sink, context, "adapter.vendor=%04X device=%04X subsys=%08lX revision=%u", desc.VendorId,
                     desc.DeviceId, (unsigned long) desc.SubSysId, desc.Revision);
        Detail::Emit(sink, context, "adapter.flags=%u dedicatedVideo=%llu dedicatedSystem=%llu sharedSystem=%llu",
                     (unsigned) desc.Flags, (unsigned long long) desc.DedicatedVideoMemory,
                     (unsigned long long) desc.DedicatedSystemMemory, (unsigned long long) desc.SharedSystemMemory);
        adapter->Release();
    }
    else
    {
        Detail::Emit(sink, context, "adapter=<not found by luid>");
    }

    factory->Release();
}

// What the device can do, asked of the device rather than remembered from what was requested. The
// feature level passed to D3D12CreateDevice is a minimum; this is the answer.
inline void ReportDevice(Sink sink, void* context, ID3D12Device* device)
{
    if (device == nullptr)
    {
        Detail::Emit(sink, context, "device=<null>");
        return;
    }

    Detail::Emit(sink, context, "device.nodeCount=%u", device->GetNodeCount());

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                   D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl {};
    fl.NumFeatureLevels = _countof(levels);
    fl.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
        Detail::Emit(sink, context, "device.maxFeatureLevel=%s", Detail::FeatureLevelName(fl.MaxSupportedFeatureLevel));

    D3D12_FEATURE_DATA_SHADER_MODEL sm {};
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        Detail::Emit(sink, context, "device.shaderModel=0x%02X", (unsigned) sm.HighestShaderModel);

    D3D12_FEATURE_DATA_D3D12_OPTIONS o {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
        Detail::Emit(sink, context, "device.resourceBindingTier=%u doublePrecision=%u minPrecision=%u",
                     (unsigned) o.ResourceBindingTier, (unsigned) o.DoublePrecisionFloatShaderOps,
                     (unsigned) o.MinPrecisionSupport);

    D3D12_FEATURE_DATA_ARCHITECTURE arch {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))))
        Detail::Emit(sink, context, "device.uma=%u cacheCoherentUma=%u tileBasedRenderer=%u", (unsigned) arch.UMA,
                     (unsigned) arch.CacheCoherentUMA, (unsigned) arch.TileBasedRenderer);

    Detail::Emit(sink, context, "device.removedReason=0x%08lX", (unsigned long) device->GetDeviceRemovedReason());
}

inline void ReportQueue(Sink sink, void* context, ID3D12CommandQueue* queue)
{
    if (queue == nullptr)
    {
        Detail::Emit(sink, context, "queue=<null>");
        return;
    }

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    Detail::Emit(sink, context, "queue.type=%u priority=%d flags=%u nodeMask=%u", (unsigned) desc.Type,
                 (int) desc.Priority, (unsigned) desc.Flags, (unsigned) desc.NodeMask);
}

// Who else in this process could answer for NGX.
//
// The snippet resolves its caller and the core resolves its implementation, and both of those depend
// on what is loaded, not on what this code intended. Listing the modules by name, with whether each
// exports the core's D3D12 entry point, is how a second resolver stops being invisible.
inline void ReportNgxModules(Sink sink, void* context)
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        Detail::Emit(sink, context, "modules=<snapshot failed %lu>", GetLastError());
        return;
    }

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    unsigned total = 0, interesting = 0;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            ++total;
            char name[128] {};
            std::snprintf(name, sizeof(name), "%ls", entry.szModule);

            char lowered[128] {};
            for (size_t i = 0; name[i] != '\0' && i + 1 < sizeof(lowered); ++i)
                lowered[i] = (char) ((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);

            const bool matches = std::strstr(lowered, "nvngx") != nullptr || std::strstr(lowered, "nvapi") != nullptr ||
                                 std::strstr(lowered, "dlss") != nullptr || std::strstr(lowered, "sl.") != nullptr ||
                                 std::strstr(lowered, "streamline") != nullptr ||
                                 std::strstr(lowered, "reshade") != nullptr ||
                                 std::strstr(lowered, "optiscaler") != nullptr;
            if (!matches)
                continue;

            ++interesting;
            const bool exportsInit = GetProcAddress(entry.hModule, "NVSDK_NGX_D3D12_Init_Ext") != nullptr ||
                                     GetProcAddress(entry.hModule, "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
            Detail::Emit(sink, context, "module=%s ngxExports=%u", name, exportsInit ? 1u : 0u);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    Detail::Emit(sink, context, "modules.total=%u ngxRelated=%u", total, interesting);
}

// Everything, in one call, in a fixed order so two reports line up.
inline void ReportAll(Sink sink, void* context, const char* who, ID3D12Device* device, ID3D12CommandQueue* queue)
{
    Detail::Emit(sink, context, "--- begin %s ---", who);
    ReportDevice(sink, context, device);
    ReportAdapter(sink, context, device);
    ReportQueue(sink, context, queue);
    ReportNgxModules(sink, context);
    Detail::Emit(sink, context, "--- end %s ---", who);
}

} // namespace DlssNr::Identity
