#!/usr/bin/env python3
"""The native D3D11 guide transport's API sequence on real devices under a local Proton (DXVK's D3D11, vkd3d-proton's
D3D12, as a game gets them): game-like depth and motion formats cross through shared textures and a shared fence into
clones made as the present guide ring makes them, and are read back byte for byte. Built with MSVC in a private copy
of the msvc-wine prefix. No OptiScaler, NGX or game; SKIP without a local Proton."""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
EXE = 'dx11-guides-transport.exe'


def proton():
    """PROTON_DIR, else the newest GE-Proton, else Valve's: the runtimes the test titles run under."""
    roots = [Path(os.environ['PROTON_DIR'])] if os.environ.get('PROTON_DIR') else []
    steam = Path.home() / '.local/share/Steam'
    roots += sorted((steam / 'compatibilitytools.d').glob('GE-Proton*'), reverse=True)
    roots += [steam / 'steamapps/common/Proton - Experimental', steam / 'steamapps/common/Proton Hotfix']
    return next((root / 'proton' for root in roots if (root / 'proton').exists()), None)


def no_game():
    # The user plays while suites run; a GPU harness beside a game is not run.
    for maps in Path('/proc').glob('[0-9]*/maps'):
        try:
            if 'steamapps/common' in maps.read_text(errors='replace'):
                raise RuntimeError('a Steam game is running; run this suite when it has exited')
        except (OSError, PermissionError):
            continue


def refuse_if_prefix_busy():
    # The msvc-wine prefix has stalled with two clients on it. This suite compiles in a private copy, but stays out
    # of the way of the shared one while a build or a shader rebuild is in flight. Only processes that are one count:
    # the compiler's and the script's own command lines, not a shell whose command text merely names the script.
    busy = []
    for cmdline in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            argv = [a for a in cmdline.read_bytes().split(b'\0') if a]
        except OSError:
            continue
        names = [Path(a.decode(errors='replace')).name for a in argv[:2]]
        if any(n.lower() in ('msbuild.exe', 'dxc.exe') for n in names) or \
                (len(names) == 2 and names[0] in ('bash', 'sh') and names[1] == 'build-local.sh'):
            busy.append(' '.join(a.decode(errors='replace') for a in argv)[:200])
    if busy:
        raise RuntimeError('the msvc-wine prefix is busy (build or dxc running); run again after it finishes:\n'
                           + '\n'.join(busy))


def build():
    refuse_if_prefix_busy()
    prefix = OUT / 'build-prefix'
    if not prefix.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    # This prefix belongs only to this suite; stopping its server touches nothing else.
    subprocess.run(['wineserver', '-k'], env=env, check=False)
    work = OUT / 'run'
    work.mkdir(parents=True, exist_ok=True)
    with (OUT / 'build.log').open('w') as log:
        p = subprocess.run([str(MSVC / 'cl'), '/nologo', '/std:c++20', '/EHsc', '/MT', '/W4', '/O1',
                            str(HERE / 'harness.cpp'), '/Fo' + str(OUT) + '/', '/Fe' + str(work / EXE), '/link',
                            'd3d11.lib', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib'],
                           env=env, cwd=OUT, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                           timeout=600)
    subprocess.run(['wineserver', '-k'], env=env, check=False)
    if p.returncode != 0:
        raise RuntimeError('build failed:\n' + (OUT / 'build.log').read_text(errors='replace')[-4000:])
    return work


def run(work, runtime, args=()):
    for stale in ('dx11-guides-transport.log', 'result.json'):
        (work / stale).unlink(missing_ok=True)
    compat = OUT / 'protonprefix'
    compat.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(compat),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / '.local/share/Steam'), SteamGameId='0',
               WINEDEBUG='-all')
    env.pop('WINEPREFIX', None)
    no_game()
    with (OUT / 'run.log').open('w') as log:
        p = subprocess.run([str(runtime), 'run', str(work / EXE), *args], cwd=work, env=env, stdout=log,
                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=600)
    harness = (work / 'dx11-guides-transport.log')
    text = harness.read_text(errors='replace') if harness.exists() else ''
    print(text.rstrip())
    result = json.loads((work / 'result.json').read_text()) if (work / 'result.json').exists() else {}
    if p.returncode != 0 or result.get('status') != 'PASS':
        raise RuntimeError(f'harness exited {p.returncode}: {result.get("first_failure") or result.get("reason") or "no result written"}'
                           f'; see {harness} and {OUT / "run.log"}')
    if 'DX11-GUIDES-TRANSPORT PASS: cases=8 crossed=8' not in text:
        raise RuntimeError('ZERO COVERAGE: not every case was crossed')


def main():
    runtime = proton()
    if runtime is None:
        print('SKIP: no local Proton (GE-Proton or Valve\'s) to run DXVK and vkd3d-proton under')
        return
    OUT.mkdir(exist_ok=True)
    no_game()
    work = build()
    print(f'running under {runtime.parent.name}')
    run(work, runtime, [a for a in sys.argv[1:] if a == '--check-d3d11'])
    print('PASS: the native D3D11 guide transport\'s sequence crossed every case byte for byte under '
          f'{runtime.parent.name}')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
