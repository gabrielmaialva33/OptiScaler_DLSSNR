# Streamline log repeat filter

```sh
python3 tests/sl-log-repeat/run.py
```

Compiles the production header `OptiScaler/hooks/Streamline_LogRepeat.h` with g++ under ASan/UBSan
and drives it with a fake clock. The input is the line that prompted it, verbatim from a Witcher 3
log of 2026-09-29 where `RSYNC: setDynamicMFGParams failed with status 1` was 4,893 of 5,955 lines,
from two threads.

Checked: the bracketed prefix (wall clock, tag, level, thread, uptime) is cut and nothing else; the
first line from a site is always written; 75 repeats a second from two threads write nothing for 10 s,
then one line carrying the 742 it stands for; the count restarts after each written line; different
text or a different status is another site and is written at once; the table is bounded and, past
256 sites, writes every new site every time instead of hiding it.

It does not run Streamline, spdlog or the callbacks in `Streamline_Hooks.cpp`; that wiring is
covered by the Release build and a game log.
