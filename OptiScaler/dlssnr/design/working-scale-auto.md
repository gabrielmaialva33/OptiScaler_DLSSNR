# What `WorkingScale=auto` means, per GPU

Status: **implemented, measured on one card below Ada.** It acts on
[model-cost-across-architectures.md](model-cost-across-architectures.md), which recorded the cost and
proposed nothing.

## The problem

`[DlssNr] WorkingScale=auto` meant 1.0 on every card. On the one card below Ada this project has
measured, an RTX 3060, that is 53.3 ms for the model at 1920x1080 and about 90 ms at 2560x1440 by
the two-point fit. The pass alone caps the game near 19 fps at 1080p. Anyone who installs a release
on an RTX 20 or 30 card and switches the pass on starts there, before they find the slider.

## What changes

`auto` now stands for a value that depends on the GPU the pass runs on:

| GPU | auto |
|---|---|
| Ada and later (RTX 40, 50) | 1.0, as before |
| Turing and Ampere (RTX 20, 30) | 0.5 |
| Not identified | 1.0, as before |

There is no new key. An explicit `WorkingScale` wins everywhere, as it always did.
`RRWorkingScale` is not touched: its default is already 0.5 on every card.

The key is now declared `SoftDefault`. Under the ordinary rule, a value equal to the declared default
of 1.0 is saved back as `auto`, which was harmless while auto was 1.0 everywhere. On a card where
auto is 0.5, an explicit 100% would have silently turned back into 50% at the next save.

## Why 0.5

It is the half-scale point that was measured below Ada. On the 3060, 960x540 cost 16.8 ms against
53.3 ms at full scale, 68% less. Subnautica 2 on the same machine measured 18.7 ms at 0.5.

The "25x" that the ini and the menu quote is the per-pixel term of one RTX 3060 against one RTX 4090,
with the operating system and driver also different. The 3060 is three to four times slower at
general compute, so architecture accounts for something like 7x of it. A 3090 against a 4060 would
land somewhere else. The text says "an RTX 3060 measured", not "RTX 30 costs".

It is also where the rest of the defaults already sit. `RRWorkingScale` defaults to 0.5, and the
default enlargement (`Transfer=1`, matched residual) is the one that removes the colour shift at 50%.

A value chosen to hit a pixel budget, dependent on the output resolution, was considered and left out.
It would turn a line through two points into a law. A flat 0.5 is only what was measured.

## What it does not do

It does not make the pass viable on these cards. 16.8 ms is a whole 60 fps frame, and Subnautica 2
ran at about 19 real fps with the pass on at 0.5. The earlier note's conclusion stands: the hardware
floor for this model is Ada. This is the less bad default for someone who switches it on anyway.

## Which GPU, and how it is identified

**The adapter of the device the pass records on**, not IdentifyGpu's "primary GPU". A machine with a
display GPU and a separate render GPU is exactly where those two differ. The one Ampere machine this
project has seen is one: an RX 570 drives the display and the 3060 renders.

- **D3D12**: the device's adapter LUID, matched against IdentifyGpu's list.
- **Vulkan**: the physical device's PCI vendor and device id, matched the same way.

If neither matches and the list holds exactly one NVIDIA adapter, that adapter is taken, and the log
says so. The model only runs on NVIDIA, so a single candidate is not a guess. This covers a LUID that
a translation layer invented differently on the two sides.

Resolved once per adapter, not per frame: IdentifyGpu hands out a copy of its whole list.

**By NVAPI architecture id, not by PCI device-id ranges.** IdentifyGpu already asks NVAPI. The 3060's
session log reports `arch=0x170`, which is `NV_GPU_ARCHITECTURE_GA100`: NVAPI uses one id per
generation, whatever the die (GA102 and GA104 are implementations under GA100 in `nvapi.h`, as the
AD10x dies are under AD100). The two generations are named one by one, `== TU100 || == GA100`, not
given as a range below Ada. The ids rise by generation, but Hopper (GH100, 0x180, with FP8 tensor
cores) sits between Ampere and Ada, and a range would catch it.

The earlier note read `0x160` "on an RTX 5090" in Feeder's `feed_opti.h` as evidence against that
order. It is not evidence. That value is the `MinHWArchitecture` the driver returns for the
SuperSampling requirements query: the oldest architecture DLSS accepts, which is Turing. It is not the
card's architecture. The note is corrected.

Device-id ranges drift and overlap. The 0x2200–0x25BF range an earlier draft called "Ampere" also
contains the H100 (0x2330). It also leaves out Ampere laptop parts above 0x25BF.

**No architecture, no change.** Without NVAPI, the id is 0 and `auto` stays 1.0. That covers a prefix
with no `nvapi64.dll` and fakenvapi, which IdentifyGpu deliberately does not trust. `[Libraries]
NvapiPath` does not help here: IdentifyGpu loads `nvapi64.dll` from System32 only. So a Proton title
that needs `NvapiPath`, such as Divinity, gets 1.0 on an RTX 3060. That fails safe, and Windows is
not affected.

## Turing is inferred, not measured

Nothing below Ada except one Ampere card has been timed. Turing has no FP8 tensor cores either, and
its tensor cores are a generation older. Nothing suggests it would be cheaper per pixel than Ampere.
Turing is included on that reasoning, and the reasoning is named here so the first Turing measurement
can overturn it.

## Invariants

- **Default-identical** (DEVELOPMENT.md rule 1) holds on Ada and later, on an unidentified GPU, and
  for any explicit `WorkingScale`. On RTX 20 and 30 with `auto` the default moves on purpose; that is
  the change.
- **One quantity, one control.** One key. The menu slider shows the scale the pass will actually use.
  While it is `auto` the value reads "(auto)", so a slider at 50% on a card nobody set to 50% says
  why. Moving it writes an explicit value, as before. An **Auto** button next to it clears that value.
  It appears only while there is a value to clear, because auto differs per GPU and so is not just
  another percentage on the slider.
- **Config round-trip.** No key was added.

## How to check it

The first dispatch logs one line per adapter:

```
DLSS-NR: WorkingScale=auto is 0.50 on NVIDIA GeForce RTX 3060 (architecture 0x170)
DLSS-NR: WorkingScale=auto is 1.00 on NVIDIA GeForce RTX 4090 (architecture 0x190)
```

With an explicit `WorkingScale` the line ends `-- not in use, WorkingScale is set`. Then
`DLSS-NR working size:` shows the size it produced.

Known and left alone:

- Until the pass has seen its device, auto reads 1.0. The menu shows "100% (auto)" before the first
  dispatch.
- On RTX 20 and 30, the first `DLSS-NR spatial contract` line of a session is logged from the
  evaluate before Dispatch runs, so it reports scale 1.0. The next line reports 0.5. The line is
  diagnostic only.
- Auto is one value for the adapter noted last. Two devices on different adapters dispatching in
  turn would re-resolve on every frame. No route does that today: the bridges put their D3D12 device
  on the render GPU.
