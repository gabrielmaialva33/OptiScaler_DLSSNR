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

- `DlssNr::Dred::EnableBeforeDeviceCreation(factory)` asks `D3D12GetDebugInterface` (through `D3d12Proxy`, so a
  proxy build reaches the real runtime; with no proxy module yet, the link-time import, which is what
  `CreateD3D12DeviceOnAdapter` then creates the device with) for `ID3D12DeviceRemovedExtendedDataSettings` and
  forces auto-breadcrumbs and page-fault tracking on, and breadcrumb contexts where the runtime has
  `ID3D12DeviceRemovedExtendedDataSettings1`. Once per process. It is called where a D3D12 device is about to be
  created: the `D3D12CreateDevice` hook, the `ID3D12DeviceFactory::CreateDevice` hook, and
  `CreateD3D12DeviceOnAdapter`, which is how the D3D11 bridge (the `Dx11BridgeHost` route) makes its own device.
  The device-factory hook also passes its factory: an Agility SDK `ID3D12DeviceFactory` keeps a configuration of its
  own, which the process-wide settings do not reach, so the same settings go through its
  `GetConfigurationInterface(CLSID_D3D12DeviceRemovedExtendedData)`, each time a device comes from it. Devices
  created before the first call are not covered, and the log line says when that is why a report is empty.
- `DlssNr::Dred::Report(device)` runs from the three places a removal is noticed: `Util::GetDeviceRemovedReason`
  (failed presents), the NR pass's own check before it records, and the D3D11 bridge's copy fence wait
  (`Dx11wDx12SC::_WaitForCopyAllocator`), which is often where a removed bridge device shows first. It returns at
  once for a live device, so a caller that asks early does not spend the report, and otherwise logs once per
  process:
  - the removal reason, and the device state (hung, fault, page fault) where the runtime has
    `ID3D12DeviceRemovedExtendedData2`;
  - for each command list the GPU had not finished, at most eight: its queue and list debug names, how many of
    its operations completed, and the operations around the stop with the first unfinished one marked `>>`; with
    `ID3D12DeviceRemovedExtendedData1`, each operation's context string beside it and the last event begun before
    the stop;
  - how many lists the ring held and how many were unfinished;
  - on a page fault, the address and up to eight allocations at it and recently freed there, by type and name.
- `DlssNr::Dred::Name(object, name, index)` and `DlssNr::Dred::ScopedEvent` give our own objects debug names and
  our own spans of work breadcrumb contexts. Both do nothing with the key off: no `SetName`, no
  `BeginEvent`/`EndEvent`, so objects keep exactly the names they had and lists record exactly what they did.

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
one-time clears, so a removal in those reads as ours directly. Those two are named whether the key is on or not.

With the key on, the rest of what the D3D11 bridge owns is named too: `Retoque D3D12 interop queue`,
`Retoque bridge interop list <n>` and `Retoque bridge interop allocator <n>`, and the synthesized frame-generation
inputs' `Retoque synthesized FG input list`, `... allocator` and `... frame list <n>`. Everything the bridge records
per frame goes on its interop list, so that list also carries three contexts: `Retoque bridge: DLSS-NR present
host`, `Retoque bridge: synthesized FG inputs` and `Retoque bridge: copy to the presenter`. A report from a hung
bridge then says which of the three the GPU was in.

The extended interfaces are optional: without `Settings1`, `Data1` or `Data2` (Windows 10 before 2004, and some
drivers), the base interface still gives the breadcrumbs and the page fault, without contexts or state.

## Folded in from `fix/native-d3d12-present` (2026-10-14)

That branch carried a DRED of its own, written the same day for a Divinity hang on Rafael's PC (the neural pass,
synthesized frame generation and DLSS-G all on the bridge's device, and a log that said only
`DXGI_ERROR_DEVICE_HUNG`): `retoque/misc/Dred.{h,cpp}`, commit `968a41ca`, switched on by `RETOQUE_DRED=1` in the
environment or a file named `retoque_dred` beside the DLL, read in `dllmain.cpp`. Two switches for one instrument
break "one quantity, one control" (`DEVELOPMENT.md` §1), so the merge into the default branch kept this module and
this key and folded the other one in:

- **Taken:** the device-factory configuration; breadcrumb contexts (`Settings1`, `Data1`) and the device state
  (`Data2`); the link-time fallback for `D3D12GetDebugInterface`; `Name` and `ScopedEvent`, with the bridge's and the
  synthesized inputs' names and the bridge's three contexts; the report from the bridge's copy fence wait.
- **Dropped:** the environment variable and the marker file, and their read in `dllmain.cpp`: `[DlssNr] Dred` is the
  one control. Its per-device report: the bridge, the D3D11-with-D3D12 upscaler and `State::currentD3D12Device` share
  one device (`WithDx12::GetD3D12Device`), so once per process says the same, as above. Its longer dump (every list
  up to 64, twelve operations before the stop) and its `where` argument: this report's eight unfinished lists with
  contexts say which span stopped, and the caller's own error line comes right before it. Its `PageFaultOutput1`/`2`
  reads, which printed nothing the base output does not.

A build from that branch needs the marker; a build from the default branch needs `[DlssNr] Dred=true` instead, and
ignores the marker.

## Validation

- `nr-invariants` holds the key to the four-point config round-trip.
- A Release build links it (`grep -a -c 'vkd3d-proton does not implement it' OptiScaler.dll` = 1).
- `bridge-lifetime` holds the bridge's report site: a failed copy-slot wait asks for a report, an idle wait does
  not.
- Not yet: a removal on native Windows with `Dred=true`. Until then, the claim is "builds and stays inert", not
  "works".
