#!/usr/bin/env python3
"""Build and run the synthesized-motion estimator on a real D3D12 device, and score it.

Compiles the production OptiScaler/shaders/synth_motion estimator with a pch/Logger shim, runs it in
an isolated Wine prefix on vkd3d-proton, and judges synth-motion-report.json against the thresholds
below. Never loads NGX, OptiScaler.dll or a game.

--source nvofa runs the same sequences through the NVIDIA Optical Flow source instead
(SynthMotionNvofa_Dx12, through dxvk-nvapi's nvofapi64.dll from a Proton install). It reports SKIP,
not FAIL, when that DLL, the source's generated shaders, or the engine itself is unavailable.
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
# dxvk-nvapi's nvofapi64.dll, for --source nvofa. The Proton that supplies vkd3d-proton is preferred,
# since dxvk-nvapi reaches the Vulkan device through vkd3d-proton's interop interfaces.
NVOFAPI = [v.parent.parent / 'nvapi/x86_64-windows' for v in VKD3D]
NVOFA_SHADERS = ('SynthMotion_NvofaPrep_Shader.h', 'SynthMotion_NvofaExpand_Shader.h')

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
#   static overlay       a crosshair, a HUD panel and outlined glyphs held still over a background
#                        panning (+8, 0) and (+8, +8) px per frame (synthesized-motion.md, "Static
#                        overlays"). Two judgements on the overlay:
#                          smeared   at most OVERLAY_SMEARED of all overlay pixels, and at most
#                                    ELEMENT_SMEARED of each element's (crosshair, panel, floating text;
#                                    the large panel would otherwise dilute a smeared crosshair), may
#                                    carry a vector over 0.5 px whose displacement reads another colour:
#                                    what frame generation drags. A stroke or fill displaced along itself
#                                    reads the same overlay and is not smeared.
#                          clear     the overlay pixels the pair can show are static (the harness leaves
#                                    out any whose 3x3 the camera's motion reproduces exactly) must have
#                                    a mean |v| under OVERLAY_EPE px.
#                        Before the expand's per-pixel choice (2026-09-28) the camera's block vector
#                        was spread over them. The background away from the overlay is held to the
#                        small-pan thresholds. The share of overlay pixels with |v| > 0.5 px, the
#                        ambiguous pixels and the band around the elements are reported, never judged.
# Only frames where Ready() is true are scored: the estimator yields no field for FFX's five warm-up
# frames after a reset. Every scored sequence must have at least MIN_READY such frames.
# GPU time is reported, never judged; it is labelled with the clock state.
#
# --source nvofa: the same thresholds for pans, static and the object -- they are about correctness,
# and the engine is quarter-pel on a grid finer than the block matcher's. What differs is contract:
#   lag                  the field is lag_frames (1) late, so the harness scores frame t against the
#                        truth of t - lag; the abandon's two-step frame moves to t=10
#   scene cut            the source has no scene-cut detection: SceneCut() must never be raised, the
#                        pair spanning the cut is not scored, and frames from CUT_FRAME + 3 on (two
#                        clean pairs after it) must be as accurate as a small pan
#   GPU time             covers the two shader passes on the list only; the engine runs on its own
#                        queue and is not timed here
# ------------------------------------------------------------------------------------------------
#
# Synthesized FG's HUD mask (SynthMotion::Overlay_Dx12), run on every frame beside the estimator, whatever
# the source (synthesized-frame-generation.md, "The HUD: near depth and a UI layer"). Its depth is what is
# judged, as FSR-FG is handed it: 1.0 near where the mask holds, 0.0 elsewhere.
#   exact                on every scored frame of every sequence: the depth is exactly 0 or 1 and agrees
#                        with the mask, and the UI layer is the frame's own rgb with the mask as alpha,
#                        byte for byte
#   no scenery           pans, the moving object, static, cut and abandon: not one pixel marked, on any
#                        frame. A false positive is a hole cut in whatever crosses it in a generated frame
#   precision            overlay_* and hud_*, from HUD_FROM: at least HUD_PRECISION of the marked pixels are
#                        overlay pixels or within 1 px of one (the rule grows its protection by 1 px on purpose)
#   recall               hud_* (the same overlay, 40 frames), mean over t >= HUD_STEADY: the share of each
#                        element's pixels marked, at least HUD_MIN_RECALL for the crosshair and the floating
#                        text, which are what the mask is for. About half of each is its 1 px dark outline,
#                        which the rule never marks (its 3x3 core reaches the scene), and the protection builds
#                        up as the scene moves past, so these are floors well under the 0.34/0.39 (8 px a
#                        frame) and 0.21/0.30 (3, 2) measured on 2026-09-28. The panel is reported only: a
#                        flat 208x76 panel is marked nowhere, its interior never sees motion on four sides
#                        (hud-protection.md, "What it costs in recall"). overlay_* is too short for recall
#   GPU time             of the mask plus its layer, reported, never judged
# HUD_FROM: t=0 has no previous frame, the rule's 8-frame entry streak protects from t=8, and the export,
# which reads last frame's protection, shows it from t=9; t=10 leaves one frame of margin.
HUD_FROM = 10
HUD_STEADY = 30
HUD_PRECISION = 0.99
HUD_MIN_RECALL = {'crosshair': 0.10, 'text': 0.15}
PAN_SMALL = dict(epe=1.0, within=0.90)
PAN_LARGE = dict(epe=2.0, within=0.80)
STATIC = dict(epe=0.25, within=0.99)
OBJECT_EPE, BACKGROUND_EPE = 2.0, 0.5
OVERLAY_SMEARED, ELEMENT_SMEARED, OVERLAY_EPE = 0.02, 0.05, 0.5
OVERLAY_ELEMENTS = ('crosshair', 'panel', 'text')
CUT_FRAME, ABANDON_FRAME = 8, 8
CUT_WINDOW = 4  # SynthMotion::Estimator_Dx12::ReadbackSlots
MIN_READY = 4
SCORED_FROM = 6  # timing: past the warm-up


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def artifact(name, source):
    # One set of run artifacts per source; the ffx names are the historical ones.
    if source == 'ffx':
        return name
    stem, dot, ext = name.partition('.')
    return f'{stem}-{source}{dot}{ext}'


def nvofapi():
    return next((p / 'nvofapi64.dll' for p in NVOFAPI if (p / 'nvofapi64.dll').exists()), None)


def nvofa_unavailable():
    # Why --source nvofa cannot run here, before anything is built; None when it can.
    missing = [h for h in NVOFA_SHADERS if not (ESTIMATOR / 'precompile' / h).exists()]
    if missing:
        return ('the NVOFA shaders are not generated (' + ', '.join(missing)
                + '); run OptiScaler/shaders/synth_motion/precompile/build.sh outside a build window')
    if nvofapi() is None:
        return 'no nvofapi64.dll (dxvk-nvapi) in any Proton install'
    return None


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
    # For --source nvofa, from the same Proton as vkd3d-proton when it has one. Only that mode loads it.
    same = vkd3d.parent.parent / 'nvapi/x86_64-windows/nvofapi64.dll'
    ofa = same if same.exists() else nvofapi()
    (WORK / 'nvofapi64.dll').unlink(missing_ok=True)
    if ofa is not None:
        shutil.copy2(ofa, WORK / 'nvofapi64.dll')
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
        'nvofapi': str(ofa) if ofa is not None else None,
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
    overlays = {}
    seqs = {s['name']: s for s in report['sequences']}
    lag = report.get('lag_frames', 0)
    detects_cuts = report.get('source', 'ffx') == 'ffx'
    refines_overlay = report.get('source', 'ffx') == 'ffx'  # the per-pixel choice is the FidelityFX expand's

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

    # Static overlay over a pan: clear overlay pixels read zero, the background away from it the pan.
    for s in report['sequences']:
        if not s['name'].startswith('overlay_'):
            continue
        frames = scored(s['frames'])
        check(len(frames) >= MIN_READY, f'{s["name"]}: only {len(frames)} ready frames')
        if not frames:
            continue
        stat = {name: (mean([region(f, name)['epe_mean'] for f in frames]),
                       mean([region(f, name)['over_half_px'] for f in frames]),
                       mean([region(f, name)['within_1px'] for f in frames]),
                       region(frames[0], name)['pixels'])
                for name in ('overlay', 'overlay_clear', 'crosshair_clear', 'panel_clear', 'text_clear',
                             'background', 'band')}
        def share(part, whole):
            return mean([region(f, part)['pixels'] / region(f, whole)['pixels'] for f in frames])
        smeared = share('overlay_smeared', 'overlay')
        per_element = {e: share(f'{e}_smeared', e) for e in OVERLAY_ELEMENTS}
        ce, cm, _, _ = stat['overlay_clear']
        be, _, bw, _ = stat['background']
        # Only the FidelityFX expand makes the per-pixel choice; the NVOFA source is left out by design
        # (synthesized-motion.md, "What it cannot fix"), so its overlay is reported, not judged.
        if refines_overlay:
            for e, value in per_element.items():
                check(value <= ELEMENT_SMEARED, f'{s["name"]}: {value * 100:.1f}% of the {e} smeared, '
                                                f'limit {ELEMENT_SMEARED * 100:.0f}%')
            check(smeared <= OVERLAY_SMEARED, f'{s["name"]}: {smeared * 100:.1f}% of overlay pixels smeared (a '
                                              f'vector over 0.5 px that reads another colour), limit '
                                              f'{OVERLAY_SMEARED * 100:.0f}%')
            check(ce < OVERLAY_EPE, f'{s["name"]}: clear overlay mean |v| {ce:.3f} >= {OVERLAY_EPE}')
        else:
            notes.append(f'{s["name"]}: overlay not judged for {report.get("source")}, which has no per-pixel '
                         f'choice; {smeared * 100:.1f}% smeared')
        check(be < PAN_SMALL['epe'], f'{s["name"]}: background mean EPE {be:.3f} >= {PAN_SMALL["epe"]}')
        check(bw >= PAN_SMALL['within'], f'{s["name"]}: background within-1px {bw:.3f} < {PAN_SMALL["within"]}')
        overlays[s['name']] = {'smeared_share': smeared, 'smeared_share_per_element': per_element, 'classes': stat}
        rows.append((s['name'], f'ovl {ce:.3f} / bg {be:.3f}', f'smear {smeared * 100:.1f}%',
                     f'moving: all {stat["overlay"][1] * 100:.1f}%, clear {cm * 100:.1f}%', '-',
                     f'{mean([f["gpu_ms"] for f in frames]):.3f}'))

    # Scene cut, for a source without detection (nvofa): never raised, and accurate again two clean
    # pairs after the cut. The pair spanning it carries no scores (the harness skips it).
    if 'cut' in seqs and not detects_cuts:
        frames = seqs['cut']['frames']
        raised = [f['t'] for f in frames if f['scene_cut_after_record'] or f['scene_cut_after_confirm']]
        check(not raised, f'cut: SceneCut() raised at {raised} by a source that promises it never is')
        recovered = [f for f in scored(frames) if f['t'] >= CUT_FRAME + 3]
        check(len(recovered) >= MIN_READY, f'cut: only {len(recovered)} scored frames after the cut')
        epe = mean([region(f, 'interior')['epe_mean'] for f in recovered])
        check(epe < PAN_SMALL['epe'], f'cut: mean EPE {epe:.3f} from t={CUT_FRAME + 3} on >= {PAN_SMALL["epe"]}')
        transient = [f for f in scored(frames) if CUT_FRAME + lag < f['t'] < CUT_FRAME + 3]
        if transient:
            notes.append('cut: first clean pair after the cut, EPE '
                         + ', '.join(f't={f["t"]} {region(f, "interior")["epe_mean"]:.3f}' for f in transient)
                         + ' (not judged: the engine may still seed from the pair across the cut)')
        rows.append(('cut', f'{epe:.3f} (after)', '-', '-', 'not detected (by contract)',
                     f'{mean([f["gpu_ms"] for f in recovered]):.3f}'))

    # Scene cut: zero field at the cut frame, SceneCut() within the readback window, nowhere else.
    if 'cut' in seqs and detects_cuts:
        frames = seqs['cut']['frames']
        flags = {f['t']: f['scene_cut_after_record'] or f['scene_cut_after_confirm'] for f in frames}
        raised = [t_ for t_, v in flags.items() if v]
        window = range(CUT_FRAME, CUT_FRAME + CUT_WINDOW + 1)
        check(any(t_ in window for t_ in raised), f'cut: SceneCut() not raised within t={CUT_FRAME}..{CUT_FRAME + CUT_WINDOW} (raised at {raised})')
        stray = [t_ for t_ in raised if t_ not in window]
        check(not stray, f'cut: SceneCut() raised outside the window at {stray}')
        # The frame that carries the cut must be Ready(): consumers read SceneCut() beside a Ready field and the
        # handoff publishes only Ready fields, so a cut on a frame that is not Ready reaches nobody. It did not
        # before 2026-09-28 (Ready() excluded the cut).
        cut_not_ready = [f['t'] for f in frames if f['scene_cut_after_record'] and not f['ready']]
        check(not cut_not_ready, f'cut: SceneCut() raised at t={cut_not_ready} on a frame that is not Ready(), '
                                 'so no consumer ever sees the cut')
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

    # Abandon: history must not advance on an abandoned recording. The field that measures the pair
    # across it arrives lag frames later.
    if 'abandon' in seqs:
        s = seqs['abandon']
        two = ABANDON_FRAME + 1 + lag
        after = next((f for f in s['frames'] if f['t'] == two), None)
        check(after is not None and after['ready'], f'abandon: frame {two} not ready after an abandoned recording')
        ok = after is not None and after['scores'] and abs(region(after, 'interior')['median_x'] - (-2 * s['dx'])) < 1.0
        check(ok, f'abandon: frame {two} median x '
                  f'{region(after, "interior")["median_x"] if after and after["scores"] else "n/a"} != {-2 * s["dx"]} '
                  '(an abandoned recording advanced the history?)')
        later = [f for f in s['frames'] if f['t'] > two and f['ready'] and f['scores']]
        for f in later:
            check(abs(region(f, 'interior')['median_x'] - (-s['dx'])) < 1.0, f'abandon: frame {f["t"]} not one step')
        rows.append(('abandon', f'{region(after, "interior")["epe_mean"]:.3f}' if after and after['scores'] else 'n/a',
                     '-', f't={two}: {region(after, "interior")["median_x"]:.2f} (want {-2 * s["dx"]})'
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
    return failures, rows, timing, extents, notes, overlays


def judge_hud(report):
    """Synthesized FG's HUD mask: exactness everywhere, no scenery marked, precision and recall on overlay_*."""
    failures, summary = [], {'sequences': {}, 'timing': {}}

    def check(cond, message):
        if not cond:
            failures.append(message)

    scored_any = False
    for s in report['sequences']:
        frames = [f for f in s['frames'] if f.get('hud') and not f['abandoned']]
        if s['timing_only']:
            ms = sorted(f['hud_gpu_ms'] for f in s['frames'] if f['t'] >= SCORED_FROM and f.get('hud_gpu_ms', 0) > 0)
            summary['timing'][s['name']] = {'width': s['width'], 'height': s['height'],
                                            'median_ms': ms[len(ms) // 2] if ms else None,
                                            'p90_ms': ms[int(len(ms) * 0.9)] if ms else None}
            continue
        check(len(frames) >= MIN_READY, f'{s["name"]}: HUD mask scored on only {len(frames)} frames')
        scored_any = scored_any or bool(frames)
        for f in frames:
            h = f['hud']
            check(h['depth_not_binary'] == 0, f'{s["name"]} t={f["t"]}: {h["depth_not_binary"]} depth values neither 0 nor 1')
            check(h['depth_mask_mismatch'] == 0, f'{s["name"]} t={f["t"]}: depth disagrees with the mask at '
                                                 f'{h["depth_mask_mismatch"]} pixels')
            check(h['layer_checked'] and h['layer_mismatch'] == 0,
                  f'{s["name"]} t={f["t"]}: UI layer differs from the frame and mask at {h["layer_mismatch"]} pixels')

        is_hud = s['name'].startswith('hud_')
        if not (is_hud or s['name'].startswith('overlay_')):
            marked = [(f['t'], f['hud']['marked']) for f in frames if f['hud']['marked'] > 0]
            check(not marked, f'{s["name"]}: scenery marked as HUD (t, pixels): {marked}')
            summary['sequences'][s['name']] = {'marked_total': sum(f['hud']['marked'] for f in frames)}
            continue

        late = [f for f in frames if f['t'] >= HUD_FROM]
        check(len(late) >= 2, f'{s["name"]}: only {len(late)} HUD frames from t={HUD_FROM}')
        if not late:
            continue
        marked = sum(f['hud']['marked'] for f in late)
        on_overlay = sum(f['hud']['marked_overlay'] for f in late)
        grown = sum(f['hud']['marked_grown'] for f in late)
        precision = grown / marked if marked else float('nan')
        strict = on_overlay / marked if marked else float('nan')
        check(marked == 0 or precision >= HUD_PRECISION,
              f'{s["name"]}: HUD mask precision {precision:.4f} (within 1 px of the overlay) < {HUD_PRECISION}; '
              f'{marked - grown} pixels of scenery marked over {len(late)} frames')

        steady = [f for f in frames if f['t'] >= HUD_STEADY] if is_hud else late
        recall = {}
        for e in OVERLAY_ELEMENTS:
            pixels = sum(f['hud']['overlay_pixels'][e] for f in steady)
            recall[e] = sum(f['hud']['overlay_marked'][e] for f in steady) / pixels if pixels else float('nan')
        pixels_all = sum(sum(f['hud']['overlay_pixels'].values()) for f in steady)
        on_steady = sum(f['hud']['marked_overlay'] for f in steady)
        recall_all = on_steady / pixels_all if pixels_all else float('nan')
        if is_hud:
            check(len(steady) >= 5, f'{s["name"]}: only {len(steady)} HUD frames from t={HUD_STEADY}')
            check(marked > 0, f'{s["name"]}: the HUD mask marked nothing from t={HUD_FROM}')
            for e, minimum in HUD_MIN_RECALL.items():
                check(recall[e] >= minimum, f'{s["name"]}: HUD mask recall on the {e} {recall[e]:.3f} < {minimum} '
                                            f'(mean over t>={HUD_STEADY})')
        summary['sequences'][s['name']] = {
            'frames': len(late), 'marked_per_frame': marked / len(late), 'precision_within_1px': precision,
            'precision_on_overlay': strict, 'scenery_marked_total': marked - grown, 'recall': recall,
            'recall_all': recall_all, 'recall_from': HUD_STEADY if is_hud else HUD_FROM, 'recall_judged': is_hud,
        }
    check(scored_any, 'ZERO COVERAGE: the HUD mask was never scored')
    return failures, summary


def execute(lock, source):
    manifest = json.loads((OUT / 'manifest.json').read_text())
    if manifest['sources'] != sources():
        raise RuntimeError('sources changed after the harness was built; rebuild required')
    for name, checksum in manifest['binaries'].items():
        if digest(WORK / name) != checksum:
            raise RuntimeError('binary changed after build: ' + name)
    if any(WORK.glob('*ngx*.dll')) or any(WORK.glob('OptiScaler*.dll')):
        raise RuntimeError('unexpected NGX/OptiScaler DLL in the isolated harness; refusing to run')
    if source == 'nvofa' and not (WORK / 'nvofapi64.dll').exists():
        skip(source, 'the harness was built without an nvofapi64.dll beside it (rebuild after installing Proton)')
        return
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='d3d12=n;d3d12core=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n;nvofapi64=n',
               VKD3D_DEBUG='warn', VK_LOADER_LAYERS_DISABLE='~implicit~')
    report_name = artifact('synth-motion-report.json', source)
    (WORK / report_name).unlink(missing_ok=True)
    locked = lock_clocks(lock)
    try:
        command(['wine', WORK / 'synth-motion-harness.exe', '--report', report_name, '--source', source], env=env,
                cwd=WORK, logfile=OUT / artifact('run.log', source), timeout=600)
    finally:
        unlock_clocks(locked)
    (OUT / artifact('clock.txt', source)).write_text('locked 2100/10501 MHz\n' if locked else 'UNLOCKED\n')
    report_judgement(manifest, source)


def skip(source, reason):
    (OUT / artifact('result.json', source)).write_text(
        json.dumps({'status': 'SKIP', 'source': source, 'reason': reason}, indent=2) + '\n')
    print(f'SYNTH-MOTION SKIP ({source}): {reason}')


def report_judgement(manifest, source):
    report_path = WORK / artifact('synth-motion-report.json', source)
    log_path = OUT / artifact('run.log', source)
    log = log_path.read_text(errors='replace') if log_path.exists() else ''
    if 'SYNTH-MOTION SKIP' in log and report_path.exists():
        skip(source, json.loads(report_path.read_text()).get('skipped', 'the harness skipped'))
        return
    if 'SYNTH-MOTION DONE' not in log or not report_path.exists():
        raise RuntimeError('ZERO COVERAGE: the harness did not complete every sequence')
    report = json.loads(report_path.read_text())
    if report.get('source', 'ffx') != source:
        raise RuntimeError(f'report is for source {report.get("source", "ffx")}, not {source}')
    failures, rows, timing, extents, notes, overlays = judge(report)
    hud_failures, hud = judge_hud(report)
    failures += hud_failures
    clock_path = OUT / artifact('clock.txt', source)
    locked = clock_path.exists() and clock_path.read_text().startswith('locked')
    clock = 'locked 2100/10501 MHz' if locked else 'UNLOCKED clocks (numbers indicative only)'

    print(f'\nsource: {source} (field {report.get("lag_frames", 0)} frame(s) late)')
    print(f'\n{"sequence":<12} {"EPE px":<20} {"<1px":<12} {"sign":<44} {"scene cut":<26} gpu ms')
    for r in rows:
        print(f'{r[0]:<12} {r[1]:<20} {r[2]:<12} {r[3]:<44} {r[4]:<26} {r[5]}')
    print(f'\ntiming ({clock}):')
    for name, t in timing.items():
        print(f'  {name}: {t["width"]}x{t["height"]} median {t["median_ms"]} ms, p90 {t["p90_ms"]} ms')
    if overlays:
        print('\nstatic overlay, per class (mean |v - truth| px, share > 0.5 px, share < 1 px, pixels):')
        for name, overlay in overlays.items():
            print(f'  {name}: {overlay["smeared_share"] * 100:.1f}% of overlay pixels smeared ('
                  + ', '.join(f'{e} {v * 100:.1f}%' for e, v in overlay['smeared_share_per_element'].items()) + ')')
            for cls, (e, m, w, n) in overlay['classes'].items():
                print(f'    {cls:<16} {e:7.3f} px  {m * 100:5.1f}% > 0.5  {w * 100:5.1f}% < 1  {n:>7} px')
    print(f'field extents (field WxH for colour WxH): {extents}')
    print(f'\nsynthesized FG HUD mask (depth 1 = marked; precision from t={HUD_FROM}):')
    for name, h in hud['sequences'].items():
        if 'recall' in h:
            print(f'  {name}: {h["marked_per_frame"]:.0f} px marked per frame, precision {h["precision_within_1px"] * 100:.2f}% '
                  f'within 1 px of the overlay ({h["precision_on_overlay"] * 100:.1f}% on it), '
                  f'{h["scenery_marked_total"]} scenery px over {h["frames"]} frames; recall from t={h["recall_from"]} '
                  f'({"judged" if h["recall_judged"] else "reported"}) '
                  + ', '.join(f'{e} {v * 100:.1f}%' for e, v in h['recall'].items())
                  + f', all {h["recall_all"] * 100:.1f}%')
        else:
            print(f'  {name}: {h["marked_total"]} px marked over all frames')
    for name, t in hud['timing'].items():
        print(f'  timing {name}: {t["width"]}x{t["height"]} mask + layer median {t["median_ms"]} ms, p90 {t["p90_ms"]} ms ({clock})')
    for n in notes:
        print('note: ' + n)

    limits = 'Synthetic content, integer motion, RGBA8 SDR input, one device and queue. Not game footage.'
    if source == 'nvofa':
        limits += ' GPU time excludes the optical-flow engine, which runs on its own queue.'
    result = {'status': 'PASS' if not failures else 'FAIL', 'source': source, 'failures': failures, 'notes': notes,
              'clock': clock, 'timing': timing, 'overlay': overlays, 'hud': hud, 'extents': extents,
              'manifest': manifest, 'limits': limits}
    (OUT / artifact('result.json', source)).write_text(json.dumps(result, indent=2) + '\n')
    if failures:
        print('\nSYNTH-MOTION FAIL:')
        for f in failures:
            print('  - ' + f)
        raise SystemExit(1)
    print(f'\nSYNTH-MOTION PASS ({source}): {len(rows)} scored sequences, sign current->previous (+y down), {clock}')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--run-only', action='store_true')
    ap.add_argument('--no-lock', action='store_true', help='do not try to lock GPU clocks for the timing numbers')
    ap.add_argument('--judge-only', action='store_true', help='re-score the last run report without running anything')
    ap.add_argument('--source', choices=('ffx', 'nvofa'), default='ffx',
                    help='the motion source to run: the FidelityFX estimator (default) or NVIDIA Optical Flow')
    args = ap.parse_args()
    if args.build_only and args.run_only:
        ap.error('choose at most one mode')
    OUT.mkdir(exist_ok=True)
    if args.judge_only:
        report_judgement(json.loads((OUT / 'manifest.json').read_text()), args.source)
        return
    if args.source == 'nvofa' and not args.build_only:
        reason = nvofa_unavailable()
        if reason is not None:
            skip(args.source, reason)
            return
    (OUT / artifact('result.json', args.source)).write_text('{"status":"RUNNING"}\n')
    if not args.run_only:
        build()
    if not args.build_only:
        execute(not args.no_lock, args.source)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, subprocess.SubprocessError, KeyError, ValueError, TypeError) as error:
        OUT.mkdir(exist_ok=True)
        source = 'nvofa' if 'nvofa' in sys.argv else 'ffx'
        (OUT / artifact('result.json', source)).write_text(
            json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
