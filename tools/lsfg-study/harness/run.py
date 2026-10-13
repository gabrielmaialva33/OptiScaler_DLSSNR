#!/usr/bin/env python3
"""Build/run the standalone LSFG D3D12 study. All artifacts live outside git."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT = Path.home() / 'Projects/personal/OptiScaler_DLSSNR/Tmp/lsfg-study'
MSVC = Path.home() / '.local/opt/msvc/bin/x64'
PREFIX = Path.home() / '.local/opt/msvc-wineprefix'


def idle():
    result = subprocess.run(['pgrep', '-af', r'[M]SBuild|[c]l\.exe|[d]xc\.exe'], capture_output=True, text=True)
    if result.returncode != 1:
        raise RuntimeError('compiler/prefix busy (retry later):\n' + result.stdout)


def command(args, env, log, cwd, timeout):
    idle()
    print('+', ' '.join(map(str, args)), flush=True)
    # Never pipe Wine output: wineserver may retain pipe handles after the client exits.
    with log.open('w') as stream:
        try:
            result = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=stream,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['wineserver', '-k'], env=env, check=False)
            raise RuntimeError(f'timed out; stopped owned Wine prefix; see {log}')
    if result.returncode:
        raise RuntimeError(f'exit {result.returncode}: {log}\n' + log.read_text(errors='replace')[-6000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--build-only', action='store_true')
    modes.add_argument('--run-only', action='store_true')
    ap.add_argument('--artifacts', type=Path, default=DEFAULT)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--multiplier', type=int, choices=(2, 3), default=2)
    ap.add_argument('--fp16', action='store_true', help='select original FP16 shader variants (+48)')
    ap.add_argument('--performance', action='store_true')
    ap.add_argument('--flow-scale', type=float, default=1.0)
    ap.add_argument('--benchmark', action='store_true')
    ap.add_argument('--width', type=int, default=1920)
    ap.add_argument('--height', type=int, default=1080)
    ap.add_argument('--iterations', type=int, default=400)
    ap.add_argument('--warmup', type=int, default=100)
    ap.add_argument('--debug', action='store_true', help='require D3D12 debug layer')
    ap.add_argument('--gpu-validation', action='store_true', help='require GPU based validation')
    ap.add_argument('inputs', nargs='*', type=Path, help='one folder of numbered PNGs or two PNG paths')
    args = ap.parse_args()
    base = args.artifacts.resolve()
    work = base / 'harness'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    if not args.run_only:
        command([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/O2',
                 '/DUNICODE', '/D_UNICODE', HERE / 'main.cpp', '/Fo' + str(work / 'main.obj'),
                 '/Fe' + str(work / 'lsfg-harness.exe'), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib',
                 'windowscodecs.lib', 'ole32.lib'], env, work / 'build.log', work, 180)
        candidates = [Path.home() / '.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows',
                      Path.home() / '.local/share/Steam/steamapps/common/Proton Hotfix/files/lib/wine/vkd3d-proton/x86_64-windows']
        candidates += sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('GE-Proton*/files/lib/wine/vkd3d-proton/x86_64-windows'), reverse=True)
        runtime = next(p for p in candidates if (p / 'd3d12.dll').exists())
        files = [runtime / name for name in ('d3d12.dll', 'd3d12core.dll')]
        files += list(sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))[-1].glob('*.dll'))
        files += [ROOT / 'OptiScaler/shaders/shader_tools/dxcompiler.dll']
        for src in files:
            dst = work / src.name
            dst.unlink(missing_ok=True)
            shutil.copy2(src, dst)
    if args.build_only:
        return
    if not args.benchmark and len(args.inputs) not in (1, 2):
        ap.error('supply a folder or two PNG paths')
    output = (args.output or base / 'out').resolve()
    output.mkdir(parents=True, exist_ok=True)
    win = lambda p: 'Z:' + str(p.resolve())
    cmd = ['wine', work / 'lsfg-harness.exe', '--shaders', win(base / 'dxil'), '--out', win(output),
           '--multiplier', str(args.multiplier), '--flow-scale', str(args.flow_scale)]
    if args.benchmark:
        for name in ('width', 'height', 'iterations', 'warmup'):
            cmd += ['--' + name, str(getattr(args, name))]
    for flag in ('fp16', 'performance', 'benchmark', 'debug', 'gpu_validation'):
        if getattr(args, flag):
            cmd += ['--' + flag.replace('_', '-')]
    cmd += list(map(win, args.inputs))
    env['WINEDLLOVERRIDES'] = 'd3d12=n;d3d12core=n;dxcompiler=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n'
    command(cmd, env, output / 'run.log', work, 300)
    print((output / 'run.log').read_text(errors='replace'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, StopIteration, subprocess.SubprocessError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
