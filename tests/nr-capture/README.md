# Comparison capture (host)

Run `python3 tests/nr-capture/run.py`. Needs `g++` with C++20 and ASan/UBSan; Pillow is used as a second PNG reader
when it is installed and skipped when it is not.

What it holds: the pure half of the same-frame comparison capture
(`docs/dlssnr/design/comparison-capture.md`), compiled from the production headers
`retoque/dlssnr/DlssNr_ImageFile.h` and `retoque/dlssnr/DlssNr_PairCapture.h`.

`test.cpp` asserts what C++ can check against itself:

- **The state machine** over a fake backend that counts every call: request, record, nothing handed to the writer
  before the ticket completes, exactly one hand-off when it does, a second request ignored while one is under way,
  the session's references released only when the list can never run again (`Ready`, not merely `Completed`), a
  ticket that never completes kept and never released and the capture then off for the session, an ignored press that
  does not move a waiting request's expiry, the untracked path read after 8 dispatches and labelled
  `assumed-8-frames`, released after 32, refusals and expiry, an allocation failure, the retained-pair bound, a
  hand-off the writer refuses, and shutdown freeing only what is proven done.
- **The menu button**, every row of DEVELOPMENT.md section 2.3.
- **Names**: the UTC stamp (epoch, a leap day, before 1970, 2100), the executable stem sanitiser, stems and file
  names, which names are ours, and the 64-pair cap over a shuffled list and in a real directory, where foreign files
  survive and a stray `.tmp` goes with its stem.
- **Write then rename**: the final name never exists while the file is written; a failed write leaves neither name.
- **The record**: one entry per key, `;` and anything outside printable ASCII replaced, valid as tEXt.

`run.py` checks the files from the other side. Each image case is a source surface laid out like a readback (rows on
a 256-byte pitch from a 512-byte offset, padding filled with bytes no file may contain) plus the file the production
writer made from it. Every PNG is walked chunk by chunk (each CRC-32 against `zlib.crc32`; IHDR; sRGB and tEXt before
the image data; IEND last), its zlib stream block by block (stored blocks only, LEN/NLEN, none empty, one per IDAT,
the Adler-32 against `zlib.adler32`), decompressed with `zlib`, and every scanline compared with what Python
recomputes from the source: BGRA swizzled, alpha dropped, ten-bit codes widened, float quantised with the production
rule emulated in float32. Block boundaries are exercised exactly (one full block, two, one row over). Every PFM is
compared bit for bit, rows bottom to top, NaN payloads and infinities kept. All 65,536 halves and every R11G11B10
channel value are held against Python's own decoding. The record's three forms (line, sidecar, tEXt) must agree, and
the repository's privacy patterns (`tests/repo-privacy`, read from its source) must match none of the generated text,
though the inputs carried a home path, a Windows profile, a Wine home and a tailnet host.

Source guards hold the integration: nothing waits for the GPU or maps a readback outside the writer thread; the
readback helpers exist once (`DlssNr_Readback.h`); no record value is set from a path, and the executable reaches it
only as its sanitised stem; `PairCapture::Poll()` sits before the first return in `Dispatch`; the record site is
guarded by `wrote && Wanted()`, comes after the stabilizer and copies `hdrCopy` (NON_PIXEL_SHADER_RESOURCE) and the
target (UNORDERED_ACCESS); the burst's "before" is `hdrCopy`; `TimingMetadata` and the capture share
`TimingContract`, and a frame that may carry a capture is labelled `capture_active` in the timing metadata; the
record site does no file search or file read (the model's facts are found by the writer); the trigger file asks for
the pair and for the burst on `burst`; `Shutdown` calls the capture's;
the bridge host labels its frames; and `DlssNr_PairCapture.cpp` is in both project files once.

Every reason the capture can show on the menu's line must have a pt-BR entry (those reach the menu through a variable,
which nr-localization's own drift guard cannot see).

Every check was shown to fail on an injected defect before this was registered. In the writers: an Adler-32 seeded
wrong, BGRA not swizzled, a subnormal half off by one exponent, PFM rows top to bottom, quantising without rounding,
the blue channel of R11G11B10 read one bit off, rows read without their pitch, every stored block marked final, a
16-bit row written at 8-bit width. In the state machine: handing off before completion, releasing on `Completed`
instead of `Ready`, releasing instead of leaking a recording that never completed, a second request accepted while
one is under way, no retained-pair bound, the untracked path read after one dispatch, tracking after the first copy,
shutdown releasing what is not ready, no request expiry, a poll that does not count. In the rest: the Compare row
missing from the button, the cap off by one, a two-digit sequence accepted as ours, ';' left in a record value.

## What this does not cover

No GPU, no D3D12, no Wine. The D3D12 backend in `DlssNr_PairCapture.cpp` -- the readback allocation and copies, the
submission ticket, `Map`/`Unmap` on the writer thread, the adapter query, `_wfopen` -- is compiled only by the real
build, and its behaviour is evidence only from a run: that the two images are the same frame, that a capture costs
what it costs, and that the files open in a stock viewer. Section 6 of the design note lists the wine-tier and
in-game checks that remain.
