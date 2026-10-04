# nr-present-guides

Host unit test for the present-time guide snapshot ring (`OptiScaler/dlssnr/DlssNr_PresentGuideRing.h`).

Run: `python3 tests/nr-present-guides/run.py` (needs `g++` with C++20, ASan and UBSan).

Exercises:
- Ring slot acquisition ensuring the currently active/read present slot is never overwritten during evaluate recording;
- Ordering and selection of the newest completed submission token;
- Cross-queue non-stalling fallback: if the newest capture is in-flight on the producer queue, picks the newest completed older slot without GPU cross-queue waits (preventing DLSS-G present deadlocks);
- In-flight present read protection: slots currently being read on the present queue (including slots other than the last-read index) are skipped by evaluate recording;
- All-slots-in-flight skip: when all available slots have in-flight reads, returns `kRingSize` to skip capture without data corruption;
- Stale capture rejection: captures older than `kPresentGuideMaxAge` (4 frames) are dropped in favor of neutral zero guides (`kRingSize`).

What remains without host coverage:
- The actual D3D12 GPU `CopyTextureRegion` commands and resource barrier transitions recorded inside `CaptureTemporal` on the game command list;
- The fake stub `CaptureTemporal` in `tests/nr-before-upscale/fakes.h` intentionally performs no GPU allocations or copies; real GPU copy execution and format conversion are verified in-game on live D3D12 devices;
- The live integration with `DlssNr::Submission::Track` and `DlssNr::Submission::Completed` runs on real D3D12 command lists and queues in-game; the host tests simulate submission tokens via pure completion predicates.
