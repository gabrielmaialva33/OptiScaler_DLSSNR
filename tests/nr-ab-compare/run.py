#!/usr/bin/env python3
"""Exercise the real offline comparator using synthetic files; no GPU or third-party packages."""
from array import array
import importlib.util
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools/nr_ab_compare.py'
spec = importlib.util.spec_from_file_location('nr_ab_compare', TOOL)
m = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = m
spec.loader.exec_module(m)


def png(path, pixels, w=16, h=16, bits=8, mode=0):
    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload))
    raw, previous = bytearray(), bytes(w * 3 * (bits // 8))
    for y in range(h):
        row = b''.join(struct.pack('>H' if bits == 16 else 'B', round(v * ((1 << bits) - 1)))
                       for px in pixels[y * w:(y + 1) * w] for v in px)
        filtered = bytearray(row)
        for x in range(len(row)):
            a = row[x - 3 * bits // 8] if x >= 3 * bits // 8 else 0
            b = previous[x]
            c = previous[x - 3 * bits // 8] if x >= 3 * bits // 8 else 0
            p = a + b - c
            paeth = a if abs(p-a) <= abs(p-b) and abs(p-a) <= abs(p-c) else b if abs(p-b) <= abs(p-c) else c
            filtered[x] = (row[x] - (0, a, b, (a+b)//2, paeth)[mode]) & 255
        raw.extend(bytes([mode]) + filtered)
        previous = row
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, bits, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def record(folder, stage, n, original, processed, w=16, h=16):
    stem = f'pair-{n}'
    png(folder / f'{stem}.original.png', original, w, h)
    png(folder / f'{stem}.processed.png', processed, w, h)
    route, surface, token = ('after-upscale', 'upscaler-output', 'after') if stage == 0 else (
        'before-upscale', 'render-input', 'before')
    r = dict(record='retoque-comparison-capture', record_version='1', route=route, surface=surface,
             hook_method='1', held='0', carried='0', completion='submission-fence', build='synthetic',
             model_version='1', model_bytes='123', gpu='fake', gpu_driver='fake', exe='fixture',
             ngx_loader='1', ngx_route='loader-routes-feature-18', width=str(w), height=str(h),
             frame=str(n), present=str(n), linear_hdr='0', white_point='1',
             original_file=f'{stem}.original.png', processed_file=f'{stem}.processed.png',
             original_format='R8G8B8A8_UNORM', processed_format='R8G8B8A8_UNORM',
             settings=f'stage={token} render=8x8 compose={w}x{h} model={w}x{h} scale=1 passes=1 apply=true hold=false compare=0 debug=0')
    path = folder / f'{stem}.json'
    path.write_text(json.dumps(r))
    return path, r


def rejects(fn):
    try:
        fn()
    except (ValueError, KeyError, TypeError):
        return
    raise AssertionError('invalid input accepted')


with tempfile.TemporaryDirectory(prefix='nr-ab-') as directory:
    root = Path(directory)
    flat = [(0.5, 0.5, 0.5)] * 256
    checker = [(0.4 if (x+y) % 2 else 0.6,) * 3 for y in range(16) for x in range(16)]
    edge = [(0.2 if x < 8 else 0.8,) * 3 for y in range(16) for x in range(16)]
    blurred = [(0.2 if x < 7 else 0.4 if x == 7 else 0.6 if x == 8 else 0.8,) * 3
               for y in range(16) for x in range(16)]
    def read(name, pixels):
        p = root / (name + '.png')
        png(p, pixels)
        return m.image(p)
    a = read('flat', flat)
    identity, sample = m.sample_pair(a, a)
    assert identity['luma_edit_mae'] == identity['opponent_colour_edit_rms'] == 0
    assert identity['gradient_change_percent'] is None  # flat does not divide by zero
    assert identity['flat_residual_laplacian_rms'] == 0
    noise, noisy_sample = m.sample_pair(a, read('noise', checker))
    assert noise['flat_residual_laplacian_rms'] > 0.7
    blur, _ = m.sample_pair(read('edge', edge), read('blur', blurred))
    assert blur['gradient_change_percent'] < 0 and blur['laplacian_change_percent'] < 0
    tint, tinted_sample = m.sample_pair(a, read('tint', [(0.6, 0.5, 0.4)] * 256))
    assert tint['rgb_edit_bias'][0] > 0.09 and tint['rgb_edit_bias'][2] < -0.09
    assert tint['opponent_colour_edit_rms'] > 0.09
    still = m.temporal(sample, sample, 0.01)
    flicker = m.temporal(sample, tinted_sample, 0.01)
    assert still['edit_delta_rms'] == 0 and flicker['stable_original_edit_delta_rms'] > 0.01
    # Motion in the original is reported, rather than credited to NR flicker.
    _, moved = m.sample_pair(read('edge2', edge), read('edge3', edge))
    moving = m.temporal(sample, moved, 0.01)
    assert moving['original_delta_rms'] > 0 and moving['edit_delta_rms'] == 0
    assert moving['stable_original_fraction'] == 0
    for bits in (8, 16):
        for mode in range(5):
            p = root / f'filter-{bits}-{mode}.png'
            png(p, checker, bits=bits, mode=mode)
            decoded = m.image(p)
            assert math.isclose(decoded.rgb[0], 0.6, abs_tol=1/255)
            assert math.isclose(decoded.rgb[3], 0.4, abs_tol=1/255)
    broken = root / 'broken.png'
    data = bytearray((root / 'flat.png').read_bytes())
    data[-5] ^= 1
    broken.write_bytes(data)
    rejects(lambda: m.image(broken))
    # HDR must retain values above 1 and negatives, with PFM bottom-up row order/endian/scale.
    pfm = root / 'hdr.pfm'
    pfm.write_bytes(b'PF\n3 3\n-1.0\n' + struct.pack('<27f', *([3., -1., 2.] * 6 + [7., 8., 9.] * 3)))
    hdr = m.image(pfm)
    assert list(hdr.rgb[:3]) == [7, 8, 9] and list(hdr.rgb[-3:]) == [3, -1, 2]
    pfm.write_bytes(b'PF\n3 3\n2.0\n' + struct.pack('>27f', *([2.] * 27)))
    assert all(v == 4 for v in m.image(pfm).rgb)
    pfm.write_bytes(b'PF\n3 3\n-1.0\n' + struct.pack('<27f', *([math.nan] * 27)))
    rejects(lambda: m.image(pfm))
    arms = [root / 's0', root / 's1']
    for d in arms:
        d.mkdir()
    for stage, folder in enumerate(arms):
        for n in (1, 2, 3):
            record(folder, stage, n, flat, flat if n != 2 else checker)
    command = [sys.executable, str(TOOL), '--stage0', str(arms[0]), '--stage1', str(arms[1])]
    result = subprocess.run(command, text=True, capture_output=True, check=True)
    report = json.loads(result.stdout)
    assert report['stage1']['temporal']['kind'] == 'consecutive-frame-ids'
    assert report['stage1']['temporal']['transitions'][0]['edit_delta_rms'] > 0
    assert report['limitations'] and 'No quality winner' in report['limitations'][0]
    path = arms[1] / 'pair-3.json'
    r = json.loads(path.read_text())
    for key, value in [('route', 'after-upscale'), ('held', '1'), ('completion', 'assumed-8-frames'),
                       ('model_version', 'unknown'), ('original_file', '../flat.png'), ('width', '99')]:
        bad = r | {key: value}
        path.write_text(json.dumps(bad))
        assert subprocess.run(command, capture_output=True).returncode == 2, key
    path.write_text(json.dumps(r | {'frame': '10', 'present': '10'}))
    report = json.loads(subprocess.run(command, capture_output=True, check=True).stdout)
    assert report['stage1']['temporal']['kind'] == 'sparse-sampled-variation'
    path.write_text(json.dumps(r | {'settings': r['settings'].replace('scale=1', 'scale=0.5')}))
    assert subprocess.run(command, capture_output=True).returncode == 2
    for bad_settings in (r['settings'].replace('model=16x16', 'model=8x8'),
                         'stage=before', r['settings'].replace('apply=true', 'apply=false')):
        path.write_text(json.dumps(r | {'settings': bad_settings}))
        assert subprocess.run(command, capture_output=True).returncode == 2
    path.write_text(json.dumps(r))
    # Render-sized Stage 1 is accepted without resizing, and exposes its own extent.
    for p in arms[1].glob('*'):
        p.unlink()
    _, raw_record = record(arms[1], 1, 1, flat[:64], flat[:64], 8, 8)
    result = subprocess.run(command, text=True, capture_output=True, check=True)
    assert json.loads(result.stdout)['stage1']['provenance']['width'] == '8'
    # Legacy burst: padded rows, final row unpadded (D3D12 footprint total), and missing files.
    burst = root / 'burst'
    burst.mkdir()
    (burst / 'manifest.txt').write_text('frames 3\nbefore width 8 height 8 format 28 rowPitch 256\n'
                                       'after width 8 height 8 format 28 rowPitch 256\n')
    for n in range(3):
        for side in ('before', 'after'):
            v = 150 if n == 1 and side == 'after' else 128
            row = bytes([v, v, v, 255]) * 8
            (burst / f'{side}_{n:02d}.raw').write_bytes((row + bytes(224)) * 7 + row)
    summary = m.burst(burst, raw_record, 128, 0.01)
    assert len(summary['transitions']) == 2 and summary['transitions'][0]['edit_delta_rms'] > 0.08
    assert 'No frame ids' in summary['warning']
    (burst / 'after_02.raw').unlink()
    assert subprocess.run(command + ['--stage1-burst', str(burst)], capture_output=True).returncode == 2
    timing = root / 'timing.log'
    line = ('DLSS-NR gpu timing confirmed: run=1 contract=A model_identity=M render=8x8 compose=16x16 '
            'model=16x16 stage=after capture_active=0 model_reset=false carried=false cadence=1 passes=1/1 '
            'dispatch_ms=8 model_ms=7 outside_model_ms=1 proof=sealed-single-execution-fence')
    timing.write_text('\n'.join([line, line.replace('model_ms=7', 'model_ms=5'),
                                 line.replace('capture_active=0', 'capture_active=1'),
                                 line.replace('stage=after', 'stage=after-fallback'),
                                 line.replace('contract=A', 'contract=B')]))
    cost = m.gpu_cost(timing, 'after')
    assert len(cost['groups']) == 2
    assert cost['groups'][0]['model_ms']['median'] == 6
    assert cost['groups'][0]['outside_model_ms']['median'] == 1
    assert cost['excluded'] == {'capture_active': 1, 'stage': 1}
    rejects(lambda: m.gpu_cost(timing, 'before'))
    # Typed float raw formats used by Crimson Desert; reject unknown formats and invalid pitches.
    for fmt, code in ((10, 'e'), (2, 'f')):
        raw = root / f'float-{fmt}.raw'
        px = struct.pack('<4' + code, 2, -1, 0.5, 1)
        raw.write_bytes(px * 9)
        assert list(m.raw_image(raw, (3, 3, fmt, 3 * len(px))).rgb[:3]) == [2, -1, 0.5]
    rejects(lambda: m.raw_image(raw, (3, 3, 999, 48)))
    rejects(lambda: m.raw_image(raw, (3, 3, 2, 1)))

print('PASS: NR A/B PNG/PFM/raw decoding, identity/blur/noise/colour/flicker controls, provenance and CLI refusals')
