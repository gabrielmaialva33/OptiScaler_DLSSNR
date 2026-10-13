# Present NR — virtual backbuffers on Proton

Status: **design investigation, not ready for production implementation**. Repository baseline
`7e968ea3`, 2026-09-14. No production, configuration, shader or test changes. No new GPU run.

This develops source isolation from [nr-content-renewal.md](nr-content-renewal.md), including its
review addendum. The target remains opt-in/default-off, one selected D3D12 SDR swapchain under
Proton, with no frame generation. Native Windows, D3D11 and Vulkan applications are outside this
proposal. The earlier hard constraint of **no new CPU synchronisation in Present** is retained.

## Verdict

**The central idea survives: do not write the NR answer into anything the game uses as its next
source.** A partial update to a private game-facing texture can then preserve the game's own pixels
instead of our enhancement. This removes the particular provenance ambiguity of the write-back
design, provided the resource boundary really is closed.

The new vkd3d evidence substantially strengthens feasibility. It does not make replacing GetBuffer
alone sufficient. The runtime's user texture is ordinary, but its **ownership, index selection,
resize checks and presentation consumption are managed by the swapchain**. Our texture would not
automatically join that management.

Three requirements remain unsolved for a transparent, no-wait production slice: public-reference
accounting through resize, a guaranteed available fallback submission when transport itself is busy,
and same-present duplicate suppression of the opaque NGX evaluation without a CPU wait. They are
identified below rather than hidden behind a passing source-isolation test. ReShade also needs an
explicit integration order; arbitrary injector coexistence is not supported by this design.

This is **not** a finding that ordinary textures cannot work under vkd3d-proton. It is a finding that
the requested complete contract is larger than one resource substitution. Stop before implementation
until those conditions have a reviewed answer; the test plan separates what can already be falsified
from what still needs a design decision.

## What vkd3d already does, and what it does not do for us

The source was checked at **vkd3d-proton `81d96933a40207023a812a6f522caefd7e36b73b`**. This is a
pinned source observation, not identification of the DLL currently running a particular game.

- `dxgi_vk_swap_chain_allocate_user_buffer`, `swapchain.c:753-781`, creates a committed DEFAULT-heap
  texture: target dimensions/format, one mip/layer/sample, ALLOW_RENDER_TARGET, layout UNKNOWN,
  placement alignment, node masks 1 and initial PRESENT state.
- `GetImage`, `:694-700`, returns the selected `user.backbuffers[]` resource by QueryInterface;
  `GetImageIndex`, `:703-707`, returns the user index. The blit path separately reads that resource
  and addresses Vulkan presentation images (`:2398-2408,2456`).
- The swapchain holds private resource references (`:820-822`), checks public references before
  ChangeProperties (`:863-866`), and drains presentation work before reallocation (`:880-881`).

[Pinned swapchain source](https://github.com/HansKristian-Work/vkd3d-proton/blob/81d96933a40207023a812a6f522caefd7e36b73b/libs/vkd3d/swapchain.c#L753).

Use these names throughout:

```text
Today:       game -> U_i -> vkd3d presentation transfer -> V_k
Write-back:  game -> U_i -> scratch -> NR -> O -> U_i -> vkd3d -> V_k
Virtual:     game -> T_i ---------> NR -> O -> U_i -> vkd3d -> V_k
Fallback:    game -> T_i ------------------> U_i -> vkd3d -> V_k
```

`U_i` is the D3D12 user buffer returned by the real DXGI swapchain. `V_k` is the runtime's Vulkan
presentation image; do not equate its index/count with `i`. `T_i` is our application-facing texture.
`O` is a separate, privately owned NR composition output. In particular **T is not the in-place NR
output scratch**. It can replace the input transport copy, not the separate answer surface.

Do not access vkd3d's arrays, private reference counters, Vulkan images or blit semaphores from the
host. Public D3D12 creation and DXGI presentation remain the implementation boundary. Their current
implementation is evidence to test against; its layout and behaviour may change. No equivalent
native-Windows resource-identity claim has been established.

## The missing relationship: same texture description, different owner

The resource QueryInterface implementation accepts the D3D12 resource/base interfaces and
ID3DDestructionNotifier; it does not expose a DXGI parent-swapchain interface in this revision.
Public AddRef establishes an internal keepalive on the zero-to-one transition. GetHeapProperties
returns the resource's stored properties/flags for ordinary committed allocations.
[Pinned resource implementation, resource.c:2229-2277,2858-2893](https://github.com/HansKristian-Work/vkd3d-proton/blob/81d96933a40207023a812a6f522caefd7e36b73b/libs/vkd3d/resource.c#L2229).

Thus the missing relationship is chiefly **in the owner**, not a secret field in GetDesc that the
game can query. Its effects are nevertheless observable:

| Runtime relationship | Ordinary T with a host ComPtr | Consequence |
|---|---|---|
| U remains allocated through the runtime's private reference when public references reach zero | T remains allocated through our ordinary public reference | Stable object identity is achievable, but the public reference accounting is different |
| Resize checks public references to every registered U | The real resize still checks U, never T | A game-held T reference no longer prevents the real resize |
| Index selects a registered U and the runtime's blit reads it | T is absent from that registry | Every real presentation needs a host transfer into the matching U, including NR-off/failure frames |
| Runtime retirement drains its own presentation use of U | Runtime knows nothing about extra NR/compare/cache work using T or O | Our recording, completion and retirement proofs remain necessary |

Concrete externally observable counterexample:

1. The application calls GetBuffer and retains a reference.
2. It calls ResizeBuffers without releasing it.
3. Without substitution that reference keeps U's public count nonzero and the checked runtime
   rejects the resize. With plain T substitution, and after our U references are released, the
   runtime can accept: it sees no outstanding reference to U.

Keeping old T alive prevents a use-after-free of that COM object, but does not restore the failed
resize semantics: the game's cached T can become a valid texture that no longer contributes to the
display. Keeping a permanent reference to U instead makes *every* resize fail. Neither is correct
emulation. Public AddRef/Release return values are not a portable external-reference oracle.

The vkd3d code itself supplies a private/public accounting mechanism; that mechanism is not a
public service made available to our replacement texture. Do not inspect its object layout, call
internal incref functions, or repeatedly Release until resize happens to succeed.

This is the tempering of the new evidence: **resource type/creation compatibility improves greatly;
swapchain ownership compatibility does not follow from matching that type.** A production answer
needs a separately reviewed resource-lifetime accounting layer, or an explicitly narrower contract
that gives up this observable compatibility. The latter is not silently selected here.

## 1. Texture generations, rotation and lifecycle

### Creation and selection

Commit to virtualisation at swapchain construction, before any application/injector can cache U.
Enabling it later at the first interesting Present is too late. Select one supported swapchain;
all others remain unchanged. Require the actual presenting DIRECT queue, same device/node, SDR
colour space and a supported, copy-compatible format. Start with RGBA8; BGRA can use the copy-only
transport, but NR admission additionally needs a verified typed-output path. No speculative UAV flag
on T, no RGBA/BGRA byte copy claimed to convert channels, no silent HDR-to-SDR conversion.

Allocate **exactly N T textures**, where N is the realised DXGI buffer count, not an FG constant or
a guessed triple-buffer size. Mirror each U's public description and heap properties on the same
device; use the vkd3d allocation above as the expected profile, then verify the actual objects.
Keep the model/output slots separate from this public buffer ring.

`GetBuffer(i, iid)` returns QueryInterface on the same T_i object throughout the generation. Validate
arguments/index and return the queried interface's real HRESULT and reference. Do not return U when
a requested interface on T fails: that would open the isolation boundary. All swapchain interface
versions must resolve to the same virtual wrapper; the existing QueryInterface implementation already
returns `this` for supported DXGI versions (`wrapped_swapchain.cpp:585-673`).

Forward GetCurrentBackBufferIndex to the real swapchain (`:1223-1227`). On each call to Present,
sample its actual index and use T_i/U_i. Do not advance an independent counter on failure, TEST,
occlusion or DO_NOT_WAIT. Do not synchronise it to the Vulkan image index. A slot retains *that
slot's* game content: rotating to another slot does not magically copy the most recently displayed
game image into it.

Fully initialise the T ring before first exposure/use, with an explicitly documented initial-image
policy (deterministic zero for the harness). This setup must be submitted/ordered, not just recorded.
The virtual host must not infer that an uninitialised region is a valid previous game frame. Ordinary
game rules for initial writes and cross-buffer partial-update bookkeeping still apply.

### ResizeBuffers / ResizeBuffers1

The required transaction is:

1. Stop admission for the generation; exclude concurrent lifecycle operations. Prove completion and
   seal all host recordings using T/O/U before the first release or descriptor cleanup. Failed drain
   preserves resources and returns the real error; never convert a timeout to successful retirement.
2. Establish that application references obey resize's contract. **This is the accounting gap above.**
   It cannot be inferred from the real U resize once T is substituted.
3. Preserve the old T generation and metadata for rollback. Release our references/views/lists that
   prevent real U resize, including overlay references, only after their uses are safe.
4. Call the real resize once. On failure, keep old T identities/content/descriptions and reacquire
   old U references as needed. Do not publish a speculative count or advance the generation.
5. On success, obtain the actual new U descriptions/count and build the new T generation; publish
   it atomically, invalidate comparisons/history/output caches and initialise owned resources.

Zero count, zero dimensions and UNKNOWN format have DXGI meanings; do not make zero-sized T or
guess dimensions before the real outcome. Prepare replacement allocations before the irreversible
resize where dimensions are known. If the real resize succeeds but new T allocation fails, it is
**not** possible to promise rollback of the real swapchain. Enter a reported failed virtual
generation, with no stale successful Present/GetBuffer, rather than expose enhanced U as a fallback.
This exceptional failure policy must be reviewed with the lifetime layer before implementation.
[ResizeBuffers contract](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers).

The current wrapper cannot supply this transaction unchanged: it cleans overlay targets at
`wrapped_swapchain.cpp:898` before WaitForGPUIdle at `:917`; that void helper (`:176-222`) does not
prove successful completion to its caller. ResizeBuffers1 changes `_device` before the underlying
result (`:1307-1312`). Those observations concern this wrapper, not the separately fixed D3D11 bridge.

For ResizeBuffers1, reject unsupported node/queue combinations before changes; preserve the actual
per-buffer queue association, or restrict the virtual mode to the original single queue. A proposed
queue change cannot mutate ownership metadata until the real operation succeeds.

### ResizeTarget, fullscreen and shutdown

Do not recreate T just because ResizeTarget or SetFullscreenState was called. Forward the operation
under lifecycle admission control, inspect the realised buffer identities/descriptions, and preserve
T if the buffers did not change. Normal WM_SIZE/ResizeBuffers completes the buffer resize. Do not
silently change objects still cached by the game when only a display mode changed. If a supported
runtime replaces buffers through another operation, that operation must execute the same generation
transaction; otherwise that runtime/path is unsupported. Current ResizeTarget simply forwards at
`:1090-1092`, and SetFullscreenState forwards at `:842`.

Shutdown retires host resources only by tracked completion, with a separate device-lost path.
Application-held T interfaces stay valid COM objects until their references are released. This does
not entitle the host to destroy storage still used by GPU recordings. Keep the current module's
record/submit/seal/retire discipline rather than assuming the runtime's U drain covers T and O.

### Turning NR off is not undoing virtualisation

There are two lifetimes. The startup/default-off choice leaves the existing wrapper path untouched.
Once a generation has exposed T, disabling NR must leave virtualisation installed and execute
**copy-only T -> U on every presentation** until swapchain recreation. The game may have cached T
and RTVs for minutes. Returning U from the next GetBuffer would split the game's resources.

This copy-only behaviour is mandatory, including model-init failure. It is not the default-off
path and must not be described as cost-free when disabled after activation. No configuration or
menu changes are made here; a future startup key still needs the four-point round trip.

## 2. Identity and observable queries

Prefer a native resource object with stable identity; do not invent a new texture every GetBuffer
or rebind an existing public object to unrelated storage during resize. An owning reference held by
the virtual generation plus normal QueryInterface/AddRef/Release is sufficient for ordinary pointer
caching **within** that generation, but not for the resize reference checks discussed above.

| Application query/use | Proposed handling / remaining limit |
|---|---|
| IUnknown identity, repeated GetBuffer, Resource/Resource1/Resource2 QI | Same native T identity; QI performs AddRef; test every supported alias. Never answer an unsupported IID with U |
| GetDesc / GetDesc1, GetHeapProperties | Match the actual public U allocation profile, including alignment/flags/node masks. Do not add ALLOW_UNORDERED_ACCESS to T merely for NR |
| GetDevice, resource methods, descriptors | T belongs to the same native device and normal resource methods operate on it. Resource/device proxy combinations still need integration tests |
| SetName, private data/interface data | Belong to T and survive until that T is destroyed; do not store game data on U instead or expose U via a private-data escape |
| IDXGISurface / IDXGIResource / GetParent on the texture | Not implemented by the inspected vkd3d resource QI; return T's actual result, not a fake swapchain parent. This is a Proton observation, not a Windows guarantee |
| Destruction notifier / zero-public-reference behaviour | Real destruction must follow ownership. Notifier timing and resize rejection expose the lifetime difference; they are not fixed by copying GetDesc |
| Map, shared handles, protected sessions, tiling/residency queries | Honour native T's actual capabilities; do not fabricate answers. Shared/protected/cross-API modes require their own ownership audit and are excluded from the first profile |
| Another injector compares T with independently obtained U | They are intentionally different objects. Compatibility requires intercepting/partitioning that access, not forging pointer equality |

We have not exhaustively tested these interfaces against the installed runtime. The fact that both
resources use the same vkd3d class is strong evidence for ordinary query compatibility; it is not a
universal claim about all introspection or external consumers.

A full resource COM facade could account for application references and resize semantics, but it
also needs correct unwrapping at every D3D12 device/list entry that consumes the resource, plus
consistent QueryInterface identity. Passing an arbitrary facade directly to a native driver method
is not a generic solution. Alternatively, intercepting native resource reference operations needs
its own concurrency/runtime-compatibility design. **Neither layer exists here; neither is selected
as a harmless implementation detail in this note.** ResTrack tracks descriptors/submission, not a
complete virtual-resource COM lifetime.

## 3. States, dataflow and partial writes

PRESENT and COMMON have the same D3D12 resource-state value, zero. A game's transition of T to
PRESENT is therefore legal for an ordinary texture; it does not submit that texture to a Vulkan
presentation engine. The enhanced-barrier specification also aliases the common/present layout;
interoperation still needs the normal access/synchronisation rules.
[Resource states](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_resource_states),
[enhanced barriers](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html).

At the supported Present boundary, game work has left T in COMMON/PRESENT and U is in PRESENT.
On the exact presenting queue, the host would record:

```text
NR accepted:
  T: COMMON -> NON_PIXEL_SHADER_RESOURCE
  NR reads T and zero guides; composition writes separate O (UAV)
  T: NON_PIXEL_SHADER_RESOURCE -> COMMON
  O: UAV -> COPY_SOURCE; U: PRESENT -> COPY_DEST
  CopyResource(U, O)
  O: COPY_SOURCE -> UAV; U: COPY_DEST -> PRESENT

NR unavailable/refused/busy:
  T: COMMON -> COPY_SOURCE; U: PRESENT -> COPY_DEST
  CopyResource(U, T)
  T: COPY_SOURCE -> COMMON; U: COPY_DEST -> PRESENT

then our overlay on U, then real Present (runtime transfers U to V).
```

No NR, comparator, cache restore or overlay operation may write its answer into T. No engine
upscale-triggered NR route may have already enhanced T's upstream source. Do not use
`Dispatch(..., T, ..., T)` to save an output allocation. The existing separate-source Dispatch path
reads colour without writing it (`DlssNr_Dx12.cpp:2074-2082,2933-2946`); it also currently equates
source/output inequality with before-upscale context (`:2407-2412,2593-2615`). A future host must
separate that context from pointer aliasing instead of inheriting misleading creation/history gates.
Set SDR and output state explicitly; clearing AfterRayReconstruction still changes the governing
working-scale setting and must be documented in the eventual implementation.

The source-isolation proof is induction on writes: initialise T independently; the game updates it
from its own clean history; the host only reads T; all enhanced output stays in O/U/V. Hence a later
partial update to T cannot preserve *our* previous NR output. This assumes no path imports O/U/V
back into game-visible inputs. It does not promise that an unrelated ReShade effect is itself free
of temporal feedback, or that a game intentionally capturing the displayed image cannot import it.

Partial rendering is not the same as DXGI partial presentation. Each T_i preserves its own contents;
the application still has to maintain valid contents across a buffer ring. A full NR result can
change pixels outside the game's dirty rectangle. Copying all of O to U and forwarding the old dirty
rectangles would misdescribe what changed. Full clean images can be presented with full-frame damage;
scroll/partial contracts that rely on the compositor reconstructing missing content need additional
clean-image assembly, or must be excluded. The inspected vkd3d Present currently ignores
`pPresentParameters` (`swapchain.c:1127`); do not turn that implementation detail into a portable
partial-update guarantee. The harness's first partial-write test must maintain a fully valid T.

## 4. Fallback must be a working presenter

Virtualisation makes transport mandatory. Skipping NR while calling the real Present on unchanged U
freezes or repeats an old display image. Every normal non-test presentation must choose **exactly
one current-frame transfer**: O -> U after complete composition, or intact T -> U otherwise.

- Reserve transport resources independently from the NR allocator/model slots. A busy model must
  not consume the last usable fallback list. Initialisation/extent settling, unsupported model
  format, signature rejection and NR-off-after-activation all use copy-only transport.
- A false Dispatch may have recorded setup or failed model work. T remains read-only, but the host
  must close/track that work and restore states. Submit only defined setup work; use a separate
  valid fallback recording if the failed list is poisoned. Never copy partial O or infer a valid
  answer merely from CPU feature creation.
- Queue-order the transfer before real Present. The no-FG single-queue profile removes the need
  for an invented pre-flip CPU wait, but still requires validation of the runtime handoff. The
  inspected vkd3d path enqueues its present callback on the command queue before advancing the
  user index (`swapchain.c:1210-1215`). Existing synchronous harness results do not test this host.
- Track all reads of T and writes/reads of O/U, even fallback-only work. Restore T before any later
  game writes; unknown cross-queue writers are unsupported. Do not let CPU index rotation stand
  in for GPU completion or safe allocator reuse.

**A remaining hard case is transport exhaustion.** A pre-recorded fallback list cannot simply be
re-executed while its previous execution is in flight; D3D12 explicitly requires prior completion.
Neither can its allocator be reset early. A larger finite ring reduces frequency, but does not prove
availability without a bound on outstanding presents. Unbounded allocation is not a lifetime policy.
[ExecuteCommandLists rules](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists).

If even fallback cannot be submitted, return an actual presentation failure and retain resources;
never return success while presenting stale U. That handles catastrophic errors honestly, but it
does **not** satisfy an unconditional busy-host fallback guarantee. To close this point, the next
design must prove a transport-slot bound for the supported presentation profile, reuse an already
safe producer recording boundary, or explicitly negotiate backpressure. It may not add a new CPU
wait in Present. This is a blocker for the requested complete slice, not a reason to weaken fallback.

There is no promise of successful display after device loss or allocation failure. Once work was
submitted, a failed Signal cannot cancel it or establish completion; use the existing unknown-work
retention discipline. Resize may drain at its explicit lifecycle boundary, but Present may not wait
for a resize worker or reclaim busy resources synchronously.

## 5. Who can reach U without our GetBuffer

Changing `wrapped_swapchain.cpp:799-802` controls callers of **that wrapper only**. The current code
intentionally retains and passes real objects elsewhere:

| Consumer / path | Evidence | Required boundary |
|---|---|---|
| Factory creation and globals | `DxgiFactory_Hooks.cpp:515,531-554`; `DxgiFactory_WrappedCalls.cpp:358,374-398` create/unwrap the real object and save `currentRealSwapchain` before wrapping | Decide at creation; audit every factory variant. A lower injector can already have cached U before our wrapper exists |
| Present / menu | Wrapped Present calls LocalPresent with `_real` (`wrapped_swapchain.cpp:784`); overlay at `:496` receives it. Menu fetches buffers at `menu_overlay_dx.cpp:95`, caches resources at `:110`, submits on `currentSCCommandQueue` at `:488` | Intentional display-side consumer: draw only on U, after the host transfer; match the queue/generation. Passing the virtual wrapper here would draw on T instead |
| Overlay lifecycle | `menu_overlay_dx.cpp:117-125,440-443` releases cached buffers and resets an index-selected allocator; state is global (`:25-36`) | Include overlay uses in drain/reuse proof; selected-swapchain identity alone is insufficient. Existing menu allocation/reuse is not certified by this note |
| FG hooks and outputs | `FG_Hooks.cpp:676,730,912,967,1389` obtain buffers from their own swapchain arguments; native Streamline can retain its own objects | Refuse FG, including native/unknown activity, before exposing T. NoFG enum outputs are not proof of no native FG. Unexpected activation requires a failed/unsupported session or recreation, not live unvirtualisation |
| HudFix / ResTrack | `ResTrack_dx12.cpp:424-443` gates HudFix on FG; `:472-488,533-535,587-589` consult `scBuffers` for view handling; resize populates that list from `_real` at `wrapped_swapchain.cpp:1056-1073` | HudFix stays inactive; do not arm it to solve isolation. ForceHDR is incompatible with this SDR profile. Distinguish application T identities from display U in any shared bookkeeping |
| Streamline native-interface escape | `Streamline_Hooks.cpp:556-560` forwards the call; its attach is commented out at `:2060-2061`. `Util.cpp:665-684` unwraps a private Streamline IID | Do not assume all interface routes are intercepted. An active proxy topology must be audited or excluded, even without FG |
| Unknown QI on our wrapper | `wrapped_swapchain.cpp:672-673` returns E_NOINTERFACE rather than forwarding unknown interfaces | Preserve the closed boundary in virtual mode; returning raw U through an unwrapping convenience would defeat it |

### ReShade is an ordering problem, not a DLL-name check

Inspected **ReShade `eeb2c76aea8e00200d88b479c9036c5ea4d06d5e`**. Its D3D12 swapchain implementation
is constructed with its original object (`dxgi_swapchain.cpp:81-100`), GetBuffer forwards (`:290-292`),
and QueryInterface explicitly exposes its original object for the unwrapped IID (`:184-192`). Its
D3D12 API `get_back_buffer` also queries the stored original swapchain. Its effects execute from
`on_present` before forwarding Present, with D3D12 queue locking (`:962-979`).
[DXGI wrapper](https://github.com/crosire/reshade/blob/eeb2c76aea8e00200d88b479c9036c5ea4d06d5e/source/dxgi/dxgi_swapchain.cpp#L81),
[D3D12 buffer access](https://github.com/crosire/reshade/blob/eeb2c76aea8e00200d88b479c9036c5ea4d06d5e/source/d3d12/d3d12_impl_swapchain.cpp#L38).

- **Game -> our virtual wrapper -> ReShade -> runtime:** normal game GetBuffer can return T;
  ReShade's lower original references can still reach U. A useful ordering is host transfer then
  ReShade on U then runtime. It is safe only if no proxy/addon exposes U back to the game or T's
  producers, and queue/lifetime ownership is established.
- **Game -> ReShade -> our wrapper -> runtime:** ReShade may receive T as its backbuffer and write
  effects into it before our Present. NR can still avoid writing its own result there, but T is
  now game-plus-ReShade history, not exclusively the game's history. Unchanged or partial frames
  can accumulate ReShade's own effects. This topology does not meet the clean-T contract as stated.
- **Injector cached real buffers during creation, bypasses Present, or exposes them through an addon:**
  our later GetBuffer substitution cannot revoke those pointers. The virtualisation boundary is open.

Our loader supports ReShade (`dllmain.cpp:1163-1179`), so this is not hypothetical topology to ignore.
The first profile must exclude unintegrated ReShade/addons and other swapchain proxies. Presence or
absence of one DLL name is not proof of topology; a future compatible profile needs a documented
resource/queue handoff and tests of both load orders. Do not silently disable someone else's injector,
or advertise generic ReShade support on the strength of GetBuffer forwarding alone.

The claim that feedback is impossible is therefore conditional: it holds inside a closed T/O/U
partition. No same-process wrapper can promise isolation from arbitrary injected code that retains
or deliberately exposes the real display resource. That is a compatibility boundary, not an argument
against source isolation itself.

## Duplicate rejection: a valid purpose for a signature, still not a free scheduler

Comparing current T against the last clean T processed is now meaningful. A collision used **only
to reject inference and copy current T to U** loses an enhancement, not current image content.
Reusing cached O on a hash-only equality is different: a collision could display the wrong frame.
Exact equality or another proven identity is required for cached-output substitution.

The decision still has to reach the consumer in time. The CPU cannot skip the current NGX call using
a GPU hash whose result it has not observed. A fence wait would violate the inherited restriction;
an older slot's hash does not describe T after new game writes. SetPredication/ExecuteIndirect could
gate our shader/copy commands but have no established conditional-evaluation contract for the opaque
NGX call (`dlssnr_forwarder.cpp:889-948`). CPU state/history still advances in our current Dispatch
(`DlssNr_Dx12.cpp:2005,3202`). See the detailed analysis in [nr-content-renewal.md](nr-content-renewal.md).

Thus virtualisation fixes feedback safety **even if NR is redundantly evaluated**, but it does not
by itself deliver the requested second-evaluation skip. Unconditional inference with predicated
copy-back must not be reported as passing that test. A frozen snapshot evaluated later is safe
input, but choosing its output for a newer T requires a current equality check or an explicitly
buffered display design. It also adds storage/transport and changes the one-copy cost argument.

No zero-wait NGX duplicate scheduler is claimed here. This is the third blocker for the complete
requested contract; a tests-only conditional-NGX experiment or a revised scheduling contract needs
review before production work.

## Cost, with the corrected baseline

At 3440x1440 RGBA8, one image is **19,814,400 bytes (19.8144 MB decimal)**. The runtime's U-to-V
presentation transfer already exists and remains in both designs; it may be a blit/render operation,
not necessarily a byte-copy with the same dimensions. Its cost was not measured here.

| Additional host work | Payload per processed present |
|---|---|
| In-place write-back design: U -> input scratch, O -> U | 39.6288 MB, two full copies |
| Virtual design: read T directly, O -> U | 19.8144 MB, one full copy |
| Virtual copy-only fallback: T -> U | 19.8144 MB, one full copy even though no NR ran |

The user is right about **halving additional outer-copy payload in the direct-input case**. It is
not proof of half the total bandwidth/time: NR encode/model/composition still read/write their own
surfaces. One copy implies 39.6288 MB logical read-plus-write accesses, not measured VRAM traffic.
Neither copy volume shrinks with model working scale. T must remain read-only to NR; in-place
processing would recover the old contamination, not another optimisation.

N additional T surfaces cost N images over the runtime baseline: 39.63 MB for two or 59.44 MB for
three, before alignment/metadata and output/model allocations. The old single input scratch may be
removed; the whole T ring is not necessarily less VRAM. Exact last-input comparison needs retained
input and full-image reads; separately computed hashes also read image content. Snapshots added for
asynchronous deduplication would consume some or all of the saved transfer volume.

The existing after-RR 2.90 ms at 0.5x and 6.36 ms at 1.0x measurements are not timings of this host.
Use the existing GPU timing/submission machinery later to separate transport, comparator, model,
composition and runtime-present latency. Collect results asynchronously, not via a new present-thread
readback wait. No FPS, copy milliseconds or compatibility improvement is claimed as measured here.

## 6. Harness tests that could reject this design

Extend `tests/dlssnr-loopback --present-nr` later with a real virtual swapchain boundary. The simulated
game must obtain T **only through the public wrapper GetBuffer**, cache its pointers and create its
RTVs normally. The presenter privately holds U. Directly assigning the fixture to a test-owned T
while bypassing GetBuffer would not test the production boundary. Preserve the original mode.

The current harness restores a complete fixture each time (`present_nr.h:452-464`) and checks source
bytes (`:523-526`). Split producer, virtual presenter and independent observer. Maintain an exact CPU
oracle for every T_i; append readbacks for T, O and U and consume them outside the measured Present
path. The current Wait calls (`:98,498,557`) must remain in the old control, not secretly validate
the new no-wait path. Source-isolation and duplicate-scheduling results must be reported separately.

| Test | Required result |
|---|---|
| GetBuffer(i) twice, every supported QI, release/reacquire while generation lives | Stable IUnknown/resource identity, valid refs and matching descriptors/heap properties; no U escape |
| Full image A, NR, read T after completion | T remains byte-exact A while U contains the expected NR answer |
| Partial update P to the same T_i on a later visit, keeping all other bytes | T equals P(A), never P(E(A)). Compare output with an independent clean P(A) control using equivalent model history; changed pixels outside P alone are not evidence of accumulation |
| Repeated clean A, same slot and another slot | With a real duplicate scheduler, second NGX evaluation count stays unchanged and current T is still delivered. A no-scheduler prototype must report this requirement FAIL/NOT IMPLEMENTED, not call it a skip |
| Forced hash collision between different clean inputs | Copy current T unchanged; no cached wrong O. Reject-only loss of enhancement is the expected outcome |
| Model busy, setup-only false, NR disabled after activation, model allocation/evaluate failure | Current T reaches U intact through independent fallback; no stale-image successful Present |
| Delay GPU past transport-slot reuse | No allocator Reset/re-execution while pending, no CPU wait in admission; prove fallback capacity or report the unsatisfied bound |
| Hold an application T ref across ResizeBuffers | Same rejection as baseline U. This exposes the missing reference-accounting layer immediately |
| Release app refs, resize count/extent/format; inject failed real resize | Successful generation replacement uses actual index/count; failure preserves old T objects/content and description |
| ResizeTarget, fullscreen/windowed, TEST, occlusion, DO_NOT_WAIT retry | No fabricated index advance or premature T replacement; display transfer occurs only for the actual presentation path |
| Delay host and runtime work during resize/shutdown | No T/O/U destruction while used; report unknown completion instead of successful retirement |
| Menu open/closed, both ReShade wrapper orders, native-interface escape | Prove which resources each actor reads/writes. Unknown or open partitions are unsupported, not a pass |
| New binary with startup key absent/off | Same public resources, copy count and behaviour as baseline; no virtual allocations/hooks |

Initialise all T slots before partial-update testing; retaining bytes in one slot does not initialise
the others. Test two/three buffers and count shrink/grow. A valid flip-sequential setup is useful for
preservation tests; don't depend on discarded U contents. For partial Present1/scroll, compare against
the actual baseline runtime semantics before claiming support.

For image quality, capture before/after pairs and exact hashes, MAE/max. Use the T readback as the
primary no-feedback oracle: model history can change O over time without any source contamination.
Use equivalent/reset history for the independent output reference, rather than confusing temporal
differences with recursive enhancement. Do not re-run the earlier UI-correction or chroma probes.

For flip ordering, queued readbacks prove resource contents, not what scanout consumed. Add an
independent presentation trace/observer and delay host submission intentionally; verify that the
runtime reads the correct U after the host transfer without a host CPU fence wait. Preserve compiler
prefix exclusion in the runner. No such tests were added or executed for this note.

## Review and stop point

The source-isolation argument and one-versus-two outer-copy arithmetic are sound **under the stated
partition**. The Proton discovery removes much of the resource-type risk and is incorporated as
positive evidence, not dismissed. It does not provide our wrapper with the runtime's private owner.

Before a production implementation, resolve:

1. **Resize/reference semantics:** design and test public-reference accounting for T, including QI,
   failed resize and cached pointers, without depending on vkd3d's private object layout.
2. **Mandatory transport under load:** establish a bounded, no-new-wait fallback submission path;
   a finite ring and index rotation alone are not the proof.
3. **Actual duplicate suppression:** establish how current-input equality suppresses the NGX
   evaluation without a CPU stall, or explicitly revise that requirement. Source isolation does
   not answer it.
4. **Resource boundary:** restrict or integrate external proxy/injector paths before T is exposed;
   runtime detection after an unknown actor cached U cannot repair the boundary.

Per DEVELOPMENT.md sections 1 and 3: this document adds no live setting/control and changes no
off-path behaviour, shader layout or passthrough logic. A future startup key needs declaration,
read, save and shipped-INI documentation together. Activated virtualisation's copy-only lifetime
must be explicit. Ownership and retirement need an adversarial review independent of a clean build.
Native Vulkan resource management stays with the runtime; no private Vulkan handles are adopted.

Only this new design note was written. Source/API inspection is evidence; no new build, harness run,
test, deployment or measurement is claimed. The complete production slice remains blocked on the
listed contracts, rather than being declared impossible on Proton or approved because T can be created.
