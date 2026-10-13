# nr-pulse-metric

Host suite for the pulse metric (`[DlssNr] PulseMetric`, `docs/dlssnr/design/pulse-metric.md`): the light step per
frame, the change of `log2(output light / frame light)` from one frame to the next, split into model frames, carried
frames and frames of a model that runs every frame.

Run: `python3 tests/nr-pulse-metric/run.py` (needs `g++` with C++20, ASan and UBSan).

`test.cpp` compiles the production header `retoque/dlssnr/DlssNr_Pulse.h`, the code the renderer runs on every
readback, and drives it with synthetic grids and frame sequences:

- the edit's light from the GPU's 16x16 grid: the tiles' sums, so a bright tile weighs as much as its light, with
  empty and non-finite tiles left out and a black frame giving NaN rather than a step;
- the kind of a frame from the cadence in effect and whether it was carried;
- pairing: only consecutive sequence numbers under 250 ms apart make a step; a hole, a gap, a non-finite frame and a
  readback that arrives after a newer one each break or are ignored as they should be;
- a window per 120 steps, filed by the later frame's kind, with mean |step|, signed mean, largest step and the
  model-over-carried ratio; the menu's words and the log line, number for number;
- reset dropping the window, the last frame and the reading; the log gate (first reading, then once per 10 s);
- a synthetic session where the model's light drifts by a random walk: at cadence 1 every frame steps; at cadence 3
  the carried frames of a still view step by nothing and the model frame by three frames' worth.

`run.py` also guards the GPU half and its wiring: the constants equal the cbuffer, the registers equal the root
signature, the shader's grid equals the header's, the bytecode is optional, the pass is built in one place behind the
key and the bytecode (off builds and records nothing), one sample per present, readbacks recorded before a change
are dropped, the target goes back in the state it arrived in, the pass is parked through the retired list, and the
files are registered in the project.

What it does not cover: the shader itself, a real device, a real readback, or whether the number tracks what an eye
sees. That is the in-game check in the design note.
