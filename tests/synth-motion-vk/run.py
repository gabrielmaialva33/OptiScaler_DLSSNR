#!/usr/bin/env python3
"""Build and run the Vulkan synthesized-motion estimator on a real Vulkan device under Wine, and score it.

Compiles the production retoque/shaders/synth_motion/SynthMotion_Vk.cpp with a pch shim beside harness.cpp, runs it in
an isolated Wine prefix on winevulkan (the host's driver) under the Khronos validation layer's core and synchronization
checks, and judges synth-motion-report.json with tests/synth-motion-d3d12's thresholds (its judge()): the two
estimators share the frames, the sequences and the scores (sequences.h), so they are held to the same numbers. Never
loads NGX, OptiScaler.dll or a game.

On top of the D3D12 suite's judgement: the validation layer must have been active (a deliberately invalid fence flag
reached the messenger with its VUID) and must have reported no error. SKIP (exit 0) when the device cannot run the
estimator (SynthMotion::VkUnsupported says why)."""
import argparse
import hashlib
import importlib.util
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
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
ESTIMATOR = ROOT / 'retoque/shaders/synth_motion'
D3D12_SUITE = ROOT / 'tests/synth-motion-d3d12'
EXE = 'synth-motion-vk-harness.exe'


def d3d12_runner():
    """The D3D12 suite's run.py, for its thresholds and judge(): one contract, one judgement."""
    spec = importlib.util.spec_from_file_location('synth_motion_d3d12_run', D3D12_SUITE / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sources():
    precompile = ESTIMATOR / 'precompile'
    files = ([HERE / 'harness.cpp', HERE / 'run.py', D3D12_SUITE / 'sequences.h', D3D12_SUITE / 'run.py'] +
             sorted((HERE / 'stubs').glob('*.h')) + [ESTIMATOR / 'SynthMotion_Vk.h', ESTIMATOR / 'SynthMotion_Vk.cpp'] +
             sorted(precompile.glob('*_Shader_Vk.h')))
    return {str(p.relative_to(ROOT)): digest(p) for p in files}


def command(args, *, env, logfile, cwd=ROOT, timeout=600, expected=0):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    if p.returncode != expected:
        raise RuntimeError(f'exit {p.returncode}, expected {expected}: {logfile}\n'
                           + logfile.read_text(errors='replace')[-4000:])


def build(runner):
    if not list((ESTIMATOR / 'precompile').glob('*_Shader_Vk.h')):
        raise RuntimeError('the Vulkan estimator\'s SPIR-V is not generated: run retoque/shaders/synth_motion/'
                           'precompile/build.sh')
    runner.refuse_if_prefix_busy()
    build_prefix = OUT / 'build-prefix'
    if not build_prefix.exists():
        OUT.mkdir(parents=True, exist_ok=True)
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(build_prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(build_prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    WORK.mkdir(parents=True, exist_ok=True)
    objdir = OUT / 'obj'
    objdir.mkdir(exist_ok=True)
    try:
        command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
                 # The stub comes first so that "pch.h" resolves to the test shim.
                 '/I' + str(HERE / 'stubs'), '/I' + str(ESTIMATOR), '/I' + str(ROOT / 'external/vulkan/include'),
                 HERE / 'harness.cpp', ESTIMATOR / 'SynthMotion_Vk.cpp',
                 '/Fo' + str(objdir) + '/', '/Fe' + str(WORK / EXE), '/link',
                 '/LIBPATH:' + str(ROOT / 'OptiScaler/library/vulkan'), 'vulkan-1.lib'],
                env=env, logfile=OUT / 'build.log', timeout=900)
    finally:
        # This prefix belongs only to this suite; stopping its server touches nothing else.
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime missing')
    for dll in redists[-1].glob('*.dll'):
        (WORK / dll.name).unlink(missing_ok=True)
        shutil.copy2(dll, WORK / dll.name)
    (OUT / 'manifest.json').write_text(json.dumps({
        'sources': sources(),
        'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'binaries': {p.name: digest(p) for p in WORK.iterdir() if p.suffix in ('.dll', '.exe')},
    }, indent=2) + '\n')


def runtime_env():
    return dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
                WINEDLLOVERRIDES='vcruntime140=n;vcruntime140_1=n;msvcp140=n',
                VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
                VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0')


def check_manifest():
    manifest = json.loads((OUT / 'manifest.json').read_text())
    if manifest['sources'] != sources():
        changed = sorted(k for k in set(manifest['sources']) | set(sources())
                         if manifest['sources'].get(k) != sources().get(k))
        raise RuntimeError('--run-only with sources changed since the build: ' + ', '.join(changed))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    args = parser.parse_args()

    runner = d3d12_runner()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.run_only:
        build(runner)
    else:
        check_manifest()
    if args.build_only:
        print('BUILD OK')
        return 0

    env = runtime_env()
    report_path = WORK / 'synth-motion-report.json'
    report_path.unlink(missing_ok=True)
    try:
        command(['wine', WORK / EXE, '--report', 'synth-motion-report.json'], env=env, cwd=WORK,
                logfile=OUT / 'run.log', timeout=900)
    finally:
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)

    report = json.loads(report_path.read_text())
    result = {'status': 'PASS', 'failures': [], 'notes': []}
    if report.get('skipped'):
        result.update(status='SKIP', reason=report['skipped'])
        (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'SKIP: {report["skipped"]}')
        return 0

    failures, rows, timing, extents, notes, overlays = runner.judge(report)
    if not report.get('validation_active'):
        failures.append('the validation layer was not active (the negative control never reached the messenger)')
    if report.get('validation_errors', 1) != 0:
        failures.append(f'{report.get("validation_errors")} validation error(s); see artifacts/run.log')
    for width, height, colour_width, colour_height in extents:
        if (width, height) != (colour_width, colour_height):
            failures.append(f'a field of {width}x{height} for a {colour_width}x{colour_height} colour')

    print(f'device: {report.get("device")}; validation active, {report.get("validation_errors")} error(s)')
    print(f'{"sequence":<14} {"EPE":<22} {"within 1 px":<16} {"sign / overlay":<40} {"scene cut":<26} GPU ms')
    for row in rows:
        print(f'{row[0]:<14} {row[1]:<22} {row[2]:<16} {row[3]:<40} {row[4]:<26} {row[5]}')
    for name, t in timing.items():
        print(f'{name}: {t["width"]}x{t["height"]} median {t["median_ms"]:.3f} ms, p90 {t["p90_ms"]:.3f} ms '
              '(clocks UNLOCKED)')
    for note in notes:
        print('NOTE:', note)

    result.update(failures=failures, notes=notes, timing=timing, device=report.get('device'),
                  status='FAIL' if failures else 'PASS')
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if failures:
        for failure in failures:
            print('FAIL:', failure)
        return 1
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
