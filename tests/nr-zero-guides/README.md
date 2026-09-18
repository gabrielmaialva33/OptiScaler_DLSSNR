# NR zero guides

Run `python3 tests/nr-zero-guides/run.py`. The runner compiles the production
`OptiScaler/dlssnr/DlssNr_ZeroGuides.{h,cpp}` — the bodies themselves, not a copy — against strict
host D3D12 fakes with ASan/UBSan.

## What this covers

The pass reads engine depth and motion out of the parameter block a game handed its upscaler. A host
that originates the evaluate itself has no such block, and `Dispatch` rejects a null guide. These
stand in: `R32_FLOAT` depth and `R16G16_FLOAT` motion at the frame's size, zero-filled once.

The formats and the zero fill are the ones `tests/dlssnr-loopback --cold-nr` drove through the
production forwarder into feature 18 with no game NGX call in the process — 48 evaluations, 48
successes, all fence-complete under Proton.

The rule worth a suite of its own is the clear. `ClearUnorderedAccessViewFloat` takes two descriptors
for one view: the CPU handle must come from a heap that is **not** shader visible, and the GPU handle
from one that **is** and that is bound on the list. One heap satisfies neither half, and on a real
driver with no validation layer the wrong arrangement is invisible — the guides simply hold whatever
the allocator gave them, and the model is guided by noise. The fakes carry each descriptor's heap
identity, so a clear is checked against the rule rather than against itself.

The other half is that a clear is initialization, and a clear recorded onto a list nobody executed is
not initialization. `ConfirmExecuted` and `AbandonRecording` are how the caller says which happened;
an abandoned recording leaves the zeros owed and a later confirm cannot claim them.

Nine mutations of the production code were confirmed to fail this suite: the CPU handle taken from
the shader-visible heap (the shape the donor's version got wrong), an unbound heap, a depth cleared to
one instead of zero, a descriptor offset that ignores the heap stride, recording treated as execution,
an abandon that forgives the debt, an untyped view, a missing transition to the read state, and an
allocation failure that leaks what it acquired.

## What it does not cover

No GPU, no driver, no model. It proves the provider obeys the D3D12 contract and its own
initialization bookkeeping; it says nothing about whether zero guides produce an image worth having.
They do not: the model reads a permanently still motion field as a still scene and smears anything
that moves. This is bring-up, and a synthesised motion field is separate work.
