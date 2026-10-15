# Disposable native NGX probe

Step 0 items 1–2 of [native-linux-route.md](../../../docs/dlssnr/design/native-linux-route.md).
This is an explicitly invoked Linux x86-64 GPU experiment, not a CI/host test or a Retoque backend.
It lives in `tools/probes/` so adding it cannot make CI execute GPU code or fetch proprietary models.
Nothing under `OptiScaler/` or the production runtime is changed.

```bash
python3 tools/probes/native-ngx/run.py --run
```

Requires host `g++`, Vulkan loader, the already-present `external/vulkan/include` headers,
`vulkaninfo`, `nvidia-smi`, `strace`, and Python 3.11+. It deliberately selects RTX 4090.
No Wine, MSVC or prefix access. No game may be mapped under `steamapps/common`; the runner checks
`/proc/[0-9]*/maps` before inventory and before the probe, and exits if any match is visible.
As with the prescribed grep check, processes inaccessible to this user cannot be inspected.

SDK pin: NVIDIA/DLSS `374959484e79a640feaba44c93ac8cfb0a03f5b5`. The runner fetches public headers,
the official Linux `libnvsdk_ngx.a` API wrapper and release SR model `libnvidia-ngx-dlss.so.310.9.1`
to `~/.cache/retoque-native-ngx/<commit>/`. It verifies the archive/model SHA-256 and records all
SDK-file hashes. No NVIDIA file or binary is vendored in this repository. The public wrapper
loads the driver's native core; the probe does not call a guessed core ABI or invent a vtable.
The wrapper archive is required: the driver's exported names are not a substitute for the SDK ABI.

Each execution gets a fresh `run-<UTC timestamp>/` containing the ELF, compiler command/log,
packages and clocks/power inventory, native Wayland/X11 surface enumeration, Vulkan capability
inventory, NGX callback log, full file/process/network strace, model discovery messages, opened
paths and a SHA-256 manifest. Clock/power configuration is not changed. The probe prints device
and driver UUIDs, queue families, heap budgets, exact optimal-tiling external-image tuples and
binary/timeline external-semaphore capabilities; these queries do not perform an import/export.

The process uses an isolated cache-local NGX configuration with updater disabled and leaves
signature checking enabled. **That configuration alone is insufficient to prevent the core from
executing the updater.** Before its first SDK call the probe installs an inherited process-local
seccomp filter denying `execve`/`execveat` (including alternate-ABI bypass), with a negative control
requiring `EPERM`. It exits if the guard cannot be installed. This is not a patch to NGX, a system
configuration change or signature bypass. The strace should show updater exec attempts denied;
the runner rejects a successful updater exec. The initial, unguarded investigation accidentally
ran four updater processes despite the disabled setting; all exited 1 with no file-mutating
operations observed for those processes. See the measured note for that incident and limitations.

SR control: own project ID, SDK 0x15, public extension queries, a graphics+compute queue,
640×360 → 1280×720 SR creation using the SDK helper, then submit and prove feature-initialization
fence completion before release. This is a **create/initialization control**, not an SR quality or
evaluate test. Capability getters report their own result separately from the returned value.

NR diagnostic: feature 18 through the public generic API, using the capability object and existing
Retoque NR enable/width/height keys. The public SDK has no documented complete native NR parameter
contract. A missing-model failure occurs before an NR model can validate those parameters; this
probe does not claim that a loaded native model would accept them. No inference runs when creation
fails. If creation succeeds, exit 3 explicitly requests the deterministic evaluate/identity/fence/
pixel-diff follow-up before any go decision; success is not silently inferred from a feature handle.

Exit 0: SR control completed and NR did not create. Exit 2: setup/positive control invalid; do not
interpret as an NR no-go. Exit 3: NR created, evaluation follow-up required. Timeout or Vulkan
failure exits without destroying possibly pending resources; process teardown owns that cleanup.
No layer, helper transport, game injection or updater download is implemented.

Format `probe.cc` with clang-format 22 only. This standalone `.cc` uses Linux/public SDK headers,
not the Windows DLL's precompiled header. A host regression suite is intentionally not added.
