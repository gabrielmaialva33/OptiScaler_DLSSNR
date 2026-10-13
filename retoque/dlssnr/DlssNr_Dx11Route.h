#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// Neural Rendering and a D3D11 game's own DLSS (docs/dlssnr/design/dx11-native-dlss-guides.md, steps 1 and 2).
//
// A D3D11 game that calls DLSS gets one of two DLSS routes. "dlss" is the driver's own D3D11 DLSS, evaluated on the
// game's device and context with the game's parameter block: a pass-through that never forms a D3D12 resource, list
// or queue, so nothing on it can host the pass. "dlss_12" evaluates the same call as a D3D12 DLSS on OptiScaler's
// D3D11-to-D3D12 bridge, which runs the pass right after it with its copies of the game's depth and motion
// (IFeature_Dx11wDx12::Evaluate). With NR on, "dlss" was a route on which the pass did nothing and nothing said so.
//
// The decisions are here, standard library only, so tests/nr-dx11-route compiles this header as it is.
// DlssNr_Dx11Upscaler.h is the layer that reads the config and writes the log.
//
//   PreferBridge  whether CreateFeature's default becomes dlss_12 instead of native DLSS (step 2)
//   Decide        what to say when the route in use leaves the pass inert (step 1)
//
// With NR off both are no-ops whatever else is true: PreferBridge answers false and Decide answers None, so the
// D3D11 route is exactly what it was before this header existed.

namespace DlssNr::Dx11Route
{
// The D3D11 super-resolution route an evaluate took, as far as Neural Rendering is concerned.
enum class Kind : uint8_t
{
    Other,       // FSR, XeSS, ray reconstruction, or nothing seen yet: not judged here
    NativeDlss,  // "dlss": the driver's D3D11 DLSS on the game's device; the pass cannot follow it
    BridgedDlss, // "dlss_12": a D3D12 DLSS on the bridge, which runs the pass after it
};

// Step 2. upstreamDefault is what NVSDK_NGX_D3D11_CreateFeature chose before it applies [Upscalers] Dx11Upscaler:
// native DLSS when the GPU is DLSS-capable and the original NGX loaded, FSR 2.2 otherwise. Only that default is
// replaced, and only while the key is unset. A key the user set, in the ini or in the menu, is never overridden:
// Dx11Upscaler=dlss keeps native DLSS, and step 1 then says what that costs. A GPU that does not get native DLSS by
// default does not get dlss_12 either. FeatureProvider_Dx11::GetFeature writes the key with whatever it created, so
// after the first feature of a session the key is set and this answers false: NR switched on later, over a native
// DLSS feature, keeps native DLSS until a restart, and step 1 reports it.
inline bool PreferBridge(bool nrEnabled, bool upscalerKeySet, Kind upstreamDefault)
{
    return nrEnabled && !upscalerKeySet && upstreamDefault == Kind::NativeDlss;
}

// Said once in the log when PreferBridge has changed the default.
inline constexpr const char* kAutoChoiceText =
    "Neural Rendering is on and [Upscalers] Dx11Upscaler is auto, so this D3D11 game's DLSS is created as dlss_12 "
    "(DLSS on OptiScaler's D3D12 bridge, which carries the pass) instead of native D3D11 DLSS. Dx11Upscaler=dlss "
    "keeps native D3D11 DLSS, where the pass does not run.";

// Step 1. What the NR panel and the log say about the route in use.
enum class Report : uint8_t
{
    None,     // nothing to say: NR off, or a route this header does not judge
    NoPass,   // native D3D11 DLSS, HookMethod 0 or 1: the pass belongs after the upscaler, and cannot get there
    NoGuides, // native D3D11 DLSS, HookMethod 2: the game's depth and motion never reach the present host
};

struct Inputs
{
    bool nrEnabled = false;
    Kind route = Kind::Other;
    uint32_t hookMethod = 1; // [DlssNr] HookMethod as configured; anything but 2 places the pass after the upscaler
};

inline Report Decide(const Inputs& in)
{
    if (!in.nrEnabled || in.route != Kind::NativeDlss)
        return Report::None;

    // HookMethod 0 (Auto) on a D3D11 game stays on the upscaler's output (there is no D3D12 present hook to move to),
    // so it is the after-upscaler case too. 2 is the bridge's present host, which runs without an upscaler at all,
    // when it was built at swapchain creation; native DLSS hands it nothing.
    return in.hookMethod == 2 ? Report::NoGuides : Report::NoPass;
}

// The text for a report, in English. The menu runs it through Localization::Tr, so each one has a pt-BR entry
// (tests/nr-dx11-route checks); the log writes it as it is. nullptr for None. Neither claims the pass is or is not
// running: a present host built earlier keeps running whatever HookMethod says now, and the panel's own status line
// above this one says whether the pass ran.
inline const char* ReportText(Report report)
{
    switch (report)
    {
    case Report::NoPass:
        return "This game's DLSS runs natively on D3D11 (Dx11Upscaler dlss), inside the driver on the game's own "
               "device, where the pass cannot run after it. DLSS w/Dx12 (dlss_12) runs the same DLSS on OptiScaler's "
               "D3D12 bridge, which carries the pass: pick it under Upscalers, or set Dx11Upscaler=dlss_12.";
    case Report::NoGuides:
        return "This game's DLSS runs natively on D3D11 (Dx11Upscaler dlss), so its depth and motion never reach the "
               "pass: at present it can only run without them. DLSS w/Dx12 (dlss_12) runs the same DLSS on "
               "OptiScaler's D3D12 bridge, which hands them over: pick it under Upscalers, or set "
               "Dx11Upscaler=dlss_12.";
    case Report::None:
        break;
    }

    return nullptr;
}

// The log says each report once per process, at the first evaluate that leads to it, whatever the frame rate: a
// route that moves away and comes back, or a game that recreates its feature on every resolution change, is not
// told again. Atomic because the evaluate thread is the game's and the latch outlives any one feature.
class LogOnce
{
  public:
    bool First(Report report)
    {
        if (report == Report::None)
            return false;

        const uint32_t bit = 1u << static_cast<uint32_t>(report);
        return (_said.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }

  private:
    std::atomic<uint32_t> _said { 0 };
};

// The panel reports on D3D11 evaluates that are still arriving. A feature the game released, or a loading screen
// that renders no DLSS, leaves nothing to report on, and the line goes after this long. A sample stamped after the
// panel read the clock (the evaluate thread is not the panel's) has a negative age and counts as current.
inline constexpr std::chrono::milliseconds kSeenFor { 1000 };

inline Kind Seen(Kind last, std::chrono::steady_clock::duration age) { return age <= kSeenFor ? last : Kind::Other; }
} // namespace DlssNr::Dx11Route
