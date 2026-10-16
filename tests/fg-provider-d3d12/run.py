#!/usr/bin/env python3
"""Full production OptiScaler + actual local DLSS-G plugin. No game and no substitute backend."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
WORK = OUT / 'run'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
DLLS = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def guard():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        comm = ''
        try:
            comm = (proc / 'comm').read_text().strip().lower()
            if comm in ('msbuild.exe', 'cl.exe', 'link.exe'):
                raise RuntimeError('compiler prefix already in use')
            mappings = (proc / 'maps').read_text()
            if str(WORK) in mappings:
                raise RuntimeError('an earlier provider fixture still maps its staged DLLs')
            if '/steamapps/common/' in mappings:
                raise RuntimeError('a Steam common application is mapped; close it before the GPU fixture')
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            if proc.exists() and proc.stat().st_uid == os.getuid() and comm not in ('systemd', '(sd-pam)'):
                raise RuntimeError('cannot inspect an owned process for active game mappings')


def execute(command, *, cwd, env, log, timeout):
    print('+', ' '.join(map(str, command)), flush=True)
    with log.open('w') as stream:
        child = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return child.wait(timeout=timeout)
        except BaseException:
            # A wrapper can exit on TERM while its compiler/Wine descendants ignore it.
            # Always escalate the owned process group, independently of its leader's state.
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
            finally:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
            raise


def stage(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + '.stage')
    temporary.unlink(missing_ok=True)
    shutil.copy2(source, temporary)
    temporary.replace(target)  # Never truncate a DLL that a process may still have mapped.


def judge(text):
    phases = re.findall(r'^PHASE phase=(\d+) enabled=(\d+) app=(\d+) actual=(\d+) '
                        r'steady_app=(\d+) steady_actual=(\d+) ok=(\d+) completed=(\d+)$', text, re.M)
    if len(phases) != 4 or 'ORACLE=real-output-plugin' not in text:
        raise RuntimeError('ZERO COVERAGE: complete real-plugin phase record missing')
    frames = [tuple(map(int, row)) for row in re.findall(
        r'^FRAME phase=(\d+) app=(\d+) actual=(\d+) status=(\d+) max=(\d+)$', text, re.M)]
    if len(frames) != 336 or [row[1] for row in frames] != list(range(1, 337)):
        raise RuntimeError('incomplete or non-monotonic per-frame progression')
    result = []
    for index, row in enumerate(phases):
        phase, enabled, app, actual, steady_app, steady_actual, ok, completed = map(int, row)
        expected = 48 if index == 1 else 96
        if phase != index or enabled != (index != 1) or app != expected or completed != expected:
            raise RuntimeError('incomplete frame progression or completion')
        if steady_app != expected - 32 or ok != steady_app:
            raise RuntimeError('missing steady-state successful plugin status')
        if (enabled and steady_actual < steady_app + 16) or (not enabled and steady_actual != steady_app):
            raise RuntimeError('plugin actual-present surplus disagrees with enable state')
        observed = [row for row in frames if row[0] == phase]
        if (len(observed) != app or sum(row[2] for row in observed) != actual
                or sum(row[2] for row in observed[32:]) != steady_actual
                or any(row[3] != 0 for row in observed[32:])
                or any(row[4] < 1 for row in observed if enabled)):
            raise RuntimeError('phase counters disagree with actual per-frame plugin observations')
        result.append(dict(phase=phase, enabled=bool(enabled), app=app, actual=actual,
                           steady_app=steady_app, steady_actual=steady_actual, completed=completed))
    total_app, total_actual = sum(x['app'] for x in result), sum(x['actual'] for x in result)
    marker = f'PASS: FG-PROVIDER app={total_app} actual={total_actual} phases=4 resizes=1 oracle=real-plugin'
    if marker not in text or len(re.findall(r'^FRAME ', text, re.M)) != total_app:
        raise RuntimeError('incomplete frame records or final marker')
    return result


def cleanup_self_test():
    with tempfile.TemporaryDirectory(prefix='retoque-fg-timeout-') as temporary:
        folder = Path(temporary)
        ready = folder / 'ready'
        child_code = ('import os,signal,time;from pathlib import Path;'
                      'signal.signal(signal.SIGTERM,signal.SIG_IGN);'
                      f'Path({str(ready)!r}).write_text(str(os.getpid()));time.sleep(60)')
        parent_code = ('import subprocess,sys,time;from pathlib import Path;'
                       f'p=subprocess.Popen([sys.executable,"-c",{child_code!r}]);'
                       f'ready=Path({str(ready)!r});'
                       '\nwhile not ready.exists(): time.sleep(.01)'
                       '\ntime.sleep(60)')
        descendant = None
        try:
            try:
                execute([sys.executable, '-c', parent_code], cwd=folder, env=dict(os.environ),
                        log=folder / 'log', timeout=1)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise RuntimeError('timeout cleanup negative control did not time out')
            if not ready.is_file():
                raise RuntimeError('timeout cleanup child never became ready')
            descendant = int(ready.read_text())
            stat = Path(f'/proc/{descendant}/stat')
            for _ in range(100):
                if not stat.exists() or stat.read_text().split(') ', 1)[1].split()[0] == 'Z':
                    break
                time.sleep(.01)
            else:
                raise RuntimeError('timeout left SIGTERM-ignoring descendant alive')
        finally:
            if descendant is None and ready.is_file():
                descendant = int(ready.read_text())
            if descendant is not None:
                try:
                    os.kill(descendant, signal.SIGKILL)
                except ProcessLookupError:
                    pass
    print('PASS: wrapper timeout kills SIGTERM-ignoring descendant (host only)')


def self_test():
    rows = ['ORACLE=real-output-plugin']
    actual_sum = 0
    frame_id = 0
    for phase in range(4):
        count = 48 if phase == 1 else 96
        actual = count if phase == 1 else count * 2
        steady = count - 32
        for _ in range(count):
            frame_id += 1
            rows.append(f'FRAME phase={phase} app={frame_id} actual={1 if phase == 1 else 2} status=0 max=1')
        rows.append(f'PHASE phase={phase} enabled={int(phase != 1)} app={count} actual={actual} '
                    f'steady_app={steady} steady_actual={steady if phase == 1 else steady * 2} '
                    f'ok={steady} completed={count}')
        actual_sum += actual
    rows.append(f'PASS: FG-PROVIDER app=336 actual={actual_sum} phases=4 resizes=1 oracle=real-plugin')
    valid = '\n'.join(rows)
    judge(valid)
    for invalid in (valid.replace('steady_actual=128', 'steady_actual=64', 1),
                    valid.replace('ORACLE=real-output-plugin', ''),
                    valid.replace('completed=96', 'completed=95', 1),
                    valid.replace('enabled=0', 'enabled=1', 1),
                    valid.replace('steady_actual=16', 'steady_actual=0', 1), valid.replace('FRAME phase=0 app=1 actual=2 status=0 max=1\n', '', 1)):
        try:
            judge(invalid)
        except RuntimeError:
            pass
        else:
            raise RuntimeError('judge accepted a broken evidence record')
    print('PASS: FG provider evidence judge and six negative controls (host only)')
    cleanup_self_test()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', type=Path, default=ROOT / 'x64/out/OptiScaler.dll')
    parser.add_argument('--kit', type=Path, default=Path(os.environ['RETOQUE_FG_KIT']) if 'RETOQUE_FG_KIT' in os.environ else None)
    parser.add_argument('--proton', type=Path, default=Path(os.environ['RETOQUE_PROTON']) if 'RETOQUE_PROTON' in os.environ else None)
    parser.add_argument('--dlss', type=Path, help='existing nvngx_dlss.dll required by native NGX routing')
    parser.add_argument('--strict', action='store_true', help='missing prerequisites fail instead of SKIP')
    parser.add_argument('--self-test', action='store_true', help='exercise only the host evidence judge')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'result.json').write_text('{"status":"RUNNING"}\n')
    steam = Path.home() / '.local/share/Steam'
    kit = args.kit
    if kit is None:
        candidates = sorted((Path.home() / '.local/state/dlss5/rafael-kits').glob('*/streamline'), reverse=True)
        kit = next((p for p in candidates if all((p / name).is_file() for name in DLLS)), None)
    proton = args.proton
    if proton is None:
        candidates = sorted((steam / 'compatibilitytools.d').glob('GE-Proton*/proton'), reverse=True)
        candidates += [steam / 'steamapps/common/Proton - Experimental/proton']
        proton = next((p for p in candidates if p.is_file()), None)
    dlss = args.dlss
    if dlss is None:
        dlss = next(iter(sorted((Path.home() / '.local/state/dlss5/rafael-kits').glob('*/nvngx_dlss.dll'), reverse=True)), None)
    missing = []
    if dlss is None or not dlss.is_file():
        missing.append('existing nvngx_dlss.dll for native NGX routing (--dlss)')
    if not (MSVC / 'cl').is_file():
        missing.append('MSVC cl wrapper (MSVC_BIN)')
    if not PREFIX.is_dir():
        missing.append('compiler prefix (WINEPREFIX)')
    if not args.dll.is_file():
        missing.append('production OptiScaler.dll (--dll, build-local.sh first)')
    if kit is None or any(not (kit / name).is_file() for name in DLLS):
        missing.append('complete user-supplied Streamline/DLSS-G kit (--kit / RETOQUE_FG_KIT)')
    if proton is None or not proton.is_file():
        missing.append('installed Proton executable (--proton / RETOQUE_PROTON)')
    if not os.environ.get('DISPLAY') and not os.environ.get('WAYLAND_DISPLAY'):
        missing.append('graphical session')
    if missing:
        result = {'status': 'FAIL' if args.strict else 'SKIP', 'missing': missing, 'runtime_coverage': False}
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(result['status'] + ': ' + '; '.join(missing))
        if args.strict:
            raise SystemExit(1)
        return
    lock_path = Path.home() / '.cache/retoque-wt/msvc-prefix.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('compiler prefix locked by another task')
        guard()
        WORK.mkdir(parents=True, exist_ok=True)
        stage(args.dll, WORK / 'OptiScaler.dll')
        stage(dlss, WORK / 'nvngx_dlss.dll')
        for name in DLLS:
            stage(kit / name, WORK / 'streamline' / name)
            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll'):
                stage(kit / name, WORK / 'input' / name)
        redists = sorted((MSVC.parents[1] / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'))
        if not redists:
            raise RuntimeError('MSVC runtime missing')
        for path in redists[-1].glob('*.dll'):
            stage(path, WORK / path.name)
        (WORK / 'OptiScaler.ini').write_text('[FrameGen]\nEnabled=auto\nFGInput=dlssg\nFGOutput=dlssg\n'
            'FGNvngxReplacement=None\n[DLSSG]\nInterpolationCount=1\n[Menu]\nOverlayMenu=false\n'
            '[DlssNr]\nEnabled=false\n[Vulkan]\nSkipHooks=true\n[Log]\nLogToFile=true\nLogLevel=1\n'
            '[Spoofing]\nDxgi=false\nVulkan=false\nStreamlineSpoofing=false\n')
        for name in ('fg-provider.log', 'OptiScaler.log'):
            (WORK / name).unlink(missing_ok=True)
        source_hashes = {path.name: sha(path) for path in (HERE / 'harness.cpp', HERE / 'run.py')}
        compile_env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
        exe = WORK / 'fg-provider.exe'
        command = [MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
                   '/I' + str(ROOT / 'external/streamline'), HERE / 'harness.cpp',
                   '/Fo' + str(OUT / 'harness.obj'), '/Fe' + str(exe), '/link',
                   'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
        code = execute(command, cwd=ROOT, env=compile_env, log=OUT / 'build.log', timeout=300)
        if code:
            raise RuntimeError('fixture compile failed; see artifacts/build.log')
        staged_assets = {'OptiScaler.dll': sha(WORK / 'OptiScaler.dll'),
                         'nvngx_dlss.dll': sha(WORK / 'nvngx_dlss.dll'),
                         **{name: sha(WORK / 'streamline' / name) for name in DLLS},
                         **{'input/' + name: sha(WORK / 'input' / name) for name in DLLS
                            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll')}}
        guard()
        compat = OUT / 'protonprefix'
        compat.mkdir(exist_ok=True)
        env = dict(os.environ, STEAM_COMPAT_DATA_PATH=str(compat),
                   STEAM_COMPAT_CLIENT_INSTALL_PATH=str(steam), SteamGameId='0',
                   WINEDEBUG='-all', PROTONFIXES_DISABLE='1',
                   PROTON_FSR4_UPGRADE='0', PROTON_FFX3_UPGRADE='0', PROTON_FFX4_UPGRADE='0',
                   PROTON_DLSS_UPGRADE='0', PROTON_XESS_UPGRADE='0', PROTON_USE_OPTISCALER='0', PROTON_LOG='1', PROTON_LOG_DIR=str(OUT),
                   VK_LOADER_LAYERS_DISABLE='~implicit~',
                   WINEDLLOVERRIDES='vcruntime140,vcruntime140_1,msvcp140=n')
        code = execute([proton, 'run', exe], cwd=WORK, env=env, log=OUT / 'proton.log', timeout=240)
        text = (WORK / 'fg-provider.log').read_text(errors='replace') if (WORK / 'fg-provider.log').exists() else ''
        if code:
            raise RuntimeError(f'provider exited {code}; see artifacts/run/fg-provider.log and proton.log')
        phases = judge(text)
        after_assets = {'OptiScaler.dll': sha(WORK / 'OptiScaler.dll'),
                        'nvngx_dlss.dll': sha(WORK / 'nvngx_dlss.dll'),
                        **{name: sha(WORK / 'streamline' / name) for name in DLLS},
                         **{'input/' + name: sha(WORK / 'input' / name) for name in DLLS
                            if name not in ('sl.dlss_g.dll', 'nvngx_dlssg.dll')}}
        if source_hashes != {path.name: sha(path) for path in (HERE / 'harness.cpp', HERE / 'run.py')}:
            raise RuntimeError('fixture sources changed during build/run; provenance invalid')
        if staged_assets != after_assets:
            raise RuntimeError('staged runtime assets changed during execution; provenance invalid')
        opti = (WORK / 'OptiScaler.log').read_text(errors='replace')
        if 'DLSSG_Dx12::Dispatch' not in opti or 'Result: Ok' not in opti:
            raise RuntimeError('ZERO COVERAGE: production DLSS-G dispatch evidence missing')
        result = {'status': 'PASS', 'runtime_coverage': True, 'phases': phases,
                  'dll_sha256': staged_assets['OptiScaler.dll'], 'dlss_sha256': staged_assets['nvngx_dlss.dll'],
                  'assets': {name: staged_assets[name] for name in DLLS}, 'staged_assets_unchanged': True,
                  'sources': source_hashes,
                  'input_assets': {name: value for name, value in staged_assets.items() if name.startswith('input/')},
                  'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
                  'cases_not_exercised': {'mfg_above_x2': 'SKIP: this fixture requests only one generated frame; no higher-ratio hardware claim',
                                          'menu': 'SKIP: no synthetic menu input in this fixture'},
                  'limits': ['x2 only; no MFG hardware claim', 'plugin actual-present counters, not scanout capture',
                             'no perceptual-quality or latency metric', 'Proton is not native D3D12 validation',
                             'no Vulkan/D3D11/menu coverage']}
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('PASS: full production DLSS-G provider; ' + str(OUT / 'result.json'))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'result.json').write_text(json.dumps({'status': 'FAIL', 'runtime_coverage': False,
                                                   'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
