#pragma once

#include "DlssNr_Dx11Route.h"

#include <Config.h>

#include <atomic>
#include <chrono>

struct ID3D11DeviceContext;
struct NVSDK_NGX_Parameter;

// The D3D11 DLSS route as NVSDK_NGX_D3D11_CreateFeature, NVSDK_NGX_D3D11_EvaluateFeature and the NR panel see it
// (docs/dlssnr/design/dx11-native-dlss-guides.md, steps 1 and 2). The decisions are in DlssNr_Dx11Route.h; this layer
// reads the config, keeps the route of the last evaluate for the panel, and writes the log.
//
// Header-only, so the module gains no translation unit: the two call sites in inputs/NVNGX_DLSS_Dx11.cpp are one
// line each. With NR off, Dx11DefaultUpscaler hands back what it was given and NoteDx11Evaluate returns before it
// stores or logs anything.

namespace DlssNr
{
// Step 3, the native D3D11 guide transport (DlssNr_Dx11Guides.cpp), which NoteDx11Evaluate hands each evaluate to.
void CarryNativeDx11Guides(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params, bool nativeDlss, bool evaluated);
bool NativeDx11GuidesCarriedRecently();

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
inline std::atomic<bool> g_autoChoiceLogged { false };
} // namespace Dx11RouteDetail

// CreateFeature's default for super resolution. Called between upstream's own default and the Dx11Upscaler key,
// which is applied after this and still wins: dlss_12 in place of native DLSS while NR is on and the key is unset.
inline ::Upscaler Dx11DefaultUpscaler(::Upscaler upstreamDefault)
{
    const Config& cfg = *Config::Instance();

    if (!Dx11Route::PreferBridge(cfg.DlssNrEnabled.value_or_default(), cfg.Dx11Upscaler.has_value(),
                                 Dx11RouteDetail::KindOf(upstreamDefault)))
        return upstreamDefault;

    if (!Dx11RouteDetail::g_autoChoiceLogged.exchange(true, std::memory_order_relaxed))
        LOG_INFO("DLSS-NR: {}", Dx11Route::kAutoChoiceText);

    return ::Upscaler::DLSS_on12;
}

// Every D3D11 super-resolution evaluate, with the upscaler its feature is, the context and parameters it was
// evaluated with, and whether it succeeded. Keeps the route for the panel, hands a native DLSS evaluate's guides to
// the transport (step 3), and logs a report the first time one applies; per frame on any other route it is two
// relaxed stores and a config read.
inline void NoteDx11Evaluate(::Upscaler upscaler, ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params,
                             bool evaluated)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
        return;

    const auto route = Dx11RouteDetail::KindOf(upscaler);
    Dx11RouteDetail::g_lastRoute.store(route, std::memory_order_relaxed);
    Dx11RouteDetail::g_lastAt.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                    std::memory_order_relaxed);

    // Before the report, so that an evaluate the transport carries is not reported as one whose guides are missing.
    CarryNativeDx11Guides(context, params, route == Dx11Route::Kind::NativeDlss, evaluated);

    const auto report =
        Dx11Route::Decide({ true, route, cfg.DlssNrHookMethod.value_or_default(), NativeDx11GuidesCarriedRecently() });

    if (Dx11RouteDetail::g_reported.First(report))
        LOG_WARN("DLSS-NR: {}", Dx11Route::ReportText(report));
}

// What the panel reports: the report for the route the last D3D11 evaluate took, while evaluates are still arriving.
// Its text is Dx11Route::ReportText, untranslated.
inline Dx11Route::Report Dx11RouteReport()
{
    const Config& cfg = *Config::Instance();

    const auto at = std::chrono::steady_clock::time_point(
        std::chrono::steady_clock::duration(Dx11RouteDetail::g_lastAt.load(std::memory_order_relaxed)));
    const auto route = Dx11Route::Seen(Dx11RouteDetail::g_lastRoute.load(std::memory_order_relaxed),
                                       std::chrono::steady_clock::now() - at);

    return Dx11Route::Decide({ cfg.DlssNrEnabled.value_or_default(), route, cfg.DlssNrHookMethod.value_or_default(),
                               NativeDx11GuidesCarriedRecently() });
}
} // namespace DlssNr
