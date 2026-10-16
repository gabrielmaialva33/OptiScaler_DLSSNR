# Streamline game camera forwarding

Run `python3 tests/fg-game-camera/run.py` on the host with g++.

The suite compiles the real Streamline `sl::Constants` definition, the owned camera cache,
the production camera-construction/`SetConstants` output statements, input-frame mapping,
`StartNewFrame`/`GetDispatchIndex`/counter methods and cache-clearing lifecycle prefixes under
ASan/UBSan. It covers high external frame IDs, present resetting the logical counter, queued
versus resource-driven dispatch decisions with different frame-ahead allowances,
delayed input, exact dispatch selection, invalid replacements, missing constants at ring reuse,
deactivation/resource release, Unreal fallback, invalid/incomplete matrix groups, optional
lens/pose validation and concurrent input/present access.

The DirectX projection constructor and SDK callback are fakes. This verifies CPU forwarding
and lifetime decisions, not camera correctness in a particular game or GPU/image quality.
