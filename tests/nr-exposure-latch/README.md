# NR exposure plausibility latch

```bash
python3 tests/nr-exposure-latch/run.py
```

The runner compiles `retoque/dlssnr/DlssNr_Exposure.h` with g++ C++20, ASan/UBSan, and `-Werror`.

## What this covers

- **Normal game**: an exposure that stays stable or moves gently never latches over 1,000 readings.
- **Crimson Desert's eye adaptation**: the implied white points logged on 2026-10-12 (0.10..307, with a jump from
  0.28 to 303 inside 120 readings) and ten-stop sweeps inside [0.02, 512] never latch. The 8-stop swing test that
  came first latched on exactly this, which is why it was removed.
- **One end alone**: hundreds of readings above 512, or below 0.02, never latch (a loading screen, a black frame).
- **Both ends**: one reading at each end does not latch; two at each end do, once, with the extremes recorded.
- **Control**: an exposure jumping between 0.00025 and 116 latches within a few readings.
- **Ignored readings**: NaN, infinity, zero and negative values change nothing.
- **Reset**, and **fallback behaviour**: when latched, `Exposure::WhitePoint` for source 1 returns the slider and
  `Exposure::LiveWanted` is false; sources 0 and 2 are unaffected.
