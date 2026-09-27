#!/usr/bin/env python3
"""Analyze the ControlMask A/B readbacks; numpy + Pillow, pixels only, no claims from logs.

Each comparison pairs a mask trial with the unmasked trial that shares its UseAutoMask and skin
settings and ran before any mask was written. Both went through identical sources, history and model
creation, and base-r0 against base-r1 measures what two unmasked runs differ by, so anything above that
floor is the mask. Split by the HUD cover the harness saved, so a mask over the HUD can be told apart
from one that moved the whole picture."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

FRAMES = (1, 8, 15, 16, 24, 31)
PAIRS = (
    ('base-r0', 'base-r1', 'repeat floor: two unmasked runs'),
    ('a0s0-null', 'a1s0-null', 'UseAutoMask 0 vs 1, no mask'),
    ('a0s0-null', 'a0s0-zeros', 'mask all 0 (auto off)'),
    ('a0s0-null', 'a0s0-ones', 'mask all 1 (auto off)'),
    ('a0s0-null', 'a0s0-hud', 'mask 1 over HUD (auto off)'),
    ('a1s0-null', 'a1s0-hud', 'mask 1 over HUD (auto on)'),
    ('a1s0-null', 'a1s0-ones', 'mask all 1 (auto on)'),
    ('a0s0-null', 'a0s0-cleared', 'null written after masks'),
    ('a0s0-null', 'a0s0-ones-r16f', 'mask all 1, R16_FLOAT (auto off)'),
    ('a0s0-null', 'a0s0-ones-r32f', 'mask all 1, R32_FLOAT (auto off)'),
    ('a0s0-null', 'a0s0-ones-rgba8', 'mask all 1, RGBA8 (auto off)'),
)
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
    report = {'hud_pixels': int(cover.sum()), 'rest_pixels': int(rest.sum()), 'pairs': []}
    print(f'HUD cover: {report["hud_pixels"]} px, rest: {report["rest_pixels"]} px')
    print(f'{"comparison":<34} {"frame":>5}  {"HUD mae/max/changed":>24}  {"rest mae/max/changed":>24}')
    for a, b, label in PAIRS:
        entry = {'a': a, 'b': b, 'label': label, 'frames': {}}
        for frame in FRAMES:
            x = read(root / f'{a}-f{frame}-after.ppm', hashes)
            y = read(root / f'{b}-f{frame}-after.ppm', hashes)
            hud, other = metric(x, y, cover), metric(x, y, rest)
            entry['frames'][frame] = {'hud': hud, 'rest': other}
            print(f'{label:<34} {frame:>5}  {hud["mae"]:>9.4f}/{hud["max"]:>3}/{hud["changed"]:>7.4f}'
                  f'  {other["mae"]:>9.4f}/{other["max"]:>3}/{other["changed"]:>7.4f}')
        report['pairs'].append(entry)
    report['sha256'] = hashes
    (root / 'mask-report.json').write_text(json.dumps(report, indent=1))
    print(f'report: {root / "mask-report.json"}')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent / 'artifacts/mask-run')
