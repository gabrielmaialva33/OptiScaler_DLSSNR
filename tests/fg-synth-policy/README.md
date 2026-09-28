# Synthesized FG policy and the motion handoff

Run `python3 tests/fg-synth-policy/run.py` from the repository root. It builds `test.cpp` with g++
C++20, `-Werror`, ASan and UBSan in a temporary directory and includes the production headers as
they are: `OptiScaler/inputs/FG/Synth_Policy.h`, `OptiScaler/inputs/FG/Synth_Hud.h` and
`OptiScaler/shaders/synth_motion/SynthMotion_Handoff.h`. None touches the GPU, so nothing is faked beyond
two empty `ID3D12Device` / `ID3D12Resource` structs.

Before compiling, `run.py` also holds the HUD mask's thresholds (`shaders/synth_motion/SynthOverlay_Dx12.cpp`)
equal to DLSS-NR's (`shaders/dlssnr/DlssNr_UiMask_Dx12.cpp`), which they copy: one rule, one set of
thresholds, two passes.

## What it exercises

- **The handoff.** DLSS-NR's present hosts and synthesized FG both want the motion field of the same
  base frame; the first to estimate it publishes it, the second takes it. The cases cover:
  - both orders (the D3D11 bridge, where NR publishes; native D3D12, where FG does);
  - a consumer never taking its own field back, which also covers a route with no transport, where the
    base frame never advances;
  - device, extent and base-frame mismatches;
  - `Withdraw` before an estimator is released;
  - refusing incomplete publications.
- **The warm-up wait.** A consumer whose estimator is still warming announces it, and the other
  consumer, with no estimator of its own, waits instead of building a second. The cases cover:
  - `AnnounceWarming` / `PeerWarming` in both orders, never one's own, and only for this base frame,
    device and extent;
  - `WithdrawWarming` taking back only the releasing consumer's own announcement;
  - `WaitForPeer` only with no estimator and a warming peer;
  - its bound, `kMaxPeerWaitFrames` fresh frames in a row, and a frame with no announcement ending it;
  - a loading screen at a frame every two seconds, which never counts toward the bound, and a stale gap
    mid-wait starting the count over.
- **Fast-motion response.** The threshold and its 75% release, the two calm samples needed to leave,
  a spike resetting that count, switching it off mid-burst, and a scene cut repeating exactly one frame.
- **The low-fps floor.** Skipping below it, the 15% resume hysteresis, a pause restarting the
  average instead of counting as a low rate, and the floor winning over a pending cut.
- **Duplicate presents.** Advised once for an interleaved 30-in-60 cadence, with exact counts after
  the ring wraps. Not advised in three cases:
  - a paused game (all identical);
  - a moving game with a rare identical frame;
  - identical frames in long runs.

- **The HUD keys** (`Synth_Hud.h`). Near depth (`SynthesizedHudDepth`) never runs without synthesized
  motion, so its default of on leaves the input unchanged until motion is on; the layer
  (`SynthesizedHudLayer`) runs without motion but not under `DisableUI`; the mask is recorded exactly when
  one of them runs, over all sixteen combinations. For the feed: the mask's depth only for a frame whose
  mask executed, and the layer not before it has been written once.

## What it does not cover

The readback of motion rows, the estimator, the HUD mask's pixels (`tests/synth-motion-d3d12` runs those
on a GPU), FSR-FG's reaction to Reset, depth or a UI layer, the bridge and D3D12 call sites
(`tests/bridge-lifetime` covers the bridge's calls with fakes), and anything about image quality. Design:
`OptiScaler/dlssnr/design/synthesized-frame-generation.md`, "Motion into FG, and emulator behaviour" and
"The HUD: near depth and a UI layer".
