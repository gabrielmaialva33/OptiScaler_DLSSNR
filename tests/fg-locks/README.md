# Frame-generation swapchain locks

Run `python3 tests/fg-locks/run.py` with Python and g++.

This host suite compiles the real `OwnedMutex`/`OwnedLockGuard`, the acquisition and
release scopes sliced from both wrapped resize methods, all three FG backends'
`ReleaseSwapchain` bodies, and the end of FSR-FG's dispatch. Scripted device work
changes the mutex setting, selected FG object and output, or throws during cleanup.
The assertions require the original mutex to remain held during work, and exactly
that acquisition to be released afterwards. A pre-existing lock must remain held
when the operation acquired none. Cases also cover missing present queues, missing
FG, foreign windows, nested owner-1 release, and dispatch retaining another scope's
owner-1 lock. Builds run under ASan and UBSan with warnings treated as errors.

The resize slice replaces only device work; it retains the acquisition/release
code and their lexical scope verbatim. No DXGI, FFX, Streamline, XeFG, GPU, or actual
thread races are exercised. The production FG instances currently live until
process exit: hooks allocate them and teardown releases their resources rather
than deleting the object. A future change to that ownership requires retaining the
instance for each pending lock transaction as well.
