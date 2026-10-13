# D3D12 NGX core shutdown ABI

## Reproduction and cause

Before the ABI fix, the corrected Proton loopback harness completed its seven extent steps with NR enabled,
then crashed in the driver during `NVSDK_NGX_D3D12_Shutdown1`. Both baseline `fb079c16`
and persistent-mapping build `61eabbce` reproduce it. Disabling NR avoids initializing
NGX core in this FSR fallback harness and completes shutdown; it does not by itself
prove a feature-lifetime problem.

The public [NVIDIA SDK header](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx.h)
declares a one-argument device shutdown. The header also warns that SDK, core and
snippet functions with the same names can have different signatures. NVNGXProxy loads
raw driver-core exports, so the SDK typedef cannot establish their ABI.

Local disassembly of NVIDIA 610.57.04 `/usr/lib/nvidia/wine/_nvngx.dll` provides the
specific evidence for the adapter:

- Device shutdown at RVA `0x5fd90` preserves incoming RDX (argument two), obtains
  the device LUID, then passes that pointer as R9 to the internal routine at `0x3ad80`.
- That routine stores R9 in R12 and writes a remaining-instance count through it at
  `0x3af44`. The original one-argument call leaves RDX unspecified. In the crash,
  R12 is `0x14`, explaining the invalid write.
- All-device shutdown at `0x5fd20` supplies its own local unsigned output to the
  same routine. Device shutdown dereferences the supplied device, so a null device
  must use the separate all-device entry point.

The two-argument private signature is inferred from this installed driver's binary,
not presented as a documented or versioned NVIDIA public API.

## Change and boundaries

Keep the public one-argument typedef, including the frame-generation provider's API.
Use a distinct two-argument typedef only for the raw core export. The public proxy
getter returns an adapter that supplies local writable storage and propagates the
core result. A null device routes to the all-device export, or returns NotImplemented
when that export is unavailable. Uninitialized/missing-export getter behavior remains.

This change allocates nothing and runs only during shutdown. It also corrects the raw
core call for ordinary DLSS users with NR disabled; per-frame paths and hooks are unchanged. It does not change NR
recording, resource retirement, descriptors, shader inputs, configuration or UI. No
new synchronization or GPU-lifetime guarantee is claimed. Other graphics APIs retain
their existing declarations; their private ABI has not been established here.

An earlier experiment releasing NR features, snippet and capability parameters before
core shutdown reached its cleanup log but reproduced the same invalid write. Those
production changes were reverted. The patch and logs remain only in ignored local
`x64/dispatch-validation/shutdown-order-experiment/` evidence.

## Validation

`tests/nr-shutdown` compiles the actual adapter and getter under ASan/UBSan. It checks
writable initialized output storage, public function type, device identity, success
and error propagation, null-device routing and unavailable exports. Fakes cannot prove
the private driver ABI or GPU teardown.

The Release build passed, all 11 host suites passed, and the corrected real Proton
loopback completed seven steps / five feature creations / 4371 upscale evaluations,
logged NR composition at both output sizes, and returned `0x00000001` from shutdown
with normal process exit. The harness compiler recovered one Wine stall automatically.
Evidence, binary hashes and the source patch are in `x64/dispatch-validation/core-abi/`.
This validates one installed driver/runtime and this harness, not every driver,
concurrent device teardown, in-process NGX reinitialization or game behavior.
