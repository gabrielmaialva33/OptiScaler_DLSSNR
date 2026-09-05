# NR submission model tests

Run `python3 tests/nr-submission/run.py` from the repository root. Requires g++ with
C++20, pthreads, ASan and UBSan. A pass requires exit zero and both PASS lines;
assertion, sanitizer or boundary-guard failure is a failed test.

Tests compile the production portable lifetime model. They cover missing/delayed
submission, duplicate execution/replay, multiple queues, Reset before fence
completion, failed Reset, failed Signal, device-loss UINT64_MAX, exact-epoch
notification across Reset, bounded admission, 3000 retirements and 1000 concurrent
Reset/notification pairs under the caller's mutex contract. A source guard checks
both actual queue-hook paths notify after their original ExecuteCommandLists.

These tests do NOT execute COM, Detours, D3D12, NGX or GPU work. Hook coverage,
unknown wrapper implementations and real queue/device-loss behavior require
separate integration validation. No game installations or markers are accessed.
This directory is outside the release package's explicit artifact allow-list.
