# nr-present-guides

Host unit test for the present-time guide snapshot ring (`OptiScaler/dlssnr/DlssNr_PresentGuideRing.h`).

Run: `python3 tests/nr-present-guides/run.py` (needs `g++` with C++20, ASan and UBSan).

Exercises:
- Ring slot acquisition ensuring the currently active/read present slot is never overwritten during evaluate recording;
- Ordering and selection of the newest completed submission token;
- Cross-queue non-stalling fallback: if the newest capture is in-flight on the producer queue, picks the newest completed older slot without GPU cross-queue waits (preventing DLSS-G present deadlocks).
