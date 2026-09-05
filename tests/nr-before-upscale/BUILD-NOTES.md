# Local Release verification, 2026-09-05

The normal command is `./build-local.sh Release`. No MSBuild `/m` was introduced;
the existing script uses `/p:CL_MPCount=16` for compiler parallelism.

The incremental build stalled under the workstation's Wine 11.17 runtime. MSBuild
and CL waited with effectively no compiler CPU progress, while wineserver and a
winedevice service thread consumed CPU continuously. A three-second syscall trace
observed 70,471 repeated `0xc0000022` (STATUS_ACCESS_DENIED) replies on that thread's
wineserver channel. The request opcode was `0xfe`, `get_next_device_request` in the
[matching Wine protocol source](https://github.com/wine-mirror/wine/blob/wine-11.17/include/wine/server_protocol.h).
Attaching a debugger and tracing confirmed the runtime loop; they did not identify
why access was denied or prove which compiler wait depended on that loop.

After verifying that the dedicated `msvc-wineprefix` served only this build, stopping
that prefix's wineserver and rerunning the unchanged build command completed Release.
Reusing the warm prefix reproduced the stall; restarting that same prefix again
completed the coverage build. This is a bounded workaround, not a fix for Wine or
proof that a particular synchronization backend is responsible. Do not kill unrelated
Wine/Proton processes. No permanent Wine setting or game prefix was changed.

Before replacing local build outputs, the previous Release and `x64/out` DLLs and
forwarder were copied with SHA-256 manifests to the workstation state backup directory.
That preserves the previous **local build**, not a claim that it was validated in game.
Game installations and markers were not modified. Runtime traces, successful/failed
build logs and host test output are retained under the project-specific workstation
state directory `~/.local/state/crimson-desert-dlss5/nr-before-upscale-validation/`.

Release success is a compile/link check. It does not validate the NGX model, actual
GPU resource completion, pixels or performance; see the design note's GPU lifetime
boundary and the suite README's coverage acceptance criteria before any later game A/B.
