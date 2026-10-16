#!/usr/bin/env python3
"""MSVC/DirectXMath and real D3D12 regression fixture, including failing mutation controls."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
WORK = OUT / 'run'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def generate():
    inputs = (ROOT / 'OptiScaler/inputs/FG/Streamline_Inputs.cpp').read_text()
    header = (ROOT / 'OptiScaler/framegen/IFGFeature_Dx12.h').read_text()
    fakes = (ROOT / 'tests/fg-tag-extents/fakes.h').read_text()
    # Keep the same surrounding consumer/config shims, replace all fake D3D12 types with the real API.
    begin = fakes.index('using UINT = unsigned;')
    end = fakes.index('using VkImage =', begin)
    fakes = fakes[:begin] + fakes[end:]
    fakes = fakes.replace('    bool enabled = true;', '    bool enabled = true;\n    Option<bool> FGSkipReset;')
    resource = header[header.index('struct Dx12Resource'):header.index('struct LockedDx12Resource')]
    fakes = fakes.replace('// @resource@', resource)
    adapter = function(inputs, 'static std::optional<FG_ResourceType> GetResourceType(')
    adapter += '\n' + function(inputs, 'bool Sl_Inputs::reportResource(')
    origins = ('        res.left = tag.extent ? tag.extent.left : 0;',
               '        res.top = tag.extent ? tag.extent.top : 0;')
    for statement in origins:
        if adapter.count(statement) != 1:
            raise RuntimeError('cannot instrument tag origin mutation')
        adapter = adapter.replace(statement, '#ifndef MUTATE_ORIGIN\n' + statement + '\n#endif')
    (OUT / 'tag_production.inc').write_text(fakes + adapter)
    output = (ROOT / 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp').read_text()
    begin = output.index('    sl::Constants constData = {};')
    end = output.index('    auto frameId = static_cast<uint32_t>(willDispatchFrame);', begin)
    apply = '    _gameCamera.Apply(willDispatchFrame, constData);'
    if output.count(apply) != 1:
        raise RuntimeError('cannot instrument camera mutation')
    camera = output[begin:end]
    # Preserve the actual order, including backend metadata assignments which could overwrite the camera.
    ortho = '    constData.orthographicProjection = sl::Boolean::eFalse;'
    camera = camera.replace(ortho, '#ifdef MUTATE_CAMERA_ORDER\n' + apply + '\n#endif\n' + ortho)
    camera = camera.replace(apply + '\n\n', '#if !defined(MUTATE_CAMERA) && !defined(MUTATE_CAMERA_ORDER)\n' + apply + '\n#endif\n\n')
    (OUT / 'camera_production.inc').write_text(camera + '\nreceived = constData;\nreturn true;\n')


def guard():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        comm = ''
        try:
            comm = (proc / 'comm').read_text().strip().lower()
            if comm in ('msbuild.exe', 'cl.exe', 'link.exe'):
                raise RuntimeError('compiler prefix is already in use; retry after the build finishes')
            if '/steamapps/common/' in (proc / 'maps').read_text():
                raise RuntimeError('a game/Steam common application is mapped; close it before the GPU fixture')
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            # The user service manager/PAM session are non-dumpable and do not host games.
            if proc.exists() and proc.stat().st_uid == os.getuid() and comm not in ('systemd', '(sd-pam)'):
                raise RuntimeError('cannot inspect an owned process for active game mappings')


def execute(args, env, log, timeout=180):
    print('+', ' '.join(map(str, args)), flush=True)
    with (OUT / log).open('w') as stream:
        process = subprocess.Popen(list(map(str, args)), cwd=WORK, env=env, stdin=subprocess.DEVNULL,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except BaseException:
            # Own the wrapper and its children; never reset the shared Wine prefix.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            raise
    return code, (OUT / log).read_text(errors='replace')


def stage(source):
    target = WORK / source.name
    temporary = target.with_suffix(target.suffix + '.stage')
    temporary.unlink(missing_ok=True)
    shutil.copy2(source, temporary)
    temporary.replace(target)


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    (OUT / 'result.json').write_text('{"status":"RUNNING"}\n')
    # One user command also retains the broader frame/lifecycle/invalid-input CPU coverage.
    for suite in ('fg-game-camera', 'fg-tag-extents'):
        subprocess.run([sys.executable, ROOT / 'tests' / suite / 'run.py'], check=True)
    lock_path = Path.home() / '.cache/retoque-wt/msvc-prefix.lock'
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('shared MSVC prefix is locked by another task')
        guard()
        generate()
        proton = next((path for path in sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('*/files/lib/wine'), reverse=True)
                       if (path / 'vkd3d-proton/x86_64-windows/d3d12.dll').exists()
                       and (path / 'dxvk/x86_64-windows/dxgi.dll').exists()), None)
        if proton is None:
            raise RuntimeError('no installed Proton with vkd3d-proton and DXVK')
        for name in ('d3d12.dll', 'd3d12core.dll'):
            stage(proton / 'vkd3d-proton/x86_64-windows' / name)
        stage(proton / 'dxvk/x86_64-windows/dxgi.dll')
        redists = sorted((MSVC.parents[1] / 'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'))
        if not redists:
            raise RuntimeError('MSVC runtime missing')
        for dll in redists[-1].glob('*.dll'):
            stage(dll)
        compile_env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
        runtime_env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
                           WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=n;vcruntime140,vcruntime140_1,msvcp140=n',
                           VKD3D_DEBUG='warn', DXVK_LOG_LEVEL='warn', VK_LOADER_LAYERS_DISABLE='~implicit~')
        results = {}
        for name, macro, expected_failure in (('current', None, None),
                ('no-camera', 'MUTATE_CAMERA', 'FAIL: camera projection preservation'),
                ('camera-order', 'MUTATE_CAMERA_ORDER', 'FAIL: camera optional values preservation'),
                ('zero-origin', 'MUTATE_ORIGIN', 'FAIL: crop pixels')):
            exe = WORK / (name + '.exe')
            command = [MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
                       '/I' + str(HERE), '/I' + str(OUT), '/I' + str(ROOT / 'retoque'),
                       '/I' + str(ROOT / 'external/streamline'), HERE / 'harness.cpp',
                       '/Fo' + str(OUT / (name + '.obj')), '/Fe' + str(exe)]
            if macro:
                command.append('/D' + macro)
            command += ['/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib']
            code, text = execute(command, compile_env, name + '-build.log', 300)
            if code:
                raise RuntimeError(name + ' build failed\n' + text[-5000:])
            code, text = execute(['wine', exe], runtime_env, name + '-run.log')
            if expected_failure:
                if code != 1 or expected_failure not in text:
                    raise RuntimeError(name + ' negative control did not fail as intended\n' + text[-4000:])
                results[name] = {'status': 'EXPECTED_FAIL', 'reason': expected_failure}
            else:
                required = ('PASS: camera DirectXMath cases=', 'PASS: D3D12 tag crop readback cases=64',
                            'PASS: FG inputs D3D12 fixture', 'DEBUG_LAYER=')
                if code or any(marker not in text for marker in required):
                    raise RuntimeError('current fixture failed or had zero coverage\n' + text[-5000:])
                results[name] = {'status': 'PASS', 'output': [line for line in text.splitlines()
                                  if line.startswith(('PASS:', 'DEBUG_LAYER='))]}
            print(name + ': ' + results[name]['status'], flush=True)
        sources = ['OptiScaler/inputs/FG/Streamline_Inputs.cpp', 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp',
                   'OptiScaler/framegen/IFGFeature_Dx12.h', 'retoque/framegen/dlssg/GameCamera.h',
                   'tests/fg-inputs-d3d12/harness.cpp', 'tests/fg-inputs-d3d12/camera.h',
                   'tests/fg-inputs-d3d12/run.py', 'tests/fg-tag-extents/fakes.h']
        result = {'status': 'PASS', 'cases': results,
                  'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  'dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
                  'sources': {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                  'runtime': str(proton), 'limits': ['production slices, not the complete OptiScaler DLL',
                  'SDK consumer is captured, no real Streamline/DLSS-G inference or generated frames',
                  'camera math is CPU DirectXMath; GPU executes the texture transport oracle',
                  'native D3D12 validation only when DEBUG_LAYER=active']}
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('PASS: automated FG input regression; ' + str(OUT / 'result.json'))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / 'result.json').write_text(json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
