#!/usr/bin/env python3
"""Build and run the DLSS-NR loopback harness.

Reuses the already-built x64/out/OptiScaler.dll rather than rebuilding the solution: this harness
tests the NR path through the production exports, and rebuilding here would only add a second
place where the build can stall. Point --dll at another build to test that one instead.
"""
import argparse, os, shutil, signal, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
RUN = OUT / 'run'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))


def run(args, *, env=None, log=None, timeout=600):
    args = list(map(str, args))
    if log:
        with open(log, 'wb') as f:
            p = subprocess.run(args, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                               stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
    else:
        p = subprocess.run(args, cwd=ROOT, env=env, timeout=timeout)
    if p.returncode != 0:
        raise SystemExit(f'FAILED ({p.returncode}): {" ".join(args)}'
                         + (f'\n  see {log}' if log else ''))


def _cpu_ticks(pid):
    try:
        f = open(f'/proc/{pid}/stat').read().split()
        return int(f[13]) + int(f[14])
    except Exception:
        return 0


def _wine_build_pids():
    # msvc-wine currently launches Hostx64/x64/cl.exe; Windows path casing is not stable.
    out = subprocess.run(['pgrep', '-if', r'[h]ostx64.x64.(cl|link)\.exe'], capture_output=True, text=True)
    return [int(x) for x in out.stdout.split()]


def run_watched(args, *, env, log, attempts=4):
    """Run a wine-side compiler command, recovering from the msvc-wine stall.

    The stall is not MSBuild's: a bare cl.exe through the msvc-wine wrapper hangs the same way,
    every process at zero CPU while wineserver spins on a core, and it never resumes. Same shape
    as build-local.sh's watchdog, same recovery: tear it down, reset the prefix, run again."""
    args = list(map(str, args))
    for attempt in range(1, attempts + 1):
        with open(log, 'wb') as f:
            p = subprocess.Popen(args, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                 stdout=f, stderr=subprocess.STDOUT)
            dead = 0
            while p.poll() is None:
                size = Path(log).stat().st_size
                # A stalled wrapper may never spawn a compiler. Monitor it too rather than
                # making an empty compiler list disable recovery indefinitely.
                pids = _wine_build_pids() or [p.pid]
                before = sum(_cpu_ticks(q) for q in pids)
                time.sleep(20)
                after = sum(_cpu_ticks(q) for q in pids)
                grew = Path(log).stat().st_size != size
                if p.poll() is None and after == before and not grew:
                    dead += 1
                    print(f'  compile attempt {attempt}: stalled sample {dead}/3')
                else:
                    dead = 0
                if dead >= 3:
                    print(f'  compile attempt {attempt}: hung; resetting wineserver and retrying')
                    p.kill()
                    for q in _wine_build_pids():
                        try:
                            os.kill(q, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                    time.sleep(2)
                    subprocess.run(['wineserver', '-k'], env=env)
                    time.sleep(2)
                    break
            else:
                if p.returncode == 0:
                    return
                raise SystemExit(f'FAILED ({p.returncode}): {" ".join(args)}\n  see {log}')
    raise SystemExit(f'compiler hung {attempts} times; see {log}')


def check_nr_coverage():
    """Reaching EvaluateFeature is not the point; reaching the NR pass is. Without this the harness
    reports PASS while the module it exists to exercise never ran once -- the silent pass the Vulkan
    harness was built to rule out."""
    log = RUN / 'OptiScaler.log'
    if not log.exists():
        raise SystemExit('ZERO COVERAGE: OptiScaler wrote no log')
    text = log.read_text(errors='replace')
    compositions = text.count('DLSS-NR composition')
    if compositions == 0:
        for line in text.splitlines():
            if 'DLSS-NR create failed' in line or 'DLSS-NR unavailable' in line:
                print(f'  {line.strip()}')
        raise SystemExit('ZERO COVERAGE: the NR pass never composed a frame')
    print(f'  NR compositions: {compositions}')


def check_cold_coverage(harness):
    # Upscale/composition logs cannot attest to this mode. The forwarder evaluates feature 18
    # directly; demand both successful NGX returns and completed GPU submissions.
    import re
    match = re.search(r'^COLD-NR PASS: core_init=1 feature18=1 attempts=(\d+) '
                      r'successes=(\d+) gpu_completed=(\d+)$', harness, re.M)
    if not match or tuple(map(int, match.groups())) != (48, 48, 48):
        raise SystemExit('ZERO COVERAGE: cold NR did not complete 48 successful feature-18 evaluations')


def check_present_coverage(harness):
    import re
    match = re.search(r'^PRESENT-NR PASS: attempts=(\d+) successes=(\d+) presents=(\d+) '
                      r'pending_resize_drains=(\d+) controls=(\d+) capture_pairs=(\d+)$', harness, re.M)
    if not match or tuple(map(int, match.groups())) != (48, 48, 48, 2, 3, 9):
        raise SystemExit('ZERO COVERAGE: present NR did not complete all evaluations, controls and pending resizes')
    # Demand new, complete readbacks, not only successful NGX return codes or stale files.
    for generation, (w, h) in enumerate(((1280, 720), (960, 540), (1280, 720)), 1):
        for frame in (0, 1, 15):
            for side in ('before', 'after'):
                path = RUN / f'g{generation}-f{frame}-{side}.ppm'
                header = f'P6\n{w} {h}\n255\n'.encode()
                if not path.exists():
                    raise SystemExit(f'MISSING READBACK: {path}')
                data = path.read_bytes()
                if not data.startswith(header) or len(data) != len(header) + w * h * 3:
                    raise SystemExit(f'INVALID READBACK: {path}')
    print(f'  present transport coverage accepted; inspect before/after images in {RUN}')


def run_under_proton(cold_nr=False, present_nr=False, hud_ab=False, composition_ab=False, cold_size=None,
                     cold_ui=None, cold_sdk=None, cold_load_opti=False, cold_from_d3d11=False,
                     cold_siblings=False):
    """Run the harness the way a Steam game runs: through Proton, in a compatdata prefix of its own.

    Hand-mirroring what Proton provides (vkd3d-proton, dxvk-nvapi, the driver's nvngx pair and the
    NGXCore registry key) got the NR model as far as FAIL_PlatformError and then to a page fault;
    the model links NVAPI (45 NvAPI_ references) and expects the full environment. Proton's own
    script assembles that environment; nothing here reproduces it by hand."""
    steam = Path.home() / '.local/share/Steam'
    proton = next((p for p in [steam / 'steamapps/common/Proton - Experimental/proton',
                               steam / 'steamapps/common/Proton Hotfix/proton'] if p.exists()), None)
    if proton is None:
        raise SystemExit('no Proton install found under ~/.local/share/Steam/steamapps/common')
    compat = OUT / 'protonprefix'
    compat.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ,
               STEAM_COMPAT_DATA_PATH=str(compat),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam),
               # The harness LoadLibrary()s OptiScaler.dll by name, so no dxgi override is needed
               # for it; Proton keeps its own DXVK/vkd3d overrides.
               WINEDEBUG='-all,+timestamp,+pid,+tid,+seh,+unwind,+loaddll',
               # Proton does not pass the child's stdout through; the harness writes its own
               # dlssnr-loopback.log, and PROTON_LOG captures wine-side crashes into OUT.
               # Proton refuses to create its per-process log without SteamGameId. Zero
               # labels this standalone harness without opting into a game's compatibility fixes.
               SteamGameId='0', PROTON_LOG='1', PROTON_LOG_DIR=str(OUT))
    for stale in ('OptiScaler.log', 'dlssnr-loopback.log'):
        (RUN / stale).unlink(missing_ok=True)
    if present_nr:
        for stale in RUN.glob('*-f*-*.ppm'):
            stale.unlink()
        if composition_ab:
            (RUN / 'composition-report.json').unlink(missing_ok=True)
            (RUN / 'composition-vs-direct.png').unlink(missing_ok=True)
        if hud_ab:
            (RUN / 'hud-report.json').unlink(missing_ok=True)
            for stale in RUN.glob('hud-f*.png'):
                stale.unlink()
    print(f'running under {proton.parent.name}')
    p = subprocess.run([str(proton), 'run', str(RUN / 'dlssnr-loopback.exe'), 'OptiScaler.dll'] +
                       (['--present-composition-ab'] if composition_ab else ['--present-hud-ab'] if hud_ab else ['--present-nr'] if present_nr else ['--cold-nr'] if cold_nr else [])
                       + (['--cold-size', cold_size] if cold_nr and cold_size else [])
                       + (['--cold-ui-correction', cold_ui] if cold_nr and cold_ui else [])
                       + (['--cold-core-sdk', cold_sdk] if cold_nr and cold_sdk else [])
                       + (['--cold-load-optiscaler'] if cold_nr and cold_load_opti else [])
                       + (['--cold-device-from-d3d11'] if cold_nr and cold_from_d3d11 else [])
                       + (['--cold-sibling-snippets'] if cold_nr and cold_siblings else []),
                       cwd=RUN, env=env, timeout=600)
    hlog = RUN / 'dlssnr-loopback.log'
    harness = hlog.read_text(errors='replace') if hlog.exists() else ''
    if harness:
        print('--- harness log ---')
        print(harness.rstrip())

    if present_nr:
        if p.returncode != 0:
            raise SystemExit(f'present NR probe exited {p.returncode}; see {RUN / "dlssnr-loopback.log"}')
        if composition_ab:
            import re
            if not re.search(r'^COMPOSITION-AB PASS: trials=25 attempts=1200 successes=1200 presents=1200 pending_resize_drains=50 controls=75 capture_pairs=225$', harness, re.M):
                raise SystemExit('ZERO COVERAGE: incomplete composition sweep')
            run([sys.executable, HERE / 'analyze_composition.py', RUN])
        elif hud_ab:
            import re
            if not re.search(r'^HUD-AB PASS: trials=6 attempts=192 successes=192 presents=192 controls=6 capture_pairs=192$', harness, re.M):
                raise SystemExit('ZERO COVERAGE: incomplete HUD A/B')
            run([sys.executable, HERE / 'analyze_hud.py', RUN])
        else:
            check_present_coverage(harness)
        return

    if cold_nr:
        if p.returncode != 0:
            raise SystemExit(f'cold NR probe exited {p.returncode}; see {RUN / "dlssnr-loopback.log"}')
        check_cold_coverage(harness)
        return

    # A crash inside NGX shutdown is a real finding, not a broken test: everything the suite exists
    # to exercise already ran. Report it as its own failure so it is neither masked nor confused
    # with the NR pass not running. Games rarely call Shutdown1, which is why only this reaches it.
    if p.returncode != 0 and 'teardown: Shutdown' in harness and 'PASS' not in harness:
        check_nr_coverage()
        raise SystemExit('the sweep completed, then NVSDK_NGX_D3D12_Shutdown1 crashed '
                         f'(exit {p.returncode}); see artifacts/run/dlssnr-loopback.log')
    if p.returncode != 0:
        crash = sorted(OUT.glob('steam-*.log'), key=lambda f: f.stat().st_mtime)
        if crash:
            tail = [l for l in crash[-1].read_text(errors='replace').splitlines()
                    if any(k in l.lower() for k in ('page fault', 'unhandled', 'exception', 'backtrace', 'dlssnr-loopback'))]
            if tail:
                print('--- proton log ---'); print('\n'.join(tail[-12:]))
        raise SystemExit(f'harness exited {p.returncode}')
    check_nr_coverage()
    raise SystemExit(0)


def main():
    global RUN
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', default=str(ROOT / 'x64/out/OptiScaler.dll'),
                    help='OptiScaler build whose NGX exports are driven')
    ap.add_argument('--runtime', choices=('proton', 'wine'), default='proton',
                    help='proton (default): run under the local Proton, which sets up DXVK, '
                         'vkd3d-proton, dxvk-nvapi and the NGXCore registry exactly as a game '
                         'gets them. wine: bare prefix with only vkd3d-proton borrowed; reaches '
                         'NR but its feature create fails with FAIL_PlatformError.')
    ap.add_argument('--cold-size', default=None, metavar='WxH',
                    help="with --cold-nr: the size to create and evaluate at (default 640x360). "
                         "The production present host was refused at 3440x1440 on metadata this mode "
                         "proves accepted at 640x360, so the size is a variable now, not a constant.")
    ap.add_argument('--cold-ui-correction', default=None, metavar='N',
                    help='with --cold-nr: the UI-correction argument to create with (default 0). '
                         'Production passes 1.')
    ap.add_argument('--cold-core-sdk', default=None, metavar='N',
                    help='with --cold-nr: the SDK version to initialise the CORE with (default 0). '
                         'Production passes NVSDK_NGX_Version_API (0x15) whenever no game NGX init '
                         'populated State, which is exactly the no-upscaler case.')
    ap.add_argument('--cold-load-optiscaler', action='store_true',
                    help="with --cold-nr: LoadLibrary the production OptiScaler.dll before the cold "
                         "sequence and then leave it alone. Puts this process's one remaining "
                         "difference from production -- OptiScaler's hooks, installed from DllMain -- "
                         "into the control that passes. Nothing is called on it.")
    ap.add_argument('--cold-device-from-d3d11', action='store_true',
                    help="with --cold-nr: build the D3D12 device the way production does -- a D3D11 "
                         "device, its adapter through IDXGIDevice, D3D12CreateDevice on that at "
                         "feature level 11_0 -- instead of enumerating an adapter and asking for 12_0.")
    ap.add_argument('--cold-sibling-snippets', action='store_true',
                    help="with --cold-nr: load nvngx_dlss/dlssd/dlssg beside the NR snippet, as a game "
                         "folder has them and OptiScaler loads them. The identity diff found this to be "
                         "the only named difference left between this process and production.")
    ap.add_argument('--skip-build', action='store_true',
                    help='reuse the selected mode\'s artifacts/{run,cold-run,present-run}/dlssnr-loopback.exe')
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--cold-nr', action='store_true',
                    help='probe core cold defaults and NR feature 18 through the production forwarder; '
                         'no OptiScaler Init, DLSS or RR feature; isolated artifacts/cold-run')
    modes.add_argument('--present-nr', action='store_true',
                       help='controlled SDR swapchain, NR, fence-drained resize and before/after readback; '
                            'isolated artifacts/present-run; requires Proton')
    experiments = ap.add_mutually_exclusive_group()
    experiments.add_argument('--hud-ab', action='store_true', help='with --present-nr: paired HUD/UICorrection experiment')
    experiments.add_argument('--composition-ab', action='store_true', help='with --present-nr: direct/composed and strength sweep')
    args = ap.parse_args()
    if args.composition_ab and not args.present_nr:
        ap.error('--composition-ab requires --present-nr')
    if args.hud_ab and not args.present_nr:
        ap.error('--hud-ab requires --present-nr')
    if args.present_nr and args.runtime != 'proton':
        ap.error('--present-nr requires --runtime proton')
    if args.hud_ab or args.composition_ab:
        import importlib.util
        for module in ('numpy', 'PIL'):
            if importlib.util.find_spec(module) is None:
                ap.error('image analysis requires numpy and Pillow in the runner Python environment')
    standalone = args.cold_nr or args.present_nr
    if args.present_nr:
        RUN = OUT / ('composition-run' if args.composition_ab else 'hud-run' if args.hud_ab else 'present-run')
    if args.cold_nr:
        RUN = OUT / 'cold-run'

    dll = Path(args.dll)
    if not dll.exists():
        raise SystemExit(f'no OptiScaler build at {dll} — run ./build-local.sh Release first')

    OUT.mkdir(exist_ok=True)
    RUN.mkdir(exist_ok=True)
    # Cold NR does not load OptiScaler, which is the point of it -- unless it is being asked to, in
    # which case the DLL has to be next to the harness like it is next to a game.
    if not standalone or args.cold_load_optiscaler:
        shutil.copy2(dll, RUN / 'OptiScaler.dll')

    if args.cold_load_optiscaler:
        # Written here rather than documented, because the first run without it looked like the
        # failure under investigation: the process died right after the DLL loaded, before it could
        # create a device, and the runner reported ZERO COVERAGE. That was the startup update check
        # going to the network in a headless Proton prefix, not a refusal by anything.
        #
        # Only things that make a small diagnostic process behave like one. None of it touches how
        # D3D12 or DXGI objects are created, which is the axis this flag exists to test.
        (RUN / 'OptiScaler.ini').write_text(
            '; Written by run.py for --cold-load-optiscaler. Not a user config.\n'
            '[Log]\nLogToFile=true\nLogLevel=2\n\n'
            '[Menu]\nOverlayMenu=false\n\n'
            '[Hotfix]\nCheckForUpdate=false\n')

    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all')
    if not args.skip_build:
      # Compiling goes through the same msvc-wine prefix that build-local.sh uses. Two clients on
      # one wineserver is the condition under which the build has stalled; refuse rather than race.
      # Running the built harness does not touch that prefix, so --skip-build is exempt.
      busy = subprocess.run(['pgrep', '-f', r'[M]SBuild\.exe|[b]uild-local\.sh'], capture_output=True)
      if busy.returncode == 0:
          raise SystemExit('a solution build is using the msvc-wine prefix; compile after it finishes, or --skip-build')
      run_watched([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4',
         '/I' + str(ROOT / 'external/nvngx_dlss_sdk'), HERE / 'harness.cpp',
         '/Fo' + str(OUT / 'harness.obj'),
         '/Fe' + str(RUN / 'dlssnr-loopback.exe'),
         '/link', 'd3d12.lib', 'd3d11.lib', 'dxgi.lib', 'user32.lib'],
        env=dict(env, WINE_MSVC_RAW_STDOUT='1'), log=OUT / 'build.log')
    if not (RUN / 'dlssnr-loopback.exe').exists():
        raise SystemExit('no harness binary; run without --skip-build first')

    # NR needs the forwarder beside the DLL (the NVIDIA snippet rejects callers whose module path
    # lacks "nvngx.dll") and the model itself. Both come from the local kit, symlinked rather than
    # copied: the model is 165 MB and this directory is disposable.
    kit = Path.home() / '.local/opt/dlssnr-kit'
    # A known-good install also supplies the DLSS model and the Streamline set. Whether NR needs
    # them is exactly what this harness is for, so link whatever is there and let the log say.
    donor = Path(os.environ.get(
        'LOOPBACK_DONOR',
        str(Path.home() / '.local/share/Steam/steamapps/common/Crimson Desert/bin64')))
    if donor.is_dir() and not standalone:
        for src in sorted(donor.glob('sl.*.dll')) + [donor / 'nvngx_dlss.dll']:
            if src.exists():
                link = RUN / src.name
                link.unlink(missing_ok=True)
                link.symlink_to(src)
    names = ['nvngx.dll_dlssnr.dll', 'nvngx_dlssnr.dll', '_nvngx.dll']
    if args.cold_sibling_snippets:
        # The other feature snippets a game folder carries. They are only staged so that
        # LoadLibrary can find them; nothing calls into them, which is also true in production.
        names += ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']

    for name in names:
        src = kit / name
        # Cold bring-up should test the installed driver, not a possibly stale kit copy.
        # Leave the legacy sweep's dependency selection unchanged.
        installed_core = Path('/usr/lib/nvidia/wine/_nvngx.dll')
        if standalone and name == '_nvngx.dll' and installed_core.exists():
            src = installed_core
        # Test the forwarder built alongside this DLL. The kit remains a fallback
        # for external builds that do not ship their matching forwarder.
        if name == 'nvngx.dll_dlssnr.dll' and (dll.parent / name).exists():
            src = (dll.parent / name).resolve()
        siblings = ('nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll')
        if not src.exists() and name in siblings:
            # The kit may not carry them; a game folder does. Fall back to the one we know has them.
            game = Path.home() / ('.local/share/Steam/steamapps/common/Divinity Original Sin 2/DefEd/bin') / name
            if game.exists():
                src = game

        if not src.exists():
            if standalone and name not in siblings:
                raise SystemExit(f'SETUP FAILURE: missing cold NR dependency {src}; no NGX result')
            if name in siblings:
                print(f'  warning: {name} not found; the sibling-snippet axis is incomplete')
                continue
            print(f'  warning: {name} not in {kit} — NR will report itself unavailable')
            continue
        link = RUN / name
        link.unlink(missing_ok=True)
        link.symlink_to(src)

    # OverlayMenu=false selects the menu drawn inside upscaler Evaluate, rather than
    # disabling the menu. Keep the swapchain-overlay route selected: this harness has
    # no swapchain, so it does not initialize unrelated ImGui rendering during the sweep.
    # The harness only reads NR behaviour; it never writes into a game install.
    if not standalone:
        (RUN / 'OptiScaler.ini').write_text(
            '[Menu]\nOverlayMenu=true\n[DlssNr]\nEnabled=true\n[Log]\nLogToFile=true\nLogLevel=2\n')

    if args.runtime == 'proton':
        return run_under_proton(args.cold_nr, args.present_nr, args.hud_ab, args.composition_ab, args.cold_size,
                                args.cold_ui_correction, args.cold_core_sdk, args.cold_load_optiscaler,
                                args.cold_device_from_d3d11, args.cold_sibling_snippets)

    # A separate runtime prefix: the compiler prefix is configured for MSVC, not for graphics, and
    # running the app there conflates "the harness is wrong" with "this prefix has no D3D12".
    runtime = OUT / 'wineprefix'
    renv = dict(os.environ, WINEPREFIX=str(runtime), WINEDEBUG='-all',
                WINEDLLOVERRIDES='d3d12,d3d12core=n')
    if not runtime.exists():
        runtime.mkdir(parents=True)
        run(['wineboot', '-i'], env=renv, log=OUT / 'wineboot.log', timeout=300)

    # A bare Wine prefix has no working D3D12: the adapter enumerates but device creation returns
    # E_INVALIDARG. Games work because Proton ships vkd3d-proton, so borrow those two DLLs and
    # override them to native. Copied, never linked, and only into this disposable prefix.
    vkd3d = next((p for p in [
        Path.home() / '.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows',
        Path.home() / '.local/share/Steam/steamapps/common/Proton Hotfix/files/lib/wine/vkd3d-proton/x86_64-windows',
    ] if p.is_dir()), None)
    if vkd3d is None:
        raise SystemExit('vkd3d-proton not found in any Proton install; D3D12 will not initialise')
    system32 = runtime / 'drive_c/windows/system32'
    for name in ('d3d12.dll', 'd3d12core.dll'):
        target = system32 / name
        # Wine leaves its own copies read-only; replace rather than write over them.
        target.unlink(missing_ok=True)
        shutil.copy2(vkd3d / name, target)
    print(f'vkd3d-proton from {vkd3d.parents[2].parents[1].name}')

    print(f'running against {dll}')
    if args.cold_nr:
        (RUN / 'dlssnr-loopback.log').unlink(missing_ok=True)
    p = subprocess.run(['wine', str(RUN / 'dlssnr-loopback.exe'), 'OptiScaler.dll'] +
                       (['--cold-nr'] if args.cold_nr else []),
                       cwd=RUN, env=renv, timeout=300)
    if p.returncode != 0:
        raise SystemExit(p.returncode)
    if args.cold_nr:
        check_cold_coverage((RUN / 'dlssnr-loopback.log').read_text(errors='replace'))
    else:
        check_nr_coverage()
    raise SystemExit(0)


if __name__ == '__main__':
    main()
