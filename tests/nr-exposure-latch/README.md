# NR exposure plausibility latch

```bash
python3 tests/nr-exposure-latch/run.py
```

The runner compiles `OptiScaler/dlssnr/DlssNr_Exposure.h` with g++ C++20, ASan/UBSan, and `-Werror`.

## What this covers

- **Normal stable/slow game**: an exposure that remains stable or moves gently never latches over 1,000 readings.
- **One wild reading**: a single out-of-range reading does not latch.
- **Three out-of-range readings**: three readings outside [0.02, 512] (non-consecutive) latch the session.
- **Swinging window**: an 8.1-stop swing within 120 readings latches, while the same 8.1-stop swing spread smoothly over 200 readings does not.
- **Reset**: `Reset()` restores the unlatched state and clears all history and diagnostics.
- **Fallback behaviour**: when latched, `Exposure::WhitePoint` for source 1 falls back to the manual slider, and `Exposure::LiveWanted` is false.
