# Moving-scene evidence checks

Run `python3 tests/nr-moving-evidence/run.py` with Python's standard library.

This runs the independent analyzer's tests from `dlssnr-loopback/test_moving.py`:
known projected motion must agree with the depth/camera oracle, zero or nonfinite
motion must fail, disabled processing must fail, repeated-output noise must not
hide the processing effect, and the timing percentiles use nearest rank.

The actual analyzer is shared with the real moving-scene GPU experiment. These
checks exercise its decisions on known inputs without Wine, NGX or a GPU; they
cannot establish rendering or model correctness.
