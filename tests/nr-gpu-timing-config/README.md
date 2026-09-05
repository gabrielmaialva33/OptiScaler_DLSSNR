# NR GPU timing configuration tests

Run `python3 tests/nr-gpu-timing-config/run.py` from the repository. Requires a C++23
compiler with AddressSanitizer/UndefinedBehaviorSanitizer and the checked-out SimpleIni headers.

The runner extracts the real `CustomOptional`, timing accessors, INI read/save expressions
and serializers from `Config.h/.cpp`. It tests defaults, strict malformed-input rejection,
interval bounding, memory and disk round-trips, runtime precedence, retaining the interval
while disabled, and concurrent setter/snapshot/save transactions. It also checks all four
configuration points and unique MSBuild registrations for the timing implementation.

Any failed assertion or compiler/sanitizer error is a failure. Passing proves the configuration
contract on the host; it does not execute D3D12, validate timestamps or prove an allocation-free
disabled renderer. Those requirements belong to the timing and renderer tests and the real-game
acceptance procedure.
