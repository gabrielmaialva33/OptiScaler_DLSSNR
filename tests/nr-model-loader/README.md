# NR model loaders

Run `python3 tests/nr-model-loader/run.py` from anywhere. It builds in a temporary directory with
g++ C++23, ASan and UBSan and `-Werror`, then runs two source checks in Python. About two seconds.

## Why this suite exists

Two decisions about loading `nvngx_dlssnr.dll` are made from raw bytes, before anything is called.
Both come from `OptiScaler/dlssnr/DlssNr_PeScan.h`, which is header-only and free of
`<windows.h>` so it can be tested here as shipped:

- **`ModelLoader=direct`** (`DlssNr_DirectRuntime`) needs the model's `GetModuleFileNameW/A`
  import slots, found by name, so it can answer the model's caller check without the forwarder DLL.
  The technique is wilsjo2's (OptiScaler-DLSSNR-PreSR-Multipass v0.8.1–0.8.3, GPL-3).
- **The 616.64 route** (`DlssNr_NgxInfo`, the `UseProxy` guard in `DlssNr_Proxy`) reads the
  NGX loader's feature-id → snippet-name table to see whether entry 18 names `dlssnr`. The table
  layout is NIGos/dlss5-bridge's (MIT). See `OptiScaler/dlssnr/design/ngx-driver-616.md`.

## What it exercises

1. **`test.cpp`**, against synthetic PE32+ images laid out as the loader maps them:
   - the import scan finds W and A slots, across more than one DLL; skips ordinal and bound-only
     imports; does not match near-miss names; and refuses (returns false, empty result) a missing
     import directory, an unterminated descriptor list, a missing IAT, a name outside the image, a
     name with no NUL, a thunk array running off the image, a misaligned IAT, PE32, i386, non-MZ,
     short headers and a negative `e_lfanew`;
   - the table scan reports Present / Absent for entry 18 = `u"dlssnr"` / `u""`, with the table's
     offset; Unknown for a different name, a null entry, a wrong anchor at entry 11, a wrong base,
     a table in a read-only section, one cut by its section's end, a section past the image, and a
     non-image; and finds the real table after a near-match decoy;
   - `ModelKnownToFaultOnLoaderRoute` / `KnownFaultingPairing` as a truth table (310.8.0.0 and older
     fault on the Present route only; 310.8.2.0 does not; all-zero is not a verdict);
   - `DriverFromLoaderVersion`, **sliced out of `DlssNr_NgxInfo.cpp`** by `run.py` (that file needs
     `<windows.h>`): 32.0.16.1664 → 616.64, 32.0.15.6636 → 566.36, leading-zero minors, and empty
     for shapes that are not NVIDIA's.
2. **Local evidence**, when the files exist on this machine (override with `NR_LOADER_DLL`,
   `NR_MODEL_DLL`): the real `/usr/lib/nvidia/wine/_nvngx.dll` must have a recognisable table, and
   the real model must have a parsable import table with at least one caller-path slot. Mapped from
   disk by section at the preferred base. CI has neither file and runs the synthetic cases only.
   On 2026-09-28, with Linux driver 615.71.09 (wine loader **32.0.16.1691**) and model 310.8.2.0:
   the loader *routes feature 18 itself* (table at +0x15fa60), and the model has one W and one A slot.
3. **Drift** (`run.py`): the direct runtime's `QueryScalingRatio`, `Create`, `Evaluate`, `SetExtras`
   and `Release` write the same `set{UInt,Float,Resource}(capabilityParams, "NAME", value)` sequence
   as the forwarder's `dlssnr_query_scaling_ratio`, `dlssnr_call_create`, `dlssnr_call_evaluate`,
   `dlssnr_call_set_extras` and `dlssnr_call_release`, in the same order and with the same values
   (63 writes today); make the same `Guarded(...)` model calls (so the same application id and
   `Init_Ext` arguments); use the forwarder's `FaultFilter` verbatim and its `kFaulted`; scope the
   caller alias around the call in `Guarded`; and never `return Guarded(...)` (a tail call).
4. **Wiring** (`run.py`): `EnsureForwarder` picks `ModelLoader=direct` before loading the forwarder;
   the NGX line is logged before `ModelLog::Install` at the first build; `ProbeD3D11` checks for a
   null forwarder before `GetProcAddress` (a null module there searches the game's executable); the
   proxy refuses the faulting pairing before it creates feature 18; and the forwarder is the default.

All of the above was mutation-checked when the suite was written: fourteen single-point mutations
(a changed value, a dropped write, the application id, the fault filter, the alias restore, a tail
call, the version predicate, the IAT alignment check, the ordinal skip, the table anchor, the
writable-section filter, the minor padding, the default and the proxy guard) each fail it.

## What it does not cover

- It runs no model and no NGX. Whether the model **accepts** the adapted answer is only shown in a
  game: `DLSS-NR direct: N caller-path import slot(s) of the model adapted`, then a feature built
  with no `FAIL_PlatformError`. Whether the 616.64 route **faults** was measured by dlss5-bridge on
  Windows, not here.
- It does not patch or re-read a loaded image; `VirtualProtect`, the compare-exchange on the slot and
  the per-thread alias are Windows-only and reviewed, not executed.
- The drift check compares source text. It holds the two call sequences equal; it does not prove
  either is what the model wants.
