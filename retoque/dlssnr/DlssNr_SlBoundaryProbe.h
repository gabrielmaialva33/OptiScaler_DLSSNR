#pragma once

#include <windows.h>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

// The game's own Streamline swapchain, where the present pass runs ahead of the game's DLSS-G
// (docs/dlssnr/design/nr-before-dlssg.md). It began as step 0's probe, which only watched and logged; its findings
// (step 0b, Crimson Desert) are what step 1 stands on, and the probe keeps logging them.
//
// Step 1: at the game's Present, before forwarding it, the present pass runs on the game's native queue
// (RunPresentPass with boundary=true). Streamline signals a fence on that queue inside the forwarded call and DLSS-G's
// own queue waits on it before reading the backbuffer, so the pass is ordered ahead of DLSS-G's read with no CPU wait;
// the entries below (the wrapped swapchain, which under DLSS-G also sees its generated presents, and the FG present
// hook) stand down while this one is live. The HUD is edited with the rest of the frame and the game's tags are left
// as they are (the note's H1).
//
// What step 0 measured, still logged once per window:
//   - presents at the wrapped entry per game present, and whether they arrive nested inside the game's Present;
//   - the queue DLSS-G's FrameGeneration evaluate lists execute on, native to native, and whether inside the Present;
//   - whether DLSSG.Backbuffer is the buffer the game's swapchain reported current at its Present;
//   - Signal and Wait on the game's native queue and on DLSS-G's (step 0b).
//
// Installed only when [DlssNr] Enabled is true with HookMethod=2 as sl.interposer.dll is hooked, so every other
// configuration has no detour at all. It detours the interposer's CreateDXGIFactory/1/2 and slUpgradeInterface, then
// CreateSwapChain/CreateSwapChainForHwnd on the factories they return and Present/Present1 on the swapchains those
// create, each only when its code lies inside sl.interposer.dll. One log line per two seconds while presents arrive.
namespace DlssNr::SlBoundaryProbe
{
// From StreamlineHooks::hookInterposer. A no-op unless the configuration above holds.
void Install(HMODULE interposer);

// From StreamlineHooks::unhookInterposer, before the interposer's own detours go.
void Uninstall();

// From RunPresentPass, on every call. fgHook: the call came from OptiScaler's own FG present hook, not the wrapped
// swapchain; those are not counted.
void NoteWrappedPresent(ID3D12CommandQueue* queue, bool fgHook);

// From the NGX evaluate of the FrameGeneration feature, DLSS-G's own.
void NoteDlssgEvaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* params);

// From ResTrack_Dx12::hkExecuteCommandLists, before the original.
void NoteExecute(ID3D12CommandQueue* queue, unsigned count, ID3D12CommandList* const* lists);
} // namespace DlssNr::SlBoundaryProbe
