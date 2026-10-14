#!/usr/bin/env python3
"""tools/install-windows.ps1 (and its .cmd wrapper) on fake game folders, in PowerShell 7.

Two parts. The static checks run everywhere: both files are ASCII (Windows PowerShell 5.1 reads a script
without a byte-order mark in the ANSI code page), the .cmd keeps CRLF and only starts the .ps1, and the
release packager ships both. The cases (cases.ps1) need PowerShell: they run in a pinned PowerShell 7.4
container, streamed in with tar because the docker context may be remote (bind mounts would resolve on the
other host), or in a local pwsh when there is no docker. With neither, that part prints SKIP and the suite
passes on the static checks alone, so the host tier stays green where docker is absent.

    python3 tests/windows-installer/run.py [--runner auto|docker|local]
"""
from pathlib import Path
import argparse
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PS1 = REPO / 'tools' / 'install-windows.ps1'
CMD = REPO / 'tools' / 'install-windows.cmd'
PACKAGER = REPO / 'tools' / 'package_release.ps1'

# PowerShell 7.4 LTS on Ubuntu 22.04, pinned by digest so a re-tag cannot change what the cases run under.
IMAGE = ('mcr.microsoft.com/powershell:lts-7.4-ubuntu-22.04'
         '@sha256:663a172849444279d8e7b50e19977b6e42380eae6856bdc8931ca08e2e03e92a')

# What the cases need under one root, at the paths deploy-to-game.sh expects (it finds OptiScaler.ini at
# its own ../OptiScaler.ini).
STAGE = {
    'tools/install-windows.ps1': PS1,
    'tools/deploy-to-game.sh': REPO / 'tools' / 'deploy-to-game.sh',
    'OptiScaler.ini': REPO / 'OptiScaler.ini',
    'cases.ps1': HERE / 'cases.ps1',
}


def static_checks():
    failures = []
    for path in (PS1, CMD, HERE / 'cases.ps1'):
        data = path.read_bytes()
        bad = [i for i, b in enumerate(data) if b > 0x7F]
        if bad:
            line = data[:bad[0]].count(b'\n') + 1
            failures.append(f'{path.relative_to(REPO)}: non-ASCII byte at line {line}')
    cmd = CMD.read_bytes()
    if cmd.count(b'\n') != cmd.count(b'\r\n'):
        failures.append('tools/install-windows.cmd: a line ends in LF only; cmd.exe can then miss a label '
                        '(tools/.gitattributes should check it out with CRLF)')
    text = cmd.decode('ascii', 'replace')
    for needle in ('-NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%"', 'set "SCRIPT=%~dp0install-windows.ps1"'):
        if needle not in text:
            failures.append(f'tools/install-windows.cmd: expected {needle!r}')
    # The wrapper only starts the script: no policy change, no download, no admin request of its own.
    for forbidden in ('Set-ExecutionPolicy', 'Invoke-WebRequest', 'curl', 'bitsadmin', 'runas', 'reg add'):
        if forbidden.lower() in text.lower():
            failures.append(f'tools/install-windows.cmd: contains {forbidden!r}')
    packager = PACKAGER.read_text()
    if '@("install-windows.ps1", "install-windows.cmd")' not in packager:
        failures.append('tools/package_release.ps1: does not ship install-windows.ps1 and install-windows.cmd')
    for refusal in ('STALE forwarder', "Pattern '^Enabled=true'"):
        if refusal not in packager:
            failures.append(f'tools/package_release.ps1: lost {refusal!r}')
    return failures


def stage_tar():
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w') as tar:
        for name, source in STAGE.items():
            info = tar.gettarinfo(str(source), arcname=name)
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            with source.open('rb') as handle:
                tar.addfile(info, handle)
    return buffer.getvalue()


def docker_blocker():
    """Why the container cannot run here, or None."""
    if not shutil.which('docker'):
        return 'docker not on PATH'
    try:
        probe = subprocess.run(['docker', 'version', '--format', '{{.Server.Version}}'],
                               capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return 'docker daemon did not answer within 30 s'
    if probe.returncode != 0:
        return 'docker daemon not reachable: ' + (probe.stderr.strip().splitlines() or ['?'])[-1]
    if subprocess.run(['docker', 'image', 'inspect', IMAGE], capture_output=True).returncode != 0:
        print(f'pulling {IMAGE}', flush=True)
        try:
            pull = subprocess.run(['docker', 'pull', IMAGE], capture_output=True, text=True, timeout=900)
        except subprocess.TimeoutExpired:
            return f'pulling {IMAGE} took over 15 minutes'
        if pull.returncode != 0:
            return f'could not pull {IMAGE}: ' + (pull.stderr.strip().splitlines() or ['?'])[-1]
    return None


def run_docker():
    cmd = ['docker', 'run', '--rm', '-i', '--network', 'none', IMAGE, 'bash', '-c',
           'mkdir /w && tar -x -C /w && exec pwsh -NoProfile -NonInteractive -File /w/cases.ps1 /w']
    return subprocess.run(cmd, input=stage_tar(), timeout=900).returncode


def run_local(pwsh):
    with tempfile.TemporaryDirectory(prefix='windows-installer-') as tmp:
        with tarfile.open(fileobj=io.BytesIO(stage_tar())) as tar:
            tar.extractall(tmp, filter='data')
        return subprocess.run([pwsh, '-NoProfile', '-NonInteractive', '-File', str(Path(tmp) / 'cases.ps1'), tmp],
                              timeout=900).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--runner', choices=('auto', 'docker', 'local'), default='auto')
    args = parser.parse_args()

    failures = static_checks()
    for failure in failures:
        print(f'FAIL: static: {failure}')
    if not failures:
        print('PASS: static checks (ASCII, CRLF wrapper, packager allow-list)')

    pwsh = shutil.which('pwsh')
    blocker = None
    if args.runner in ('auto', 'docker'):
        blocker = docker_blocker()
    if args.runner == 'docker' or (args.runner == 'auto' and blocker is None):
        if blocker:
            print(f'FAIL: --runner docker: {blocker}')
            return 1
        print(f'running the cases in {IMAGE}', flush=True)
        code = run_docker()
    elif pwsh:
        print(f'running the cases in the local {pwsh}' + (f' ({blocker})' if blocker else ''), flush=True)
        code = run_local(pwsh)
    elif args.runner == 'local':
        print('FAIL: --runner local: pwsh not on PATH')
        return 1
    else:
        print(f'SKIP: the PowerShell cases need docker or pwsh ({blocker}; no pwsh on PATH)')
        code = 0
    return 1 if failures or code != 0 else 0


if __name__ == '__main__':
    sys.exit(main())
