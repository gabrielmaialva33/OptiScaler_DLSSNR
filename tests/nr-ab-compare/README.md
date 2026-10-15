# NR Stage A/B comparator

Run `python3 tests/nr-ab-compare/run.py` (inside the usual memory-capped host scope).
No third-party packages, Wine or GPU. Tests import the real `tools/nr_ab_compare.py` and run its CLI.

Synthetic files cover RGB PNG8/16 and filters 0–4, CRC rejection, PFM orientation/endian/scale and
non-finite rejection, typed raw FP16/FP32, padded raw rows including D3D12's unpadded last row,
identity, blur, added high-frequency noise, RGB tint, stable and alternating edits, original motion,
consecutive/sparse frame IDs, stage/fallback/held/completion/provenance/settings/size/path refusals,
different stage extents without resizing, model-extent drift within an arm, incomplete contracts,
missing raw frames, and GPU timing stage/capture exclusion and contract separation. A flat original yields null percent
sharpness change rather than division by zero. Raw bursts explicitly lack fence/provenance proof.

These tests validate arithmetic and file contracts, not game image quality, final upscaler output,
GPU capture completion, pacing or perceptual noise/flicker classification.
