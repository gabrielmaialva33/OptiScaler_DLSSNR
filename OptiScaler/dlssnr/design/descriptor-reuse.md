# DLSS-NR: reusing descriptors in DispatchPass

## What it changes

`DlssNr_Dx12::DispatchPass` wrote five SRVs and two UAVs on every call — two driver view-creation
calls per slot, three dispatches a frame, so twenty-one per frame — even when the ring slot came
back to exactly the bindings it already held. In steady state the same few resources (the game's
output, the private copies, the guide clones) are bound in the same order every frame. Each slot now
keeps a key describing what its descriptors were last built from, and a view is created only when
that key moved.

## Why a view can be reused at all

A view here is a pure function of two things, and of nothing else:

1. **The resource.** `Shader_Dx12::CreateShaderResourceView` and `CreateUnorderedAccessView` read
   `GetDesc()` and nothing more. `Dimension` and `DepthOrArraySize` choose the view dimension,
   `MipLevels` its mip count, `Format` its format.
2. **The arguments the call passes.** The format override, the mip slice, and — since `97c94b05` —
   `translateTypeless`.

If both are unchanged, the bytes this call would write are the bytes already in the slot.

## What is in the key, and why each part earns its place

| field | why |
|---|---|
| `res` | the resource being viewed |
| `generation` | see below |
| `dimension`, `depthOrArraySize`, `mipLevels`, `format` | the desc fields that shape the view |
| `width`, `height` | shape no view; discriminators for an address handed back to a different resource |
| `viewFormat`, `mipLevel`, `translateTypeless` | the creation arguments |

`DispatchPass` names the creation arguments once, as `kViewFormat`, `kViewMipLevel` and
`kViewTranslateTypeless`, and feeds the same three both to the create call and to the key. They are
constants today. They are named rather than written inline because the previous version of this
cache was designed when `translateTypeless` did not exist as a parameter, and its design note had to
say "if the view policy becomes configurable, its identity must be added to the cache key" — which
is a sentence that only works if someone reads it. Naming them makes the key the place the next
argument has to go, and `tests/nr-dispatch/run.py` fails if a call site passes something the key
does not carry.

## The generation

Shape equality answers "would the descriptor bytes be the same", not "does this still name the same
allocation". An NR scratch texture freed and re-created at the same size — a feature rebuild does
exactly that — can come back at the same `ID3D12Resource*` address over different memory, and
nothing readable from the resource would show it.

`g_nrScratchGeneration` advances on every successful `CreateScratch` and is part of every key, so
one allocation retires every cached descriptor at once. That is coarse on purpose: a per-resource
registry would have to be unwound along every release path, and scratch allocation happens on init,
on resize and on rebuild, never per frame. The cost of retiring everything is one pass of view
creation at a moment that was already re-creating resources.

It never resets on shutdown, because the next session's allocator can hand back this session's
addresses. It saturates rather than wrapping, because a wrapped generation is the one value that
makes a stale key look fresh — and since a saturated counter can no longer retire anything,
`DescriptorReuseAvailable()` turns reuse off entirely at that point rather than leaving keys nothing
can invalidate.

## What this does not cover

A **game-owned** resource released and replaced at the same address with an identical description.
Those have no lifetime token we can read, and pointer plus shape is all any cache of someone else's
resources can key on. This is the residual risk, and it is the same one the upstream design
(`scottmudge` `708f1dd8`) carries; the shape fields narrow it, they do not close it.

## What is proven, and what is not

`tests/nr-dispatch` executes the real constructor and the real `DispatchPass` against fakes, and
asserts descriptor **contents**, not call counts — a cache that skips a write it should have made
leaves the previous incarnation's record in the slot, which counting cannot see. Nine mutations of
the production code were confirmed to fail the suite: six reshapes (one per desc field in the key),
a key that drops the generation, a saturation check that stops failing closed, a write that does not
record its key, and a call site that passes an argument outside the key. Three further mutations
drop a key member that is constant today; they are caught by comparing `BindKey`'s members against
its own `operator==`, since no behavioural test can see them.

**Not proven: that this is faster.** Fewer descriptor API calls is a call count, not a profile. In
the fixture, 145 dispatches over a 48-slot ring fall from 725 SRV and 290 UAV creations to 240 and
96. What that is worth on a real driver, on a real frame, is unmeasured, and the cache also adds a
key comparison to the dispatch path. A D3D12 validation-layer run and a CPU profile are still owed.

## Provenance

The shape of this — per-slot keys, no allowlist — is `scottmudge/custom_dlssnr` `708f1dd8`. Taken
here with the key widened: `Dimension`, `DepthOrArraySize` and `MipLevels` shape the view and were
not in the original key, the creation arguments were not either, and the generation is from this
fork's earlier attempt (`perf/nr-descriptor-cache`, `f1b8b6d1`), whose allowlist of seven hand-named
scratch fields this design does not need.

The constants half of `708f1dd8` — skipping the `Map`/`memcpy`/`Unmap` of the 256-byte constants
buffer when its bytes are unchanged — is **not** taken. This fork's upload buffers stay mapped for
their lifetime, so that write is already a plain `memcpy` into a live pointer; trading it for a
compare against a shadow copy is not a clear win, and it adds 256 bytes of state per ring slot.
