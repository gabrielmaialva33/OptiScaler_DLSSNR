#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): OptiScaler's own frame-generation swapchain for LSFG at x3 and
// x4, in place of FFX's (tools/lsfg-study/MFG_PRESENTER.md). The swapchain is a fork of FFX's
// (MfgSwapChain_Dx12.{h,cpp}); this is what connects it to FSR-FG.
//
// FSRFG_Dx12 creates it instead of FFX's swapchain context when Wanted(), and routes every FFX call that may reach a
// swapchain through D3D12_Configure / D3D12_DestroyContext, drop-ins with FfxApiProxy's signatures. They forward to
// FfxApiProxy untouched unless the context is one of ours, or the descriptor is an ffxConfigureDescFrameGeneration
// whose swapChain is one of ours. With FFX's swapchain in use nothing is ours, so FSR-FG at x2 takes the same calls
// with the same arguments as before.

#include <d3d12.h>
#include <dxgi1_6.h>

#include <ffx_api.h>

#include "MfgLatch.h"

namespace Lsfg::Presenter
{

// Whether our swapchain replaces FFX's: [LSFG] Enabled and [LSFG] Multiplier 3 or 4 when the first FG swapchain of
// the process is created. Latched then, because FGHooks detours the FG swapchain class's methods once per process
// (MFG_PRESENTER.md 1.4); a later change across the 2/3 boundary is logged and waits for a restart.
bool Wanted();

// Whether our swapchain was latched (the LSFG summary names the presenter by it).
bool InUse();

// Which swapchain was latched, Undecided before the first FG swapchain. The menu reads it to tell a multiplier that
// applies at the next frame from one that waits for a restart (MfgLatch.h).
LsfgMfg::Latch CurrentLatch();

// FSRFG_Dx12::CreateSwapchainInternal / CreateSwapchain1Internal, in place of FfxApiProxy::D3D12_CreateContext with
// ffxCreateContextDescFrameGenerationSwapChainNewDX12 / ...ForHwndDX12. On success *swapChain is ours (refcount 1,
// the game's) and *context a record of ours holding a second reference, released by D3D12_DestroyContext.
ffxReturnCode_t CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                IDXGISwapChain** swapChain, ffxContext* context);
ffxReturnCode_t CreateSwapChain1(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, HWND hwnd,
                                 DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                 IDXGISwapChain1** swapChain, ffxContext* context);

// Drop-ins for FfxApiProxy::D3D12_Configure / D3D12_DestroyContext in FSRFG_Dx12.
ffxReturnCode_t D3D12_Configure(ffxContext* context, const ffxConfigureDescHeader* desc);
ffxReturnCode_t D3D12_DestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb);

} // namespace Lsfg::Presenter
