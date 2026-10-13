# Menu Mode: Sustaining Neural Rendering During Inactive Upscaler Windows

Status: **design note; not yet implemented.** Proposes optional continuation of Neural Rendering during game menus, inventories, and pause screens when upscaler evaluation calls cease.

---

## 1. Where this comes from

In many modern engines, opening a menu or pause screen halts game world rendering and stops upscaler evaluations entirely. In current OptiScaler DLSS-NR builds, this causes an abrupt visual disconnect: Neural Rendering instantly disengages, resulting in noticeable luminance and contrast pops while browsing menus.

The solution is established across community projects:

1. **optimizer-fps-dlss5** (BeliyG3, commit `1714312`, October 2026, MIT licence):
   - `docs/MENU_MODE.md:8-10`:
     > *"It starts on its own at least 150 ms and 5 presents after the game's last NR frame, and ends at the game's next NR frame. That frame restarts NR's history"*.
   - `hosts/reshade/menu_state.h:34-35`: defines engagement window `kEntryPresents = 5; entryMs = 150.0`.
   - `hosts/reshade/menu_guides.cpp:25,103`: snapshots the last valid depth buffer and feeds a synthesized zero-motion vector field.
   - Acceptance validation (`docs/dev/menu-mode-acceptance.md`): output quality measured 44–49 dB PSNR against reference; transition reset cost measured at 5.7 dB across the first five recovery frames.
   - Verified in-game in *Star Wars Jedi: Fallen Order* via OptiScaler's D3D11 bridge.
2. **RenoDX** (`v8.5dev`, MIT licence):
   - `present_path.hpp:40-43, 161-169`: treats guide captures older than the age limit as menu/loading states, continuing presentation passes on neutral guides with bidirectional history resets.

---

## 2. What exists in this codebase today

- **Present skipping on stale guides:**
  In `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp:5884` (under `HookMethod=2`, Present mode):
  ```cpp
  if (temporalValid && temporalSeq == g_nrLastEnhancedSeq)
  {
      ++g_presentSkips;
      ReportRuntimeStatus(false, "held temporal guides: waiting for fresh upscale");
      return;
  }
  ```
  If the game stops evaluating DLSS, the present pass immediately aborts.
- **Inactive under standard HookMethod=1 (Upscaled):**
  In `DlssNr_Dx12.cpp:5717-5718`, the present pass exits immediately under the default hook configuration (`HookMethod=1`). When the game pauses and stops calling DLSS evaluate, NR does not run anywhere.
- **Existing reusable components:**
  - `DlssNr_ZeroGuides.h/.cpp`: zero-motion and constant-depth generator (`DlssNr::ZeroGuides`);
  - `ZeroGuideReset` logic;
  - `DlssNr_UiMask*`: HUD protection mask designed to preserve sharp 2D UI elements.

---

## 3. Proposed design

An opt-in configuration (`[DlssNr] MenuMode=true`, default `false`) that bridges upscaler inactivity:

```
[ Normal Gameplay ] ───▶ Upscaler evaluates each frame ───▶ NR runs via normal path
                                  │
                       (Pause menu opened: >150 ms & >5 presents since last evaluate)
                                  │
                                  ▼
[ Menu Mode Active ] ──▶ Force NR model reset
                     ──▶ Engage present hook with:
                           • Swapchain backbuffer as color input
                           • Captured last-frame depth (or 1.0f neutral plane)
                           • Zero motion vectors
                           • UI Protection Mask active over 2D interface
                                  │
                       (Game unpaused: new upscaler evaluate detected)
                                  │
                                  ▼
[ Return to Gameplay ] ─▶ Force NR model reset ──▶ Resume normal upscaler path
```

### 3.1 Transition state machine
- **Engagement condition:**
  $t_{\text{current}} - t_{\text{last\_upscale}} \ge 150 \text{ ms} \quad \text{and} \quad \text{presents\_since\_upscale} \ge 5$.
- **Entry action:**
  Set `g_nr.reset = true` (flushes previous motion history so in-game motion does not smear into static menu elements).
- **Execution:**
  Route frame through `RunPresentPass`. Feed `depth` from last captured frame, `motion` from zero-cleared buffer, and run `DlssNr_UiMask` to mask out fonts, buttons, and HUD boxes.
- **Exit action:**
  Upon observing the next real upscale evaluate call, immediately trigger another `g_nr.reset = true` to prevent menu imagery from corrupting the returning 3D camera history.

### 3.2 Handling `HookMethod=1` (Upscaled mode)
Under `HookMethod=1`, `RunPresentPass` normally stands down entirely. When `MenuMode` is enabled and menu inactivity is confirmed, `RunPresentPass` temporarily activates as an opportunistic fallback, relinquishing presentation as soon as the upscaler evaluation resumes.

---

## 4. Where it goes

1. **`OptiScaler/Config.h` / `Config.cpp`:**
   Declare `CustomOptional<bool> DlssNrMenuMode { false };`, mapped to `[DlssNr] MenuMode`.
2. **`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`:**
   - In `RunPresentPass`: implement timeout detection tracking elapsed presents and wall-clock time since `g_upscaleRenders` last incremented.
   - Switch to zero guides and set reset flags upon state transition.
3. **`OptiScaler/dlssnr/DlssNr_Menu.cpp`:**
   Add toggle under the Compatibility or Advanced sub-panel.

---

## 5. Interactions with existing features

- **Invariant 1 (Default-Identical):**
  Disabled by default (`MenuMode=false`). Behavior remains byte-identical to current code when unset.
- **HUD Protection (`hud-protection.md`):**
  Crucial synergy. In menus, textual elements and panels dominate the screen. Without the UI mask, the neural model sharpens font edges inconsistently. The UI mask must remain enforced during menu mode.
- **Model Cadence (`model-cadence.md`):**
  Must automatically drop to cadence 1 (`runModel = true` every frame) during menu mode, as motion vector interpolation over zero motion adds no value and introduces phase jitter.

---

## 6. Risks

1. **Animated menu backgrounds:**
   If a pause menu renders an animated 3D character with moving camera or particle effects in the background, zero motion vectors will cause slight temporal ghosting across background motion.
2. **Unpause transition penalty:**
   BeliyG3 measured a ~5.7 dB PSNR drop across the first five frames following unpause due to the mandatory model history reset. This is visually preferable to smearing, but represents a transient settling period.

---

## 7. Test plan

1. **Host unit test (`tests/nr-menu-mode`):**
   Simulate presentation frames with injected evaluate gaps:
   - Verify engagement triggers at exactly $>150 \text{ ms}$ and $\ge 5$ presents;
   - Verify `reset` flag is pulsed on both entry and exit;
   - Confirm `MenuMode=false` never activates present hooks under `HookMethod=1`.
2. **In-game validation:**
   - Test in *Cyberpunk 2077* and *Crimson Desert*. Open inventory/map; confirm luminance stays stable and text remains crisp without artifacts.

---

## 8. Open questions

- Should the depth buffer in menu mode be frozen to the last valid 3D depth, or replaced with a flat far-plane ($1.0$) to avoid depth edge artifacts against menu UI?
