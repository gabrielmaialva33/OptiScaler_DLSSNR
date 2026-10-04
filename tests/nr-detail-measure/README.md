# nr-detail-measure

Host test suite for "Measure detail" CPU tile reduction and statistical aggregation (`OptiScaler/dlssnr/DlssNr_DetailStats.h`).

Run: `python3 tests/nr-detail-measure/run.py` (needs `g++` with C++20, ASan and UBSan).

Exercises:
- `ReduceGrid` with zero/empty tiles (handles empty tiles and sets `detailBand = NAN`);
- `ReduceGrid` over uniform synthetic tile grids (verifies correct weighting by measured pixel counts across 256x64 grid);
- Derived metric calculations: `Flicker()`, `Saturation()`, `ShadowDarkening()`, and `AddedBand()`;
- `Accumulator` aggregation over 60 samples: mean computation, textual phrase generation, and delta comparison against prior measurement runs.
