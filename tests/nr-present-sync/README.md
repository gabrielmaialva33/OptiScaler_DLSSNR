# NR present sync

```bash
python3 tests/nr-present-sync/run.py
```

The runner compiles `OptiScaler/dlssnr/DlssNr_PresentSync.h` with g++ C++20, ASan/UBSan, and `-Werror`.

## What this covers

- **Auto without frame generation** does not wait: the write-back and the flip are on one queue.
- **Auto with any sign of frame generation** waits: called from a frame generation present hook, OptiScaler's own FG
  output selected, DLSS-G options or frame counts seen, or Streamline's DLSS-G plugin / `nvngx_dlssg.dll` loaded.
- **An explicit `true` or `false`** wins over all of that.

## What this does not cover

The inputs are gathered in `RunPresentPass` (`DlssNr_Dx12.cpp`) from `State` and the loader; that code and the wait
itself are not exercised here. Whether the write-back can race a paced flip is the design note's question
(`docs/dlssnr/design/nr-present-hook.md`), not this suite's.
