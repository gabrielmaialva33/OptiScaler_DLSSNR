# `lsfg-presenter`

Study branch only (`study/lsfg-d3d12`). LSFG at x3 and x4 through OptiScaler's own frame-generation swapchain, a fork
of FidelityFX SDK 2.3.0's `FrameInterpolationSwapchainDX12` widened to N generated frames per real frame
(`retoque/lsfg/presenter/`, design in `tools/lsfg-study/MFG_PRESENTER.md`).

## What it covers

1. **`MfgPacing.h`**, compiled unmodified under ASan/UBSan (`test.cpp`):
   - `[LSFG] Multiplier` to generated frames (2, 3, 4; anything else is x2);
   - `PresentDelta`: at two frames per entry it is FFX's formula bit for bit (`FrameInterpolationSwapchainDX12.cpp:882-884`)
     over a grid of frame rates, deviations, variance factors and margins; at three and four the even split; an
     entry's presents never add up to more than one real frame; no average presents at once; no division by zero;
   - output sets: consecutive presents write different sets, and a set is free after its last generated frame's
     composition (FFX's `base + doPresent`, widened);
   - the presenter's timing model at 24-82.5 fps and x2-x4: no entry outruns the next real frame, the presented
     rate is (N + 1) x the real rate, and the steady-state gaps are the even split;
   - the 10 s summary (`PresentWindow`): counts, per-real ratio, percentiles, over-target counting, and the latency
     figures (per generated frame GPU completion, per frame present after the game's `Present`, lists per real frame);
   - one interpolation list per generated frame (MFG_PRESENTER.md 4.2): `ListForFrame` puts frame k in list k and the
     rest in the last list; the generator's placement over the lists it was handed and the presenter's mapping over the
     lists it used agree; `ReadyTimes` turns an entry's GPU timestamps into per-frame completion times and refuses a
     slot whose resolve has not landed (a start not newer than the last one), an end before its start, a span over 1 s.
   **`MfgLatch.h`**, what `[LSFG] Multiplier` does in a running process (MFG_PRESENTER.md 1.4 and 5): the started-at
   × chosen matrix (FFX's swapchain latched at 2 or with LSFG off: 3 and 4 wait for a restart and x2 runs; ours latched
   at 3 or 4: 2, 3 and 4 all apply), the undecided latch, invalid values read as 2, the generated frames the router
   hands our swapchain (N = M − 1, one with LSFG off) agreeing with what the menu calls active, and N = 1 pacing as
   FFX's x2.
2. **Source guards** on what the D3D12 half must never do (it cannot run here):
   - `FSRFG_Dx12.cpp` sends nothing aimed at the swapchain context, and no FG configure naming `_swapChain`, straight
     to `FfxApiProxy`; both creation sites make our swapchain only behind `Lsfg::Presenter::Wanted()` and FFX's
     with FFX's exact call otherwise; the one FFX hand-off stays one;
   - the router ends by forwarding to `FfxApiProxy` unchanged, and configures FFX's FG context with
     `NO_SWAPCHAIN_CONTEXT_NOTIFY` when the swapchain is ours; `Wanted()` is a latch on `Enabled && Multiplier > 2`;
   - our swapchain's `QueryInterface` answers the DXGI interfaces and its own IID only, and no FFX interface name or
     IID appears in the fork;
   - the callback is asked for N frames and the presenter shows what it wrote; a failed callback's lists are closed;
     the interpolation queue waits for its output set ahead of every list;
   - the per-frame lists: handed over (as `Lsfg::DispatchDescFrameLists`) only when there is more than one; each used
     list executed in its own submission followed by its own interpolation fence value; the unused ones dropped; each
     generated frame carries its own list's fence and its composition waits for it; the entry's first wait is the first
     list's fence; the presenter's timestamps are resolved by the last list, and its timing objects are released only
     in `shutdown()` after the threads stop and the interpolation queue drains;
   - LSFG's side: `FfxDispatch` places frame k with `ListForFrame` and reports the lists it used; `Context_Dx12::Record`
     issues what a frame leaves pending (its output's barrier) on that frame's own list before switching, binds its heap
     and forgets its pipeline on the next list, keeps the clears and the ingest on the first, and ends and resolves its
     own GPU timing on the last; `Lsfg::TakesFrame` unlinks the descriptor before FFX's dispatch can see it;
   - outputs are released after `waitForPresents`, the UI composition and debug pacing objects only in `shutdown()`
     after the threads stop and the presents drain, and none of them in statics;
   - `Lsfg::TakesFrame` lowers `numGeneratedFrames` to 1 when FFX interpolates instead of LSFG;
   - the router's FG configure takes N from `GeneratedFramesLive` and hands it to our swapchain at every configure
     (the live multiplier); `CurrentLatch` maps the latch; the menu's Multiplier row reads the latch and `MfgLatch.h`,
     marks the items that wait for a restart, writes `LsfgMultiplier` only on a changed selection, and sits inside
     the block disabled with LSFG off;
   - every `MfgSwapChain_*` file keeps AMD's MIT notice and says it was modified; the six shader headers under
     `presenter/precompile/` are the SDK's bytes (checked when `external/FidelityFX-SDK-v2` is present).

## What it does not cover

D3D12 and DXGI: the swapchain's threads, fences, queues, composition and presents, and LSFG's recording. Those were
run in PCSX2 under vkd3d-proton (MFG_PRESENTER.md, section 9). Native Windows with the debug layer is still to do.

## Run

```bash
python3 tests/lsfg-presenter/run.py
```
