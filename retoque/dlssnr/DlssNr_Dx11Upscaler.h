#pragma once

#include "DlssNr_Dx11Route.h"

#include <Config.h>

#include <atomic>
#include <chrono>

// The D3D11 DLSS route as NVSDK_NGX_D3D11_EvaluateFeature and the NR panel see it
// (docs/dlssnr/design/dx11-native-dlss-guides.md, step 1). The decision is in DlssNr_Dx11Route.h; this layer reads
// the config, keeps the route of the last evaluate for the panel, and writes the log.
//
// Header-only, so the module gains no translation unit: the call site in inputs/NVNGX_DLSS_Dx11.cpp is one line.
// With NR off, NoteDx11Evaluate returns before it stores or logs anything.

namespace DlssNr
{
namespace Dx11RouteDetail
{
inline Dx11Route::Kind KindOf(::Upscaler upscaler)
{
    switch (upscaler)
    {
    case ::Upscaler::DLSS:
        return Dx11Route::Kind::NativeDlss;
    case ::Upscaler::DLSS_on12:
        return Dx11Route::Kind::BridgedDlss;
    default:
        return Dx11Route::Kind::Other;
    }
}

inline std::atomic<Dx11Route::Kind> g_lastRoute { Dx11Route::Kind::Other };
inline std::atomic<std::chrono::steady_clock::rep> g_lastAt { 0 };
inline Dx11Route::LogOnce g_reported;
} // namespace Dx11RouteDetail

// Every D3D11 super-resolution evaluate, with the upscaler its feature is. Keeps the route for the panel and logs a
// report the first time one applies; per frame it is two relaxed stores and a config read.
inline void NoteDx11Evaluate(::Upscaler upscaler)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
        return;

    const auto route = Dx11RouteDetail::KindOf(upscaler);
    Dx11RouteDetail::g_lastRoute.store(route, std::memory_order_relaxed);
    Dx11RouteDetail::g_lastAt.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                    std::memory_order_relaxed);

    const auto report = Dx11Route::Decide({ true, route, cfg.DlssNrHookMethod.value_or_default() });

    if (Dx11RouteDetail::g_reported.First(report))
        LOG_WARN("DLSS-NR: {}", Dx11Route::ReportText(report));
}

// The panel's line, untranslated: the report for the route the last D3D11 evaluate took while evaluates are still
// arriving, nullptr when there is nothing to say.
inline const char* Dx11RouteNotice()
{
    const Config& cfg = *Config::Instance();

    const auto at = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(Dx11RouteDetail::g_lastAt.load(std::memory_order_relaxed)));
    const auto route = Dx11Route::Seen(Dx11RouteDetail::g_lastRoute.load(std::memory_order_relaxed),
                                       std::chrono::steady_clock::now() - at);

    return Dx11Route::ReportText(
        Dx11Route::Decide({ cfg.DlssNrEnabled.value_or_default(), route, cfg.DlssNrHookMethod.value_or_default() }));
}
} // namespace DlssNr
