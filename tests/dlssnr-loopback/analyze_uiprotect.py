#!/usr/bin/env python3
"""Analyze the UI-protection A/B readbacks; numpy only, pixels only, no claims from logs.

Every trial ran the HUD fixture with identical sources and history and differs only in the UI layers
handed to the model (hud-protection.md). Two questions, split by the HUD cover the harness saved:

1. Protection: inside the cover, how far is the output from the SOURCE (the before image)? The
   contract says an alpha of 1 with the model's own input as Backbuffer hands the pixel back, so the
   masked trials should approach zero there while base does not.
2. Collateral: outside the cover, how far is each trial from BASE? A mask over the HUD only must leave
   the rest of the picture as base made it (up to the repeat floor, base vs base-repeat)."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

FRAMES = (1, 8, 15, 16, 24, 31)
TRIALS = ('base', 'bb-only', 'alpha-hud', 'ui-a-hud', 'alpha-hud-uic0', 'alpha-hud-nobb', 'alpha-ones',
          'base-repeat')
HEADER = b'P6\n1280 720\n255\n'


def read(path, hashes):
    raw = path.read_bytes()
    if not raw.startswith(HEADER) or len(raw) != len(HEADER) + 1280 * 720 * 3:
        raise RuntimeError(f'incomplete/invalid capture {path}')
    hashes[path.name] = hashlib.sha256(raw).hexdigest()
    return np.frombuffer(raw[len(HEADER):], dtype=np.uint8).reshape(720, 1280, 3)


def metric(a, b, where):
    delta = np.abs(a.astype(np.int16) - b.astype(np.int16))[where]
    if delta.size == 0:
        return {'mae': 0.0, 'max': 0, 'changed': 0.0}
    return {'mae': round(float(delta.mean()), 4), 'max': int(delta.max()),
            'changed': round(float(np.count_nonzero(delta)) / delta.size, 4)}


def main(directory):
    root = Path(directory)
    hashes = {}
    cover = read(root / 'mask-hud-cover.ppm', hashes)[..., 0] > 127
    rest = ~cover
    report = {'hud_pixels': int(cover.sum()), 'rest_pixels': int(rest.sum()), 'trials': {}}
    print(f'HUD cover: {report["hud_pixels"]} px, rest: {report["rest_pixels"]} px')
    print(f'{"trial":<16} {"frame":>5}  {"HUD vs source mae/max":>22}  {"rest vs base mae/max":>21}')
    for trial in TRIALS:
        report['trials'][trial] = {}
        for frame in FRAMES:
            source = read(root / f'{trial}-f{frame}-before.ppm', hashes)
            out = read(root / f'{trial}-f{frame}-after.ppm', hashes)
            base = read(root / f'base-f{frame}-after.ppm', hashes)
            hud = metric(out, source, cover)
            other = metric(out, base, rest)
            report['trials'][trial][frame] = {'hud_vs_source': hud, 'rest_vs_base': other}
            print(f'{trial:<16} {frame:>5}  {hud["mae"]:>12.4f}/{hud["max"]:>3}      '
                  f'{other["mae"]:>12.4f}/{other["max"]:>3}')
    # The verdict the design note asks for, on the settled frames.
    settled = (15, 24, 31)
    def mean_of(trial, key):
        return float(np.mean([report['trials'][trial][f][key]['mae'] for f in settled]))
    verdict = {
        'base_hud_vs_source': mean_of('base', 'hud_vs_source'),
        'alpha_hud_hud_vs_source': mean_of('alpha-hud', 'hud_vs_source'),
        'ui_a_hud_hud_vs_source': mean_of('ui-a-hud', 'hud_vs_source'),
        'uic0_hud_vs_source': mean_of('alpha-hud-uic0', 'hud_vs_source'),
        'repeat_floor_rest': mean_of('base-repeat', 'rest_vs_base'),
        'alpha_hud_rest_vs_base': mean_of('alpha-hud', 'rest_vs_base'),
    }
    verdict['protects'] = verdict['alpha_hud_hud_vs_source'] < 0.25 * max(verdict['base_hud_vs_source'], 1e-6)
    verdict['gated_by_uicorrection'] = abs(verdict['uic0_hud_vs_source'] - verdict['base_hud_vs_source']) < 0.05
    report['verdict'] = verdict
    print('verdict:', json.dumps(verdict))
    report['sha256'] = hashes
    (root / 'ui-protect-report.json').write_text(json.dumps(report, indent=1))
    print(f'report: {root / "ui-protect-report.json"}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent / 'artifacts/ui-protect-run')
