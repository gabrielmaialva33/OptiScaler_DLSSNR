# DRED — say what the GPU was doing when the device went

Status: implemented, **off by default** (`[DlssNr] Dred`), built and not yet run where it can work. Under
Proton it only logs that vkd3d-proton does not implement DRED; the first real run has to be on native Windows
(Rafael's RTX 3060).

## The problem

A device removal on native Windows reaches us as one HRESULT. The guide resample that removed Rafael's device at
its first frame (`13f10d60`, `reduced-scale-guides.md`, "Native Windows") logged `DXGI_ERROR_DEVICE_REMOVED` and
nothing else. Finding the 48-byte constant-buffer view behind it took a review of every descriptor the pass
creates, because the log could not say whether the GPU died in our work or the game's, let alone in which
command.

D3D12 has the instrument for this: Device Removed Extended Data. With it on, the runtime keeps, per command list,
a ring of the operations recorded and a counter the GPU advances as it completes them (auto-breadcrumbs), and on a
page fault the allocations that lived at the faulting address or had just been freed there.

## What it does

- `DlssNr::Dred::EnableBeforeDeviceCreation()` asks `D3D12GetDebugInterface` (through `D3d12Proxy`, so a proxy
  build reaches the real runtime) for `ID3D12DeviceRemovedExtendedDataSettings` and forces auto-breadcrumbs and
  page-fault tracking on. Once per process. It is called where a D3D12 device is about to be created: the
  `D3D12CreateDevice` hook, the `ID3D12DeviceFactory::CreateDevice` hook, and `CreateD3D12DeviceOnAdapter`, which
  is how the D3D11 bridge (the `Dx11BridgeHost` route) makes its own device. Devices created before the first
  call are not covered, and the log line says when that is why a report is empty.
- `DlssNr::Dred::Report(device)` runs from the two places a removal is noticed: `Util::GetDeviceRemovedReason`
  (failed presents) and the NR pass's own check before it records. It returns at once for a live device, so a
  caller that asks early does not spend the report, and otherwise logs once per process:
  - the removal reason;
  - for each command list the GPU had not finished, at most eight: its queue and list debug names, how many of
    its operations completed, and the operations around the stop with the first unfinished one marked `>>`;
  - how many lists the ring held and how many were unfinished;
  - on a page fault, the address and up to eight allocations at it and recently freed there, by type and name.

Names come from the game, its middleware and the driver; they are copied bounded (96 characters) with anything
unprintable replaced. The operation and allocation names are tables by value, not switches on the enumerators,
so an SDK older than 10.0.26100 still compiles them; a value past the table prints as its number.

## Why off by default

Breadcrumbs cost a write per operation on every command list, the game's included, and page-fault tracking keeps
allocation history. That is the right trade for a run chasing a removal and the wrong one for everyone else. The
setting takes effect at device creation, so changing it needs a restart.

## Proton

vkd3d-proton does not implement DRED: `D3D12GetDebugInterface` refuses the settings interface. The log says so
and names the equivalent, `VKD3D_CONFIG=breadcrumbs`, which writes vkd3d-proton's own breadcrumb report into its
log. A removal under Proton still gets the usual reason line.

## Telling our work from the game's

The after-upscale pass records into the caller's command list (the game's, or the upscaler's on OptiScaler's own
device), so a removal there appears under that list's name, and what tells it apart is the operation sequence
around the stop: our encode, the model's dispatches and the resolve. The lists NR owns are named,
`DLSS-NR present list <n>` for the present pass and `DLSS-NR present host setup <n>` for the bridge host's
one-time clears, so a removal in those reads as ours directly.

Not done: **`ID3D12DeviceRemovedExtendedData1`** adds breadcrumb context strings (Windows 11). The base
interface is used so this works back to Windows 10 1903.

## Validation

- `nr-invariants` holds the key to the four-point config round-trip.
- A Release build links it (`grep -a -c 'vkd3d-proton does not implement it' OptiScaler.dll` = 1).
- Not yet: a removal on native Windows with `Dred=true`. Until then, the claim is "builds and stays inert", not
  "works".
