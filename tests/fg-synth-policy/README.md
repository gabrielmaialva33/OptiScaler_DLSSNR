# Synthesized FG policy and the motion handoff

Run `python3 tests/fg-synth-policy/run.py` from the repository root. It builds `test.cpp` with g++
C++20, `-Werror`, ASan and UBSan in a temporary directory and includes the production headers as
they are: `OptiScaler/inputs/FG/Synth_Policy.h` and `OptiScaler/shaders/synth_motion/SynthMotion_Handoff.h`.
Neither touches the GPU, so nothing is faked beyond two empty `ID3D12Device` / `ID3D12Resource`
structs.

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
  - its `kMaxPeerWaitMs` bound, and a frame with no announcement ending the wait.
- **Fast-motion response.** The threshold and its 75% release, the two calm samples needed to leave,
  a spike resetting that count, switching it off mid-burst, and a scene cut repeating exactly one frame.
- **The low-fps floor.** Skipping below it, the 15% resume hysteresis, a pause restarting the
  average instead of counting as a low rate, and the floor winning over a pending cut.
- **Duplicate presents.** Advised once for an interleaved 30-in-60 cadence, with exact counts after
  the ring wraps. Not advised in three cases:
  - a paused game (all identical);
  - a moving game with a rare identical frame;
  - identical frames in long runs.

## What it does not cover

The readback of motion rows, the estimator, FSR-FG's reaction to Reset, the bridge and D3D12 call
sites (`tests/bridge-lifetime` covers the bridge's calls with fakes), and anything about image
quality. Design: `OptiScaler/dlssnr/design/synthesized-frame-generation.md`, "Motion into FG, and
emulator behaviour".
