# NR native D3D11 guides (host)

```bash
python3 tests/nr-dx11-native-guides/run.py
python3 tests/run_all.py --only nr-dx11-native-guides -v
```

Step 3 of `docs/dlssnr/design/dx11-native-dlss-guides.md`, option (c): a D3D11 game that keeps the driver's own
D3D11 DLSS (`Dx11Upscaler=dlss`) has its depth and motion copied, right after each evaluate, into the D3D11 bridge's
shared textures, waited for on the bridge's D3D12 queue, and copied on that queue into the present guide ring, where
the bridge's present host takes them through the identity rules of `present-real-guides.md`. Under g++ C++20,
ASan/UBSan and `-Werror`, the runner does two things.

1. **Source guards** on the transport (`retoque/dlssnr/DlssNr_Dx11Guides.cpp`), its one call site
   (`NoteDx11Evaluate` in `NVNGX_DLSS_Dx11.cpp`, through `DlssNr_Dx11Upscaler.h`), the capture core it shares with
   the D3D12 route (`CaptureBridgedGuides` and `GatherGuideFrame` in `DlssNr_Dx12.cpp`), the present host's half
   (the presented size published in `CapturedPresentGuides`) and shutdown.
2. **`decide.cpp`** compiles the production header `retoque/dlssnr/DlssNr_Dx11Guides.h`.

## What this covers

- **Inert off its route**: NR off, HookMethod other than 2, an upscaler other than native DLSS, or no bridge present
  host answers `NotWanted` whatever else is true, and the source guard holds that the transport looks at nothing --
  not the context, the parameters, the bridge or its own state -- before that answer.
- **Every refusal alone, and in order**: the evaluate failed, a deferred context, no depth or motion (absent, or not a
  2D texture), a shape the copy would not carry (multisampled, an array, no mips; mips are fine), no output, and an
  output that is not the size the bridge presents (another view of the game, as Palworld's 1691x1356 and 3396x1356
  evaluates beside its 3440x1440 one); unknown before the bridge's first present, where the identity's rule 2
  decides. Mending them one at a time walks the list to `Carried`.
- **The slot rotation** against a simulated GPU that completes lists late: over 100000 frames a set is written only
  once the list that last read it has completed, and a set still in flight is refused, never waited for. It checks
  `SlotFree` as `CarryLocked` uses it; that the transport's sets are upstream's cache sets is a `static_assert` in the
  transport, held here by a source guard.
- **A stale cache** (`CacheFree`): upstream's prepare releases every set of a cache a device change made stale, so it
  is prepared only once nothing the transport submitted is in flight.
- **The missing host** (`NoHost`): NR on, HookMethod 2 and native DLSS with no bridge hosting the pass, exactly, and
  its one line.
- **Which copy runs a shader** (`RunsShader`): only R24G8 and R32G8X24 typeless over legacy handles reach upstream's
  `DepthTransfer_Dx11`, the one step whose bindings are saved and restored.
- **The window and its line**: the exact line, the 2 s / 30 s cadence, `NotWanted` never counted, a window without a
  carry showing the last refused evaluate's guides instead of a carried one's.
- **Source guards** on the order that keeps D3D11 off D3D12 and nothing written or freed under the GPU: the slot check
  before any write, the stale-cache check after the bridge's `Init` and before any prepare, the own depth copy
  rebuilt for another D3D11 device; around upstream's depth conversion only, the compute shader, shader resources
  and unordered-access views and the whole output merger saved and put back with append counters kept, and no
  constant buffer or sampler round trip; `DepthTransfer_Dx11::Dispatch` held to the calls that assumes, and
  upstream's transport to copies, signals and waits on the context;
  depth and motion only, through upstream's transport with its D3D11-to-D3D12 sync; a texture the game shares already
  refused (it would be read live); the shared copies in `COMMON` across the APIs; close before execute, a list that
  captured nothing not executed and abandoned; the set marked busy before the signal; no wait, flush or event on the
  carry, and none on a device change (old objects freed only if already complete); a failed build not retried on
  every evaluate; a bounded drain only at shutdown, which runs before the locks the capture takes after its own
  (`DlssNr::Shutdown` has no caller in the DLL today, so that path is held by the guard, not by a run).

## What this does not cover

No D3D11, no D3D12, no GPU, no game. Whether DXVK shares a given depth or motion format with vkd3d-proton and whether
the copy lands are `tests/dx11-guides-transport`'s (wine tier, the API sequence on real devices under Proton); whether
the present pass's picture is right only a run shows. The present host's use of the capture is
`tests/nr-present-guide-identity`'s: the ring, the stamps and the identity rules are the same for every producer.
