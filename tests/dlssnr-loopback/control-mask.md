# ControlMask on a controlled presented frame

2026-09-27. Production baseline `dbadb2b8`.

**Result: `DLSSNR.ControlMask` had no observable effect.** In thirteen trials, every masked output was
byte-identical to its unmasked twin, at every captured frame. That held for four texture formats, for
masks of all zeros, all ones and one over the HUD, and with the automatic mask on and off. In the same
run, `DLSSNR.UseAutoMask` alone changed 31–44% of the output channels. So the harness can see a mask
setting move the picture; this one did not.

Like [the UICorrection result](ui-correction.md), this does not establish that the model never reads
the key. It establishes that nothing it did reached this output, on this model, in these conditions.

## Why it was tested

A draft change (2026-09-27) passed `DLSSNR.ControlMask` and its four subrect keys from
`DlssNr_Proxy::Run`. The names are real: the model binary (`nvngx_dlssnr.dll`, sha256
`6eb209e7…`) lists them among its resource inputs, right after `Output` and before `UI`, and
`UseAutoMask` and `SkinStructureStrength` sit next to them. But the draft wired the key into the
disabled driver-core path only, and no caller passed a mask. Before any production path grows a
control for it, this measures whether it does anything.

The hypothesis the trials were built around came from those neighbours: a caller-supplied mask that
stands in for the automatic one. So most trials set `SkinStructureStrength` 0 against local structure
1. A region the model treats as masked would then lose structure, and one it does not would keep it.

## Experiment

```bash
python3 tests/dlssnr-loopback/run.py --present-nr --mask-ab
```

- **Hardware and model:** RTX 4090, NVIDIA 615.71.09, Proton Experimental. The model is `6eb209e7…`,
  the same file every other measurement in this project used.
- **Frame:** the HUD fixture from the UICorrection experiment, 1280×720 SDR, 32 frames per trial.
  Production composition bytecode and the feature-18 forwarder, one pass at full resolution,
  Transfer 1, zero guides, UICorrection 1.
- **Model per trial:** every trial creates its own feature with Reset on frame 0 and identical
  history.
- **The mask:** set before CreateFeature and again before every evaluate, through the same
  unsigned-long-long parameter slot the forwarder uses for every resource. Its subrect is the full
  frame. Every write is read back with `Get` and must match.
- **Mask states:** 1.0 is the format's maximum. The HUD cover is every pixel any of the 32 frames' HUD
  changes, grown by 2 px: 262216 px, against 659384 px elsewhere.
- **Null after masks:** the parameter block is shared across trials, so an unmasked trial writes null
  over the last mask. The last R8 trial repeats the null control after the masks, to show that null
  restores the unmasked result.

| trial | mask | format | UseAutoMask | skin |
|---|---|---|---|---|
| base-r0, base-r1 | none | – | 1 | −1 |
| a0s0-null | none | – | 0 | 0 |
| a1s0-null | none | – | 1 | 0 |
| a0s0-zeros / -ones / -hud | 0 / 1 / HUD | R8_UNORM | 0 | 0 |
| a1s0-hud / -ones | HUD / 1 | R8_UNORM | 1 | 0 |
| a0s0-cleared | none, after the masks | – | 0 | 0 |
| a0s0-ones-r16f / -r32f / -rgba8 | 1 | R16_FLOAT / R32_FLOAT / R8G8B8A8_UNORM | 0 | 0 |

## Measured result

Each comparison is RGB8 after-images at frames 1, 8, 15, 16, 24 and 31, split by the HUD cover.
`analyze_mask.py` writes `artifacts/mask-run/mask-report.json`, with every capture's sha256.

| comparison | HUD MAE / max | rest MAE / max |
|---|---|---|
| base-r0 vs base-r1 (repeat floor) | 0 / 0 | 0 / 0 |
| UseAutoMask 0 vs 1, no mask | 0.33–0.57 / 10–18 | 0.42–0.52 / 7–10 |
| every mask trial vs its unmasked twin | 0 / 0 | 0 / 0 |
| null after the masks vs null before | 0 / 0 | 0 / 0 |

All 416 evaluations succeeded. All 13 ApplyModel=0 controls reproduced the source exactly.

## What this does not establish

- **Other models:** another model build, preset or style might read the key.
- **Separate UI layers:** the HUD is inside `Color`, as in production. No `UI`, `UIAlpha` or
  `Backbuffer` inputs were supplied, and a control mask might only matter alongside those.
- **Other conditions:** HDR, reduced working scale and real motion were not tried.
- **Every value:** masks were uniform or binary. A fractional mask was not tried.

## What it changed

Nothing in production. No path sets the key, and none should until something shows it doing
something. A menu control for it would be a dead control. The `--mask-ab` mode stays so the question
can be asked again, in one command, of the next model.
