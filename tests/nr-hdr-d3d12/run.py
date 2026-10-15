#!/usr/bin/env python3
"""Real D3D12 HDR conversion and swapchain probe; production shader/pass, no NGX/game/deployment.
Run under the shared msvc-prefix flock and only when no game has steamapps/common mappings.
"""
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
WORK = OUT / 'run'
MSVC = Path.home() / '.local/opt/msvc/bin/x64'
PREFIX = Path.home() / '.local/opt/msvc-wineprefix'
PROTON = next((path for path in sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('*/files/lib/wine'), reverse=True)
               if (path / 'vkd3d-proton/x86_64-windows/d3d12.dll').exists()
               and (path / 'dxvk/x86_64-windows/dxgi.dll').exists()), None)


def run(args, env, log, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    with (OUT / log).open('w') as output:
        proc = subprocess.run(list(map(str, args)), cwd=WORK, env=env, stdin=subprocess.DEVNULL,
                              stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
    if proc.returncode:
        raise RuntimeError(f'{log}: exit {proc.returncode}\n' + (OUT / log).read_text(errors='replace')[-6000:])


def stage_runtime(source):
    # Installed Proton DLLs can be read-only; replace our prior staged inode rather than overwriting it.
    target = WORK / source.name
    pending = target.with_suffix(target.suffix + '.stage')
    pending.unlink(missing_ok=True)
    shutil.copy2(source, pending)
    pending.replace(target)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    (OUT / 'result.json').write_text('{"status":"RUNNING"}\n')
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    sources = [HERE / 'harness.cpp', ROOT / 'retoque/shaders/dlssnr/DlssNr_PresentHdr_Dx12.cpp',
               ROOT / 'OptiScaler/shaders/Shader_Dx12.cpp', ROOT / 'OptiScaler/shaders/Shader_Common.cpp',
               ROOT / 'OptiScaler/gpu_time/GpuTime_Dx12.cpp']
    run([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
         '/I' + str(HERE / 'stubs'), '/I' + str(ROOT / 'retoque'), '/I' + str(ROOT / 'OptiScaler'),
         '/I' + str(ROOT / 'OptiScaler/include'), *sources, '/Fo' + str(OUT) + '/',
         '/Fe' + str(WORK / 'hdr-harness.exe'), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib',
         'd3dcompiler.lib', 'user32.lib'], env, 'build.log')
    if PROTON is None:
        raise RuntimeError('no local Proton with vkd3d-proton and DXVK')
    print('Runtime:', PROTON, flush=True)
    for name in ('d3d12.dll', 'd3d12core.dll'):
        stage_runtime(PROTON / 'vkd3d-proton/x86_64-windows' / name)
    # Native DXGI is needed for vkd3d-proton's swapchain interop. No files in a game/prefix are changed.
    stage_runtime(PROTON / 'dxvk/x86_64-windows/dxgi.dll')
    redists = sorted((MSVC.parents[1] / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('MSVC runtime missing')
    for dll in redists[-1].glob('*.dll'):
        stage_runtime(dll)
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;vcruntime140,vcruntime140_1,msvcp140=n',
               VKD3D_DEBUG='warn', DXVK_LOG_LEVEL='warn', VK_LOADER_LAYERS_DISABLE='~implicit~')
    # Do not force DXVK_HDR: report the runtime/display's actual advertised support.
    env.pop('DXVK_HDR', None)
    run(['wine', WORK / 'hdr-harness.exe'], env, 'run.log', 180)
    text = (OUT / 'run.log').read_text(errors='replace')
    for required in ('PASS: profile 1 identity', 'PASS: profile 2 identity', 'PASS: HDR conversion GPU checks',
                     'DISPLAY_WHITE=', 'HDR10_PRESENT='):
        if required not in text:
            raise RuntimeError('missing coverage: ' + required)
    hdr = next(line for line in text.splitlines() if line.startswith('HDR10_PRESENT='))
    result = {'status': 'PASS', 'hdr10_present': hdr, 'limits': 'Production conversion/base classes and shaders; no NGX or production present hook. Native debug layer and real HDR display still required.',
              'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'dxil_sha256': hashlib.sha256((ROOT / 'retoque/shaders/dlssnr/precompile/DlssNr_PresentHdr_Shader.cso').read_bytes()).hexdigest()}
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(text[-4000:])


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        OUT.mkdir(exist_ok=True)
        (OUT / 'result.json').write_text(json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
