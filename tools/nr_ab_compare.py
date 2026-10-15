#!/usr/bin/env python3
"""Compare NR capture arms without resampling or claiming a perceptual quality score.

Run: uv run tools/nr_ab_compare.py --stage0 captures/after --stage1 captures/before
See docs/dlssnr/design/nr-before-upscale.md for acquisition and metric definitions.
Only Python's standard library is required. Inputs are never modified.
"""
import argparse
from array import array
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import struct
import sys
import zlib

LIMITATIONS = [
    "No quality winner: sharpness includes noise, ringing and hallucinated detail.",
    "Stage 1 is render-input before SR; Stage 0 is upscaler-output. No resizing or final-screen equivalence.",
    "Noise proxy is residual Laplacian RMS in low-gradient original regions; texture and aliasing also contribute.",
    "RGB bias and opponent-colour RMS are not perceptual Delta E; PNG codes and linear HDR are different domains.",
    "Temporal residual variation is not motion compensated. Pan, jitter, disocclusion and lighting can resemble flicker.",
    "Same-scene correspondence across stages cannot be established from these captures; inspect the originals.",
    "Capture copies perturb timing; collect GPU costs separately with capture_active=0.",
    "Recorded HDR white is the CPU value; live exposure may differ. Hold source 0 fixed for this protocol.",
    "Model version and byte count are not a model hash; retain the actual model SHA-256 with the run.",
    "Sidecars do not record every option (e.g. output stabilizer/UI protection); retain INIs and control these manually.",
]


@dataclass
class Image:
    width: int
    height: int
    rgb: array


def extent(w, h):
    if min(w, h) < 3 or w * h > 32_000_000:
        raise ValueError("image must be at least 3x3 and at most 32 million pixels")


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos, payload, header, ended = 8, bytearray(), None, False
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        size = struct.unpack_from(">I", data, pos)[0]
        tag, chunk = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
        if pos + 12 + size > len(data):
            raise ValueError("truncated PNG data")
        crc = struct.unpack_from(">I", data, pos + 8 + size)[0]
        if zlib.crc32(tag + chunk) != crc:
            raise ValueError("PNG CRC mismatch")
        if tag == b"IHDR":
            if header is not None or size != 13 or pos != 8:
                raise ValueError("invalid PNG IHDR")
            header = struct.unpack(">IIBBBBB", chunk)
        elif tag == b"IDAT":
            payload.extend(chunk)
        elif tag == b"IEND":
            ended = True
            break
        pos += size + 12
    if not header or not ended:
        raise ValueError("incomplete PNG")
    w, h, bits, colour, compression, filtering, interlace = header
    extent(w, h)
    if bits not in (8, 16) or colour not in (2, 6) or any((compression, filtering, interlace)):
        raise ValueError("only non-interlaced RGB/RGBA PNG8/16 is supported")
    channels, unit = (3 if colour == 2 else 4), bits // 8
    bpp, stride = channels * unit, w * channels * unit
    expected = h * (stride + 1)
    inflater = zlib.decompressobj()
    raw = inflater.decompress(payload, expected + 1)
    if len(raw) != expected or not inflater.eof or inflater.unused_data:
        raise ValueError("PNG decompressed size mismatch")
    rgb, previous = array('f'), bytearray(stride)
    for y in range(h):
        start = y * (stride + 1)
        mode, row = raw[start], bytearray(raw[start + 1:start + stride + 1])
        if mode > 4:
            raise ValueError("invalid PNG row filter")
        if mode:
            for x in range(stride):
                left, up = (row[x - bpp] if x >= bpp else 0), previous[x]
                upper_left = previous[x - bpp] if x >= bpp else 0
                if mode == 1:
                    predictor = left
                elif mode == 2:
                    predictor = up
                elif mode == 3:
                    predictor = (left + up) // 2
                else:
                    p = left + up - upper_left
                    distances = (abs(p - left), abs(p - up), abs(p - upper_left))
                    predictor = (left, up, upper_left)[distances.index(min(distances))]
                row[x] = (row[x] + predictor) & 255
        for x in range(0, stride, bpp):
            if bits == 8:
                rgb.extend(v / 255 for v in row[x:x + 3])
            else:
                rgb.extend(v / 65535 for v in struct.unpack_from('>HHH', row, x))
        previous = row
    return Image(w, h, rgb)


def read_pfm(path):
    with path.open('rb') as f:
        def line():
            while True:
                value = f.readline()
                if not value:
                    raise ValueError("truncated PFM header")
                if value.strip() and not value.startswith(b'#'):
                    return value.strip()
        if line() != b'PF':
            raise ValueError("only RGB PFM supported")
        w, h = map(int, line().split())
        extent(w, h)
        scale = float(line())
        if not math.isfinite(scale) or scale == 0:
            raise ValueError("invalid PFM scale")
        raw = f.read(w * h * 12 + 1)
    if len(raw) != w * h * 12:
        raise ValueError("PFM payload size mismatch")
    values = array('f')
    values.frombytes(raw)
    if (scale < 0) != (sys.byteorder == 'little'):
        values.byteswap()
    rgb = array('f')
    for y in reversed(range(h)):
        rgb.extend(v * abs(scale) for v in values[y * w * 3:(y + 1) * w * 3])
    return Image(w, h, rgb)


def image(path):
    if path.suffix == '.png':
        result = read_png(path)
    elif path.suffix == '.pfm':
        result = read_pfm(path)
    else:
        raise ValueError(f"unsupported image extension: {path.suffix}")
    if not all(math.isfinite(v) for v in result.rgb):
        raise ValueError(f"non-finite pixels: {path}")
    return result


def rms(values):
    return math.sqrt(statistics.fmean(v * v for v in values)) if values else None


def sample_pair(original, processed, scale=1.0, grid=128, flat=0.01):
    if (original.width, original.height) != (processed.width, processed.height):
        raise ValueError("original and processed dimensions differ; no implicit resampling")
    w, h = original.width, original.height
    extent(w, h)
    # Uniform subsampling of centres; derivatives always use adjacent NATIVE pixels.
    step = max(1, math.ceil(max(w - 2, h - 2) / grid))
    gradients, laplacians = [[], []], [[], []]
    residual, originals, processed_luma, colour, bias = [], [], [], [], [[], [], []]
    flat_lap = []
    def luma(a, i):
        return (0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2]) / scale
    for y in range(1, h - 1, step):
        for x in range(1, w - 1, step):
            i = 3 * (y * w + x)
            ls, gs, laps = [], [], []
            for a in (original.rgb, processed.rgb):
                c = luma(a, i)
                left, right = luma(a, i - 3), luma(a, i + 3)
                up, down = luma(a, i - 3 * w), luma(a, i + 3 * w)
                ls.append(c)
                gs.append(math.hypot(right - left, down - up) / 2)
                laps.append(left + right + up + down - 4 * c)
            for k in range(2):
                gradients[k].append(gs[k])
                laplacians[k].append(laps[k])
            originals.append(ls[0])
            processed_luma.append(ls[1])
            residual.append(ls[1] - ls[0])
            if gs[0] <= flat:
                flat_lap.append(laps[1] - laps[0])
            delta = [(processed.rgb[i + k] - original.rgb[i + k]) / scale for k in range(3)]
            for k in range(3):
                bias[k].append(delta[k])
            colour.extend((delta[0] - delta[1], delta[2] - delta[1]))
    metrics = {
        'sample_count': len(residual), 'sample_stride_pixels': step,
        'original_gradient_rms': rms(gradients[0]), 'processed_gradient_rms': rms(gradients[1]),
        'original_laplacian_rms': rms(laplacians[0]), 'processed_laplacian_rms': rms(laplacians[1]),
        'luma_edit_mae': statistics.fmean(abs(v) for v in residual),
        'luma_edit_bias': statistics.fmean(residual), 'opponent_colour_edit_rms': rms(colour),
        'rgb_edit_bias': [statistics.fmean(v) for v in bias],
        'flat_region_fraction': len(flat_lap) / len(residual), 'flat_residual_laplacian_rms': rms(flat_lap),
    }
    for name in ('gradient', 'laplacian'):
        a, b = metrics[f'original_{name}_rms'], metrics[f'processed_{name}_rms']
        metrics[f'{name}_change_percent'] = 100 * (b / a - 1) if a > 1e-12 else None
    return metrics, (originals, processed_luma, residual)


def temporal(previous, current, flat):
    od = [b - a for a, b in zip(previous[0], current[0])]
    pd = [b - a for a, b in zip(previous[1], current[1])]
    rd = [b - a for a, b in zip(previous[2], current[2])]
    stable = [v for v, o in zip(rd, od) if abs(o) <= flat]
    return {'original_delta_rms': rms(od), 'processed_delta_rms': rms(pd),
            'edit_delta_rms': rms(rd), 'stable_original_fraction': len(stable) / len(od),
            'stable_original_edit_delta_rms': rms(stable)}


def settings(record):
    return dict(re.findall(r'(?:^|\s)(\w+)=(\S+)', record['settings']))


def child(parent, name):
    if not isinstance(name, str) or Path(name).name != name or '\\' in name or name in ('', '.', '..'):
        raise ValueError("sidecar image names must be sibling basenames")
    return parent / name


def load_records(path, stage):
    paths = [path] if path.is_file() else sorted(path.glob('*.json'))
    records = []
    for p in paths:
        r = json.loads(p.read_text())
        if r.get('record') != 'retoque-comparison-capture' or str(r.get('record_version')) != '1':
            raise ValueError(f"not a v1 comparison sidecar: {p}")
        contract = settings(r)
        required = {'stage', 'render', 'compose', 'model', 'scale', 'passes', 'apply', 'hold', 'compare', 'debug'}
        if not required <= contract.keys():
            raise ValueError(f'incomplete timing contract: {p}')
        if contract['apply'] not in ('true', '1') or any(contract[k] not in ('false', '0') for k in ('hold', 'compare', 'debug')):
            raise ValueError('requires model application, no hold/compare/debug view')
        if int(r['frame']) <= 0 or int(r['present']) <= 0:
            raise ValueError('capture needs positive frame/present ids')
        route, surface, token = ('after-upscale', 'upscaler-output', 'after') if stage == 0 else (
            'before-upscale', 'render-input', 'before')
        if (r.get('route'), r.get('surface'), settings(r).get('stage')) != (route, surface, token):
            raise ValueError(f"wrong actual stage/route/surface (including fallback): {p}")
        if str(r.get('hook_method')) != '1' or str(r.get('held')) != '0' or str(r.get('carried')) != '0':
            raise ValueError("A/B requires HookMethod=1, HoldFrame=false and fresh model frames")
        for key in ('build', 'model_version', 'model_bytes', 'gpu', 'gpu_driver', 'exe', 'ngx_loader', 'ngx_route'):
            if not r.get(key) or r[key] == 'unknown':
                raise ValueError(f"missing provenance {key}: {p}")
        if r.get('completion') != 'submission-fence':
            raise ValueError("pair capture lacks submission-fence proof; do not use assumed completion for A/B")
        records.append((p, r))
    if not records:
        raise ValueError(f"no captures: {path}")
    records.sort(key=lambda pr: int(pr[1]['frame']))
    if len({int(r['frame']) for _, r in records}) != len(records):
        raise ValueError("duplicate frame ids; use one process per arm and separate directories")
    return records


def signature(r):
    return {k: r[k] for k in ('build', 'model_version', 'model_bytes', 'gpu', 'gpu_driver', 'exe',
                             'ngx_loader', 'ngx_route', 'linear_hdr')} | {
        'settings': {k: v for k, v in settings(r).items()
                     if k not in ('stage', 'compose', 'model', 'compose_format')}}


def analyse(records, grid, flat):
    frames, transitions, previous, last = [], [], None, None
    base = records[0][1]
    for path, r in records:
        if signature(r) != signature(base) or settings(r) != settings(base) or any(r[k] != base[k] for k in (
                'width', 'height', 'original_format', 'processed_format')):
            raise ValueError("settings, provenance or dimensions changed within an arm")
        files = [child(path.parent, r[k]) for k in ('original_file', 'processed_file')]
        original, processed = (image(p) for p in files)
        if (original.width, original.height) != (int(r['width']), int(r['height'])):
            raise ValueError("sidecar dimensions disagree with image")
        linear = str(r['linear_hdr']) == '1'
        if linear != (files[0].suffix == '.pfm') or files[0].suffix != files[1].suffix:
            raise ValueError("ambiguous transfer: require PNG display codes or linear-HDR PFM")
        white = float(r['white_point']) if linear else 1.0
        if not math.isfinite(white) or white <= 0:
            raise ValueError("invalid white point")
        metrics, sampled = sample_pair(original, processed, white, grid, flat)
        frames.append({'sidecar': path.name, 'frame': int(r['frame']), 'present': int(r['present']),
                       'white_point': white, 'metrics': metrics,
                       'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [path, *files]}})
        if previous is not None:
            transitions.append({'frame_gap': int(r['frame']) - int(last['frame']),
                                'present_gap': int(r['present']) - int(last['present']),
                                **temporal(previous, sampled, flat)})
        previous, last = sampled, r
    keys = [k for k, v in frames[0]['metrics'].items() if v is None or isinstance(v, (int, float))]
    summary = {}
    for k in keys:
        values = [f['metrics'][k] for f in frames if f['metrics'][k] is not None]
        summary[k] = statistics.median(values) if values else None
    return {'provenance': base, 'units': 'linear RGB / recorded white point' if str(base['linear_hdr']) == '1'
            else 'stored PNG codes / full scale (no gamma decode)', 'frames': frames, 'median_metrics': summary,
            'temporal': {'kind': 'consecutive-frame-ids' if len(transitions) >= 2 and all(
                t['frame_gap'] == t['present_gap'] == 1 for t in transitions) else 'sparse-sampled-variation',
                'transitions': transitions}}


def raw_image(path, desc):
    w, h, fmt, pitch = desc
    extent(w, h)
    bpp = {2: 16, 10: 8, 24: 4, 26: 4, 28: 4, 87: 4}.get(fmt)
    if bpp is None or pitch < w * bpp:
        raise ValueError(f"unsupported raw DXGI format/rowPitch: {fmt}/{pitch}")
    data = path.read_bytes()
    if len(data) not in (pitch * h, pitch * (h - 1) + w * bpp):
        raise ValueError(f"raw byte count mismatch: {path}")
    rgb = array('f')
    def small(v, bits):
        mantissa, exponent = v & ((1 << bits) - 1), v >> bits
        if exponent == 31:
            raise ValueError("non-finite raw R11G11B10 pixel")
        return math.ldexp(mantissa / (1 << bits) if exponent == 0 else 1 + mantissa / (1 << bits),
                          -14 if exponent == 0 else exponent - 15)
    for y in range(h):
        for x in range(w):
            offset = y * pitch + x * bpp
            if fmt in (2, 10):
                px = struct.unpack_from('<4f' if fmt == 2 else '<4e', data, offset)[:3]
            elif fmt in (28, 87):
                px = [v / 255 for v in data[offset:offset + 3]]
                if fmt == 87:
                    px.reverse()
            else:
                v = struct.unpack_from('<I', data, offset)[0]
                px = [(v >> n & 1023) / 1023 for n in (0, 10, 20)] if fmt == 24 else (
                    small(v & 2047, 6), small(v >> 11 & 2047, 6), small(v >> 22 & 1023, 5))
            if not all(math.isfinite(v) for v in px):
                raise ValueError("non-finite raw pixel")
            rgb.extend(px)
    return Image(w, h, rgb)


def burst(path, record, grid, flat):
    text = (path / 'manifest.txt').read_text()
    count = int(re.search(r'^frames (\d+)$', text, re.M)[1])
    if not 3 <= count <= 8:
        raise ValueError("temporal burst needs 3..8 recordings")
    desc = {side: tuple(map(int, re.search(
        rf'^{side} width (\d+) height (\d+) format (\d+) rowPitch (\d+)$', text, re.M).groups()))
            for side in ('before', 'after')}
    if desc['before'][:2] != (int(record['width']), int(record['height'])):
        raise ValueError("raw burst extent differs from companion pair")
    linear = str(record['linear_hdr']) == '1'
    formats = {2: 'R32G32B32A32_FLOAT', 10: 'R16G16B16A16_FLOAT', 24: 'R10G10B10A2_UNORM',
               26: 'R11G11B10_FLOAT', 28: 'R8G8B8A8_UNORM', 87: 'B8G8R8A8_UNORM'}
    for side, key in (('before', 'original_format'), ('after', 'processed_format')):
        if formats.get(desc[side][2]) != record[key]:
            raise ValueError('raw format differs from companion pair')
    white = float(record['white_point']) if linear else 1.0
    if not math.isfinite(white) or white <= 0:
        raise ValueError('invalid burst white point')
    transitions, previous, hashes = [], None, {}
    for n in range(count):
        files = [path / f'{side}_{n:02d}.raw' for side in ('before', 'after')]
        a, b = (raw_image(p, desc[side]) for p, side in zip(files, ('before', 'after')))
        _, sample = sample_pair(a, b, white, grid, flat)
        hashes.update({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files})
        if previous is not None:
            transitions.append(temporal(previous, sample, flat))
        previous = sample
    hashes['manifest.txt'] = hashlib.sha256((path / 'manifest.txt').read_bytes()).hexdigest()
    return {'kind': 'legacy-adjacent-recordings', 'transitions': transitions, 'sha256': hashes,
            'units': 'unquantized raw RGB / companion white point' if linear else 'unquantized raw RGB',
            'warning': 'No frame ids, timestamps, settings or fence proof in raw manifest. Companion pair association '
            'and constant white point are operator assertions. Adjacent recordings need not be adjacent presents.'}


def gpu_cost(path, stage):
    """Keep unlike timing contracts separate; never match model_ms inside outside_model_ms."""
    groups, rejected = {}, {}
    for line in path.read_text(errors='replace').splitlines():
        if 'DLSS-NR gpu timing confirmed:' not in line:
            continue
        fields = dict(re.findall(r'(?:^|\s)(\w+)=(\S+)', line))
        reason = None
        for key, values in (('stage', (stage,)), ('capture_active', ('0', 'false')),
                            ('model_reset', ('0', 'false')), ('carried', ('0', 'false')),
                            ('cadence', ('1',)), ('proof', ('sealed-single-execution-fence',))):
            if fields.get(key) not in values:
                reason = key
                break
        passes = fields.get('passes', '').split('/')
        if not reason and (len(passes) != 2 or passes[0] != passes[1] or passes[0] == '0'):
            reason = 'incomplete-passes'
        if reason:
            rejected[reason] = rejected.get(reason, 0) + 1
            continue
        key = tuple(fields[k] for k in ('run', 'contract', 'model_identity', 'render', 'compose', 'model'))
        values = [float(fields[k]) for k in ('dispatch_ms', 'model_ms', 'outside_model_ms')]
        if not all(math.isfinite(v) and v >= 0 for v in values):
            raise ValueError('invalid GPU timing value')
        groups.setdefault(key, []).append(values)
    if not groups:
        raise ValueError(f'no eligible {stage} GPU timing samples: {path}')
    summaries = []
    for key, rows in groups.items():
        result = dict(zip(('run', 'contract', 'model_identity', 'render', 'compose', 'model'), key))
        result['samples'] = len(rows)
        for i, name in enumerate(('dispatch_ms', 'model_ms', 'outside_model_ms')):
            values = sorted(row[i] for row in rows)
            def percentile(p):
                index = (len(values) - 1) * p
                lower = int(index)
                return values[lower] + (values[min(lower + 1, len(values) - 1)] - values[lower]) * (index - lower)
            result[name] = {'p10': percentile(.1), 'median': percentile(.5), 'p90': percentile(.9)}
        summaries.append(result)
    return {'groups': summaries, 'excluded': rejected, 'log_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'warning': 'GPU dispatch cost, not whole-frame FPS. Timing logs and capture arms must be associated by '
            'the operator; inspect build stamps and matching settings. Groups are never pooled across contracts.'}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--stage0', type=Path, required=True, help='one sidecar or directory from Stage=0')
    ap.add_argument('--stage1', type=Path, required=True, help='one sidecar or directory from Stage=1')
    ap.add_argument('--stage0-burst', type=Path)
    ap.add_argument('--stage1-burst', type=Path)
    ap.add_argument('--stage0-log', type=Path, help='GPU timing log from Stage=0, preferably a capture-free interval')
    ap.add_argument('--stage1-log', type=Path)
    ap.add_argument('--grid', type=int, default=128, help='maximum sampled centres per dimension; native neighbours')
    ap.add_argument('--flat-threshold', type=float, default=0.01)
    ap.add_argument('--output', type=Path, help='JSON report (default stdout)')
    args = ap.parse_args(argv)
    try:
        if not 4 <= args.grid <= 1024 or not math.isfinite(args.flat_threshold) or args.flat_threshold < 0:
            raise ValueError("invalid grid/flat threshold")
        records = [load_records(args.stage0, 0), load_records(args.stage1, 1)]
        if signature(records[0][0][1]) != signature(records[1][0][1]):
            raise ValueError("A/B provenance or picture settings differ beyond stage/extents/format")
        arms = [analyse(r, args.grid, args.flat_threshold) for r in records]
        for index, path in enumerate((args.stage0_burst, args.stage1_burst)):
            if path:
                arms[index]['raw_burst'] = burst(path, records[index][0][1], args.grid, args.flat_threshold)
        for index, path in enumerate((args.stage0_log, args.stage1_log)):
            if path:
                arms[index]['gpu_cost'] = gpu_cost(path, ('after', 'before')[index])
        report = {'schema': 'retoque-nr-ab-v1', 'limitations': LIMITATIONS, 'grid': args.grid,
                  'flat_threshold': args.flat_threshold, 'stage0': arms[0], 'stage1': arms[1],
                  'interpretation': 'Compare within-pair changes and motion controls; no automatic quality ranking.'}
        output = json.dumps(report, indent=2, allow_nan=False) + '\n'
        if args.output:
            if args.output.exists():
                raise ValueError("output already exists; refusing to overwrite")
            args.output.write_text(output)
        else:
            print(output, end='')
    except (ValueError, OSError, KeyError, TypeError, AttributeError, zlib.error, struct.error) as e:
        ap.exit(2, f'error: {e}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
