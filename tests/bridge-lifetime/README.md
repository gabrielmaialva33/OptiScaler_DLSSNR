# D3D11 / D3D12 bridge lifetime

Run `python3 tests/bridge-lifetime/run.py` (host tier, g++, ASan/UBSan).

The runner extracts and compiles the production fence waiter, both wait helpers,
copy submission, both resize entry points, final Release, ownership checks,
retirement collector, drain and destructor/resource cleanup. The fakes script
Win32 event wakes, time, fence completion, HRESULTs and COM reference counts.
These are executable control-flow tests, not source-pattern assertions.

The runner also compiles `IFeature_Dx11wDx12::Init`, `ProcessDx11Textures`, its
inline `IsInited` and the intact evaluate-submission block with scripted backend
and COM dependencies. The shared waiter is extracted from `with_dx12.cpp`.
The upscaler cases catch false initialization success on timeout, registration
failure, wait failure or removal; allocator reuse after stale wakes; forgotten
pending values after a failed Signal at creation or evaluate; and an Init retry
resetting a still-pending creation allocator. They also check completion reported
at WAIT_TIMEOUT, backend creation refusal, and Reset/Close failure handling.

Cases:

- Completed and unused fences take the fast path. A missing fence/event with
  outstanding work fails. Removal before or during a wait is not completion.
- A timed-out registration wakes a later wait below its requested value. Further
  waits use only the original deadline's remainder. Timeout, registration and
  Win32 wait errors propagate; neither helper forgets pending work.
- The production copy submits work, then Signal fails. The destination reference
  and pending fence value survive. Both resize entry points return timeout without
  touching either swapchain, overlay, shadow or allocator.
- Final Release quarantines the complete wrapper on unknown completion. Its device,
  queue, presenter, explicit destination, shadow, allocator and fence references
  survive. Removing just one participating device does not release shared storage.
  Removing both uses cleanup without invoking global FG/overlay teardown.
- Retirement later completes after a replacement on the same HWND. Cleanup leaves
  the replacement's FG and overlay alone. A changed backend context also rejects
  ownership, even with the same presenter pointer. With FGPreserveSwapChain, two
  wrappers can share both presenter and context: only the newest wrapper may
  release that presenter.
- FG presenter ownership can exist without being the current game-facing wrapper.
  Resizing a secondary wrapper does not clean the current FG's overlay.
- Saved copy completion does not bypass the fresh present-queue marker. Separate
  queues use separate fences. A partial resize remains marked incomplete until
  both swapchains resize successfully; ResizeBuffers1 fallback resizes once.

## Limits and manual validation

None of the eleven existing host suites exercised this file; this is the twelfth
host suite. None of the Wine suites exercises this bridge either. Passing these
fakes proves failure decisions and ownership/reference accounting, **not GPU
synchronization**, backend-internal queue lifetime, or real device-loss behavior.

A real D3D11 bridge harness or controlled game run is still needed for asynchronous
copy/overlay overlap, FSR/XeSS internal queues, rapid resize and HWND replacement,
removed devices and driver-injected Signal failures. Do not deploy based solely
on these tests. This task builds the DLL but does not install it into any game.

## Deliberate failure behavior

Resize returns the drain HRESULT before changing resources. A partial underlying
resize returns its failure and prevents ordinary Present until a successful
resize restores a coherent pair; there is no claimed rollback of DXGI resize.

Final Release cannot report an HRESULT. Failed live-device drains retain the
wrapper in an intrusive retirement list; construction and Present poll it without
waiting. A failed copy Signal can leave its saved target permanently unreachable:
that wrapper stays quarantined until both devices are confirmed removed or the
process exits. There is no destructor on the retirement list that frees unproven
work at DLL shutdown. This intentionally trades bounded-per-failure resource
retention for avoiding reuse/free without completion.

The bridge pins the resources referenced by its own submitted copy commands and
its two known queues. FG SDK contexts and ImGui backend resources remain owned by
their existing global subsystems. Bridge-initiated cleanup is delayed and checked
against current presenter/context identity, but this does not repair independent
teardown/reinitialization paths in those subsystems or prove completion of private
SDK queues. Those are a remaining boundary of the lifetime guarantee.

## Validation recorded 2026-09-12

- Pinned `/usr/lib/llvm20/bin/clang-format --dry-run --Werror` passed on both
  production files.
- `./build-local.sh` passed in Release with the final bridge source recompiled.
  The existing watchdog recovered a Wine compiler stall; existing linker warnings
  remained. No DLL was installed into a game.
- `python3 tests/run_all.py` passed 12/12 host suites: all eleven pre-existing
  suites plus this suite. No Wine-tier coverage of this bridge is claimed.
- Manual lifetime review checked all bridge-owned destructive paths, preserved
  presenter reuse, distinct queue fences, partial resize recovery and retirement
  without callbacks under the retirement mutex. NR config, shaders, hooks and
  passthrough are unchanged; this is an unconditional bridge correctness repair.
  Other subsystems' concurrent global-state changes and independent FG/ImGui
teardown remain outside this review's safety guarantee.

## Upscaler fence-wait repair, 2026-09-14

`WaitForBridgeFence` moved unchanged from the swapchain translation unit into
`with_dx12.cpp`, declared in the existing `with_dx12.h`. The old helper was local
to `Dx11wDx12SC`, so the independently implemented `IFeature_Dx11wDx12` waits had
never acquired its completion checks. Both upscaler waits now use 5000 ms, the
same budget as the swapchain allocator wait and teardown drain. A retry of Init
also checks its prior submission before resetting allocator 0. An init failure
keeps the bridge uninitialized even if backend creation had succeeded.

The additional scope is deliberately narrow: it protects these wait decisions,
allocator resets and pending-value accounting. It does **not** give the upscaler
bridge the swapchain class's deferred retirement. `ReleaseSharedResources`,
backend destruction/fallback and global interop-cache replacement still need a
separate lifetime audit when completion is unknown. In particular, returning
false from Init is not proof that a later caller may immediately destroy the
backend's submitted resources. No end-to-end safe teardown claim is made here.

The remaining waits under `with_dx12` are two D3D12 queue waits in the swapchain
bridge and a D3D12 queue wait plus a D3D11 context wait in the shared-resource
transport. These enqueue GPU dependencies; replacing them with CPU fence waits
would change the synchronization contract. No other infinite CPU wait was found
in this scope.

Validation: the new timeout case first failed against `8971a31f` because Init
returned true. After the repair, `./build-local.sh Release` completed (existing
compiler/linker warnings), pinned clang-format 20 passed, and
`python3 tests/run_all.py` passed 12/12 host suites. The bridge suite passed again
after formatting the test sources. No game or Wine-tier bridge test was run and
no DLL was installed into a game.

## The neural present host

When the bridge was built to host the neural pass rather than frame generation, the pass is recorded
onto the same DIRECT list as the interop copy and executed on the same queue, and what reaches the
presenter is the pass's answer instead of the frame that came in. The pass adds no transfer; it
changes what is transferred.

The host is the one thing on this path that can be half-done: its guide initialization is recorded
onto that same list, so whether the list executed is the difference between zeros that exist and
zeros that do not, and only the bridge knows which happened. The cases here cover the ordinary frame,
a pass that declined, a pass that recorded but produced nothing, a list that could not be closed, a
teardown, and a bridge with no host at all. Five mutations of the bridge were confirmed to fail them:
never confirming execution, not telling the host about a dropped list, ignoring the host's output,
showing that output on a frame the host declined, and leaking its textures at teardown.

## Exclusive fullscreen and the presenter's extent

`fullscreenCases` compiles the production `SetFullscreenState`, `_EmulatesFullscreen`,
`_RecoverPresenter` and `_ResizePresenterToMatch`. It also compiles three helpers:
`PresenterExtent`, `PresenterResizeFlags` and `MatchHiddenToPresenter`. The window half of the
emulation (`_EnterBorderless` and `_LeaveBorderless`) needs Win32, so it is a counting stub. The
fake swapchain resolves a zero against its own window when it resizes, the way DXGI does.
`resizedAt` orders resizes across swapchains.

The cases:

- **Generation Zero, 2026-09-28.** The presenter is found fullscreen when the game asks for
  fullscreen, so the wrapper takes it back to a window and marks a resize owed. The game then
  resizes with zeros at the size the presenter already has, which `IsSame` would skip.
  - The call reaches the presenter first, with the zeros.
  - The hidden swapchain gets the size DXGI gave the presenter (1920x1080), not the client rect
    the wrapper sampled (1280x720).
  - Once the owed resize is paid, an unchanged resize is skipped again.
- **PCSX2, windowed 0x0.** The zero is compared against the window, not against the presenter's own
  size. The hidden swapchain gets the presenter's extent and never 1x1.
- **Flags.** `GDI_COMPATIBLE` is dropped. `ALLOW_TEARING` and `FRAME_LATENCY_WAITABLE_OBJECT` follow
  the presenter. The hidden swapchain keeps the game's flags.
- **Present recovery.**
  - Nothing happens without a transition since the last resize.
  - After a transition, the presenter alone is resized, once, behind the drain, with its interop
    buffers released first. The hidden swapchain is not resized, because the game holds its
    buffers.
  - The recovery is logged once and spent until the next transition.
  - A recovery resize that itself fails costs one attempt and no further log lines.
- **Presenter fullscreen behind the wrapper.** DXGI's own Alt+Enter handling put the presenter into
  fullscreen. It is taken back to a window, the emulation starts, and the presenter is resized.
  The output reference is returned.
- **Frame generation's presenter.** FSR-FG's swapchain, not the plain interop one, is emulated too.
  The game's `SetFullscreenState(TRUE)` and `(FALSE)` never reach it, it stays windowed, and the
  emulation enters and leaves the borderless window. Before this, it was handed the call and created
  from the game's fullscreen description, and Generation Zero with synthesized FG refused every Present
  with 887A0001 (Rafael, 2026-09-28). The old `_EmulatesFullscreen` fails this case.
- **At creation.** The hidden swapchain follows the presenter only when the two differ and the
  presenter can be read.

Six mutations of the fix were each confirmed to fail a case:

- an owed resize still skipped by `IsSame`;
- a recovery that never disarms;
- the presenter handed the resolved extent;
- the hidden swapchain not sized from the presenter;
- `SetFullscreenState` owing nothing;
- the game's raw flags handed to the presenter.

## What the bridge is built for

`predicateCases` runs the production `WantedForFrameGeneration`, `WantedForNeuralRendering` and
`WantedIdleForNeuralRendering`, extracted verbatim by `run.py`, over the FG input and output and NR's
state:

- Nothing configured builds no bridge. NR alone at present (`HookMethod=2`) builds one for NR.
- An upscaler-fed FG bridge never hosts the pass: the pass has its own route after that upscaler.
- The synthesized input builds an FG bridge for the FSR FG and DLSSG outputs. The pass rides along
  when NR is at present, and an idle host is kept when NR is off there. XeFG and Reprojection are refused
  for it, so no bridge is built for them.
- DLSSG was refused until 2026-09-28 (synthesized-frame-generation.md, "DLSS-G output"). Restoring that
  refusal in the production predicate fails these cases.
