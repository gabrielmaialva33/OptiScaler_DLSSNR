#!/usr/bin/env python3
"""Explicitly invoked, disposable native GPU probe; never registered in CI."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.request

REV = "374959484e79a640feaba44c93ac8cfb0a03f5b5"
MODEL = "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1"
ARCHIVE = "lib/Linux_x86_64/libnvsdk_ngx.a"
PINNED = {
    MODEL: "7561f16cab74e2b7ccf791bf44bb9cc43abb44bcf78f33c5663520ad4e857e02",
    ARCHIVE: "3321c4ca9adf71f345550d10bf2148ed38003151b03ec5f81c99d00949ab8202",
}
HEADERS = ["nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers_vk.h",
           "nvsdk_ngx_helpers.h", "nvsdk_ngx_params.h", "nvsdk_ngx_vk.h"]


def no_game():
    matches = []
    for p in Path('/proc').glob('[0-9]*/maps'):
        try:
            if 'steamapps/common' in p.read_text():
                matches.append(str(p))
        except FileNotFoundError:
            pass
        except PermissionError:
            pass  # Same visibility as the required grep /proc maps check.
    if matches:
        raise SystemExit('Game mapped; refusing GPU work: ' + ', '.join(matches))


def sha(p):
    with p.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cache', type=Path, default=Path.home() / '.cache/retoque-native-ngx')
    ap.add_argument('--run', action='store_true', help='required to execute GPU work')
    args = ap.parse_args()
    if not args.run:
        ap.error('pass --run explicitly; this is not a host/CI test')
    no_game()
    root = Path(__file__).resolve().parents[3]
    cache = args.cache.resolve()
    sdk = cache / REV
    result = cache / ('run-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
    result.mkdir(parents=True, exist_ok=False)
    for name in [*('include/' + h for h in HEADERS), MODEL, ARCHIVE]:
        p = sdk / name
        if not p.exists():
            p.parent.mkdir(parents=True, exist_ok=True)
            url = f'https://raw.githubusercontent.com/NVIDIA/DLSS/{REV}/{name}'
            data = urllib.request.urlopen(url, timeout=60).read()
            if data.startswith(b'version https://git-lfs.github.com/spec'):
                data = urllib.request.urlopen(f'https://media.githubusercontent.com/media/NVIDIA/DLSS/{REV}/{name}', timeout=60).read()
            p.write_bytes(data)
        if name in PINNED and sha(p) != PINNED[name]:
            raise SystemExit(f'hash mismatch: {p}')
    command = ['g++', '-std=c++17', '-O0', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', '-isystem', str(root / 'external/vulkan/include'),
               '-isystem', str(sdk / 'include'), str(Path(__file__).with_name('probe.cc')),
               str(sdk / ARCHIVE), '-lvulkan', '-ldl', '-pthread', '-o', str(result / 'probe')]
    (result / 'build-command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (result / 'build.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    # Per-process config only. This alone DOES NOT prevent updater exec; probe.cc's
    # inherited seccomp guard is the enforcement, including for the first SDK call.
    config = result / 'ngx-no-update.json'
    config.write_text(json.dumps({'file_format_version': '1.0.0', 'allow_ngx_updater': False,
                                 'ngx_models_path': str(result / 'no-updates')}) + '\n')
    env = os.environ.copy()
    env.pop('__NV_SIGNED_LOAD_CHECK', None)
    env.pop('PROTON_ENABLE_NGX_UPDATER', None)
    env['__NGX_CONF_FILE'] = str(config)
    env['PATH'] = '/usr/bin'  # make the denied exec trace concise; no global change
    for filename, cmd in [
        ('packages.txt', ['pacman', '-Q', 'nvidia-utils', 'vulkan-tools', 'vulkan-icd-loader', 'hyprland', 'gcc', 'strace']),
        ('gpu.txt', ['nvidia-smi', '--query-gpu=name,driver_version,memory.total,memory.used,pstate,clocks.current.graphics,clocks.current.memory,power.limit,power.draw', '--format=csv']),
        ('vulkan-headers.txt', ['git', '-C', str(root), 'ls-tree', 'HEAD', 'external/vulkan']),
        ('vulkaninfo.txt', ['vulkaninfo']),
    ]:
        no_game()
        with (result / filename).open('w') as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
    no_game()
    with (result / 'probe.log').open('w') as log:
        run = subprocess.run(['timeout', '90s', 'strace', '-f', '-e', 'trace=%file,process,network', '-s', '512',
                              '-o', str(result / 'trace.log'), str(result / 'probe'),
                              str(sdk / MODEL).rsplit('/', 1)[0], str(result)],
                             env=env, stdout=log, stderr=subprocess.STDOUT, timeout=100)
    trace = (result / 'trace.log').read_text()
    updater_exec = [l for l in trace.splitlines() if 'execve' in l and 'nvidia-ngx-updater' in l
                    and re.search(r'= 0$', l)]
    modules = sorted({m for m in re.findall(r'"([^"\n]*libnvidia-ngx[^"\n]*)"', trace)})
    (result / 'module-paths.json').write_text(json.dumps(modules, indent=2) + '\n')
    discovery = [l for l in (result / 'probe.log').read_text().splitlines()
                 if 'NGXLoadFromPath' in l or 'ModuleName' in l]
    (result / 'model-discovery.txt').write_text('\n'.join(discovery) + '\n')
    hashes = {str(p): sha(p) for p in sdk.rglob('*') if p.is_file()}
    # Hash actual ELF modules opened, as well as the SDK/model and source. Retain
    # every attempted file operation in trace.log, including nonexistent NR paths.
    opened = sorted(set(re.findall(r'openat\([^\n]*?"(/[^"\n]+)"[^\n]*?= [0-9]+$', trace, re.M)))
    (result / 'opened-paths.json').write_text(json.dumps(opened, indent=2) + '\n')
    for name in opened:
        p = Path(name)
        if p.is_file() and not name.startswith(('/proc/', '/sys/', '/dev/')):
            with p.open('rb') as f:
                if f.read(4) == b'\x7fELF':
                    hashes[str(p.resolve())] = sha(p)
    for p in [Path('/usr/lib/libnvidia-ngx.so.1').resolve(), Path(__file__), Path(__file__).with_name('probe.cc')]:
        if p.exists():
            hashes[str(p)] = sha(p)
    for p in result.iterdir():
        if p.is_file():
            hashes[str(p)] = sha(p)
    (result / 'manifest.json').write_text(json.dumps({'sdk_commit': REV, 'exit_code': run.returncode,
                                                    'successful_updater_execs': updater_exec, 'sha256': hashes}, indent=2) + '\n')
    print(result)
    if updater_exec:
        raise SystemExit('ERROR: updater executed despite guard')
    if run.returncode:
        raise SystemExit(f'probe exit {run.returncode}: 2 invalid control/setup, 3 NR create needs evaluate follow-up; see logs')


if __name__ == '__main__':
    main()
