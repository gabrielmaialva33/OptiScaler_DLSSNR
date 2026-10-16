# Fork compatibility regressions

Run `python3 tests/fork-compat/run.py` on the host with Python and g++.

This suite slices the real production code into three small executables, compiled
with AddressSanitizer and UndefinedBehaviorSanitizer:

- `Config.cpp`'s XeSS/XeFG/XeLL read/save statements and `readWString`, with
  `Config.h`'s real `CustomOptional` and SimpleIni. Distinct paths and individual
  overrides must stay independent through read, save into an empty INI, and read
  again. Missing keys and case-insensitive `auto` remain unset.
- Both signature hook bodies (`Crypt32_Hooks.h`, `Wintrust_Hooks.h`) against
  minimal Win32/COM-boundary fakes. Only the case-insensitive basename
  `nvngx.dll` is redirected: the driver's `_nvngx.dll`, the NR forwarder,
  similarly named files and directory names pass through. Also checks existing
  spoofing/replacement gates, non-file/empty inputs, trust-state propagation and
  FFX handling.

- `Streamline_Proxy.h`'s actual active-feature resolver, group binding, bundled
  hooks, Reflex disable and D3D12/Vulkan init entry points. Real vendored SDK
  signatures, feature IDs, Reflex options and VulkanInfo are used. Loader,
  Win32 module lookup and the shared `InitStreamline` bootstrap are scripted
  boundaries. Cases cover complete active bindings; every missing required
  sibling falling back as one group while other groups remain active; optional
  camera/PCL telemetry; SDK device-before-binding ordering; failed device/info
  registration; D3D11 bridge versus native second-device quirks; cross-API
  request guards; missing/failed Reflex options; and Vulkan refusing incomplete
  required groups after both sources fail. D3D12 preserves its nonfatal binding
  fallback policy. Init caching and VulkanInfo forwarding are checked too.

Run `python3 tests/fork-compat/run.py --mutations` to additionally verify eight
negative controls fail: missing group resets (three), delayed bridge binding,
D3D12 stealing a Vulkan request, binding after failed device registration,
calling a null Reflex export and treating incomplete Vulkan bindings as ready.
Mutations affect generated temporary translation units only.

The binding executable uses C++20 because this vendored Streamline revision's
C++23 `to_underlying` alias is invalid under GCC. Other executables keep C++23.
The tests do not call Windows trust providers, load a DLL, initialize Streamline
plugins, exercise the GPU, or run NGX/a game. The shared `InitStreamline` shim
means plugin discovery/preferences and SDK support checks are outside coverage.
