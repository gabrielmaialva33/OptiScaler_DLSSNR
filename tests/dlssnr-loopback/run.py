#!/usr/bin/env python3
"""Build and run the DLSS-NR loopback harness.

Reuses the already-built x64/out/OptiScaler.dll rather than rebuilding the solution: this harness
tests the NR path through the production exports, and rebuilding here would only add a second
place where the build can stall. Point --dll at another build to test that one instead.
"""
import argparse, os, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
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


def check_nr_coverage():
    """Reaching EvaluateFeature is not the point; reaching the NR pass is. Without this the harness
    reports PASS while the module it exists to exercise never ran once -- the silent pass the Vulkan
    harness was built to rule out."""
    log = OUT / 'run/OptiScaler.log'
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


def run_under_proton():
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
               WINEDEBUG='-all',
               PROTON_LOG='0')
    (OUT / 'run/OptiScaler.log').unlink(missing_ok=True)
    print(f'running under {proton.parent.name}')
    p = subprocess.run([str(proton), 'run', str(OUT / 'run/dlssnr-loopback.exe'), 'OptiScaler.dll'],
                       cwd=OUT / 'run', env=env, timeout=600)
    if p.returncode != 0:
        raise SystemExit(p.returncode)
    check_nr_coverage()
    raise SystemExit(0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', default=str(ROOT / 'x64/out/OptiScaler.dll'),
                    help='OptiScaler build whose NGX exports are driven')
    ap.add_argument('--runtime', choices=('proton', 'wine'), default='proton',
                    help='proton (default): run under the local Proton, which sets up DXVK, '
                         'vkd3d-proton, dxvk-nvapi and the NGXCore registry exactly as a game '
                         'gets them. wine: bare prefix with only vkd3d-proton borrowed; reaches '
                         'NR but its feature create fails with FAIL_PlatformError.')
    ap.add_argument('--skip-build', action='store_true',
                    help='reuse artifacts/run/dlssnr-loopback.exe instead of compiling')
    args = ap.parse_args()

    dll = Path(args.dll)
    if not dll.exists():
        raise SystemExit(f'no OptiScaler build at {dll} — run ./build-local.sh Release first')

    OUT.mkdir(exist_ok=True)
    (OUT / 'run').mkdir(exist_ok=True)
    shutil.copy2(dll, OUT / 'run/OptiScaler.dll')

    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all')
    if not args.skip_build:
      # Compiling goes through the same msvc-wine prefix that build-local.sh uses. Two clients on
      # one wineserver is the condition under which the build has stalled; refuse rather than race.
      # Running the built harness does not touch that prefix, so --skip-build is exempt.
      busy = subprocess.run(['pgrep', '-f', r'[M]SBuild\.exe|[b]uild-local\.sh'], capture_output=True)
      if busy.returncode == 0:
          raise SystemExit('a solution build is using the msvc-wine prefix; compile after it finishes, or --skip-build')
      run([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4',
         '/I' + str(ROOT / 'external/nvngx_dlss_sdk'), HERE / 'harness.cpp',
         '/Fo' + str(OUT / 'harness.obj'),
         '/Fe' + str(OUT / 'run/dlssnr-loopback.exe'),
         '/link', 'd3d12.lib', 'dxgi.lib', 'user32.lib'],
        env=dict(env, WINE_MSVC_RAW_STDOUT='1'), log=OUT / 'build.log')
    if not (OUT / 'run/dlssnr-loopback.exe').exists():
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
    if donor.is_dir():
        for src in sorted(donor.glob('sl.*.dll')) + [donor / 'nvngx_dlss.dll']:
            if src.exists():
                link = OUT / 'run' / src.name
                link.unlink(missing_ok=True)
                link.symlink_to(src)
    for name in ('nvngx.dll_dlssnr.dll', 'nvngx_dlssnr.dll', '_nvngx.dll'):
        src = kit / name
        if not src.exists():
            print(f'  warning: {name} not in {kit} — NR will report itself unavailable')
            continue
        link = OUT / 'run' / name
        link.unlink(missing_ok=True)
        link.symlink_to(src)

    # The harness only reads NR behaviour; it never writes into a game install.
    (OUT / 'run/OptiScaler.ini').write_text(
        '[Menu]\nOverlayMenu=false\n[DlssNr]\nEnabled=true\n[Log]\nLogToFile=true\nLogLevel=2\n')

    if args.runtime == 'proton':
        return run_under_proton()

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
    p = subprocess.run(['wine', str(OUT / 'run/dlssnr-loopback.exe'), 'OptiScaler.dll'],
                       cwd=OUT / 'run', env=renv, timeout=300)
    if p.returncode != 0:
        raise SystemExit(p.returncode)
    check_nr_coverage()
    raise SystemExit(0)


if __name__ == '__main__':
    main()
