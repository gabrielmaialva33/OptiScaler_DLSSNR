#!/usr/bin/env python3
"""Build and run the synthesized-motion estimator on a real D3D12 device, and score it.

Compiles the production OptiScaler/shaders/synth_motion estimator with a pch/Logger shim, runs it in
an isolated Wine prefix on vkd3d-proton, and judges synth-motion-report.json against the thresholds
below. Never loads NGX, OptiScaler.dll or a game.
"""
import argparse
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
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
ESTIMATOR = ROOT / 'OptiScaler/shaders/synth_motion'
VKD3D = [
    Path.home() / '.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows',
    Path.home() / '.local/share/Steam/steamapps/common/Proton Hotfix/files/lib/wine/vkd3d-proton/x86_64-windows',
    *sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('GE-Proton*/files/lib/wine/vkd3d-proton/x86_64-windows'),
            reverse=True),
]

# ------------------------------------------------------------------------------------------------
# Thresholds, and why.
#
# Uniform integer pans are the easy case for a block matcher: every block has an exact match at an
# integer offset, so anything above a pixel of mean error is a bug, not an accuracy limit.
#   pans up to 16 px     mean EPE < 1.0 px and >= 90% of interior pixels within 1 px
#   48 px pans           mean EPE < 2.0 px and >= 80% within 1 px: the coarse pyramid levels carry
#                        these, and a coarser match is expected at the finest level
#   static               mean EPE < 0.25 px and >= 99% within 1 px: standing still must read as zero
#   moving object        object interior mean EPE < 2.0 px (its blocks are clean, its edges are
#                        excluded); background mean EPE < 0.5 px away from the object's band
#   sign                 every pan's median must match current->previous with +y down on each
#                        non-zero axis, within 1 px; an inverted axis fails outright
#   scene cut            the field must be zero at the cut frame (FFX writes zero vectors there);
#                        SceneCut(), read back from the GPU, must be raised within ReadbackSlots (4)
#                        frames of the cut, and never on any frame of any other sequence
#   abandon              the frame after an abandoned recording must measure two steps of motion
#                        (history not advanced), within 1 px; the frames after that, one step
# Only frames where Ready() is true are scored: the estimator yields no field for FFX's five warm-up
# frames after a reset. Every scored sequence must have at least MIN_READY such frames.
# GPU time is reported, never judged; it is labelled with the clock state.
# ------------------------------------------------------------------------------------------------
PAN_SMALL = dict(epe=1.0, within=0.90)
PAN_LARGE = dict(epe=2.0, within=0.80)
STATIC = dict(epe=0.25, within=0.99)
OBJECT_EPE, BACKGROUND_EPE = 2.0, 0.5
CUT_FRAME, ABANDON_FRAME = 8, 8
CUT_WINDOW = 4  # SynthMotion::Estimator_Dx12::ReadbackSlots
MIN_READY = 4
SCORED_FROM = 6  # timing: past the warm-up


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def estimator_sources():
    return sorted(ESTIMATOR.glob('*.cpp')), sorted(ESTIMATOR.glob('*.h')) + sorted((ESTIMATOR / 'precompile').glob('*_Shader*.h'))


def sources():
    cpp, headers = estimator_sources()
    files = [HERE / 'harness.cpp', HERE / 'run.py'] + sorted((HERE / 'stubs').glob('*.h')) + cpp + headers
    return {str(p.relative_to(ROOT)): digest(p) for p in files}


def command(args, *, env, logfile, cwd=ROOT, timeout=600, expected=0):
    print('+', ' '.join(map(str, args)), flush=True)
    with logfile.open('w') as log:
        p = subprocess.run(list(map(str, args)), env=env, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                           stdin=subprocess.DEVNULL, timeout=timeout)
    if p.returncode != expected:
        raise RuntimeError(f'exit {p.returncode}, expected {expected}: {logfile}\n'
                           + logfile.read_text(errors='replace')[-4000:])


def refuse_if_prefix_busy():
    # The msvc-wine prefix has stalled with two clients on it; a shader rebuild (dxc) or a solution
    # build in flight is exactly that. This suite compiles in a private copy of the prefix, but still
    # stays out of the way of the shared one while it is busy.
    busy = subprocess.run(['pgrep', '-af', r'[M]SBuild\.exe|[b]uild-local\.sh|[d]xc\.exe'],
                          capture_output=True, text=True)
    if busy.returncode == 0:
        raise RuntimeError('the msvc-wine prefix is busy (build or dxc running); run again after it finishes:\n'
                           + busy.stdout)


def build():
    cpp, headers = estimator_sources()
    if not cpp or not any(h.name == 'SynthMotion_Dx12.h' for h in headers):
        raise RuntimeError(f'estimator not present: expected SynthMotion_Dx12.{{h,cpp}} under {ESTIMATOR}')
    if not list((ESTIMATOR / 'precompile').glob('*_Shader.h')):
        raise RuntimeError(f'estimator shaders not generated: no *_Shader.h under {ESTIMATOR / "precompile"}')
    refuse_if_prefix_busy()
    build_prefix = OUT / 'build-prefix'
    if not build_prefix.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(build_prefix)], check=True)
    env = dict(os.environ, WINEPREFIX=str(build_prefix), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    # This prefix belongs only to this suite; stopping its server touches nothing else.
    subprocess.run(['wineserver', '-k'], env=env, check=False)
    WORK.mkdir(parents=True, exist_ok=True)
    objdir = OUT / 'obj'
    objdir.mkdir(exist_ok=True)
    command([MSVC / 'cl', '/nologo', '/std:c++latest', '/EHsc', '/MD', '/W4', '/O2',
             # The stubs come first so that "pch.h" and <Logger.h> resolve to the test shims.
             '/I' + str(HERE / 'stubs'), '/I' + str(ESTIMATOR), '/I' + str(ROOT / 'OptiScaler'),
             '/I' + str(ROOT / 'OptiScaler/include'),
             HERE / 'harness.cpp', *cpp,
             '/Fo' + str(objdir) + '/', '/Fe' + str(WORK / 'synth-motion-harness.exe'), '/link',
             'd3d12.lib', 'dxgi.lib', 'dxguid.lib'], env=env, logfile=OUT / 'build.log', timeout=900)
    vkd3d = next((p for p in VKD3D if (p / 'd3d12.dll').exists()), None)
    if vkd3d is None:
        raise RuntimeError('no vkd3d-proton runtime found in any Proton install')
    # Proton ships these read-only and copy2 keeps the mode: replace, never write over.
    for name in ('d3d12.dll', 'd3d12core.dll'):
        (WORK / name).unlink(missing_ok=True)
        shutil.copy2(vkd3d / name, WORK / name)
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime missing')
    for dll in redists[-1].glob('*.dll'):
        (WORK / dll.name).unlink(missing_ok=True)
        shutil.copy2(dll, WORK / dll.name)
    (OUT / 'manifest.json').write_text(json.dumps({
        'sources': sources(),
        'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'vkd3d': str(vkd3d),
        'binaries': {p.name: digest(p) for p in WORK.iterdir() if p.suffix in ('.dll', '.exe')},
    }, indent=2) + '\n')


def lock_clocks(enabled):
    if not enabled:
        return False
    ok = all(subprocess.run(['sudo', '-n', 'nvidia-smi', *args], capture_output=True).returncode == 0
             for args in (['-lgc', '2100,2100'], ['-lmc', '10501']))
    if not ok:
        unlock_clocks(True)
    return ok


def unlock_clocks(locked):
    if locked:
        for args in (['-rgc'], ['-rmc']):
            subprocess.run(['sudo', '-n', 'nvidia-smi', *args], capture_output=True)


def scored(frames, skip=()):
    return [f for f in frames if f['ready'] and not f['abandoned'] and f['t'] not in skip and f['scores']]


def region(frame, name):
    return next((s for s in frame['scores'] if s['region'] == name), None)


def mean(values):
    return sum(values) / len(values) if values else float('nan')


def judge(report):
    failures, rows, notes = [], [], []
    seqs = {s['name']: s for s in report['sequences']}

    def check(cond, message):
        if not cond:
            failures.append(message)

    # Pans: accuracy and sign.
    for s in report['sequences']:
        if not s['name'].startswith('pan_'):
            continue
        frames = scored(s['frames'])
        check(len(frames) >= MIN_READY, f'{s["name"]}: only {len(frames)} ready frames (field never produced?)')
        if not frames:
            continue
        epe = mean([region(f, 'interior')['epe_mean'] for f in frames])
        within = mean([region(f, 'interior')['within_1px'] for f in frames])
        mx = mean([region(f, 'interior')['median_x'] for f in frames])
        my = mean([region(f, 'interior')['median_y'] for f in frames])
        big = max(abs(s['dx']), abs(s['dy'])) > 16
        lim = PAN_LARGE if big else PAN_SMALL
        check(epe < lim['epe'], f'{s["name"]}: mean EPE {epe:.3f} >= {lim["epe"]}')
        check(within >= lim['within'], f'{s["name"]}: within-1px {within:.3f} < {lim["within"]}')
        signs = []
        for axis, d, m in (('x', s['dx'], mx), ('y', s['dy'], my)):
            if d == 0:
                continue
            if abs(m - (-d)) < 1.0:
                signs.append(f'{axis}: current->previous')
            elif abs(m - d) < 1.0:
                signs.append(f'{axis}: INVERTED')
                failures.append(f'{s["name"]}: {axis} axis inverted (median {m:.2f}, content moved {d:+d})')
            else:
                signs.append(f'{axis}: unclear ({m:.2f})')
                failures.append(f'{s["name"]}: {axis} median {m:.2f} matches neither {-d} nor {d}')
        gpu = mean([f['gpu_ms'] for f in frames])
        rows.append((s['name'], f'{epe:.3f}', f'{within * 100:.1f}%', ', '.join(signs), '-', f'{gpu:.3f}'))

    # Static.
    if 'static' in seqs:
        frames = scored(seqs['static']['frames'])
        check(len(frames) >= MIN_READY, f'static: only {len(frames)} ready frames')
        epe = mean([region(f, 'interior')['epe_mean'] for f in frames])
        within = mean([region(f, 'interior')['within_1px'] for f in frames])
        check(epe < STATIC['epe'], f'static: mean EPE {epe:.3f} >= {STATIC["epe"]}')
        check(within >= STATIC['within'], f'static: within-1px {within:.3f} < {STATIC["within"]}')
        rows.append(('static', f'{epe:.3f}', f'{within * 100:.1f}%', '-', '-',
                     f'{mean([f["gpu_ms"] for f in frames]):.3f}'))

    # Moving object.
    if 'object' in seqs:
        frames = scored(seqs['object']['frames'])
        check(len(frames) >= MIN_READY, f'object: only {len(frames)} ready frames')
        oe = mean([region(f, 'object')['epe_mean'] for f in frames])
        be = mean([region(f, 'background')['epe_mean'] for f in frames])
        ow = mean([region(f, 'object')['within_1px'] for f in frames])
        check(oe < OBJECT_EPE, f'object: interior mean EPE {oe:.3f} >= {OBJECT_EPE}')
        check(be < BACKGROUND_EPE, f'object: background mean EPE {be:.3f} >= {BACKGROUND_EPE}')
        rows.append(('object', f'obj {oe:.3f} / bg {be:.3f}', f'obj {ow * 100:.1f}%', '-', '-',
                     f'{mean([f["gpu_ms"] for f in frames]):.3f}'))

    # Scene cut: zero field at the cut frame, SceneCut() within the readback window, nowhere else.
    if 'cut' in seqs:
        frames = seqs['cut']['frames']
        flags = {f['t']: f['scene_cut_after_record'] or f['scene_cut_after_confirm'] for f in frames}
        raised = [t_ for t_, v in flags.items() if v]
        window = range(CUT_FRAME, CUT_FRAME + CUT_WINDOW + 1)
        check(any(t_ in window for t_ in raised), f'cut: SceneCut() not raised within t={CUT_FRAME}..{CUT_FRAME + CUT_WINDOW} (raised at {raised})')
        stray = [t_ for t_ in raised if t_ not in window]
        check(not stray, f'cut: SceneCut() raised outside the window at {stray}')
        at_cut = next((f for f in frames if f['t'] == CUT_FRAME and f['scores']), None)
        zero = region(at_cut, 'cutzero') if at_cut else None
        check(zero is not None and zero['epe_mean'] < 1.0,
              f'cut: field at the cut frame is not zero (mean |v| {zero["epe_mean"] if zero else "n/a"})')
        steady = scored(frames, skip=(CUT_FRAME,))
        first = min(raised) if raised else None
        # Not judged, reported: frames after the cut that claim Ready() while the GPU already zeroed
        # the field, because SceneCut() is only known after its readback.
        zero_ready = [f['t'] for f in frames if CUT_FRAME < f['t'] <= CUT_FRAME + CUT_WINDOW and f['ready']
                      and f['scores'] and region(f, 'interior') and abs(region(f, 'interior')['median_x']) < 0.5]
        if zero_ready:
            notes.append(f'cut: Ready() true with an all-zero field at t={zero_ready}, before SceneCut() arrived '
                         f'(t={first}); a consumer trusting Ready() sees zero motion and no reset across the cut')
        rows.append(('cut', f'{mean([region(f, "interior")["epe_mean"] for f in steady if region(f, "interior")]):.3f} (steady)',
                     f'|v| at cut {zero["epe_mean"]:.2f}' if zero else '-', '-',
                     f'raised at {raised}' + (f' (+{first - CUT_FRAME} frames)' if first is not None else ''),
                     f'{mean([f["gpu_ms"] for f in steady]):.3f}'))
    for s in report['sequences']:
        if s['name'] == 'cut':
            continue
        stray = [f['t'] for f in s['frames'] if f['scene_cut_after_record'] or f['scene_cut_after_confirm']]
        check(not stray, f'{s["name"]}: SceneCut() raised at {stray} with no cut in the content')

    # Abandon: history must not advance on an abandoned recording.
    if 'abandon' in seqs:
        s = seqs['abandon']
        after = next((f for f in s['frames'] if f['t'] == ABANDON_FRAME + 1), None)
        check(after is not None and after['ready'], f'abandon: frame {ABANDON_FRAME + 1} not ready after an abandoned recording')
        ok = after is not None and after['scores'] and abs(region(after, 'interior')['median_x'] - (-2 * s['dx'])) < 1.0
        check(ok, f'abandon: frame {ABANDON_FRAME + 1} median x '
                  f'{region(after, "interior")["median_x"] if after and after["scores"] else "n/a"} != {-2 * s["dx"]} '
                  '(an abandoned recording advanced the history?)')
        later = [f for f in s['frames'] if f['t'] > ABANDON_FRAME + 1 and f['ready'] and f['scores']]
        for f in later:
            check(abs(region(f, 'interior')['median_x'] - (-s['dx'])) < 1.0, f'abandon: frame {f["t"]} not one step')
        rows.append(('abandon', f'{region(after, "interior")["epe_mean"]:.3f}' if after and after['scores'] else 'n/a',
                     '-', f't={ABANDON_FRAME + 1}: {region(after, "interior")["median_x"]:.2f} (want {-2 * s["dx"]})'
                     if after and after['scores'] else '-', '-', '-'))

    # Timing-only sequences.
    timing = {}
    for s in report['sequences']:
        if s['timing_only']:
            ms = sorted(f['gpu_ms'] for f in s['frames'] if f['t'] >= SCORED_FROM and f['gpu_ms'] > 0)
            timing[s['name']] = {'width': s['width'], 'height': s['height'],
                                 'median_ms': ms[len(ms) // 2] if ms else None,
                                 'p90_ms': ms[int(len(ms) * 0.9)] if ms else None}
    # Field extent actually produced (the interface promises the colour extent).
    extents = sorted({(f['field_width'], f['field_height'], s['width'], s['height'])
                      for s in report['sequences'] for f in s['frames'] if f['field_width']})
    return failures, rows, timing, extents, notes


def execute(lock):
    manifest = json.loads((OUT / 'manifest.json').read_text())
    if manifest['sources'] != sources():
        raise RuntimeError('sources changed after the harness was built; rebuild required')
    for name, checksum in manifest['binaries'].items():
        if digest(WORK / name) != checksum:
            raise RuntimeError('binary changed after build: ' + name)
    if any(WORK.glob('*ngx*.dll')) or any(WORK.glob('OptiScaler*.dll')):
        raise RuntimeError('unexpected NGX/OptiScaler DLL in the isolated harness; refusing to run')
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='d3d12=n;d3d12core=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n',
               VKD3D_DEBUG='warn', VK_LOADER_LAYERS_DISABLE='~implicit~')
    report_path = WORK / 'synth-motion-report.json'
    report_path.unlink(missing_ok=True)
    locked = lock_clocks(lock)
    try:
        command(['wine', WORK / 'synth-motion-harness.exe', '--report', 'synth-motion-report.json'], env=env,
                cwd=WORK, logfile=OUT / 'run.log', timeout=600)
    finally:
        unlock_clocks(locked)
    (OUT / 'clock.txt').write_text('locked 2100/10501 MHz\n' if locked else 'UNLOCKED\n')
    report_judgement(manifest)


def report_judgement(manifest):
    report_path = WORK / 'synth-motion-report.json'
    log = (OUT / 'run.log').read_text(errors='replace') if (OUT / 'run.log').exists() else ''
    if 'SYNTH-MOTION DONE' not in log or not report_path.exists():
        raise RuntimeError('ZERO COVERAGE: the harness did not complete every sequence')
    report = json.loads(report_path.read_text())
    failures, rows, timing, extents, notes = judge(report)
    locked = (OUT / 'clock.txt').exists() and (OUT / 'clock.txt').read_text().startswith('locked')
    clock = 'locked 2100/10501 MHz' if locked else 'UNLOCKED clocks (numbers indicative only)'

    print(f'\n{"sequence":<12} {"EPE px":<20} {"<1px":<12} {"sign":<44} {"scene cut":<26} gpu ms')
    for r in rows:
        print(f'{r[0]:<12} {r[1]:<20} {r[2]:<12} {r[3]:<44} {r[4]:<26} {r[5]}')
    print(f'\ntiming ({clock}):')
    for name, t in timing.items():
        print(f'  {name}: {t["width"]}x{t["height"]} median {t["median_ms"]} ms, p90 {t["p90_ms"]} ms')
    print(f'field extents (field WxH for colour WxH): {extents}')
    for n in notes:
        print('note: ' + n)

    result = {'status': 'PASS' if not failures else 'FAIL', 'failures': failures, 'notes': notes, 'clock': clock,
              'timing': timing, 'extents': extents, 'manifest': manifest,
              'limits': 'Synthetic content, integer motion, RGBA8 SDR input, one device and queue. Not game footage.'}
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if failures:
        print('\nSYNTH-MOTION FAIL:')
        for f in failures:
            print('  - ' + f)
        raise SystemExit(1)
    print(f'\nSYNTH-MOTION PASS: {len(rows)} scored sequences, sign current->previous (+y down), {clock}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--run-only', action='store_true')
    ap.add_argument('--no-lock', action='store_true', help='do not try to lock GPU clocks for the timing numbers')
    ap.add_argument('--judge-only', action='store_true', help='re-score the last run report without running anything')
    args = ap.parse_args()
    if args.build_only and args.run_only:
        ap.error('choose at most one mode')
    OUT.mkdir(exist_ok=True)
    if args.judge_only:
        report_judgement(json.loads((OUT / 'manifest.json').read_text()))
        return
    (OUT / 'result.json').write_text('{"status":"RUNNING"}\n')
    if not args.run_only:
        build()
    if not args.build_only:
        execute(not args.no_lock)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyError, ValueError, TypeError) as error:
        OUT.mkdir(exist_ok=True)
        (OUT / 'result.json').write_text(json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
