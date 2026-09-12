# NR without a game DLSS call — where the evaluate could come from when nothing asks for it

Status: **design only, and deliberately blocked behind a prerequisite.** Not scheduled. Written after
surveying the ReShade-addon ecosystem that solves the same problem a different way (2026-09-11), so
the reasoning does not have to be rediscovered.

## The problem

The neural pass runs because the game calls DLSS. `inputs/NVNGX_DLSS_Dx12.cpp` and `NVNGX_DLSS_Vk.cpp`
evaluate after upscale, and that evaluate is the only thing that drives the pass. A game that ships no
DLSS never makes the call, so there is nothing to hook and the module is inert — correctly, but it
means the whole catalogue of DLSS-less titles is out of reach.

This is not a gap only we have. It is the defining problem of the whole DLSS 5 add-on scene, and two
projects have already shipped answers to it.

## How the ReShade side solves it

`jlrouzies-fr/DLSS5-Feeder` **fabricates the call**. It builds a complete DLSS DLAA contract out of
what ReShade already holds — the frame being processed, the depth buffer ReShade detected, and motion
vectors it estimates itself — runs a genuine DLSS evaluate on a private D3D12 device, lets a DLSS 5
add-on hook into that evaluate, and copies the neural result back into the frame, all inside ReShade's
effect chain. `NIGos/dlss5-dx11-bridge` does the equivalent for D3D11 and Vulkan and states the quality
cost of the estimated half plainly: motion vectors from NVIDIA Optical Flow are "lower quality".

Worth reading their own conclusions before repeating the work. `PLAN-PROXY-SWAPCHAIN.md` in that
repository is a design-plus-spike for making a DLSS-less game actually *render fewer pixels* via a proxy
swapchain, and its verdict is **"design + spike only. No add-on code. Nothing here is scheduled"** —
because three parties already hook the DXGI factory (ReShade, NvPresent64/Smooth Motion, and the game's
overlay stack), and hook order decides who wraps whom. That is a warning about the neighbourhood, not
just about their design.

## Why our position is different, and better

We are not a detour over NVIDIA's `_nvngx.dll`. We *are* the NGX implementation the process talks to:
installed under a name the game imports, loaded at process start, and our LoadLibrary hooks hand our own
module to anything asking for `nvngx.dll` or `_nvngx.dll`. The Feeder's own `src/feed_opti.h` says this
about us explicitly, and it is why it treats us as a first-class target rather than a competitor.

So the missing piece for us is *smaller* than for them. They must synthesize the contract **and** find
an NGX implementation to accept it. We already are that implementation, with the upscaler backends, the
neural pass, the composition and the menu all in place. What we lack is only a **caller**.

## What the contract needs, against what this tree already has

| Input | Where it would come from | State |
|---|---|---|
| Colour | The swapchain backbuffer, at present time | Have it. `wrapped/wrapped_swapchain.cpp` already hands the overlay a backbuffer there |
| Exposure | `DlssNr_ExposureScan` already scans for one and holds a value when the game offers none | Have it |
| Depth | `resource_tracking/ResTrack_dx12.cpp` already hooks `OMSetRenderTargets` (`ResTrack_dx12.h:511-514`, implementation at `ResTrack_dx12.cpp:1128`) and therefore already *sees* every depth-stencil descriptor the game binds | Machinery exists; the heuristic does not |
| Motion vectors | Nothing produces them, and it turns out not to matter for a first cut | Absent, and survivable — see below |

Depth is the encouraging one. We do not have to add a hook or guess from scratch — the hook that
observes the game's own depth binding is already installed and already running for the exposure-scan
resource notes. What is missing is the selection heuristic on top of it (which of the bound
depth-stencils is *the* scene depth), and that heuristic is where ReShade has years of accumulated
special-casing. A wrong depth is worse than no depth: the model would be guided by a surface that does
not describe the scene.

Motion vectors looked like the wall when this note was first written. They are not, and the correction
came from the last place anyone would look for it: the projects running feature 18 over **video**.

`SAOG0721/DaVinci-Resolve-DLSS5` is an OpenFX filter that runs the neural model on film footage inside
DaVinci Resolve. At 79 stars it is by an order of magnitude the most-used thing in this whole ecosystem,
and it states its contract plainly: **"Motion is zero-filled `R16G16_FLOAT`; depth is zero-filled
`R32_FLOAT`"**, with "real motion/depth are later work". It produces a visible, useful neural change
anyway. So the model runs, and does something worth having, with **no guides at all**.

`jessicanataliagta/DLSS-5-Video-Player` takes the middle road and says why: "Normal encoded video does
not contain the original game engine Z-buffer or object motion vectors. Those buffers are discarded when
the game is rendered into a 2D movie, so the player reconstructs temporal guides from consecutive
frames" — motion as `R16G16_FLOAT` current-to-previous in input-pixel units, a depth proxy, and Halton
subpixel jitter shared between the generated guides and the NGX parameters. Its own caveat is the honest
one: "These are reconstructed video guides, not the original engine buffers. Their quality depends on
the source material."

That reorders everything. Guides are a **quality axis, not a gate**: zero-filled works, reconstructed
from consecutive frames works better, real engine buffers work best. Optical flow under Proton stops
being the question that has to be answered before anything else can start, and becomes an optimisation
to reach for once there is something to optimise.

## Who would originate the evaluate

Not a new mechanism: the natural host is a **present-time dispatch**, and that already exists — in the
sibling fork, not here. `scottmudge/custom_dlssnr` carries `bc223dae` "DLSS-NR: add the Present hook,
the ReShade addon's way of running the pass" (~754 lines across `hooks/FG_Hooks.cpp`,
`wrapped/wrapped_swapchain.cpp`, `shaders/dlssnr/DlssNr_Dx12.cpp`, plus config and menu), with four
follow-ups that park the old temporals, close the list when dropped, wait for the pass before the flip,
and stop stalling the present thread. The commit subject names the lineage: it is the ReShade addon's
way of running the pass, brought inside OptiScaler.

That ordering matters for sequencing. A present-time NR dispatch is useful on its own — it reaches games
where the after-upscale hook does not fire — and it is a prerequisite for this note rather than part of
it. Porting it is its own design note and its own review.

## Sequencing, honestly

1. Port the present-time dispatch (`bc223dae` and its four follow-ups) under its own design note. It is
   the host everything else needs, and it is useful on its own: it reaches games where the after-upscale
   hook never fires.
2. A synthesized same-resolution contract with **zero-filled motion and depth**, behind a config key
   that is off by default and inert when off, like every other `[DlssNr]` key. This is the cheapest
   thing that can possibly work, it settles the only question that actually matters — whether the model
   produces something worth having on a frame we assembled ourselves — and it no longer has to be
   guessed at. `SAOG0721/Magpie` ships it and the write-up in `bmitch87/DLSS5VKLayer`'s
   `extracted_pipeline_notes.md` §6 gives the contract in full:

   | resource | format | bound to NGX |
   |---|---|---|
   | Depth | `R32_FLOAT`, source extent | yes, as `DLSSNR.Depth` |
   | Motion | `R16G16_FLOAT` | yes, as `DLSSNR.MVec` |
   | Confidence | `R8_UNORM` | **no** — never passed; it only drives their own downsample blending |

   Created `BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS`, cleared **once** with
   `ClearUnorderedAccessViewFloat(..., {0,0,0,0})` and never rewritten, so they are permanently zero
   and cost nothing per frame. History reset is raised on initialise, resize, scene change, capture
   interruption, device recreation and long pause. The motion contract they declare alongside is
   `CurrentToPrevious`, `SourcePixels`, `RelativeInverse`, `depthInverted = true`.

   Their selector is worth copying rather than inventing: `guidanceMode` 0 = use what is available,
   1 = force zero, 2 = motion only with zero depth, 3 = depth only with zero motion. That is one key
   with four values instead of two independent booleans, which is also what this module's
   one-quantity-one-control invariant would push us to.

   Note what this does *not* settle: everything above is D3D11-to-D3D12 interop inside a Windows
   process that owns its own device. Ours would be a D3D12 or Vulkan path inside a game's device. The
   formats, the clear-once trick and the mode taxonomy carry over; the interop does not.
3. Only if step 2 earns it: guides. Consecutive-frame motion reconstruction first, because it needs no
   hardware feature and no Proton unknowns; a depth-selection heuristic over the existing
   `OMSetRenderTargets` observations second; optical flow last.

   On optical flow, one fact worth having before anyone plans that step. Measured on this workstation
   (RTX 4090, driver 615.71.09, 2026-09-11), the host Vulkan driver exposes **six** queue families, and
   family 5 carries `VK_QUEUE_OPTICAL_FLOW_BIT_NV`. So the cheap route is not NVOFA's own API at all --
   it is `VK_NV_optical_flow`, a Vulkan queue we are already in a position to use from the Vulkan path.
   Wine's Vulkan is a thin pass-through to the host ICD, so the family list inside a Proton process
   should be the same one, but that is an inference and has not been confirmed from inside a game.

The earlier draft of this note had the NVOFA probe as step 1 and said the idea was dead if it failed.
That was wrong, and wrong in an expensive direction: it put an unanswerable platform question in front
of work that does not depend on it.

## Two facts about this tree, measured from outside

The Feeder's `feed_opti.h` records a measurement of *our* module that we never took ourselves —
OptiScaler-DLSSNR v0.2.0 as `winmm.dll`, driver 616.64, RTX 5090, 300/300 evaluates at 640x360:
**DLSS alone 0.25 ms/frame; with the neural pass 3.4 ms (dlss), 3.1 ms (xess), 2.1 ms (fsr31)**. The
pass runs whatever the upscaler is, and costs roughly 2–3 ms at that size. Useful as an external
baseline, on hardware we do not have.

It also names a diagnosability edge we should own: `CreateFeature` returns Success even when the
upscaler silently fell back (`nvngx_dlss.dll` missing), and `Evaluate` returns Success on frames the
pass skipped — so "routed" never proves the neural model was created. Their answer is to check for
`nvngx.dll_dlssnr.dll` and `nvngx_dlssnr.dll` in the process after the first evaluate. Ours should be
that the menu and the log make "the pass ran zero frames" impossible to miss.

## The one-consumer rule is a hard rule here

With our nvngx redirect live, another add-on's own NGX module — Deep Fried Chicken's
`deep-fried-chicken-nvngx.dll`, renodx-dlss5's `_nvngx.dll` — is handed *our* module when it loads, and
our dlss backend then calls the real driver core, where that add-on's detours fire **a second neural
pass** on the same frame. Two neural passes is not a quality note, it is a wrong image, and this tree
did not detect it. `dlssnr/DlssNr_Consumers.{h,cpp}` now scans the process module list once, at the
moment the neural forwarder loads, and names the offender on the log. Detection is by module name
because the mechanism cannot be observed from inside: a detour on the core looks like the core.
