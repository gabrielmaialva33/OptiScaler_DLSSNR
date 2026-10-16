"""Serial ABBA orchestration for run.py --moving-scene; no independent build or prefix."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys


@contextmanager
def lease(work):
    lock_path = Path.home() / '.cache/retoque-wt/msvc-prefix.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('compiler/GPU fixture lease held by another task')
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit():
                continue
            comm = ''
            try:
                comm = (proc / 'comm').read_text().strip().lower()
                if comm in ('msbuild.exe', 'cl.exe', 'link.exe'):
                    raise RuntimeError('compiler prefix already in use')
                mappings = (proc / 'maps').read_text()
                if str(work) in mappings:
                    raise RuntimeError('an earlier moving fixture still maps its staged files')
                for line in mappings.splitlines():
                    if '/steamapps/common/' not in line:
                        continue
                    component = line.split('/steamapps/common/', 1)[1].split('/', 1)[0]
                    if not component.startswith(('Proton', 'SteamLinuxRuntime')):
                        raise RuntimeError('a Steam application is mapped; close it before the GPU fixture')
            except (FileNotFoundError, ProcessLookupError):
                continue
            except PermissionError:
                if proc.exists() and proc.stat().st_uid == os.getuid() and comm not in ('systemd', '(sd-pam)'):
                    raise RuntimeError('cannot inspect an owned process for active game mappings')
        yield


def execute(command, *, cwd, env, log, timeout):
    with log.open('wb') as stream:
        child = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return child.wait(timeout=timeout)
        except BaseException:
            # Proton's wrapper may exit before its Wine descendants. Always signal the entire
            # session's process group on timeout/interruption, even when the leader already exited.
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                pass
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
            raise


def gpu_state():
    hardware = subprocess.run(['nvidia-smi', '--query-gpu=name,driver_version,pstate,clocks.gr,clocks.mem',
                               '--format=csv,noheader'], capture_output=True, text=True)
    return dict(query=hardware.stdout.strip(), returncode=hardware.returncode)


def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run_trials(runner, proton, env, dll):
    root = runner.RUN
    gpu_states = {}
    # Invalidate success evidence before any trial mutation, including failure/timeout paths.
    for name in ('moving-report.json', 'moving-report.json.tmp', 'provenance.json'):
        (root / name).unlink(missing_ok=True)
    for name in ('off-a', 'on-a', 'on-b', 'off-b'):
        directory = root / name
        if directory.exists():
            shutil.rmtree(directory)
    # A fresh bounded directory for every trial; never reuse output from an earlier invocation.
    for name in ('off-a', 'on-a', 'on-b', 'off-b'):
        directory = root / name
        directory.mkdir()
        for filename in ('OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'nvngx_dlssnr.dll',
                         'nvngx_dlss.dll', '_nvngx.dll', 'dlssnr-loopback.exe', 'scene.hlsl'):
            source = root / filename
            if not source.is_file():
                raise RuntimeError(f'moving prerequisite missing: {filename}')
            (directory / source.name).symlink_to(source.resolve())
        enabled = name.startswith('on')
        (directory / 'OptiScaler.ini').write_text(
            '; Controlled moving scene. Written by run.py; no game files are modified.\n'
            '[Upscalers]\nDx12Upscaler=dlss\n'
            '[FrameGen]\nFGOutput=none\n'
            '[Menu]\nOverlayMenu=true\n'
            '[Hotfix]\nCheckForUpdate=false\n'
            '[Log]\nLogToFile=true\nLogLevel=2\n'
            '[DlssNr]\nEnabled=' + str(enabled).lower() + '\n'
            'HookMethod=1\nWorkingScale=1\nWhitePointSource=0\nWhitePointScale=1\n'
            'ApplyModel=true\nTransferStrength=1\nColourStrength=1\nSessionGuard=false\n')
        print(f'  moving scene trial {name}: production DLAA, NR={enabled}', flush=True)
        gpu_states[name] = dict(before=gpu_state())
        returncode = execute([proton, 'run', directory / 'dlssnr-loopback.exe',
                              'OptiScaler.dll', '--moving-scene'], cwd=directory,
                             env=env, log=directory / 'proton.log', timeout=300)
        gpu_states[name]['after'] = gpu_state()
        if returncode:
            raise SystemExit(f'moving scene {name} failed ({returncode}); see {directory}')
        print(f'  {name}: 80 evaluations finished; see {directory / "moving.json"}', flush=True)
    status = subprocess.run(['git', 'status', '--porcelain=v1', '--untracked-files=normal'], cwd=runner.ROOT,
                            check=True, capture_output=True, text=True).stdout
    diff = subprocess.run(['git', 'diff', 'HEAD', '--binary'], cwd=runner.ROOT,
                          check=True, capture_output=True).stdout
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=runner.ROOT,
                          check=True, capture_output=True, text=True).stdout.strip()
    files = [root / 'OptiScaler.dll', root / 'nvngx.dll_dlssnr.dll', root / 'nvngx_dlssnr.dll',
             root / 'nvngx_dlss.dll', root / '_nvngx.dll', root / 'dlssnr-loopback.exe', root / 'scene.hlsl']
    source_files = [runner.HERE / name for name in ('harness.cpp', 'moving_scene.h', 'scene.hlsl',
                                                   'analyze_moving.py', 'run_moving.py', 'run.py')]
    provenance = dict(schema=1, git_head=head, git_status=status.splitlines(),
                      git_diff_sha256=hashlib.sha256(diff).hexdigest(),
                      files={p.name: dict(sha256=digest(p), size=p.stat().st_size) for p in files},
                      fixture_sources={p.name: digest(p) for p in source_files},
                      proton=str(proton), proton_sha256=digest(proton),
                      gpu_states=gpu_states,
                      environment={k: os.environ[k] for k in ('VKD3D_CONFIG', 'DXVK_CONFIG', 'PROTON_ENABLE_NVAPI',
                                                             'CUDA_VISIBLE_DEVICES') if k in os.environ},
                      timing_scope='public EvaluateFeature GPU duration, DLSS plus optional NR; warmup=64 n=16',
                      limitations=['unlocked clocks and concurrent GPU load can confound comparisons',
                                   'synthetic opaque cube, not game image quality or FPS'])
    (root / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    subprocess.run([sys.executable, str(runner.HERE / 'analyze_moving.py'), str(root)], check=True)
    print(f'  moving evidence: {root / "moving-report.json"}', flush=True)
