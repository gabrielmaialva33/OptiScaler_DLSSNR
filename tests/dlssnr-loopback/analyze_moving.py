#!/usr/bin/env python3
"""Independent geometric/processing checks and descriptive moving-scene metrics (stdlib)."""
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

TRIALS = ('off-a', 'on-a', 'on-b', 'off-b')
FRAMES = range(64, 80)


def percentile(values, q):
    if not values or not all(math.isfinite(x) and x > 0 for x in values):
        raise ValueError('timing samples must be nonempty, finite and positive')
    return sorted(values)[max(0, math.ceil(len(values) * q) - 1)]


def transform(point, matrix):
    return [sum(point[j] * matrix[j * 4 + i] for j in range(4)) for i in range(4)]


def previous_projection(meta, x, y, depth):
    w, h = meta['width'], meta['height']
    jx, jy = meta['jitter']
    # Inverse current model-view-projection reconstructs LOCAL space from GPU raster depth.
    ndc = [2 * (x + .5 - jx) / w - 1, 1 - 2 * (y + .5 - jy) / h, depth, 1]
    local = transform(ndc, meta['inverse'])
    previous = transform(local, meta['previous'])
    if not all(math.isfinite(v) for v in local + previous):
        raise ValueError('projection must be finite')
    if local[3] <= 0 or previous[3] <= 0:
        raise ValueError('previous geometry projects behind the camera')
    result = (.5 + .5 * previous[0] / previous[3], .5 - .5 * previous[1] / previous[3], previous[2]/previous[3])
    if not all(math.isfinite(v) for v in result):
        raise ValueError('projected coordinates must be finite')
    return result


def check_guides(meta, depth, motion):
    w, h = meta['width'], meta['height']
    for key, size in (('inverse', 16), ('previous', 16), ('jitter', 2)):
        values = meta[key]
        if len(values) != size or not all(math.isfinite(v) for v in values):
            raise ValueError(f'{key} metadata must have {size} finite values')
    if len(depth) != w * h or len(motion) != w * h * 2:
        raise ValueError('guide extent mismatch')
    if not all(math.isfinite(v) for v in depth + motion):
        raise ValueError('guide pixels must be finite')
    errors, magnitude = [], []
    for y in range(2, h - 2, 5):
        for x in range(2, w - 2, 5):
            p = y * w + x
            d = depth[p]
            # Avoid silhouettes where bilinear motion samples have no geometric correspondence.
            if not 0 < d < .99999 or any(abs(depth[q] - d) > .002 for q in (p-1, p+1, p-w, p+w)):
                continue
            u, v, _ = previous_projection(meta, x, y, d)
            jx, jy = meta['jitter']
            expected = (u * w - (x + .5 - jx), v * h - (y + .5 - jy))
            actual = motion[2*p:2*p+2]
            errors.append(max(abs(a-b) for a, b in zip(expected, actual)))
            magnitude.append(math.hypot(*expected))
    if len(errors) < 20:
        raise ValueError('insufficient geometric guide coverage')
    if sum(magnitude) / len(magnitude) < .02:
        raise ValueError('motion fixture is stationary')
    if max(errors) > .1:
        raise ValueError(f'motion disagrees with depth/projection oracle: max={max(errors):.6f}px')
    return dict(samples=len(errors), max_error_px=max(errors), mean_motion_px=sum(magnitude)/len(magnitude))


def processing_coverage(off, on, repeat):
    if not len(off) == len(on) == len(repeat) or not off:
        raise ValueError('processing pixel counts differ')
    if not all(math.isfinite(v) for data in (off, on, repeat) for v in data):
        raise ValueError('processing pixels must be finite')
    mae = sum(abs(a-b) for a, b in zip(off, on)) / len(off)
    repeat_mae = sum(abs(a-b) for a, b in zip(on, repeat)) / len(off)
    if mae <= 1e-6:
        raise ValueError('processing coverage missing: enabled output equals disabled output')
    if repeat_mae >= mae:
        raise ValueError('repeat difference obscures the processing effect')
    return dict(mae=mae, repeat_mae=repeat_mae)


def read_raw(path, channels, w, h, code='e'):
    data = path.read_bytes()
    expected = w * h * channels * struct.calcsize(code)
    if len(data) != expected:
        raise ValueError(f'incomplete readback {path.name}: {len(data)} != {expected}')
    values = list(struct.unpack('<' + code * (w*h*channels), data))
    if not all(math.isfinite(v) for v in values):
        raise ValueError(f'nonfinite readback {path.name}')
    return values


def rgb(values):
    return [v for i, v in enumerate(values) if i % 4 != 3]


def sample_rgb(values, x, y, w, h):
    if not (0 <= x < w-1 and 0 <= y < h-1):
        return None
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x-ix, y-iy
    result = []
    for channel in range(3):
        a = values[(iy*w+ix)*4+channel] * (1-fx) + values[(iy*w+ix+1)*4+channel] * fx
        b = values[((iy+1)*w+ix)*4+channel] * (1-fx) + values[((iy+1)*w+ix+1)*4+channel] * fx
        result.append(a*(1-fy)+b*fy)
    return result


def analyze(root, negative=None):
    manifests, outputs, guides = {}, {}, {}
    hashes = {}
    timing = {}
    negative_controls = []
    for trial in TRIALS:
        directory = root / trial
        m = json.loads((directory / 'moving.json').read_text())
        if m['frames'] != 80 or m['measured'] != 16 or m['width'] != 640 or m['height'] != 360:
            raise ValueError(f'incomplete fixed trial {trial}')
        log = (directory / 'dlssnr-loopback.log').read_text(errors='replace')
        if 'MOVING-SCENE PASS: evaluations=80 captures=16' not in log:
            raise ValueError(f'incomplete GPU trial {trial}')
        opti = (directory / 'OptiScaler.log').read_text(errors='replace')
        if trial.startswith('on') and 'DLSS-NR composition' not in opti:
            raise ValueError(f'production NR composition absent in {trial}')
        w, h = m['width'], m['height']
        if [f['frame'] for f in m['samples']] != list(FRAMES):
            raise ValueError('frame identity missing or reordered')
        timing[trial] = dict(n=16, p50_ms=percentile([f['gpu_ms'] for f in m['samples']], .5),
                             p95_ms=percentile([f['gpu_ms'] for f in m['samples']], .95),
                             samples_ms=[f['gpu_ms'] for f in m['samples']])
        outputs[trial], guides[trial] = [], []
        for sample in m['samples']:
            frame = sample['frame']
            stem = directory / f'f{frame}'
            current_hashes = {}
            for side in ('source', 'depth', 'motion'):
                data = stem.with_suffix('.' + side).read_bytes()
                current_hashes[side] = hashlib.sha256(data).hexdigest()
            if trial == 'off-a':
                hashes[frame] = current_hashes
            elif current_hashes != hashes[frame]:
                raise ValueError(f'non-deterministic scene/guides in {trial} frame {frame}')
            depth = read_raw(stem.with_suffix('.depth'), 1, w, h, 'f')
            motion = read_raw(stem.with_suffix('.motion'), 2, w, h)
            if negative == 'zero-motion':
                motion = [0.] * len(motion)
            if trial == 'off-a':
                meta = dict(sample, width=w, height=h)
                guides[trial].append(check_guides(meta, depth, motion))
                if frame == 64 and negative is None:
                    try:
                        check_guides(meta, depth, [0.] * len(motion))
                    except ValueError as error:
                        if 'motion' not in str(error):
                            raise
                        negative_controls.append(dict(control='zero-motion', reason=str(error)))
                    else:
                        raise ValueError('zero-motion negative control accepted')
            source = read_raw(stem.with_suffix('.source'), 4, w, h)
            output = read_raw(stem.with_suffix('.output'), 4, w, h)
            outputs[trial].append(output)
            if trial == 'off-a':
                guides['data'] = guides.get('data', []) + [(depth, motion, sample)]
            if frame in (64, 79):
                # Preview assumes linear SDR, and is explicitly display-only (metrics use raw data).
                image = bytes(round(min(1., max(0., v)) ** (1/2.2) * 255) for v in rgb(output))
                stem.with_suffix('.ppm').write_bytes(f'P6\n{w} {h}\n255\n'.encode() + image)
        manifests[trial] = m
    if hashes[64]['source'] == hashes[79]['source']:
        raise ValueError('moving source is frozen')
    if negative == 'disabled-processing':
        outputs['on-a'] = outputs['off-a']
        outputs['on-b'] = outputs['off-a']
    if negative is None:
        control = rgb(outputs['off-a'][0])
        try:
            processing_coverage(control, control, control)
        except ValueError as error:
            negative_controls.append(dict(control='disabled-processing', reason=str(error)))
        else:
            raise ValueError('disabled-processing negative control accepted')
    processing = []
    temporal = []
    for i, frame in enumerate(FRAMES):
        off, on, repeat = (rgb(outputs[t][i]) for t in ('off-a', 'on-a', 'on-b'))
        processing.append(processing_coverage(off, on, repeat))
        # Disabled repeated runs must remain reproducible too, before any temporal comparison.
        if outputs['off-a'][i] != outputs['off-b'][i]:
            raise ValueError('disabled DLSS output is not reproducible')
        if i == 0:
            continue
        depth, motion, sample = guides['data'][i]
        previous_depth, _, previous_sample = guides['data'][i-1]
        current_edit = [a-b for a, b in zip(outputs['on-a'][i], outputs['off-a'][i])]
        previous_edit = [a-b for a, b in zip(outputs['on-a'][i-1], outputs['off-a'][i-1])]
        changes = []
        w, h = 640, 360
        for y in range(2, h-2, 3):
            for x in range(2, w-2, 3):
                p = y*w+x
                if not 0 < depth[p] < .99999:
                    continue
                # p indexes a raw raster guide. DLAA outputs are de-jittered, so compare the
                # current edit at p-currentJitter with the previous edit there + scene motion.
                cx = x - sample['jitter'][0]
                cy = y - sample['jitter'][1]
                px = cx + motion[2*p]
                py = cy + motion[2*p+1]
                qx = round(px + previous_sample['jitter'][0])
                qy = round(py + previous_sample['jitter'][1])
                if not (1 <= qx < w-1 and 1 <= qy < h-1):
                    continue
                q = qy*w+qx
                _, _, expected_depth = previous_projection(dict(sample, width=w, height=h), x, y, depth[p])
                if abs(previous_depth[q]-expected_depth) > .002:
                    continue
                current_rgb = sample_rgb(current_edit, cx, cy, w, h)
                previous_rgb = sample_rgb(previous_edit, px, py, w, h)
                if current_rgb is not None and previous_rgb is not None:
                    changes.extend(abs(a-b) for a, b in zip(current_rgb, previous_rgb))
        if not changes:
            raise ValueError('no temporal surface matches')
        temporal.append(dict(frame=frame, channels=len(changes), residual_mae=sum(changes)/len(changes)))
    return dict(schema=1, scope='D3D12 DLAA + production NR; descriptive synthetic metrics, no quality/FPS verdict',
                guide_oracle=guides['off-a'], processing=processing, timing=timing, negative_controls=negative_controls,
                motion_compensated_edit_temporal=temporal, input_sha256=hashes)


def main():
    root = Path(sys.argv[1])
    report = analyze(root, sys.argv[2] if len(sys.argv) > 2 else None)
    provenance = root / 'provenance.json'
    if provenance.exists():
        report['provenance'] = json.loads(provenance.read_text())
    temporary = root / 'moving-report.json.tmp'
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(root / 'moving-report.json')
    print('MOVING ANALYSIS PASS: deterministic guides, production processing and 2 negative controls')
    for trial, timing in report['timing'].items():
        print(f"  {trial}: Evaluate GPU p50={timing['p50_ms']:.3f} p95={timing['p95_ms']:.3f} ms n={timing['n']}")


if __name__ == '__main__':
    main()
