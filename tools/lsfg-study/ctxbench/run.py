#!/usr/bin/env python3
"""Build and run the LSFG context bench (main.cpp) under the msvc-wine prefix and vkd3d-proton.

It drives retoque/lsfg/LsfgFrameGen_Dx12.cpp as FSR-FG's callback does, prints one hash per generated frame of a
moving sequence (compare two builds or two schedules byte for byte) and the context's own GPU time per generated
frame. Artifacts go to ~/.cache/lsfg-ctxbench/<name>/, never into the worktree.

  # this worktree's code; one build per name
  python3 tools/lsfg-study/ctxbench/run.py --name head -- --schedule ab
  # an older revision's retoque/lsfg, for an identity check against it (its Record has no schedule argument)
  python3 tools/lsfg-study/ctxbench/run.py --name base --baseline 0ba40b6a
  # run only, extra arguments go to the bench: --width --height --flow --performance --frames --iterations
  # --warmup --schedule waves|perpass|ab --output-copy --bgra --dump N
  # where the time goes, wave by wave (a build of its own: the timestamps are compiled in only here)
  python3 tools/lsfg-study/ctxbench/run.py --name waves --wave-timing -- --frames 2 --iterations 500 --wave-timing
  python3 tools/lsfg-study/ctxbench/run.py --name head --run-only -- --frames 0 --iterations 2000

The compiler prefix is serial: nothing else may use it meanwhile (CLAUDE.md, "Build").
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
STUDY = Path.home() / 'Projects/personal/OptiScaler_DLSSNR/Tmp/lsfg-study'
CACHE = Path.home() / '.cache/lsfg-ctxbench'
MSVC = Path.home() / '.local/opt/msvc/bin/x64'
PREFIX = Path.home() / '.local/opt/msvc-wineprefix'
LSFG_FILES = ('LsfgFrameGen_Dx12.cpp', 'LsfgFrameGen_Dx12.h', 'LsfgGraph.h', 'LsfgDxil.h', 'LsfgShaderSource.cpp',
              'LsfgShaderSource.h')


def idle():
    result = subprocess.run(['pgrep', '-af', r'[M]SBuild|[c]l\.exe|[d]xc\.exe|[l]sfg-harness\.exe|[l]sfg-ctxbench\.exe'],
                            capture_output=True, text=True)
    if result.returncode != 1:
        raise RuntimeError('compiler/prefix busy (retry later):\n' + result.stdout)


def command(args, env, log, cwd, timeout):
    idle()
    print('+', ' '.join(map(str, args)), flush=True)
    # Never pipe Wine output: wineserver may keep the pipe open after the client exits.
    with log.open('w') as stream:
        try:
            result = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=timeout)
        except subprocess.TimeoutExpired:
            subprocess.run(['wineserver', '-k'], env=env, check=False)
            raise RuntimeError(f'timed out; stopped the prefix; see {log}')
    if result.returncode:
        raise RuntimeError(f'exit {result.returncode}: {log}\n' + log.read_text(errors='replace')[-4000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', required=True, help='build name: ~/.cache/lsfg-ctxbench/<name>/')
    ap.add_argument('--baseline', help='git revision whose retoque/lsfg to build instead of the worktree')
    ap.add_argument('--run-only', action='store_true')
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--wave-timing', action='store_true',
                    help='build with LSFG_WAVE_TIMING (a timestamp after every wave; run with --wave-timing)')
    ap.add_argument('--input', type=Path, default=STUDY / 'input_translation/0.png')
    ap.add_argument('--shaders', type=Path, default=STUDY / 'dxil')
    ap.add_argument('rest', nargs=argparse.REMAINDER)
    args = ap.parse_args()

    work = CACHE / args.name
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    win = lambda p: 'Z:' + str(Path(p).resolve()).replace('/', '\\')

    if not args.run_only:
        if args.baseline:
            src = work / 'src'
            (src / 'lsfg').mkdir(parents=True, exist_ok=True)
            for name in LSFG_FILES:
                data = subprocess.run(['git', '-C', str(ROOT), 'show', f'{args.baseline}:retoque/lsfg/{name}'],
                                      capture_output=True, check=True).stdout
                (src / 'lsfg' / name).write_bytes(data)
            include = src
            defines = ['/DLSFG_BASELINE']
        else:
            include = ROOT / 'retoque'
            defines = ['/DLSFG_WAVE_TIMING'] if args.wave_timing else []
        sources = [HERE / 'main.cpp', include / 'lsfg/LsfgFrameGen_Dx12.cpp', include / 'lsfg/LsfgShaderSource.cpp']
        command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W3', '/O2', '/DUNICODE', '/D_UNICODE',
                 *defines, '/I' + win(HERE), '/I' + win(include), *map(win, sources), '/Fo' + win(work) + '\\',
                 '/Fe' + win(work / 'lsfg-ctxbench.exe'), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib',
                 'windowscodecs.lib', 'ole32.lib'], env, work / 'build.log', work, 300)
        candidates = [Path.home() / '.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows']
        runtime = next(p for p in candidates if (p / 'd3d12.dll').exists())
        files = [runtime / name for name in ('d3d12.dll', 'd3d12core.dll')]
        files += list(sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))[-1].glob('*.dll'))
        for src in files:
            dst = work / src.name
            dst.unlink(missing_ok=True)
            shutil.copy2(src, dst)
    if args.build_only:
        return

    rest = [a for a in args.rest if a != '--']
    out = work / 'out'
    out.mkdir(exist_ok=True)
    cmd = ['wine', work / 'lsfg-ctxbench.exe', '--shaders', win(args.shaders), '--input', win(args.input), '--out',
           win(out), *rest]
    env['WINEDLLOVERRIDES'] = 'd3d12=n;d3d12core=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n'
    log = out / 'run.log'
    command(cmd, env, log, work, 900)
    print(log.read_text(errors='replace'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, StopIteration, subprocess.SubprocessError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
