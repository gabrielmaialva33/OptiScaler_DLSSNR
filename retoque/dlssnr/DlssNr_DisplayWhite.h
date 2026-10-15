#pragma once

#include "DlssNr_PresentHdr.h"

#include <windows.h>
#include <dxgi.h>

#include <cwchar>
#include <mutex>
#include <vector>

namespace DlssNr::PresentHdr
{
// The display's SDR white, not a measurement of the game's chosen paper white. Match the monitor's source name,
// then query the active path's target identity. Cloned/ambiguous targets fail closed to the documented fallback.
inline WhiteLevel QueryDisplayWhite(HMONITOR monitor)
{
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)))
        return {};

    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS ||
            pathCount == 0 || pathCount > 128 || modeCount > 512)
            return {};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        const LONG result =
            QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (result != ERROR_SUCCESS)
            return {};

        WhiteLevel white;
        unsigned matches = 0;
        for (UINT32 i = 0; i < pathCount; ++i)
        {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
            name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            name.header.size = sizeof(name);
            name.header.adapterId = path.sourceInfo.adapterId;
            name.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS ||
                std::wcscmp(name.viewGdiDeviceName, info.szDevice) != 0)
                continue;
            if (++matches != 1)
                return {};
            DISPLAYCONFIG_SDR_WHITE_LEVEL level {};
            level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            level.header.size = sizeof(level);
            level.header.adapterId = path.targetInfo.adapterId;
            level.header.id = path.targetInfo.id;
            const bool queried = DisplayConfigGetDeviceInfo(&level.header) == ERROR_SUCCESS;
            white = SelectSdrWhite(level.SDRWhiteLevel, queried);
        }
        return white;
    }
    return {};
}

// Called only for a selected HDR conversion. No display enumeration on the SDR/default-off path. A five-second
// refresh also catches SDR-slider/topology changes without modifying the application's window procedure.
inline WhiteLevel DisplayWhiteForSwapchain(IDXGISwapChain* swapchain)
{
    if (swapchain == nullptr)
        return {};
    HMONITOR monitor = nullptr;
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(swapchain->GetContainingOutput(&output)) && output != nullptr)
    {
        DXGI_OUTPUT_DESC desc {};
        if (SUCCEEDED(output->GetDesc(&desc)))
            monitor = desc.Monitor;
    }
    if (output != nullptr)
        output->Release();
    if (monitor == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(swapchain->GetDesc(&desc)) || desc.OutputWindow == nullptr)
            return {};
        monitor = MonitorFromWindow(desc.OutputWindow, MONITOR_DEFAULTTONULL);
    }
    static std::mutex mutex;
    static WhiteCache cache;
    std::lock_guard lock(mutex);
    const auto now = GetTickCount64();
    const auto identity = reinterpret_cast<uintptr_t>(monitor);
    if (cache.NeedsRefresh(identity, now))
        cache.Store(identity, now, QueryDisplayWhite(monitor));
    return cache.Value();
}
} // namespace DlssNr::PresentHdr
