# nr-present-guides

Host unit test for the present-time guide snapshot ring (`OptiScaler/dlssnr/DlssNr_PresentGuideRing.h`).

Run: `python3 tests/nr-present-guides/run.py` (needs `g++` with C++20, ASan and UBSan).

Exercises:
- Ring slot acquisition ensuring the currently active/read present slot is never overwritten during evaluate recording;
- Ordering and selection of the newest completed submission token;
- Cross-queue non-stalling fallback: if the newest capture is in-flight on the producer queue, picks the newest completed older slot without GPU cross-queue waits (preventing DLSS-G present deadlocks);
- Slot invalidation and fallback to neutral zero guides (`kRingSize`) when no completed snapshot is available;
- In-flight present read protection: slots currently being read on the present queue are skipped by evaluate recording.

What remains without host coverage:
- The actual D3D12 GPU `CopyTextureRegion` commands and resource barrier transitions recorded inside `CaptureTemporal` on the game command list;
- The fake stub `CaptureTemporal` in `tests/nr-before-upscale/fakes.h` intentionally performs no GPU allocations or copies; real GPU copy execution and format conversion are verified in-game on live D3D12 devices.
