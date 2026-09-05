#!/usr/bin/env python3
"""Build/run real D3D12 timing in an isolated Wine prefix; never loads NGX or a game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path.home() / '.local/opt/msvc-wineprefix'
RUNTIME = Path.home() / '.local/share/Steam/compatibilitytools.d/GE-Proton11-6-x86_64/files/lib/wine/vkd3d-proton/x86_64-windows'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    files = list(HERE.glob('*.cpp')) + list(HERE.glob('*.py')) + list(HERE.glob('stubs/**/*.h'))
    files += [ROOT / 'OptiScaler/dlssnr' / name for name in (
        'DlssNr_GpuTiming.cpp', 'DlssNr_GpuTiming.h', 'DlssNr_GpuTimingModel.h',
        'DlssNr_Submission.cpp', 'DlssNr_Submission.h', 'DlssNr_SubmissionModel.h')]
    return {str(path.relative_to(ROOT)): digest(path) for path in files}


def command(args, *, env, logfile, cwd=ROOT, timeout=300, expected=0):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        process = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=timeout)
    if process.returncode != expected:
        raise RuntimeError(f'exit {process.returncode}, expected {expected}: {logfile}\n'
                           + logfile.read_text(errors='replace')[-5000:])


def build():
    build_prefix = OUT / 'build-prefix'
    if not build_prefix.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(build_prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(build_prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    # This prefix belongs only to this harness. Do not stop the solution compiler or game prefixes.
    subprocess.run(['wineserver', '-k'], env=env, check=False)
    work = OUT / 'run'
    work.mkdir(exist_ok=True)
    command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/Od',
             '/I' + str(HERE / 'stubs'), '/I' + str(ROOT / 'OptiScaler/dlssnr'),
             '/I' + str(ROOT / 'OptiScaler/include'), HERE / 'harness.cpp',
             ROOT / 'OptiScaler/dlssnr/DlssNr_GpuTiming.cpp', ROOT / 'OptiScaler/dlssnr/DlssNr_Submission.cpp',
             '/Fo' + str(OUT) + '/', '/Fe' + str(work / 'nr-timing-harness.exe'), '/link',
             '/LIBPATH:' + str(ROOT / 'OptiScaler/library/detours'), 'detours.lib', 'd3d12.lib',
             'dxgi.lib', 'dxguid.lib'], env=env, logfile=OUT / 'build.log', timeout=600)
    for name in ('d3d12.dll', 'd3d12core.dll'):
        shutil.copy2(RUNTIME / name, work / name)
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime missing')
    for dll in redists[-1].glob('*.dll'):
        shutil.copy2(dll, work / dll.name)
    (OUT / 'manifest.json').write_text(json.dumps({
        'sources': sources(),
        'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'binaries': {path.name: digest(path) for path in work.iterdir() if path.suffix in ('.dll', '.exe')},
    }, indent=2) + '\n')


def execute():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    if manifest['sources'] != sources():
        raise RuntimeError('source changed after harness build; rebuild required')
    work = OUT / 'run'
    for name, checksum in manifest['binaries'].items():
        if digest(work / name) != checksum:
            raise RuntimeError('binary changed after build: ' + name)
    if any(work.glob('*ngx*.dll')) or any(work.glob('sl.*')) or any(work.glob('optiscaler_skip*')):
        raise RuntimeError('unexpected NGX/Streamline/marker in isolated harness; refusing execution')
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='d3d12=n;d3d12core=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n',
               VKD3D_DEBUG='warn', VK_LOADER_LAYERS_DISABLE='~implicit~')
    command(['wine', work / 'nr-timing-harness.exe'], env=env, cwd=work, logfile=OUT / 'run.log', timeout=120)
    if 'PASS: OFF, normal, replay same queue, replay second queue, Reset-before-GPU;' not in (OUT / 'run.log').read_text():
        raise RuntimeError('missing explicit mandatory-case PASS')
    command(['wine', work / 'nr-timing-harness.exe', '--zero-coverage'], env=env, cwd=work,
            logfile=OUT / 'negative.log', timeout=60, expected=1)
    if 'ZERO COVERAGE:' not in (OUT / 'negative.log').read_text():
        raise RuntimeError('negative control did not fail on zero coverage')
    (OUT / 'result.json').write_text(json.dumps({'status': 'PASS', 'manifest': manifest,
        'limits': 'Real D3D12 copies, timing and submission core; ResTrack shim. No NGX, production ResTrack or game.'}, indent=2) + '\n')
    print((OUT / 'run.log').read_text(errors='replace')[-3000:])
    print('PASS: zero-coverage negative control rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    args = parser.parse_args()
    if args.build_only and args.run_only:
        parser.error('choose at most one mode')
    OUT.mkdir(exist_ok=True)
    (OUT / 'result.json').write_text('{"status":"RUNNING"}\n')
    if not args.run_only:
        build()
    if not args.build_only:
        execute()


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyError, ValueError) as error:
        OUT.mkdir(exist_ok=True)
        (OUT / 'result.json').write_text(json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
