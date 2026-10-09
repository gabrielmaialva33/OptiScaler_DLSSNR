# NR render size resolution

```bash
python3 tests/nr-render-size/run.py
```

The runner compiles `OptiScaler/dlssnr/DlssNr_RenderSize.h` against mock NGX parameter blocks with g++ C++20, ASan/UBSan, and `-Werror`.

## What this covers

- **Subrect present**: returns the valid non-zero subrect dimensions.
- **Subrect absent / zero with fallback**: when subrect dimensions are missing or both zero, uses `Width`/`Height` when both are present and `Width < OutWidth` (following `IFeature::GetRenderResolution`).
- **Subrect absent with Width == OutWidth**: returns `0/0` (not a scaled render subrect).
- **One key present and zero**: a subrect key present with value zero is treated as not stated; incomplete subrect pairs are preserved for caller validation; fallback keys with zero or missing dimensions return `0/0`.
