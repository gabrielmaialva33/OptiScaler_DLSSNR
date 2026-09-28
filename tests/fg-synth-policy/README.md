# Synthesized FG policy, its motion statistics, and the motion handoff

Run `python3 tests/fg-synth-policy/run.py` from the repository root. It builds `test.cpp` with g++
C++20, `-Werror`, ASan and UBSan in a temporary directory and includes the production headers as
they are: `OptiScaler/inputs/FG/Synth_Policy.h`, `OptiScaler/inputs/FG/Synth_MotionStats.h` and
`OptiScaler/shaders/synth_motion/SynthMotion_Handoff.h`. None of them touches the GPU, so nothing is
faked beyond two empty `ID3D12Device` / `ID3D12Resource` structs.

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
- **The motion statistics** (`SynthMotionStats::Measure`). The rows are R16G16_FLOAT halves, laid out
  as `SynthInputs` reads them back: `kRows` rows on a pitch aligned to 512 bytes, every `kStride`-th
  pixel read. The cases cover:
  - a uniform pan: exact counts of vectors and pairs, every statistic at the pan, nothing incoherent;
  - a still frame with negative zeros, and a width that is not a multiple of the stride, whose moving
    padding is never read;
  - the median and p90 by nearest rank on a known distribution, and the per-component median vector;
  - the pair rule: the 2 px floor and the 25% of the larger vector, in any direction, and alternating
    8x8 blocks, where every block boundary disagrees;
  - non-finite vectors, which stay out of every statistic but `allZero` and break the pairs beside
    them;
  - the summary window's means, maxima and reset.
- **The coherence rule.** It works on rows measured through the statistics:
  - a coherent 40 px pan keeps generating at the suggested 0.25. This case fails on the rule before
    it: `Configure(12, 0)` repeated every frame of it.
  - alternating +-30 px blocks repeat;
  - alternating +-8 px blocks, under 1% of the width, keep generating;
  - the cap still forces a repeat, alone, beside the rule, and at Generation Zero's old 12 px;
  - both at 0, the default, repeat nothing;
  - hysteresis: each level holds at 75% of its threshold, two calm samples end the response, and a
    held sample starts the count over;
  - a threshold change decides afresh, and the same values every frame reset nothing.
- **The cap on its own** (`SynthesizedFastMotion`). These are the original fast-motion cases on the
  new signature, and they pass unchanged. They cover:
  - the threshold and its 75% release, and a sample exactly at the release level not counting as
    calm;
  - the two calm samples needed to leave, with a spike resetting that count;
  - switching it off mid-burst;
  - a scene cut repeating exactly one frame.
- **The low-fps floor.** Skipping below it, the 15% resume hysteresis, a pause restarting the
  average instead of counting as a low rate, and the floor winning over a pending cut.
- **Duplicate presents.** Advised once for an interleaved 30-in-60 cadence, with exact counts after
  the ring wraps. Not advised in three cases:
  - a paused game (all identical);
  - a moving game with a rare identical frame;
  - identical frames in long runs.

## What it does not cover

- The GPU side: the copy of the rows into the readback, the estimator, FSR-FG's reaction to Reset,
  and the bridge and D3D12 call sites (`tests/bridge-lifetime` covers the bridge's calls with fakes).
- Anything about image quality.
- Whether the thresholds suit a real game. They are the design's estimates, to be tuned from the
  10 s summary in the log.

Design: `OptiScaler/dlssnr/design/synthesized-frame-generation.md`, "Motion into FG, and emulator
behaviour" and "Coherence, not speed".
