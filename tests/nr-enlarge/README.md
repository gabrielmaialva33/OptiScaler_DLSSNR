# Transfer 2: the edit enlarged by a private DLSS SR

Run `python3 tests/nr-enlarge/run.py`. Needs Python and a `g++` with C++20, `_Float16`, ASan and UBSan.
Design: `OptiScaler/dlssnr/design/dlss-enlargement.md`.

Three binaries, each built under ASan/UBSan with `-Werror`:

- **`rules.cpp`** compiles `dlssnr/DlssNr_Enlarge.h` as shipped. The decision table for every transfer
  value, every SR state and every reason the resolve is sent 1 instead of 2: native Vulkan, the model
  at or above the frame (equal, 2x, one axis above -- the supersampling case wilsjo2's build recreated
  the feature on every frame), before the upscaler, zero guides, guides the build cannot resample, the
  model-output debug view, warm-up and failure. The Vulkan 2 -> 1 map. The menu's index table (index 2
  stays Transfer 3). When the SR's history resets, its motion scale (0 while held), when a creation
  has crossed far enough to evaluate, where its guides come from. The carrier: zero is exactly
  neutral through FP16, NaN and infinity are neutral, the carrier stays in (0, 1), the decode clamps
  at 15.6, and through IEEE binary16 an edit from 1e-4 to 1 comes back within 8% with the 1/64 scale
  while a unit scale loses an edit of 3e-4 entirely.
- **`adapter.cpp`** links `dlssnr/DlssNr_PrivateSr.cpp` as built (with a stub `pch.h`, which is the
  proof it needs nothing else) against `ngx_fakes.h`, a recording driver core whose parameter table
  keeps every write with the overload it came through. Creation: one table, one SuperSampling feature
  (never Ray Reconstruction) on the given list, exactly eight parameters, flags `MVLowRes` plus
  `DepthInverted` when asked and none of IsHDR, AutoExposure, MVJittered, sharpening. Evaluation:
  sixteen parameters, resources typed, zero jitter, the motion scale as given (0 included), unit
  exposure, sharpness 0. Release frees the handle, then the table, once; abandon calls nothing; a
  missing entry point is skipped; a refused or handle-less create gives the table back.
- **`host`** is assembled by `run.py` from production text: `NgxResultName`, the retirement list
  (`PrivateSrApi` through `RetiredCapacity`, the bundle and `ParkNrEnlarger` included), the whole
  Transfer 2 section of `DlssNr_Dx12.cpp` (`PlanEnlarge`, `BuildEnlarger`, `EnlargeEdit`) and
  `RetryAfterFailure`/`EnlargeStatus`, compiled against `host_fakes.h`: resource states every barrier
  must chain through, scratch allocation, the composition pass's `DispatchPass`, the guide resample,
  submission tracking, the proxy's getters, the process-exit flag. `host_cases.h` drives it one
  Dispatch at a time in Dispatch's order. It checks that Transfer 0, 1, 3 and 4 allocate, call and
  record nothing; that the SR is built on one frame and evaluated only from a later present (two NR
  frames where no present is counted, and only once the creation completed where submissions are
  tracked); the carrier and unit-exposure dispatches; the states at the NGX call; resets on the
  model's reset, a skipped frame and hold edges; the three guide sources; a rebuild on a new size,
  depth direction, device or frame; failure held until Retry (create refused, core missing, textures
  refused, evaluate refused); release after the retirement list is whole, with a re-entrant park from
  inside the release; the parked-object cap; no NGX call at process exit; and that every texture ends
  where it started.

`run.py` also holds the sources to the shape the binaries assume: the shader's mode numbers, 1/64,
0.999, the empty-answer test and passthrough gate in the carrier, the decode before the Difference
view, exactly four Transfer 2 branches; Dispatch planning once before the timing contract, enlarging
between `Chain::Resolve` and the resolve, falling back to 1 when a planned run did not happen, and
restoring the output; the creation frame never evaluating; the release after the list settles; the
Vulkan resolve's only read of `Transfer` going through the map; the menu's table, its status line
having a text for every reason, and Retry only on failure.

## What it does not cover

No GPU, no NGX, no shader runs. Nothing here says the carrier survives DLSS, what DLSS does with it,
whether native D3D12 accepts the states and calls, or what it looks like and costs. The bytecode is
not compared with the HLSL (`nr-invariants` compares headers with bytecode only). That is the
loopback's `--transfer-ab` and a game, on Proton and on native Windows.
