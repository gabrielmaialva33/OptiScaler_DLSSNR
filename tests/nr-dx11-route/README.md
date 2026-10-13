# NR D3D11 route

```bash
python3 tests/nr-dx11-route/run.py
```

Step 1 of `docs/dlssnr/design/dx11-native-dlss-guides.md`: with Neural Rendering on, a D3D11 game's DLSS running
natively, where the pass cannot run, is reported once in the log and shown in the NR panel while it runs, naming
`dlss_12` (DLSS on the D3D12 bridge, which runs the pass) as the route that carries NR.

The runner compiles `retoque/dlssnr/DlssNr_Dx11Route.h` (the decision, standard library only) and
`retoque/dlssnr/DlssNr_Dx11Upscaler.h` (the production layer the call site and the panel use) with g++ C++20,
ASan/UBSan and `-Werror`. The second is compiled against `stubs/Config.h`, which stands in for `Config.h`,
`SysUtils.h` and `OptiTypes.h`: the three keys the layer reads, the `Upscaler` enum, and log macros that record.
`OptiScaler/` is not on the include path, so nothing else of the DLL is compiled.

## What this covers

- **The report.** Nothing with NR off, nothing for `dlss_12`, FSR, XeSS or ray reconstruction; native DLSS
  gives the after-upscaler report at `HookMethod` 0, 1 or any other value, and the no-guides report at 2. Each text
  names the reason, the setting, and `dlss_12` as the route that carries NR, and is one line.
- **Once, not per frame.** 3600 native evaluates log one line; each report is logged once per process, however the
  route moves; eight racing threads still log one.
- **The panel's window.** A route not seen for more than a second is not shown; a sample stamped after the panel
  read the clock is.
- **The production layer, as a session meets it.** NR off: no store, no line, no panel text. NR on: the report
  logged once at warning while the panel follows `HookMethod` live, the line gone for `dlss_12`, back for native DLSS
  without a second log line, gone with NR off and once evaluates stop.
- **Source guards.** `NVNGX_DLSS_Dx11.cpp` names `DlssNr` twice (the include and one one-line call); the evaluate
  is noted after it ran, with the feature's own type. The layer returns with NR off before storing or logging. The stub's `Upscaler` enum equals `OptiTypes.h`'s and its keys' defaults equal
  `Config.h`'s. The panel asks for the line only with NR on, translates it, and drops "Waiting for the upscaler to
  run." when it shows. Each report text has a pt-BR entry (`nr-localization` cannot see these: they reach `Tr`
  through a variable).

## What this does not cover

No D3D11 or D3D12 device, NGX or DLSS is involved. Whether `dlss_12` creates and evaluates in a real game, what it
costs against native DLSS, and whether the pass then runs on the game's depth and motion are step 3 of the design
note, and need a D3D11 title that calls NGX directly. The panel is not rendered; `nr-menu` drives the panel's
control flow, not this line.
