# DLSS-G and MFG on RTX 20 and RTX 30: what stands in the way, and what to do about it

Status: design for review, 2026-10-14. Nothing here is built and nothing has run in a game. The offline part of step 0
is done (the provider inventory below); the in-game part, a log-only probe on Rafael's RTX 3060, is the first slice
proposed. Written against `508d4d2d` on `feat/neurotic-parity`; line numbers are into that tree. NeuRotic line numbers
are into its clone at `7132a103`, read, not built and not run; nothing is copied. `dlssg_for_sm86` (sdli1995, GPL-3) was
read in a clone at `9621db5` (0.3.5), and its logs from Rafael's PC are quoted where they are the only measurement.
Roadmap item 5 in [../roadmap.md](../roadmap.md).

Read first: [mfg-count-override.md](mfg-count-override.md) (MFG above 2x on Ada, and the five-gate probe method),
[model-cost-across-architectures.md](model-cost-across-architectures.md) (Rafael's 3060, where NVIDIA's runtime
refused frame generation), `retoque/framegen/dlssg/MfgUnlock.h` and `MfgPatchPlan.h` (the Ada unlock and its
transactional write layer), [dred.md](dred.md) and [DEVELOPMENT.md](DEVELOPMENT.md).

## In short

- **There is no hardware gate in the evidence.** Below Ada, NVIDIA's DLSS-G stops at three software decisions and one
  missing build: Streamline's adapter admission, the NGX core comparing the card against the provider's exported
  minimum architecture (`mov eax, 0x190; ret` in every build here), the provider's own MFG count gates (which
  `MfgUnlock` already rewrites on Ada), and 31 frame-construction kernels that ship only as sm_89 SASS and
  sm_89/sm_120 PTX.
- **The networks are already built for Ampere.** Every DLSS 4 provider on this machine (310.1.0.0 to 310.9.1.0) carries
  its 39 network kernels as sm_86 SASS next to the sm_89 SASS, the same FP16 network on both (in 310.9.1, 718
  `HMMA.16816.F16` and 9 `HMMA.1688.F16` in each, no FP8), and the provider's own code has a branch that picks a
  third network class when the device reports an SM below 89. Nothing in them is sm_75.
- **The optical-flow accelerator is not involved.** None of the game-shipped 310.x providers references the Optical
  Flow SDK. The one build that does is the copy the Linux driver ships for Wine.
- **NeuRotic's experiment** (built on the MIT `mfg_ampere` port) patches all of that for exactly one provider build,
  310.9.1.0, and logs `hardwareQualification=NOT_RUN` as a constant. Nobody has run it on a 20 or 30 series card.
- **`dlssg_for_sm86` did run on Rafael's 3060**, with the NGX core answering feature 11 as supported once the
  provider's minimum architecture read 0x170. It ships NVIDIA's runtime and recompiled NVIDIA kernels inside its DLL.
  We would not.
- **Recommendation: (a), Ampere only, default off, one exact provider build at a time**, staged so that the first
  thing to run is a log-only probe, then the kernel half forward-compiled on the 4090, then the admission half on
  Rafael's card, and only then both together. On Ampere the kernel half reduces to the transformation the Ada kernel
  swap already does, with 86 where it writes 89. RTX 20 is out of scope until there is a Turing tester: it needs
  instruction rewriting inside NVIDIA's PTX, not a relabel.

## The problem, and the demand

RTX 20 and 30 owners get DLSS Super Resolution and Ray Reconstruction from NVIDIA and no frame generation at all.
Measured on Rafael's RTX 3060 (Windows 11, driver 610.62, Mortal Shell II), NVIDIA's own Streamline plugin refused at
the capability query (model-cost-across-architectures.md, "Frame generation does not exist there at all"):

```
dlss_gEntry.cpp:331   Disabling DLSS-G since it is not supported on current hardware
commonEntry.cpp:1115  NGX_*_GetFeatureRequirements feature: 11 FeatureSupported == AdapterUnsupported
commonEntry.cpp:1089  NGX_*_GetFeatureRequirements returned 0xbad00001 for adapter: 1 feature: 11
```

The demand is not hypothetical. Rafael has since run a third-party loader, `dlssg_for_sm86`, in Subnautica 2, Mortal
Shell II, Dying Light: The Beast and Monster Hunter Wilds; one of the kits we assembled for him is
`~/.local/state/dlss5/rafael-kits/20260929-dlssg-sm86/`. That project's own issue numbers ran past #560 by 0.3.5.
NeuRotic shipped an experiment for the same cards. `DLSS5-Autopilot` and `DLSS5-Swapper` advertise "RTX 20/30/40/50".

What these owners already have without any of this, in this tree:

- **The game's DLSS-G inputs, OptiScaler's frames.** With `[FrameGen] FGInput=dlssg`, upstream answers Streamline's
  DLSS-G support queries itself (`OptiScaler/hooks/Streamline_Hooks.cpp:347-363`, attached only for that input at
  `:2315-2325`) and generates frames with FSR-FG or XeFG. That is upstream's existing answer for these cards: the
  game's own frame generation menu, someone else's frames.
- **Synthesized frame generation** for titles that give no motion
  ([synthesized-frame-generation.md](synthesized-frame-generation.md)).

So the question is narrower than "frame generation on 20/30". It is whether NVIDIA's own DLSS-G image, with its MFG
counts, is worth running on cards NVIDIA declines, from the game's own provider, without shipping any NVIDIA file.

## What NVIDIA gates, and where

Evidence classes: **read** is this tree's or a binary's bytes, read here; **measured** is a log from real hardware;
**claim** is another project's statement that nothing here has checked.

| # | Gate | Where it sits | Answer below Ada | Evidence |
|---|---|---|---|---|
| G1 | Streamline admits the DLSS-G plugin | `sl.common` compares each adapter's architecture against the minimum `sl.dlss_g` declares (AD100), at `slInit`, before a device exists | plugin evicted: `Ignoring plugin 'sl.dlss_g' since it is not supported on this platform` | claim (`dlssg_for_sm86` `docs/INSTALL.en.md`, `SpoofArchToGame` row, measured by them on Streamline 2.8.0.0, FF7 Rebirth, RTX 2080 Ti). Upstream already rewrites the architecture Streamline sees inside `sl.dlss_g`'s `slOnPluginLoad`, scoped and restored (`Streamline_Hooks.cpp:956-990`, `:1087-1102`), for the replacement path only (read) |
| G2 | Hardware-accelerated GPU scheduling | Streamline (`eFeatureNotSupportedHWSchedulingDisabled`) and the NGX core (requirement flag 8) | refused when off | claim (`dlssg_for_sm86`); NVIDIA's published requirement for DLSS Frame Generation. Rafael's HAGS is **on** (measured, their `fg_gate_environment`, 2026-10-04) |
| G3 | NGX core: `GetFeatureRequirements` and `CreateFeature` for feature 11 | the driver's `_nvngx.dll` compares the GPU architecture against the provider's exported `NVSDK_NGX_GetGPUArchitecture` | `0xbad00001`, `AdapterUnsupported` at the query; `0xbad0000b` (`FAIL_OutOfDate`) at create | export bytes `b8 90 01 00 00 c3` in all ten builds (read, below); stock refusal (measured, above); **with the provider's minimum reported as 0x170 the core answered `FeatureSupported 0`, `MinHWArchitecture 0x170`, `FrameGeneration.Available 1`, and `CreateFeature(11)` succeeded at 1920x1080** (measured, Rafael's 3060, driver 616.92, Monster Hunter Wilds, `dlssg_for_sm86` logs, 2026-10-04) |
| G4 | Provider MFG count gates | `nvngx_dlssg.dll` compares against 0x1b0 (Blackwell) when it publishes and validates `DLSSG.MultiFrameCountMax` | one generated frame | read: `MfgUnlock.cpp:61-72` (310.9 shapes), `tests/mfg-pattern`; NeuRotic's two 310.9.1 gate RVAs, `0x16864` and `0x35e50` (`provider_profile.hpp:64-67`), are the immediates of our `kAdvertisePattern309` (RVA `0x16862`) and `kValidatePattern309` (RVA `0x35e4f`) |
| G5 | Wrapper ceiling and DRS clamp | `sl.dlss_g.dll`, `min(published, 3)` and a driver-profile cap | 4x / 2x | read: `MfgUnlock.cpp:30-47`, `:450-520`. Architecture-independent |
| G6 | Kernel availability | the provider's CUDA images | the 31 frame-construction kernels have nothing an sm_86 or sm_75 device can load | read: the inventory below. `dlssg_for_sm86` says the same of 310.9.1 ("sm_89 blobs that cannot be created on Ampere at all", `docs/INSTALL.en.md:76`), claim |

Not a gate but a route: the provider chooses its network implementation by SM version, and has a branch for SM below 89
(read, disassembly below).

### Why NVIDIA gates it, as far as the evidence goes

NVIDIA's public explanations, not re-verified for this note: DLSS 3 frame generation (2022) was tied to Ada's faster
Optical Flow Accelerator; DLSS 4 (January 2025) replaced hardware optical flow with an AI model and tied Multi Frame
Generation's pacing to Blackwell's hardware flip metering.

What the binaries say against that:

- **Optical flow hardware.** None of the nine game-shipped 310.x builds contains `nvofapi`, `NvOFA`, `NvOF`,
  `OpticalFlow` or even `OFA`, and none imports `nvcuda.dll` (their imports are `KERNEL32`, `USER32`, `ADVAPI32` and
  `VERSION`; CUDA is reached through the NGX core). The Linux driver's own Wine copy
  (`/usr/lib/nvidia/wine/nvngx_dlssg.dll`, nvidia-utils 615.78.08, "NVIDIA DLSS-G - DVS PRODUCTION", version 310.2.1.0)
  is a different lineage: it names `nvofapi64.dll` and `NvOFAPICreateInstance{Vk,D3D12,Cuda}`, imports `nvcuda.dll` and
  `vulkan-1.dll`, and carries 97 sm_89-only containers and no sm_120. For the providers games ship, the OFA argument
  does not apply. Turing and Ampere have OFA units anyway; the old argument was their speed, not their absence.
- **Tensor throughput.** The networks use no Ada-only tensor format. The sm_86 and sm_89 network builds disassemble to
  the same tensor work (718 `HMMA.16816.F16` and 9 `HMMA.1688.F16` each, no `QMMA`, no `IMMA`; 23,709 and 23,669 SASS
  instructions), and no PTX image in any build contains `.e4m3` or `.e5m2`. This is the opposite of the NR model, whose
  Ada build is FP8 and whose Ampere build is a 2.8x larger FP16 fallback (model-cost-across-architectures.md, "Its
  precondition"). What the 3060 lacks here is throughput, a quantity, not a capability. `dlssg_for_sm86` measured the
  whole generation group on an RTX 3080 Ti at 1080p as 1.389 ms at 2X and 2.641 ms at 4X with stock kernels (claim,
  their README); a 3060 has fewer and slower tensor cores, and nothing here has timed it.
- **Flip metering.** Ada has none either. The Ada unlock already lives with software pacing above 2X
  (`MfgUnlock.h:33-35`); Ampere would be in the same position, not a worse one, as far as anything says.
- **Product policy** cannot be evidenced either way and is not argued here.

## What the providers ship: an offline inventory

Every `nvngx_dlssg.dll` on this machine: 38 files in both Steam libraries, `.prev` copies, backups, Rafael's kit and the
Linux driver's Wine copy, which reduce to ten distinct builds by sha256. Read-only, nothing loaded or executed.

**Method**, so it can be repeated: parse the PE (sections, `TimeDateStamp`, `SizeOfImage`, the export directory for
`NVSDK_NGX_GetGPUArchitecture`, the import directory, `VS_FIXEDFILEINFO` after the `VS_VERSION_INFO` key). In
non-executable sections, walk every fatbin container (magic `50 ED 55 BA`, header size `0x10`, u64 payload size at
+8). Each image header gives kind at +0 (1 PTX, 2 SASS/ELF), header size at +4, payload size at +8, compressed size at
+16, architecture at +28, flags at +40 (`0x2000` = LZ4 block), uncompressed size at +56. Decompress PTX and read
`.version`, `.target`, `.entry` names and the instructions that need a newer target than sm_75 or sm_86. Separately,
find bare CUDA ELF images outside any container (`7F 45 4C 46`, `e_machine` 190, SM in `e_flags & 0xff` at ABI version
7) and disassemble them with `nvdisasm`. The parser lived in a scratch directory and is not committed.

| Build | sha256 | Copies | Fatbin containers | SASS in containers | PTX | Bare cubins | Loadable on sm_86 | on sm_75 |
|---|---|---|---|---|---|---|---|---|
| 310.1.0.0 | `fa00226eb2ea` | 1 | 72: 39 P89, 33 P120+P89+S89 | sm_89 x33 | sm_89 x72, sm_120 x33 | sm_86 x39, sm_89 x39 | 39 network kernels (bare SASS); 0 of 33 frame kernels | 0 |
| 310.1.0.0 | `c989c0ebd9cd` | 1 | 72 (39 + 33) | sm_89 x33 | sm_89 x72, sm_120 x33 | sm_86 x39, sm_89 x39 | 39; 0 of 33 | 0 |
| 310.1.0.0 (a `Development` folder) | `b3900006e9a6` | 1 | 77 (39 + 38) | sm_89 x38 | sm_89 x77, sm_120 x38 | sm_86 x39, sm_89 x39 | 39; 0 of 38 | 0 |
| 310.2.1.0 | `3d17996b4c4f` | 1 | 72 (39 + 33) | sm_89 x33 | sm_89 x72, sm_120 x33 | sm_86 x39, sm_89 x39 | 39; 0 of 33 | 0 |
| 310.3.0.0 | `0a6212a33eba` | 1 | 72 (39 + 33) | sm_89 x33 | sm_89 x72, sm_120 x33 | sm_86 x39, sm_89 x39 | 39; 0 of 33 | 0 |
| 310.4.0.0 | `fa7b99551ef8` | 2 | 71 (39 + 32) | sm_89 x32 | sm_89 x71, sm_120 x32 | sm_86 x39, sm_89 x39 | 39; 0 of 32 | 0 |
| 310.6.0.0 | `17e00fc66d32` | 1 | 70 (39 + 31) | sm_89 x31 | sm_89 x70, sm_120 x31 | sm_86 x39, sm_89 x39 | 39; 0 of 31 | 0 |
| 310.7.129.0 | `20c18281e88f` | 11 | 70 (39 + 31) | sm_89 x31 | sm_89 x70, sm_120 x31 | sm_86 x39, sm_89 x39 | 39; 0 of 31 | 0 |
| **310.9.1.0** | `ff6e90eb78b8` | **18** | 70 (39 + 31) | sm_89 x31 | sm_89 x70, sm_120 x31 | sm_86 x39, sm_89 x39 | 39; 0 of 31 | 0 |
| 310.2.1.0, Linux driver's Wine copy | `a80fafbfeb8a` | 1 | 97: 79 P89+S89, 18 P89 | sm_89 x79 | sm_89 x97 | sm_89 x18 | 0 | 0 |

"Loadable" applies CUDA's rules: SASS runs on the same major version at an equal or higher minor, PTX JIT-compiles
for any target at or above its own. Every build's `NVSDK_NGX_GetGPUArchitecture` is `b8 90 01 00 00 c3`. Every
game-shipped build is the same shape: 39 network kernels (`k_conv_fp16_nhwc` x16, `k_upscale` x4, `k_element_wise` x4,
`custom_block0/1_*`, `k_central_block`, `k_pooling`, `k_initial_merge`), each as a single sm_89 PTX container plus
bare sm_86 and sm_89 SASS; and 31 to 38 frame-construction kernels (`Kernel_EstimateIntermMvecsScatter`,
`Kernel_EstimatePrev2CurrScatter`, `Kernel_InputMvecProcessing`, `Kernel_WarpBlendingWeights`,
`Kernel_BlendCandidatesFused`, the `Prev2Curr*` and `Output*` inpaint kernels, `DetectMenus*`, `DL1Net_Input/Output`,
capture and debug kernels), each as Blackwell PTX, Ada PTX and Ada SASS. The frame kernels use no tensor instructions
at all (15,463 SASS instructions over the 31 sm_89 images of 310.9.1, no `HMMA`).

**Which instructions the PTX needs** (310.9.1; 310.1.0.0 identical):

| Instruction family | needs | in sm_89 PTX (70 images) | in sm_120 PTX (31) |
|---|---|---|---|
| FP8 (`.e4m3`, `.e5m2`, `.e2m1`) | sm_89 | 0 | 0 |
| `wgmma`, clusters, `setmaxnreg`, `stmatrix`, bulk copies, `griddepcontrol` | sm_90 | 0 | 0 |
| `tcgen05` | sm_100 | 0 | 0 |
| `cp.async`, `mbarrier`, `redux.sync`, `.bf16`, `.tf32` | sm_80 | 0 | 0 |
| `mma.sync.aligned.m16n8k16` | sm_80 | 27 | 0 |
| `min`/`max` on `.f16`/`.f16x2` | sm_80 | 26 | 0 |
| `cvt.rn.f16x2.f32` | sm_80 | 5 | 5 |
| `ldmatrix`, `mma ... m16n8k8` | sm_75 | 11, 1 | 0 |

So no image needs more than sm_80. Retargeting to sm_86 is a relabel at the ISA level: `ptxas` accepts it, or refuses
loudly at module load. Retargeting to sm_75 is not: three instruction families must be rewritten, in up to 27 network
images and the five `cvt` users. The largest static shared-memory declaration is 8 KB (sm_120) and 13.5 KB (sm_89),
inside every target's limit; no image declares dynamic shared memory. PTX ISA versions are 8.3, 8.5 and 8.7, which a
driver of the R570 era or later JIT-compiles; Rafael's 616.92 is well past that.

**The in-place rewrite is clean in every build.** The Ada kernel swap writes `.target sm_89 ` over `.target sm_120`
inside the LZ4 stream and assumes the bytes it edits are a literal nothing else copies (`MfgUnlock.cpp:262-299`,
`:364-366`). That had not been checked beyond the builds it ran on. Decoding each edited stream and comparing it with
the expected text: all 31 to 38 Blackwell images of every 310.x build decode to exactly the intended PTX for both
`.target sm_89 ` and `.target sm_86 `, and all 70 to 97 sm_89 images do for `.target sm_86`. No edit reaches a second
output byte. The production code does not verify this at run time; NeuRotic does (`ptx_retarget.hpp:413-439`), and
the port should too, at planning time, before anything is written.

**The provider's network branch** (310.9.1, read with `objdump`): at RVA `0x4da4d` the network factory compares an SM
number returned through the device object against `0x59` (89) and constructs one of three classes, `0x46410` above 89,
`0x44f30` at 89, `0x43a50` below (RVAs of the constructors it calls); the second network repeats the shape at RVA
`0x4db8e`. NeuRotic's profile names `0x46410` among "both PTX constructors" (`provider_profile.hpp:49-57`) and calls the
below-89 path "the SM86-only cubins chosen by the native SM<89 branch" (`provider.hpp:582-583`). That the below-89 class
loads the 39 bare sm_86 cubins is an inference from the three-way branch and the cubins' presence, not a read of the
class.

## Prior art

### NeuRotic's experiment, on `external/mfg_ampere`

`external/mfg_ampere` is MIT, copyright ImDreamt, mavismmg and nefh (`LICENSE:1-5`), "a NeuRotic host adapter" for what
was a ReShade addon (`src/addons/mfgunlock/reshade_compat.hpp:1`); the same authors' MFGAdaUnlock is what our own Ada
unlock credits (`Licenses/MFGUnlock_ATTRIBUTION.txt`). NeuRotic calls it from
`OptiScaler/mfg/ExperimentalMfgRuntime.cpp`. Paths below are under `external/mfg_ampere/src/addons/mfgunlock/` unless
they start with `OptiScaler/`.

**Which cards.** `OptiScaler/mfg/ExperimentalMfgPolicy.h:22-31`: NVIDIA, NVAPI architecture `0x170` with a non-zero
implementation (GA100, sm_80, excluded; also `architecture.hpp:108-109`) and a PCI device in `0x2200-0x25ff`, or `0x160`
with a device in `0x1e00-0x1fff`, and in both cases a name containing "GeForce RTX 30" or "GeForce RTX 20"
(`ExperimentalMfgRuntime.cpp:110-118`), matched to the D3D12 device's LUID. Two keys, `ExperimentalUnlockRTX30` and
`ExperimentalUnlockRTX20` (`OptiScaler.ini:311-312`, read once at start, `OptiScaler/Config.cpp:75`), behind a
confirmation dialog (`OptiScaler/menu/ExperimentalMfgControls.h`), plus a kill switch,
`NEUROTIC_DISABLE_EXPERIMENTAL_MFG=1` (`ExperimentalMfgRuntime.cpp:86`).

**Which build.** One: 310.9.1.0, sha256 `ff6e90eb...0b82`, `TimeDateStamp 0x6a986031`, `SizeOfImage 0x737000`
(`provider_profile.hpp:46-47`, `:267-272`), recognised by timestamp and size and then held to FNV fingerprints of 70
payload regions and five code regions. Wrappers `sl.dlss_g` 2.13.x, 2.14.x or 2.7.32.0
(`OptiScaler/mfg/MfgProviderQualification.h:21-25`); the wrapper's ceiling is read, never raised (`:26-41`). Anything
else is "unknown provider profile; observation only" and gets no write (`provider.hpp:483-485`).

**What each patch changes**, for Ampere (sm_86) and Turing (sm_75):

1. **The provider's minimum architecture.** The export must read `B8 90 01 00 00 C3` (`provider.hpp:466-470`); its
   immediate's low byte becomes the card's (`0x70` or `0x60`, `:589`), written last, after every payload write has been
   read back (`:573`, `:607-611`). This is G3.
2. **The MFG gates.** The `0xB0` low byte of both 0x1b0 comparisons becomes the card's (`provider.hpp:648-656`). Ours
   neutralises the branch instead; same decision. This is G4.
3. **Kernel images.** All 70 sm_89 PTX images, in place: the `.target` digits are edited inside the LZ4 literal, checked
   to be an independent literal by a full decode, and the image header relabelled (`ptx_retarget.hpp:413-450`). Only
   two container layouts are accepted (`:340-348`); FP8, `wgmma`, `tcgen05`, tensor maps and bulk copies are refused
   (`:124-135`). For Turing, the sm_80 families of the table above are rewritten
   (`ptx_compat.hpp:64` packed-half conversions, `:153-245` each `m16n8k16` split into two `m16n8k8` with the first
   result as the second's accumulator, `:271-418` f16 min/max through f32), recompressed, and accepted only for 27 + 5
   fingerprinted programs (`ptx_retarget.hpp:156-218`).
4. **Temporal kernels.** The Blackwell motion-vector, inpaint and inpaint-decision PTX are rebuilt as sm_86 or sm_75
   fatbins in `VirtualAlloc` memory and the provider's fatbin descriptors redirected to them
   (`blackwell_temporal.hpp:321-366`, `validated_warp.hpp:632`). The experimental route passes
   `enable_intermediate_scatter=false` (`ExperimentalMfgRuntime.cpp:214`), so no PTX instruction is rewritten here;
   the "Validated Warp" hand-written blend (`validated_warp.hpp:190`) is not on this route.
5. **Turing only:** two `jle` selectors at RVAs `0x4da50` and `0x4db91` become `nop nop` (`provider.hpp:582-587`,
   `provider_profile.hpp:59-62`), so the network factory takes the above-89 PTX class instead of the sm_86 cubins; the
   disassembly above shows these are the `cmp eax, 0x59` branches. All 39 network PTX images must then lower cleanly.
6. **Admission answers.** Inside a thread-local scope opened only for the game's own D3D12 DLSS-G queries,
   `NvAPI_GPU_GetArchInfo` reports `0x190` for the renderer's physical GPU (`ExperimentalMfgRuntime.cpp:322-334`,
   `OptiScaler/nvapi/NvApiHooks.cpp:43`); `slIsFeatureSupported`'s `eErrorNoSupportedAdapterFound` (6) becomes `eOk`
   on the matched adapter once the provider is prepared (`OptiScaler/hooks/Streamline_Hooks.cpp:356-377`,
   `capability_policy.hpp:63-72`); and NGX `GetFeatureRequirements` for feature 11 is rewritten to supported with the
   card's architecture (`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp:2173-2195`, `ExperimentalMfgRuntime.cpp:249-257`).

**How it fails.** Into "frame generation unavailable", with a reason and usually a restart
(`ExperimentalMfgPolicy.h:61-86`): `ProviderUnknown` for any other build; `TooLate` when the provider was already mapped
at session start or frame generation was created before preparation (`ExperimentalMfgSession.h:9-28`,
`ExperimentalMfgRuntime.cpp:194-196`); `UnsupportedRoute` off game-owned D3D12 Streamline (`:144`);
`DynamicNotQualified` for dynamic MFG and `OwnerConflict` beside its Ada unlock (`:165-169`); `PreparationFailed` after
a clean rollback, `RollbackFailed` and `Poisoned` when memory had to be retained; `DeviceRemoved` poisons the session
(`:270`). Once poisoned, every DLSS-G entry it guards returns not-supported (`NVNGX_DLSS_Dx12.cpp:1847`, `:2173`,
`:2386`; `Streamline_Hooks.cpp:732`, `:839`, `:1510`, `:1662`).

**Its own qualification.** None. The stage line hard-codes `hardwareQualification=NOT_RUN`
(`ExperimentalMfgRuntime.cpp:52`), the prepared line says `presentation=UNKNOWN` (`:235`) and the options line
`delivery=UNKNOWN` (`:264`). No log, fixture or document in the clone records a run on a 20 or 30 series card.

**Three things the read cannot settle**, each of which could keep it from ever reaching `Ready`:

- Its relaxation of the NGX answer fires only on a call that succeeded (`capability_policy.hpp:40`). On stock, the
  3060's answer was a failed call (`0xbad00001`). It depends on the export patch having changed the core's verdict
  first, which the `dlssg_for_sm86` log says it would (G3), provided the patch lands before the core asks.
- Its architecture scope covers `slIsFeatureSupported` and `GetFeatureRequirements`, not `slInit`, where G1 is decided
  (claim). If a game's Streamline evicts the plugin there, nothing is left to prepare.
- It prepares at `slIsFeatureSupported` and needs the provider already mapped (`ProviderMissing` otherwise,
  `ExperimentalMfgRuntime.cpp:184`); a game that asks before NGX has loaded the provider, and stops asking after a "no",
  never gets there.

**What is worth taking**: the per-build qualification table and its states (`provider_profile.hpp:13-44`: unknown,
observe only, qualified, patchable), the decode-after-edit check, writing the admission last, the LUID-matched,
thread-scoped admission answer, and the explicit failure reasons. **What is not**: Turing's instruction rewriting for
now, the `VirtualAlloc` descriptor redirects (unnecessary on Ampere, below), retargeting the 39 network images on
Ampere (the provider has sm_86 builds of them), and the NVAPI-level architecture answer (G1 is better answered where
upstream already answers it).

### `dlssg_for_sm86`, and what it showed on Rafael's card

GPL-3 (it derives from `dlssg-to-fsr3`), Windows D3D12 only. Its proxy DLL embeds an unmodified NVIDIA runtime
(310.1.0.0 sha256 `c989c0eb...`, one of the builds above, or 310.9.1.0 `ff6e90eb...`) and an "SM86 backend" of NVIDIA
kernels it extracted and recompiled, plus Coldwood1026's sm_75 family, and loads its bundled runtime whatever the game
asks for (`THIRD_PARTY_NOTICES.txt:3-27`; Rafael's `loader_*.jsonl` shows it redirecting the game's, the DriverStore's
and Monster Hunter Wilds' `_storage_` requests to the bundle). It claims bit-identical output to NVIDIA's on an RTX
3080 Ti and 3070, and on a real 2080 Ti for sm_75, including the `m16n8k16` split (`docs/INSTALL.en.md:205-209`).

Three lessons it paid for, all claims, all directly useful:

- **An architecture answer that reaches NVIDIA's own snippets hangs the GPU.** 0.3.3 rewrote `NvAPI_GPU_GetArchInfo`
  for everything; with the NVIDIA App's DLSS override or an NGX OTA update, the Super Resolution snippet loaded from the
  model store as `160_E658700.bin`, took its `arch >= 0x180` path on an RTX 30 and hung the device
  (`DXGI_ERROR_DEVICE_HUNG`, its issues #535-#542). 0.3.4 excludes NVIDIA's components by location.
- **An Ada answer passes admission but leaves the game's menu at 2X.** Games unlock 3X/4X/6X when Streamline sees
  Blackwell. For us, the count override ([mfg-count-override.md](mfg-count-override.md)) does not depend on the menu.
- **Streamline decides at `slInit`**, before the provider is loaded (G1).

Rafael's 2026-10-04 Monster Hunter Wilds session with it (`~/.local/state/dlss5/deployments/rafael-logs-20261004-mhw/`)
is the only measurement of the gates on his card with something answering them: driver 616.92 (`r616_69`), HAGS on;
the provider's minimum reported as 368 (0x170) instead of 400; the core's answer to feature 11 `FeatureSupported 0`,
`MinHWArchitecture 0x170`; `FrameGeneration.Available 1`, `FeatureInitResult 1`, `NeedsUpdatedDriver 0`;
`CreateFeature(11)` at 1920x1080 with `multi_frame_count_max 3` succeeded; 132 kernels created per feature on its own
`cubin_sm86` images.

## Options

### (a) Port the idea, default off, one exact provider build at a time

For Ampere the port is small, because the hard half is already in the tree:

- **Kernels.** Per frame-construction container, the Ada swap's transformation with a different number: the Blackwell
  PTX's `.target sm_120` becomes `.target sm_86 ` (both fourteen bytes), its label 120 becomes 86, and the Ada PTX and
  SASS are parked on 122 (`MfgUnlock.cpp:262-415`). The Blackwell PTX needs nothing above sm_80 (table above) and is
  the build whose motion-vector kernel reads three fields, which is what made generated frames advance on Ada; Ampere
  would get the same kernel. NeuRotic uses the Ada PTX for 28 of the 31 and Blackwell's for three. The network
  containers are left alone: the provider's below-89 branch has sm_86 SASS for them.
- **Admission.** The export's `0x90` becomes `0x70` (G3). Streamline's admission (G1) is answered inside `sl.dlss_g`'s
  plugin load, where upstream already rewrites the architecture and restores it afterwards
  (`Streamline_Hooks.cpp:1087-1102`); never NVAPI-wide, never for NVIDIA's snippets.
- **Count.** The existing gate patch (G4) and wrapper patches (G5), unchanged; they do not look at the architecture.

Cost: one more branch in `TryApply`, a qualification table, a decode check, the admission scope. No allocation, no
descriptor redirect, no instruction rewritten. Its risk is in running NVIDIA's code on hardware NVIDIA did not
validate it on; see Risks.

### (b) Admission only, no kernels, to see what the provider does

G1 and G3 answered, G6 left as shipped. The inventory predicts the result for every 310.x build: the core accepts,
and the runtime then fails to create its 31 frame kernels on an sm_86 device, at `CreateFeature` or the first
evaluate. `dlssg_for_sm86` says the same of 310.9.1. Run on its own, (b) produces no frames. Its value is learning
**how** the stock runtime fails when kernels are missing: a clean NGX error the game survives, or a crash. That is
exactly the path an unqualified provider would take under (a) if admission were ever granted without the kernels,
so it is worth one scratch-build run, never merged.

### (c) Do nothing

Defensible. RTX 20/30 owners have FSR-FG and XeFG behind the game's own DLSS-G menu (`FGInput=dlssg`), synthesized FG,
and `dlssg_for_sm86` for those who want NVIDIA's frames and accept how it is packaged. What (c) gives up: NVIDIA's own
image from the game's own provider, no bundled NVIDIA binary, and the MFG count override and NR-before-DLSS-G
([nr-before-dlssg.md](nr-before-dlssg.md)) on these cards.

### Recommendation

**(a), Ampere only**, with (b) as its second step on a scratch branch. The evidence moved the cost: what looked like a
retarget of 70 kernels plus temporal redirects is, on Ampere, the Ada swap with another number, an export byte, and an
admission answer upstream already knows how to scope. Turing stays out until there is someone to run it: its path is
code generation over NVIDIA's PTX in 32 programs, and every claim of its correctness so far comes from other projects.
If step 0 or 2 shows a gate this note did not foresee, the fallback is (c), and the note says so.

## Legal, EULA and redistribution

Factual, not a legal opinion; nobody has reviewed this.

- **Redistribution: none.** Nothing NVIDIA-owned is in this repository or a release; `tools/package_release.ps1` already
  refuses NVIDIA's NR model. The patch writes into the user's own copy of the game's provider, in memory, as the Ada
  unlock does. The byte patterns are anchors (`tests/mfg-pattern` checks them against synthetic buffers); a
  qualification table would hold timestamps, sizes, offsets and hashes of NVIDIA files, which are facts about them, not
  copies of them. `dlssg_for_sm86` redistributes NVIDIA's runtime and recompiled NVIDIA kernels and says they are "not
  relicensed" (`THIRD_PARTY_NOTICES.txt:3-9`); that is the line this design stays behind. NeuRotic's port embeds
  fingerprints and RVAs and one line of NVIDIA's PTX as an anchor (`blackwell_temporal.hpp:115-116`).
- **Runtime modification of a signed module.** The file keeps its Authenticode signature; NGX checks the file at load,
  and the patch lands after that, which is the point and should be said plainly (`OptiScaler.ini:399-408` says it for
  the Ada unlock). This is the category `AdaMfgUnlock` has been in since it shipped, default off; (a) adds an export
  byte and more image labels to it, not a new category.
- **Answering capability queries falsely.** New in degree. Upstream already tells Streamline that DLSS-G is supported
  when it replaces the frames (`Streamline_Hooks.cpp:347-363`), and spoofs DXGI and Streamline for DLSS on other
  vendors. Under (a), NVIDIA's own runtime would run its own frame generation on hardware NVIDIA declines, on the
  strength of our answer.
- **Licences.** The NVIDIA RTX SDK licence binds the game's developer, who redistributes `nvngx_dlssg.dll`; the GeForce
  software licence binds the user for the driver. Both are generally understood to restrict reverse engineering and
  modification. This note quotes neither; read both before step 3 ships, and record which clauses apply.
- **Reverse engineering.** The offsets and branches here came from disassembly, as the Ada unlock's did.
  Interoperability is the purpose; that is a description, not a defence.
- **Anti-cheat and support.** An online title's anti-cheat may hash NVIDIA modules in memory; the ini text should say
  not to use this in online games, and that problems with it on are not NVIDIA's or the game's to support.

## Stability and image risks

- **Kernels JIT-compiled for an architecture they were never compiled for.** PTX is meant to compile forward, and the
  instruction census says sm_86 lacks nothing these images use; a mistake is a module-load refusal, loud, not a silent
  error. What the census cannot show is a dependency on Ada behaviour outside the ISA: occupancy, warp scheduling
  assumptions, timing. Blackwell PTX JIT-compiled for Ada has run (the Ada swap); for sm_86, never here.
- **Device removal.** A kernel fault in NVIDIA's runtime is a removed device and a crashed game. DRED
  (`[DlssNr] Dred`, enabled in the D3D12 device-creation hooks at `OptiScaler/hooks/D3D12_Hooks.cpp:1458` and `:1610`)
  names the operation on Windows. There is no `__try` around the provider as there is around the NR model; a fault is
  the game's.
- **The admission answer leaking.** `dlssg_for_sm86` hung RTX 30 devices by letting its architecture answer reach
  NVIDIA's Super Resolution snippet. Scope the answer to `sl.dlss_g`'s plugin load, and never touch NVAPI.
- **Admission before qualification.** Streamline admits at `slInit`, before the provider is mapped and qualified. An
  admitted game whose provider then turns out unknown gets NVIDIA's stock refusal at `CreateFeature`. Most games treat
  that as "frame generation unavailable"; step 2 measures what the chosen game does.
- **Pacing.** No hardware flip metering, as on Ada. 3X and 4X on a 3060 are untimed anywhere.
- **Throughput.** A 3060 has a fraction of a 4090's tensor rate. The generation group's cost comes out of the rendered
  frame's time; at a low base rate generated frames break up (`dlssg_for_sm86` README, "Real-world feedback").
- **Image.** Wrong-but-running is the failure the log cannot see: frames that do not advance (the Ada symptom before the
  kernel swap), or artefacts. Only eyes on a moving camera and a capture can say. Bit-identity with NVIDIA's output is
  not claimed and cannot be checked without a Blackwell reference.
- **VRAM.** About 350 MiB at 1080p, independent of the multiplier (claim, `dlssg_for_sm86` README); fine on a 12 GB
  3060, tighter on 8 GB cards.
- **With NR.** The NR pass alone costs 16.8 ms at half scale on the 3060 (model-cost-across-architectures.md). NR and
  frame generation together on that card are not something to promise.

## How it would sit in `MfgUnlock`

A separate path in the same file, never on by default, never reached on Ada, Blackwell or a non-NVIDIA adapter.

```
TryApply(provider)                                    MfgUnlock.cpp:627
  existing: AdaMfgUnlock gates, kernel swap, wrapper  (unchanged)
  new: if BelowAdaUnlock and the adapter is Ampere (0x170, not GA100):
    q = Qualify(provider)            // table row, or observe-only
    if q is not qualified: log "observe only" and return (no writes)
    plan, one MfgPatch plan for the whole module:
      31 containers x (directive, Blackwell label, parked Ada PTX, parked Ada SASS)
      advertise and validate gates (the 310.9 shapes)
      export immediate 0x90 -> 0x70, last in the plan
    decode-check every planned directive edit (no write yet)
    MfgPatch::Commit(plan)           // MfgPatchPlan.h:96-129: stale check, write, read back, roll back
    one summary line, as today: "MFG below Ada summary (<module> <version>, <row>): kernels=ok(31) gates=ok export=ok"
```

One commit for the module, not one per container as the Ada swap does: a half-relabelled set on Ampere means some
kernels cannot load, and an export lowered over missing kernels admits a feature that cannot be created. `Commit`
already handles any plan size, and every site fits `kMaxSiteBytes` (16). The admission answer for G1 is armed only
after this commit returned `Committed` in the same process, or, because `slInit` can come first, when the provider
file the game will load hashes to a qualified row (the risk above).

**The qualification table is worth adopting for Ada too.** Today the Ada unlock patches any build its signatures match
and the summary cannot say whether that build was ever seen working. A row per build, keyed by `TimeDateStamp` and
`SizeOfImage` (a mapped image cannot be hashed stably; the file can, once, for the log), recording where it was measured
(Crimson Desert 6x on 310.9.1; Stellar Blade on 310.7.129) lets the summary say `qualified` or `unmeasured`. On Ada the
row is information and changes nothing; below Ada it is the gate. The inventory above seeds it.

**Card detection.** NVAPI's `architecture_id` 0x170 with an implementation other than GA100, which `IdentifyGpu`
already reads (`MfgUnlock.cpp:708-710` does the same for Ada). Turing (0x160) is refused with a log line naming it,
and so are GTX 16 cards, which have no tensor cores.

## Config surface

One key, `[DLSSG] BelowAdaUnlock`, `true` or `false`, default (auto) false. Four points when built (DEVELOPMENT.md rule
4): `CustomOptional<bool> FGDLSSGBelowAdaUnlock { false }` in `Config.h` beside `FGDLSSGAdaMfgUnlock` (`:1086-1087`),
the read and the save in `Config.cpp` beside `:313-314` and `:1245-1247`, and the ini block after `AdaBlackwellKernels`
(`OptiScaler.ini:425`) saying what it does, on which cards, that it needs a restart, and not to use it online.
`nr-invariants` checks `[DlssNr]` keys only; the round-trip test for this key goes with the mfg suites.

No menu control in the first version. If one is added, it appears only on an Ampere adapter with a provider loaded
(rule 2) and says that a restart applies it. The count is chosen as on Ada, through `OverrideInterpolationCount`;
this key raises nothing by itself (mfg-count-override.md: "a raised ceiling is not a chosen count").

## Steps

**Step 0 offline (done, 2026-10-14).** The inventory, the census, the decode check and the disassembly above.

**Step 0, log only.** No key: it writes nothing, and runs only on an NVIDIA adapter whose architecture is below
AD100, so on Ada and Blackwell it returns at its first comparison. Called from the two provider load sites that hand
`TryApply` its module (`OptiScaler/hooks/LibraryLoad_Hooks.cpp:118-127` and `:149-155`), once per module:

```
MFG below Ada (probe): adapter "NVIDIA GeForce RTX 3060" arch 0x170 impl 0x<n> driver <ver> HAGS <on|off>;
  provider nvngx_dlssg.dll 310.9.1.0 ts 0x6a986031 image 0x737000 sha256 ff6e90eb78b8 from <game|DriverStore|NGX models>
  row=<name|none>; minArch export 0x190; kernels: 70 containers, frame 0/31 loadable on sm_86, network cubins sm_86 39;
  gates advertise=found validate=found; no writes
```

And once, when the game's DLSS-G is first asked about, the admission line: the `sl.dlss_g` version and whether the
plugin is loaded, `slIsFeatureSupported` for DLSS-G, the NGX core's answer for feature 11 (result, flags, minimum
architecture) asked once through the proxy OptiScaler already holds (`NVNGXProxy::D3D12_GetFeatureRequirements`, as at
`OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp:1003-1006`), `CreateFeature(11)`'s result if the game calls it, and
`slDLSSGGetState`'s result and maximum from the existing one-shot probe. HAGS comes from `D3DKMTQueryAdapterInfo`,
which the `SpoofHAGS` hook already sees. If another frame-generation loader is present (a module named
`sm86_backend.dll`, or a `dlssg_sm86.ini` beside the game), the probe says so first: it would be describing that
project's runtime.

**Step 1, the kernel half on the 4090 (scratch branch).** Relabel the Blackwell PTX to sm_86 and park the Ada images,
on Ada. The driver then JIT-compiles the sm_86 PTX for sm_89, so the exact rewritten images run on this machine, in
Crimson Desert (310.9.1), at 2X and 4X with the count override. Pass: the module loads, generated frames advance with
the camera, no device removal, and the image matches the `AdaBlackwellKernels` route on a held scene by eye. This tests
the rewrite and the commit; it cannot test admission or the provider's below-89 network branch.

**Step 2, admission only on Rafael's 3060 (option b, scratch build, never merged).** G1 and G3 answered, kernels
untouched. Expected: feature 11 supported, then a `CreateFeature` or evaluate failure with a code. The question is
what the game does with it.

**Step 3, both, behind the key.** Only if 0 to 2 held. One exact provider build first (whichever Rafael's game loads,
from step 0), then others one row at a time.

**Turing** gets a step only when a Turing tester exists.

## Test plan with Rafael's RTX 3060 (native Windows)

**Before anything:** remove `dlssg_for_sm86` from the test game, using the `ROLLBACK.md` our kit's `install.ps1` wrote
(it redirects every `nvngx_dlssg.dll` load to its bundle and answers the architecture itself; with it present, our log
describes its runtime). Check HAGS is on (it was on 2026-10-04). Use a single-player title with native Streamline
DLSS-G that he has run: Monster Hunter Wilds (D3D12, logged above) or Mortal Shell II. Not an online game.

| Step | Build and ini | What he does | What the log must show | Back out |
|---|---|---|---|---|
| 0 | normal build with the probe; no new key | launch, open graphics settings, try to turn frame generation on, play two minutes, quit | the probe line with arch `0x170`, the provider's version and sha256 prefix, `minArch export 0x190`, `frame 0/31 loadable`, `network cubins sm_86 39`; the admission line with NGX feature 11 refused (`0xbad00001`, flags `0x4`) as in model-cost-across-architectures.md; no `MFG below Ada summary` | nothing was written; remove our DLL as usual |
| 2 | scratch build, `BelowAdaUnlock=true` (admission only) | same, and turn frame generation on if offered | feature 11 supported, `MinHWArchitecture 0x170`; then the failure code at create or first evaluate; whether the game kept running | delete the key line; nothing on disk changed |
| 3 | step 3 build, `BelowAdaUnlock=true`, `[DLSSG] OverrideInterpolationCount` 1 then 3, `[DlssNr] Dred=true` | frame generation on; a minute standing still, a minute panning with the HUD visible, at 2X and at 4X; a short video; PresentMon or CapFrameX if he has it | `MFG below Ada summary (...): kernels=ok(31) gates=ok export=ok`, the wrapper summary, DLSS-G evaluates rising, presents per rendered frame at 2.00 and 4.00 in windows without menus or loads (the windowed method of mfg-count-override.md); no device removal, or DRED's breadcrumbs if there is one | `BelowAdaUnlock=false` or delete the line: the next launch takes the default path unchanged; the patches are memory only; a TDR resets the driver |

For step 3, the same scene with `FGOutput=fsrfg` and with `dlssg_for_sm86` (reinstalled from the kit) gives him a
three-way comparison on his own screen. The question for each is the Ada one: does the world move in the generated
frames, or only the interface.

## Open questions

1. **Does the below-89 branch load the sm_86 network cubins once admitted?** The branch and the cubins are read; the
   link between them is inferred. Step 2 or 3's log, or the absence of a network-creation failure, answers it.
2. **Does Streamline evict the plugin at `slInit` on Rafael's games**, and does upstream's plugin-load answer reach
   that decision? `dlssg_for_sm86` measured eviction on Streamline 2.8; Monster Hunter Wilds' Streamline version is
   unknown. Step 0's admission line answers it per game.
3. **Which provider does NGX load on Windows?** Rafael's log shows requests for the game's copy, a DriverStore copy and
   a `_storage_` copy in the same process. The probe has to name the one that ends up mapped, and the table has to
   cover it. The DriverStore copy's version is unknown.
4. **What does the game do with a `CreateFeature` refusal after admission?** Step 2.
5. **Does the host fill the Blackwell kernels' parameter fields the same way below 89?** On Ada it does, or the swap
   would not have worked. Below 89 the host takes other branches for the networks; it may for this too.
6. **Is 4X worth it on a 3060?** Untimed: pacing without flip metering, the generation group's cost at the 3060's
   tensor rate, latency. Working is not the same as good, as on Ada.
7. **Where should the admission answer for G1 be armed when `slInit` precedes the provider load?** Hashing the provider
   file on disk at `slInit` is one answer; it misses OTA and DriverStore substitutions.
8. **The licences** (Legal, above), before step 3 ships.
