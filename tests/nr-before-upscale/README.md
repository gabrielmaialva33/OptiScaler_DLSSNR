# Pre-upscale NR boundary tests

From the repository root:

```sh
python tests/nr-before-upscale/run.py
python tests/nr-before-upscale/verify-invariants.py --base 660303ec
./build-local.sh Release
```

Requires Python and g++ with AddressSanitizer/UndefinedBehaviorSanitizer support.
The runner extracts the **current production** `ScopedPreUpscale` constructor and
destructor, `EvaluateAfterUpscale`, scratch allocation, and sRGB checks; changed or
missing extraction anchors fail. It compiles those functions with strict host fakes
and includes the production color-binding and stabilization helpers directly.

PASS means assertions completed with no sanitizer failure: disabled/default paths do
not read/swap parameters or allocate pre-upscale textures; typed/untyped slots are
restored even if the caller throws; no replacement occurs without a successful
composition; pre/post routing runs at most once; configured input states are restored;
nested scopes do not borrow the same scratch; unsupported contracts are rejected;
1000 failed-allocation attempts cause one allocation; 1000 changing DRS evaluations
cause none. Checks also cover the 500 ms threshold and format-change debounce.

These tests **do not run D3D12, the composition shader, the NVIDIA model, a real
upscaler, or a game**. The fake composition reports success/failure to exercise the
real boundary. GPU completion, delayed retirement, image quality and performance
require separate integration/game validation. The Vulkan overlay harness exercises
none of these NR paths and is not evidence for them.

Generated sources and executables remain in the ignored local `artifacts/` directory.
No solution/project or release packaging file includes this test.

The invariant check is specific to this shader-free port: it requires the C++
constant list, HLSL source and both bytecode headers to match the selected baseline,
checks ordered scalar correspondence, and verifies Stage declaration/read/save/INI.
It does not prove byte-identical rendered frames or run the full Config persistence code.
