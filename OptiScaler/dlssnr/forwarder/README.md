# nvngx.dll_dlssnr.dll

Neural Rendering's snippet resolves the module that owns its caller's return address and refuses
anything whose path does not contain `nvngx.dll` -- the driver core being `_nvngx.dll`. It returns
`FAIL_PlatformError` before it inspects a single argument. OptiScaler installs as `dxgi.dll` or
`winmm.dll`, so it fails that test like anything else would.

This library exists to be named correctly. It does nothing else: it forwards create, evaluate and
release, so the calls into the snippet originate from a module the snippet accepts.

It is built with the rest of the solution (`dlssnr_forwarder.vcxproj`, into `x64/<Config>/a/`), and
`build-local.sh` copies it to `x64/out/` beside `OptiScaler.dll`. It is not committed. To build it on
its own:

    cmake -S . -B build -A x64
    cmake --build build --config Release

One detail is load-bearing and not obvious. The forwarder must not `return snippetFn(...)` -- that is a
tail call, the compiler emits a `jmp`, this module's frame disappears, and the snippet then resolves its
caller to whoever called the forwarder. The result goes into a `volatile` local first.

It also contains faults. Every call into the model runs inside `__try`; the first exception of error
severity (or an escaping C++ throw) is recorded, and from then on every export refuses to enter the
model again -- release included, because a model that faulted can be left holding its own lock. The
host reads `dlssnr_fault_state` and switches Neural Rendering off for the session with the exception
code and faulting module in its log. A forwarder built before this has no such export; a host built
before it never asks, and still gets the refusals.
