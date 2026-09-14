#!/usr/bin/env python3
"""Analyze actual HUD readbacks; numpy + Pillow, no image synthesis or GPU claims from logs."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

TRIALS = ('ui0-r0', 'ui1-r0', 'ui0-r1', 'ui1-r1', 'ui0-to1', 'ui1-to0')
ROIS = {
    'full': (0, 0, 1280, 720),
    'text': (32, 112, 402, 176),
    'panel': (420, 180, 900, 470),
    'map': (930, 80, 1240, 340),
    'dynamic': (40, 550, 430, 635),
    'vacated_icon': (60, 560, 140, 625),
}


def metric(a, b):
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return {'mae': float(delta.mean()), 'max': int(delta.max()),
            'changed_channels': int(np.count_nonzero(delta)), 'channels': int(delta.size)}


def main(directory):
    root = Path(directory)
    hashes, metrics, comparisons = {}, {}, {}

    def read(trial, frame, side):
        name = f'{trial}-f{frame}-{side}.ppm'
        path = root / name
        raw = path.read_bytes()
        header = b'P6\n1280 720\n255\n'
        if not raw.startswith(header) or len(raw) != len(header) + 1280 * 720 * 3:
            raise RuntimeError(f'incomplete/invalid capture {path}')
        hashes[name] = hashlib.sha256(raw).hexdigest()
        return np.frombuffer(raw[len(header):], dtype=np.uint8).reshape(720, 1280, 3)

    for frame in range(32):
        outputs = {}
        clean = read('ui0-r0', frame, 'before')
        for trial in TRIALS:
            before = read(trial, frame, 'before')
            after = read(trial, frame, 'after')
            if not np.array_equal(clean, before):
                raise RuntimeError(f'A/B source mismatch: {trial} frame {frame}')
            if frame == 0 and not np.array_equal(before, after):
                raise RuntimeError(f'non-exact ApplyModel=0 control: {trial}')
            outputs[trial] = after
            result = {}
            for name, (x0, y0, x1, y1) in ROIS.items():
                result[name] = metric(before[y0:y1, x0:x1], after[y0:y1, x0:x1])
            metrics[f'{trial}-f{frame}'] = result
        # Repeatability is measured separately: no attribution of stochastic changes to the flag.
        pairs = [('create_0_vs_1', 'ui0-r0', 'ui1-r0'),
                 ('repeat_0', 'ui0-r0', 'ui0-r1'), ('repeat_1', 'ui1-r0', 'ui1-r1'),
                 ('switch_0to1_vs_create0', 'ui0-to1', 'ui0-r0'),
                 ('switch_0to1_vs_create1', 'ui0-to1', 'ui1-r0'),
                 ('switch_1to0_vs_create1', 'ui1-to0', 'ui1-r0')]
        for name, a, b in pairs:
            comparisons.setdefault(name, []).append(metric(outputs[a], outputs[b]))
    # The HUD must really move/disappear, not accidentally run a static fixture for all frames.
    phase_hashes = [hashes[f'ui0-r0-f{f}-before.ppm'] for f in (0, 7, 8, 15, 16, 23, 24, 31)]
    if not (phase_hashes[0] == phase_hashes[1] == phase_hashes[6] == phase_hashes[7]
            and phase_hashes[2] == phase_hashes[3] and phase_hashes[4] == phase_hashes[5]
            and len(set(phase_hashes)) == 3):
        raise RuntimeError('fixture did not execute the three intended HUD states')
    summary = {name: {'max': max(x['max'] for x in series),
                       'max_frame_mae': max(x['mae'] for x in series),
                       'changed_channels_total': sum(x['changed_channels'] for x in series)}
               for name, series in comparisons.items()}
    report = {'format': 'RGB8; MAE in byte values 0..255, not a perceptual quality score',
              'sources_equal': True, 'exact_controls': 6, 'rois': ROIS,
              'summary': summary, 'comparisons': comparisons, 'source_vs_output': metrics, 'sha256_ppm': hashes}
    (root / 'hud-report.json').write_text(json.dumps(report, indent=2) + '\n')
    # Full-size before/after plus 1:1 crops; no sharpening, difference amplification or colour edits.
    for frame in (7, 8, 16, 24, 31):
        images = [Image.fromarray(read(t, frame, side)) for t, side in
                  [('ui0-r0', 'before'), ('ui0-r0', 'after'), ('ui1-r0', 'after')]]
        canvas = Image.new('RGB', (1440, 820))
        draw = ImageDraw.Draw(canvas)
        for i, (im, label) in enumerate(zip(images, ('SOURCE', 'UICorrection=0', 'UICorrection=1'))):
            x = i * 480
            draw.text((x + 8, 8), f'{label} / frame {frame}', fill='white')
            canvas.paste(im.resize((480, 270)), (x, 30))
            canvas.paste(im.crop((420, 180, 900, 470)), (x, 320))
            canvas.paste(im.crop((32, 112, 402, 176)), (x, 630))
            canvas.paste(im.crop((40, 550, 430, 635)), (x, 715))
        canvas.save(root / f'hud-f{frame}.png')
    print(json.dumps(summary, indent=2))
    print(f'HUD analysis: 192 matching-source pairs, 6 exact controls; report and PNGs in {root}')


if __name__ == '__main__':
    main(sys.argv[1])
