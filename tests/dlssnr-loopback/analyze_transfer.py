#!/usr/bin/env python3
"""Analyze the half-size Transfer A/B readbacks; numpy only, pixels and the harness's own timing.

Every trial ran the model at 640x360 under a 1280x720 frame with identical history, so two trials of
one input differ only in the resolve. Reports, per trial, how much fine structure the applied edit
carries (the edit's own high-pass energy: after minus before, minus its 3x3 box mean), whether the
resolve produced any black pixel the source did not have, and the resolve-side GPU time."""
import hashlib
import json
import re
import sys
from pathlib import Path

import numpy as np

TRIALS = ('sdr-t0', 'sdr-t1', 'sdr-t3', 'lin-t0', 'lin-t1', 'lin-t3')
FRAMES = (1, 23)
HEADER = b'P6\n1280 720\n255\n'


def read(path, hashes):
    raw = path.read_bytes()
    if not raw.startswith(HEADER) or len(raw) != len(HEADER) + 1280 * 720 * 3:
        raise RuntimeError(f'incomplete/invalid capture {path}')
    hashes[path.name] = hashlib.sha256(raw).hexdigest()
    return np.frombuffer(raw[len(HEADER):], dtype=np.uint8).reshape(720, 1280, 3)


def box3(a):
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode='edge')
    return sum(p[y:y + a.shape[0], x:x + a.shape[1]] for y in range(3) for x in range(3)) / 9.0


def detail(before, after):
    edit = after.astype(np.float64) - before.astype(np.float64)
    return float(np.abs(edit - box3(edit)).mean()), float(np.abs(edit).mean()), int(np.abs(edit).max())


def diff(a, b):
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return {'mae': round(float(d.mean()), 4), 'max': int(d.max()),
            'changed': round(float(np.count_nonzero(d)) / d.size, 4)}


def main(directory):
    root = Path(directory)
    hashes, report = {}, {'trials': {}, 'pairs': {}, 'timing': {}}
    log = (root / 'dlssnr-loopback.log').read_text(errors='replace') if (root / 'dlssnr-loopback.log').exists() else ''
    for m in re.finditer(r'^TIMING trial=(\S+) model=(\d+)x(\d+) n=(\d+) model_ms p10=[\d.]+ median=([\d.]+) '
                         r'p90=[\d.]+ outside_model_ms median=([\d.]+)', log, re.M):
        report['timing'][m.group(1)] = {'model': f'{m.group(2)}x{m.group(3)}', 'n': int(m.group(4)),
                                        'model_ms': float(m.group(5)), 'outside_ms': float(m.group(6))}
    print(f'{"trial":<8} {"frame":>5} {"edit hp":>9} {"edit mean":>9} {"max":>4} {"new black":>9}'
          f' {"outside_ms":>10}')
    images = {}
    for trial in TRIALS:
        report['trials'][trial] = {}
        for frame in FRAMES:
            before = read(root / f'{trial}-f{frame}-before.ppm', hashes)
            after = read(root / f'{trial}-f{frame}-after.ppm', hashes)
            images[(trial, frame)] = after
            hp, mean, peak = detail(before, after)
            black = int(np.count_nonzero((after.max(axis=2) == 0) & (before.max(axis=2) > 8)))
            report['trials'][trial][frame] = {'edit_highpass': round(hp, 4), 'edit_mean': round(mean, 4),
                                              'edit_max': peak, 'new_black_pixels': black}
            t = report['timing'].get(trial, {}).get('outside_ms', float('nan'))
            print(f'{trial:<8} {frame:>5} {hp:>9.4f} {mean:>9.4f} {peak:>4} {black:>9} {t:>10.3f}')
    for kind in ('sdr', 'lin'):
        for a, b in (('t1', 't3'), ('t0', 't1')):
            key = f'{kind}-{a} vs {kind}-{b}'
            report['pairs'][key] = {f: diff(images[(f'{kind}-{a}', f)], images[(f'{kind}-{b}', f)]) for f in FRAMES}
            print(key, report['pairs'][key])
    report['sha256'] = hashes
    (root / 'transfer-report.json').write_text(json.dumps(report, indent=1))
    print(f'report: {root / "transfer-report.json"}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent / 'artifacts/transfer-run')
