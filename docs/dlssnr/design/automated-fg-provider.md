# Automated production DLSS-G provider regression

The existing FG input fixture executes production slices with a recording consumer. That is useful for
camera and crop decisions, but cannot prove that the production DLL starts Streamline, builds a DLSS-G
swapchain, or presents interpolated frames. A synthetic D3D12 application can exercise that boundary
without starting a game.

## Boundary and scene

`tests/fg-provider-d3d12` loads the complete, previously built OptiScaler DLL before creating the D3D12
device or DXGI factory. It uses a separate game-style Streamline interposer to submit frame tokens,
constants, depth, motion and HUDless colour through the same intercepted public calls as a game.
OptiScaler owns the actual Streamline/DLSS-G output and its swapchain hooks. No production source is
sliced into the application and no backend is replaced by a fake.

Native NGX passthrough also requires an existing local `nvngx_dlss.dll`: the production DLL disables
all native NGX when that SR asset is absent, including FG. The fixture stages that asset without
evaluating an SR feature.

The scene is a deterministic horizontal pan of a coloured checker plane, uploaded to real textures.
Depth is a finite perspective plane; motion is current-to-previous pixel displacement; the camera
is a valid fixed perspective camera. The plane moves while the camera stays fixed. Every submission
uses a real command queue fence. Each phase reserves 32 warmup frames, covering the production 22-frame activation debounce.
The sequence exercises initial enable, game-requested disable,
re-enable, and resize with resource recreation. Input textures remain alive until the presenting
queue and the plugin's input-processing fence have completed.

## Oracle

Successful application presents alone are insufficient. The application reads `slDLSSGGetState`
directly from the actual loaded output plugin using its PE export directory and
`slGetPluginFunction`. This deliberately bypasses OptiScaler's game-facing synthetic GetState.
The function address must belong to that actual plugin. Each active phase must observe status OK,
completed GPU work, and more plugin-reported actual presents than application presents, with a
minimum surplus of 16 frames. The disabled phase requires exactly one plugin-reported actual present per application present after
its warmup; zero observations cannot pass.
Missing plugin functions, status errors, zero generated-frame surplus, hung fences and process
failure are failures. The runner retains counters, DLL hashes, source hashes and logs.

This is a runtime regression gate for the native x2 path. Higher ratios are not assumed to be supported
on Ada. A passing run does not measure perceptual quality, input latency, display scanout, Windows
D3D12 validation or the behaviour of every game's resources. Menu/input handling and other graphics
APIs remain separate coverage.

## Prerequisites and isolation

Use an already installed local NVIDIA/Streamline kit supplied by the user. Never download a model.
Stage all assets into the suite's ignored artifacts directory, never a game directory. Compile under
the shared MSVC prefix lock; run through an installed Proton in a dedicated compatibility prefix.
Refuse an active build or game before prefix/GPU operations. Missing prerequisites produce explicit
SKIP with no PASS; strict mode turns those skips into failure. Runtime failures never become skips.
The runner must not reset the shared compiler prefix or kill unrelated Wine processes.
