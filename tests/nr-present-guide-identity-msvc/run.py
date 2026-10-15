#!/usr/bin/env python3
"""The present-guide identity cases (tests/nr-present-guide-identity) compiled by MSVC and run under Wine: the
decision header, step 1's record and line, the source-change reset, the status word and its text, and the renderer's
glue sliced out and run against the same fakes, through the compiler and standard library the DLL ships with. A
private copy of the msvc-wine prefix, no D3D12, no GPU, no NGX, no game."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
CASES = ROOT / 'tests/nr-present-guide-identity'
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))

PASSES = ('PASS: present-guide identity:', 'PASS: identity glue as the renderer has it:')


def command(args, *, env, logfile, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=OUT, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    text = logfile.read_text(errors='replace')
    if p.returncode != 0:
        raise RuntimeError(f'exit {p.returncode}: {logfile}\n' + text[-4000:])
    return text


def refuse_if_prefix_busy():
    # The msvc-wine prefix has stalled with two clients on it. This suite compiles in a private copy, but stays out
    # of the way of the shared one while a build or a shader rebuild is in flight.
    busy = subprocess.run(['pgrep', '-af', r'[M]SBuild\.exe|[b]uild-local\.sh|[d]xc\.exe'], capture_output=True,
                          text=True)
    if busy.returncode == 0:
        raise RuntimeError('the msvc-wine prefix is busy (build or dxc running); run again after it finishes:\n'
                           + busy.stdout)


def glue_unit():
    # The same slice tests/nr-present-guide-identity/run.py compiles with g++.
    renderer = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
    start = 'struct PresentGuideIdentityState\n'
    assert renderer.count(start) == 1, 'production boundary changed'
    begin = renderer.index(start)
    glue = renderer[begin:renderer.index('static void ReleasePresentTemporal()', begin)]
    unit = OUT / 'glue.cpp'
    unit.write_text((CASES / 'glue_fakes.h').read_text() + '\nnamespace DlssNr\n{\n' + glue + '\n} // namespace DlssNr\n' +
                    (CASES / 'glue_cases.h').read_text())
    return unit


def main():
    OUT.mkdir(exist_ok=True)
    refuse_if_prefix_busy()
    prefix = OUT / 'build-prefix'
    if not prefix.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    # This prefix belongs only to this suite; stopping its server touches nothing else.
    subprocess.run(['wineserver', '-k'], env=env, check=False)

    flags = ['/nologo', '/std:c++20', '/EHsc', '/MT', '/W3', '/O1', '/permissive-',
             '/I' + str(ROOT / 'retoque'), '/I' + str(ROOT / 'OptiScaler')]
    units = {'decide': CASES / 'decide.cpp', 'glue': glue_unit()}
    for name, source in units.items():
        command([MSVC / 'cl', *flags, source, '/Fo' + str(OUT / name) + '.obj', '/Fe' + str(OUT / name) + '.exe'],
                env=env, logfile=OUT / f'build-{name}.log')

    results = []
    for name, expected in zip(units, PASSES):
        text = command(['wine', OUT / f'{name}.exe'], env=env, logfile=OUT / f'run-{name}.log', timeout=300)
        line = next((l for l in text.splitlines() if l.startswith(expected)), None)
        if line is None:
            raise RuntimeError(f'{name}.exe ran but printed no "{expected}" line:\n' + text[-2000:])
        results.append(line)

    subprocess.run(['wineserver', '-k'], env=env, check=False)
    for line in results:
        print(line.replace('PASS: ', 'PASS (MSVC, Wine): ', 1))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, AssertionError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
