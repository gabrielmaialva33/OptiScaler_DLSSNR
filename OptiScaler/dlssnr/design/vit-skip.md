# ViT Skip: Bypassing the Neural Rendering ViT Bottleneck on Alternating Frames

Status: **design note; not yet implemented.** Proposes a staged implementation: Step 1 implements an NvAPI CUDA kernel profiler to measure execution breakdown on local hardware; Step 2 evaluates skipping the Vision Transformer (ViT) bottleneck kernels on alternate frames.

---

## 1. Where this comes from

The concept originates from **janblade** (`OptiScaler` fork, `origin/main` branch):

- **Commit history:**
  - `34e33c6a` (2026-09-21): introduced `DlssNrVitReuse.h` ("Reuse bottleneck");
  - `fcd45900`: enabled by default;
  - `424bc3be`: synchronized all passes to compute on the same frame;
  - `d27e6877`: added explicit UAV barrier across the skipped execution gap;
  - `cb9b9b7c` and `eac86f09`: forced off in Vulkan titles due to visual flashing;
  - `db065ec2`: introduced kernel timestamp profiler (`DlssNrKernelProfile.h`, `[DlssNr] KernelProfile=true`).
- **Core mechanism (`DlssNrVitReuse.h:3-14`):**
  > *"The ViT bottleneck (blocks 31-38, the coarsest and most stable level of the network) is one contiguous run of those launches, from cc_vit_1d_repack_2d_to_1d through cc_vit_1d_repack_1d_to_2d, and about a fifth of the evaluation. Leaving that run out on some frames leaves the previous frame's result in its output buffer ... the run's sync counters are referenced by no kernel outside it ... Anything that does not look like that turns the feature off for the session instead of guessing."*
- **NvAPI interception:**
  `NvApiHooks.cpp:206,273` redirects internal kernel launch queries through `DlssNrNative::WrapNvapi`. Intercepted interface IDs include:
  `0xad1a677d`, `0xe2436e22`, `0x24973538`, `0x41c65285`, `0xdf295ea6`.
- **Licence:** GPL-3.0 (same as OptiScaler).

---

## 2. What exists in this codebase today

- **NvAPI Hooking:**
  `OptiScaler/nvapi/NvApiHooks.cpp:190-245` intercepts `NvAPI_QueryInterface` and routes between the host driver, `fakenvapi`, and internal wrappers.
- **Model Dispatch:**
  `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp:4650-4750` evaluates the model feature (`g_nr.evaluate`) on the command list.
- **Theoretical cost model:**
  `OptiScaler/dlssnr/design/model-cost-across-architectures.md` discusses relative costs across GPU architectures based on external benchmarks, but currently lacks live kernel-level breakdown data on this machine (RTX 4090) and Rafael's PC (RTX 3060).

---

## 3. Proposed staged design

### Step 1: In-flight NvAPI Kernel Profiler (`DlssNrKernelProfile`)
Before modifying kernel execution flow, integrate the profiler as a low-overhead diagnostic tool:
1. Hook NvAPI kernel launch queries during model evaluation.
2. Intercept kernel name registrations (`cc_swin_*`, `cc_vit_*`, `cc_dec_*`).
3. Inject D3D12 timestamp queries around kernel clusters for 3 out of every 240 evaluations.
4. Read back data 90 frames later to avoid CPU readback stalls.
5. Log exact per-group execution times into `OptiScaler.log`, establishing hard evidence on kernel cost shares under Proton/Wine.

### Step 2: ViT Bottleneck Skip (`VitEvery`)
Once kernel timings confirm that `cc_vit_1d_*` accounts for $\sim 18-22\%$ of total evaluation time:
1. Wrap model evaluation in `DlssNr_Dx12.cpp` with `BeginModelEvaluate()` / `EndModelEvaluate()`.
2. When `VitEvery > 1` and the current frame is eligible:
   - Identify the contiguous run from `cc_vit_1d_repack_2d_to_1d` to `cc_vit_1d_repack_1d_to_2d`.
   - Return success without dispatching the kernels in that run.
   - Insert a UAV execution barrier across the persistent internal scratch buffer to ensure the previous frame's latent tensor remains valid and ordered for the subsequent decoder kernels.
3. Fail-safe: if kernel signatures, block counts or launch order differ from the known model profile, latch the feature off permanently for the session.

---

## 4. Where it goes

1. **`OptiScaler/nvapi/NvApiHooks.cpp`:**
   Add interception inside `hkNvAPI_QueryInterface` for CUDA kernel launch tables.
2. **`OptiScaler/dlssnr/DlssNr_KernelProfile.h`:**
   Header-only telemetry collector for Step 1.
3. **`OptiScaler/dlssnr/DlssNr_VitSkip.h`:**
   Header-only state tracker managing skip cadence and signature matching.
4. **`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`:**
   Delimit `g_nr.evaluate` with profiler/skip markers.

---

## 5. Interactions with existing features

- **Model Cadence (`model-cadence.md`):**
  Model cadence skips the *entire* model call on carried frames. ViT skip operates *inside* model evaluations. On a carried frame, ViT skip must stand down completely. On a model frame, ViT skip can run normally.
- **Multi-pass Chains (`DlssNr_Multipass.h`):**
  janblade discovered that staggering ViT skips across sequential passes in a chain caused severe high-frequency flickering (tested in *Dawnwalker*). If active in multi-pass, all passes must compute the ViT bottleneck on the identical frame.
- **Reset Invariant (`DEVELOPMENT.md` §1):**
  Any model history reset (`g_nr.reset`, scene cuts, camera teleportation) must force a complete ViT evaluation (`skip = false`).

---

## 6. Risks

1. **Vulkan / Proton instability ("lightning flashes"):**
   janblade observed dark-scene flashing under Vulkan (`cb9b9b7c`), concluding that Vulkan memory aliasing or barrier semantics caused the latent buffer to be recycled externally. Because every D3D12 call under Proton translates to Vulkan (via `vkd3d-proton` and `dxvk-nvapi`), this issue could reproduce on Linux.
2. **Model version dependence:**
   Kernel symbol names (`cc_vit_1d_*`) are private internal symbols of NVIDIA's `nvngx_dlssnr.dll`. A new model release (e.g. post-310.8) could restructure layer blocks or rename kernels, breaking the pattern.
3. **Architecture differences:**
   Tested in the wild primarily on RTX 50-series (Blackwell) and 40-series (Ada Lovelace). Behavior on Ampere (RTX 3060) remains unmeasured.

---

## 7. Test plan

1. **Host unit test (`tests/nr-vit-skip`):**
   Simulate NvAPI kernel launch stream against synthetic signature buffers, verifying:
   - Correct detection of ViT entry/exit markers;
   - Clean shutdown on malformed sequences;
   - Enforced full evaluation on reset.
2. **In-game benchmarking:**
   - Execute Step 1 (profiler) in *Crimson Desert* and *The Witcher 3*. Compare execution percentages of `swin_*` vs `vit_1d_*`.
   - If viable, activate Step 2 and test specifically for dark-scene flash artifacts under Proton.

---

## 8. Open questions

- Does `dxvk-nvapi` forward the required private NvAPI CUDA launch table IDs cleanly without overhead on Linux?
- Can the intermediate latent buffer suffer from race conditions when asynchronous compute queues run alongside graphics?
