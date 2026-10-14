#pragma once

#include <windows.h>

struct ID3D12CommandQueue;
struct ID3D12CommandList;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

// Step 0 of docs/dlssnr/design/nr-before-dlssg.md: watch the game's own Streamline swapchain, forward every call
// untouched, and log what a pass running there, above the game's DLSS-G, would have to rely on.
//
// The present pass's only D3D12 entry under the game's DLSS-G is the wrapped swapchain, below DLSS-G. The design's
// option A moves it up to the game's Present on the interposer's swapchain, where only real frames arrive. Before
// building that, three things need measuring in a game, and nothing here changes a frame:
//   - how many presents reach the wrapped entry per present the game makes, and whether they arrive nested inside
//     the game's own Present on its thread or on another thread (DLSS-G's present thread);
//   - which queue executes DLSS-G's FrameGeneration evaluate list: the one the game created its swapchain on, the
//     wrapped entry's, or another, and whether it is submitted inside the game's Present. That decides whether queue
//     order alone puts a pass ahead of DLSS-G (S1) or a wait is needed (S2, S3);
//   - whether DLSSG.Backbuffer at that evaluate is the buffer the game's swapchain reported as current at its Present.
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
