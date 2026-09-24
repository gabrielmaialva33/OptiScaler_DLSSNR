# NR invariants

Run `python3 tests/nr-invariants/run.py`. Pure Python: no compiler and no Wine prefix, so it is safe
beside a running build.

These are the mechanical guards `OptiScaler/dlssnr/design/DEVELOPMENT.md` §4 asks for, run against
the live tree rather than pinned to a baseline:

1. **Config round-trip (rule 4).** Every `CustomOptional` `DlssNr*` member in `Config.h` is read from
   `[DlssNr]` in `Config.cpp`, saved there under the same key, and present in the `[DlssNr]` section
   of the shipped `OptiScaler.ini`. Six keys are read and saved but deliberately not shipped (three
   legacy keys migrated on load, three opt-in diagnostics); they are listed in `run.py` with the
   reason each one's own code gives. A listed key that ships after all, or no longer exists, fails
   too, so the list cannot drift. The check found `HookMethod`, `RequireDlss` and `PresentSync`
   missing from the ini on its first run.
2. **Struct equals cbuffer (rule 5).** `DlssNrConstants` in `DlssNr_Common.h` and the `Params`
   cbuffer in `dlssnr.hlsl` are the same ordered list of 4-byte scalars: same types, same names (the
   HLSL `g` prefix aside). A divergence names the first scalar where they part.
3. **Headers are the committed bytecode (rule 6, the half a host can check).** `DlssNr_Shader.h` and
   `DlssNr_Shader_Vk.h` are byte-identical to what `create_header.py` makes from the `.cso` and
   `.spv` beside them, and each binary starts with its container magic.
4. **Retired identifiers.** A preprocessor gate on `DLSS_NEURAL_RENDERING` (the macro nothing defined,
   which silently dropped the present-time pass from every build), a read or save of the retired
   `Dx11BridgeHost` key, and a forwarder write of `DLSSNR.GlobalToneStrength` (a name the model never
   reads) all fail.

Each check was shown to fail on a real or injected defect before this was registered: the tree at
`2d868b87` fails check 1 on the three missing keys, and a renamed cbuffer field, a changed header
byte and a reintroduced macro gate each fail their check.

## What this does not cover

Whether the `.cso` / `.spv` are the compile of the current `dlssnr.hlsl` needs `dxc`, which lives in
the Wine prefix; that half of rule 6 stays with whoever edits the shader (`DEVELOPMENT.md` §3). Rule 7
(the passthrough gate everywhere the encode is reproduced) is a reading of shader logic, not a pattern,
and is not checked here. Nothing here runs anything on a GPU.

`tests/nr-before-upscale/verify-invariants.py` is the older, baseline-pinned version of checks 1 and 2
for one key. It asserts the shader is unchanged since `660303ec`, so it fails after any shader edit by
design; this suite is the live one.
