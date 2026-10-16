# Streamline tag extents

Run `python3 tests/fg-tag-extents/run.py` on the host.

The actual SDK resource/tag types and production `Sl_Inputs::reportResource` run against
scripted FG consumers under ASan/UBSan. Covers depth, motion, HUD-less and UI resource
origins, full-resource fallback, validity, frame slot, exact bounds, overflow-safe
rejection without changing motion dimensions, and the unchanged Vulkan branch.

These cases do not execute a GPU, resource copies or a frame generation provider.
