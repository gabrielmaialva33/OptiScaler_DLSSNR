#!/usr/bin/env python3
"""The session guard's exit signal under Wine (docs/dlssnr/design/session-guard.md, "The exit signal").

Builds the shipped retoque/dlssnr/DlssNr_ProcessEnd.cpp into harness.exe with the msvc-wine toolchain, then ends a
real process every way the note lists, one process per mode, each under every Wine found here (the system's, and the
newest GE-Proton's own), and judges what the hook recorded: every end the process brings on itself reaches the hook
exactly once, with the status the note predicts and the other threads still running; a process killed from outside
never does, and terminating another process is not mistaken for this one's end.

Never loads OptiScaler.dll, NGX or a game.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))


def natural(path):
    return [int(part) if part.isdigit() else part for part in re.split(r'(\d+)', path.parents[2].name)]


# Newest first by version number, not by text (GE-Proton9 is older than GE-Proton11).
GE_PROTON = sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('GE-Proton*/files/bin/wine'),
                   key=natural, reverse=True)

# mode -> (statuses the hook must record, in order; the Windows exit code). Every end the process brings on itself in
# user mode runs the hook once, before the other threads are stopped ("others=alive").
CASES = {
    'exit-process-0': (['0x00000000'], 0x00000000),  # a normal quit through ExitProcess
    'exit-process-3': (['0x00000003'], 0x00000003),
    'crt-exit-5': (['0x00000005'], 0x00000005),  # exit(): atexit, then ExitProcess
    'return-6': (['0x00000006'], 0x00000006),  # return from main
    'terminate-self-0': (['0x00000000'], 0x00000000),  # an engine skipping its own teardown
    'terminate-self-7': (['0x00000007'], 0x00000007),
    'terminate-self-handle-9': (['0x00000009'], 0x00000009),  # a real handle to itself
    'access-violation': (['0xC0000005'], 0xC0000005),  # unhandled exception, default end
    # The same as a game has it, without SEM_NOGPFAULTERRORBOX: the AeDebug debugger (winedbg --auto, crash dialog off
    # as in Proton) attaches and dumps first, and the end still comes through the hook with the exception code.
    'access-violation-default': (['0xC0000005'], 0xC0000005),
    # The CRT's abort() is __fastfail: ended below user mode, on Wine as on Windows, so no end is seen (an unclean
    # end for the guard, read as "no end seen"). The exit status still says what happened.
    'abort': ([], 0xC0000409),
    'own-filter-exit-3': (['0x00000003'], 0x00000003),  # the known gap: a crash handler's own exit code
    'kill-other': (['0x00000000'], 0x00000000),  # kills a watched child, then quits normally
}


def command(args, *, env, logfile, cwd=ROOT, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    if p.returncode != 0:
        raise RuntimeError(f'exit {p.returncode}: {logfile}\n' + logfile.read_text(errors='replace')[-4000:])


def refuse_if_prefix_busy():
    # The msvc-wine prefix has stalled with two clients on it. This suite compiles in a private copy, but takes that
    # copy only while nothing else is using the shared prefix.
    busy = subprocess.run(['pgrep', '-af', r'[M]SBuild\.exe|[b]uild-local\.sh|[d]xc\.exe|[C]L\.exe'],
                          capture_output=True, text=True)
    if busy.returncode == 0:
        raise RuntimeError('the msvc-wine prefix is busy (build, compiler or dxc running); run again after it '
                           'finishes:\n' + busy.stdout)


def build():
    if not (MSVC / 'cl').exists():
        raise RuntimeError(f'no msvc-wine toolchain at {MSVC}')
    build_prefix = OUT / 'build-prefix'  # taken once; delete artifacts/ to take it again after a toolchain change
    if not build_prefix.exists():
        refuse_if_prefix_busy()
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(build_prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(build_prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    subprocess.run(['wineserver', '-k'], env=env, check=False)  # this prefix belongs to this suite only
    objdir = OUT / 'obj'
    objdir.mkdir(parents=True, exist_ok=True)
    exe = OUT / 'harness.exe'
    try:
        command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MT', '/W4', '/WX', '/O2',
                 '/I' + str(HERE / 'stubs'), '/I' + str(ROOT / 'retoque'), '/I' + str(ROOT / 'OptiScaler/include'),
                 HERE / 'harness.cpp', ROOT / 'retoque/dlssnr/DlssNr_ProcessEnd.cpp',
                 '/Fo' + str(objdir) + '/', '/Fe' + str(exe), '/link', '/IGNORE:4098',
                 '/LIBPATH:' + str(ROOT / 'OptiScaler/library/detours'), 'detours.lib'],
                env=env, logfile=OUT / 'build.log', timeout=600)
    finally:
        subprocess.run(['wineserver', '-k'], env=env, check=False)
    return exe


def wines(requested):
    if requested:
        return [(f'wine-{i}', w) for i, w in enumerate(requested, 1)]
    found = []
    system = shutil.which('wine')
    if system:
        found.append(('system', system))
    if GE_PROTON:
        found.append((GE_PROTON[0].parents[2].name, str(GE_PROTON[0])))
    return found


def wine_version(wine):
    p = subprocess.run([wine, '--version'], capture_output=True, text=True, timeout=60)
    return p.stdout.strip() or p.stderr.strip()


def unix_status(code):
    # What `wine program.exe` exits with: the low byte of the Windows exit code, and 1 for a nonzero code whose low
    # byte is 0.
    return (code & 0xFF) or (1 if code else 0)


def prepare_prefix(wine, label):
    # No crash dialog, as Proton's own prefixes have it: winedbg --auto would otherwise wait on a window.
    env = dict(os.environ, WINEPREFIX=str(OUT / f'prefix-{label}'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='winemenubuilder.exe=d')
    subprocess.run([wine, 'reg', 'add', r'HKCU\Software\Wine\WineDbg', '/v', 'ShowCrashDialog', '/t', 'REG_DWORD',
                    '/d', '0', '/f'], env=env, capture_output=True, timeout=300)


def run_case(wine, label, exe, mode, statuses, code):
    work = OUT / 'run' / label
    work.mkdir(parents=True, exist_ok=True)
    record = work / f'{mode}.txt'
    child = work / f'{mode}.child.txt'
    for f in (record, child, Path(str(child) + '.ready')):
        f.unlink(missing_ok=True)
    env = dict(os.environ, WINEPREFIX=str(OUT / f'prefix-{label}'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='winemenubuilder.exe=d')
    args = [wine, exe, mode, record] + ([child] if mode == 'kill-other' else [])
    try:
        p = subprocess.run(list(map(str, args)), env=env, cwd=work, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        server = Path(wine).with_name('wineserver')
        subprocess.run([str(server) if server.exists() else 'wineserver', '-k'], env=env, check=False)
        return ['timed out after 120 s'], None
    problems = []
    lines = record.read_text().splitlines() if record.exists() else []
    seen = [line.split()[0].removeprefix('status=') for line in lines]
    if statuses is None:  # measured, reported, not judged
        return [], f'recorded {seen or "nothing"}, exit status {p.returncode}'
    if seen != statuses:
        problems.append(f'recorded {seen or "nothing"}, expected {statuses}')
    if any(not line.endswith('others=alive') for line in lines):
        problems.append(f'the hook ran after the other threads were stopped: {lines}')
    if p.returncode != unix_status(code):
        problems.append(f'exit status {p.returncode}, expected {unix_status(code)} (0x{code:08X})')
    if mode == 'kill-other':
        if not Path(str(child) + '.ready').exists():
            problems.append('the child never started')
        if child.exists():
            problems.append(f'the child killed from outside recorded an end: {child.read_text().strip()}')
    return problems, None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--wine', action='append', help='a wine binary to run under (repeatable); default: the '
                        'system wine and the newest GE-Proton found')
    args = parser.parse_args()

    OUT.mkdir(exist_ok=True)
    exe = build()
    targets = wines(args.wine)
    if not targets:
        raise RuntimeError('no wine found')

    failures = []
    for label, wine in targets:
        version = wine_version(wine)
        print(f'== {label}: {version}', flush=True)
        env = dict(os.environ, WINEPREFIX=str(OUT / f'prefix-{label}'), WINEDEBUG='-all')
        prepare_prefix(wine, label)
        for mode, case in CASES.items():
            statuses, code = case if case is not None else (None, None)
            problems, measured = run_case(wine, label, exe, mode, statuses, code)
            if measured is not None:
                print(f'INFO {label} {mode}: {measured}', flush=True)
                continue
            print(f'{"PASS" if not problems else "FAIL"} {label} {mode}' + ''.join(f'\n    {p}' for p in problems),
                  flush=True)
            failures += [f'{label} {mode}: {p}' for p in problems]
        server = Path(wine).with_name('wineserver')
        subprocess.run([str(server) if server.exists() else 'wineserver', '-k'], env=env, check=False)

    if failures:
        print(f'FAIL: {len(failures)} problem(s)')
        sys.exit(1)
    print(f'PASS: the exit signal under {len(targets)} wine build(s), {len(CASES)} ends each')


if __name__ == '__main__':
    main()
