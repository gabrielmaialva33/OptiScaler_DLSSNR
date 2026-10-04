# Model cadence

Run `python3 tests/nr-cadence/run.py` from the repository root. Needs Python and `g++` with C++20,
ASan and UBSan. Builds into a temporary directory. `OptiScaler/dlssnr/design/model-cadence.md` is
what is being tested.

## What it runs

**The scheduler** (`scheduler.cpp`), the production `OptiScaler/dlssnr/DlssNr_Cadence.h` driven the
way `DlssNr_Dx12::Dispatch` drives it: decide, then record, carry or drop. Eighteen cases:

- off (`Cadence=1`, or anything outside 2..4) runs the model every frame and keeps no surfaces, so
  nothing is allocated or dispatched;
- the pattern for N = 2, 3, 4 is by age since the last model frame, counted in presents the pass ran
  at: present ids that advance by 2..4 per call (frame generation) do not change it, and a dropped
  call shifts it without flipping it;
- the summed chain is handed to the model only on a scheduled model frame after carried ones; the
  first carried frame starts a new chain;
- a pending reset forces a model frame, is cleared only by one, and never lets a frame carry;
- a change of frame or working size, format, route, what the model reads, or the feature forces a
  model frame and drops the edit; composition changes do not; a gap over 250 ms does; a second call
  in one present stands down for it and the next and stores nothing; a failed model frame leaves
  nothing to carry;
- each refusal -- native Vulkan, driver proxy, no shader, before the upscaler, no game guides, frame
  generation (and the opt-in), Hold frame, capture, no surfaces -- runs the model every frame with
  its own status text, and keeps or releases the surfaces as it should;
- the status texts are distinct, name the cadence, and carry no bare `%` (the pt-BR pack would
  reject it).

**The per-pixel rules** (`rule.cpp`), the production
`OptiScaler/shaders/dlssnr/precompile/dlssnr_cadence_rule.h` -- the header the shader includes --
with its texture loads turned into clamped array reads and the pass's five dispatches into loops in
the order `DlssNr_Cadence_Dx12` records them. A smooth texture on an infinite plane and a stand-in
model (a light sharpening and a lift; a strong lift on a red occluder) make eleven cases:

- the depth mismatch tells a wall at 2 m from 6 m in a conventional 0..1 buffer, which plain
  relative depth cannot; the colour gate passes a lighting change on hue and partly on brightness and
  rejects another surface; the Catmull-Rom read is exact at texel centres and never overshoots a step;
- a still scene carries the model's answer exactly on every pixel, borders included, and hands the
  model zero vectors;
- an integer pan carries exactly wherever the stand-in model, the taps and the edge ramp have what
  they need, and a sub-pixel pan within interpolation error (measured 0.0016 at worst, 2.4e-5 mean);
  both beat reusing the edit in place by a wide margin;
- a pan leaving the picture hands the model an off-picture vector there and the summed chain
  elsewhere;
- background an occluder uncovers restarts its chain from its own vector, never receives the
  occluder's edit, and is handed an off-picture vector for the model;
- a surface the model frame never saw gets no fill at all: the pixel is the game's frame, exactly;
- UI protection keeps the frame under the interface, in proportion to the alpha.

**Source guards** (`run.py`) on the pass that runs them: its constant struct equals its cbuffer and
fits a 256-byte view; every register is a root-signature slot and every unused one gets a null
descriptor; the shader defines every hook the rule header reads; the bytecode is included only behind
`__has_include`; the pass is built in one place, behind the scheduler's answer, and dispatched only by
decisions a running cadence makes; the refusals are wired to the inputs the design note names; only
`GatherFrame` marks guides as the game's; the pass is parked and freed with the rest; under peripheral
compression the carry reads the packed vectors at scale 1, as the model does, rather than converting
them a second time from the game's units (which carried every edit too far, and was caught only in
review when the two features met); it is registered
in the project; the licence ships and the rules carry their attribution.

## What it does not prove

No GPU, no NGX, no shader compile. The rules run as C++ over synthetic frames: whether dxc compiles
the shader, whether the bytecode behaves as the C++ does, what the real model does across skipped
frames (BeliyG3 measured a brightness lag), and how a game looks in motion are all outside it. The
half-float residual the GPU stores is a float here, so "exact" on the GPU means to half-float
rounding of the edit. The wine tier and an in-game run are the evidence for those; see the design
note's measurement plan.
