# Preserve Streamline camera constants

Adapted from the issues identified by HomidWay's
[camera forwarding](https://github.com/HomidWay/OptiScaler-DLSSNR-PreSR-Multipass/commit/49d345819530510385703e0ac9d7877e301b4f99)
and [tag extent](https://github.com/HomidWay/OptiScaler-DLSSNR-PreSR-Multipass/commit/b0918af78f7e7f2bd1a41605518b71fb0dedf0b2)
changes, with local validation, frame identity and bounds checks.

DLSS-G currently reconstructs a symmetric projection from near/far/FOV and replaces temporal
transforms with identity. Streamline games already supply their actual projection, its inverse and
the transforms between current and previous clip space. Forward that camera group when all four
matrices are finite, initialized and mutually inverse; reject empty/identity projections and retain
the existing reconstruction for broken or incomplete input (including Unreal's known broken camera).
Optional lens and pose fields are forwarded only when valid. Motion-vector normalization, jitter,
reset policy and resource-derived flags continue to come from the output backend.

The external Streamline frame number and the backend's logical frame counter are not always equal.
Record both at `StartNewFrame`, register the logical frame in a mutex-protected, owned camera cache,
and accept input only for that exact registration. The dispatch reads only its own logical frame.
Ring reuse starts an empty registration, and deactivation/resource release invalidates every
registration. Delayed constants may update an older still-registered frame, never the current slot
by accident. Invalid replacement constants clear that frame's camera instead of keeping stale data.

The D3D12 resource-tag adapter also preserves `left` and `top` along with width and height.
The output backend already carries these origins to its Streamline tags. Reject explicit extents
outside the native texture before updating dimensions; subtraction-based checks avoid integer
overflow. An absent or empty extent retains the full-texture fallback with a zero origin.
The Vulkan adapter is unchanged.

Host tests exercise the actual SDK types and production input/dispatch slices. They establish the
forwarding and lifetime contract. `python3 tests/fg-inputs-d3d12/run.py` adds an automated MSVC
fixture with real DirectXMath projections, temporal reprojection and 64 GPU texture crops/readbacks,
plus failing camera/origin mutation controls. No game is needed for those regression checks.
It does not execute the complete DLL or real DLSS-G provider and does not establish generated-frame
quality. Representative game validation and native Windows debug validation remain release checks.
