#!/usr/bin/env python3
"""Run image comparisons through Part A's compare.py (requires Pillow and NumPy)."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from run import DEFAULT, HERE, idle


def execute(cmd, log, cwd):
    idle()
    with log.open('w') as stream:
        subprocess.run(list(map(str, cmd)), stdout=stream, stderr=subprocess.STDOUT,
                       stdin=subprocess.DEVNULL, cwd=cwd, timeout=300, check=True)


def main():
    base = DEFAULT
    spec = importlib.util.spec_from_file_location('compare', HERE.parent / 'compare.py')
    compare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(compare)
    root = base / 'out/comparisons'
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    for sequence in ('translation', 'rotzoom'):
        for half, multiplier, performance, flow in [(True, 2, False, 1.), (False, 2, False, 1.),
                                                  (True, 3, False, 1.), (False, 3, False, 1.),
                                                  (True, 2, True, .5)]:
            name = f'{sequence}-fp{16 if half else 32}-m{multiplier}-{"perf" if performance else "quality"}-flow{flow:g}'
            case = root / name
            case.mkdir(exist_ok=True)
            cmd = [sys.executable, HERE / 'run.py', '--run-only', '--output', case,
                   '--multiplier', multiplier, '--flow-scale', flow]
            if half:
                cmd += ['--fp16']
            if performance:
                cmd += ['--performance']
            cmd += [base / f'input_{sequence}']
            execute(cmd, case / 'driver.log', case)
            if half and multiplier == 2 and not performance and flow == 1.:
                reference = base / f'ref/{sequence}/1.png'
                reference_kind = 'Part A'
            else:
                native = case / 'native'
                native.mkdir(exist_ok=True)
                cmd = ['lsfg-vk-cli', 'debug', '-w', 1920, '-h', 1080, '-m', multiplier,
                       '-f', flow, '-o', native]
                if not half:
                    cmd += ['-a']
                if performance:
                    cmd += ['-p']
                cmd += [base / f'input_{sequence}']
                execute(cmd, case / 'vulkan.log', case)
                reference = native / '1.dds'
                reference_kind = 'native CLI, matching options'
            # Part A's native exporter saves only the last acquire for each input.
            candidate = case / f'1_{multiplier-1}.png'
            execute([sys.executable, HERE.parent / 'compare.py', reference, candidate,
                     '--save-diff', case / 'diff.png'], case / 'comparison.log', case)
            metrics = compare.compute_metrics(compare.load_image(reference), compare.load_image(candidate), 2.)
            row = {'case': name, 'sequence': sequence, 'precision': 'FP16' if half else 'FP32',
                   'multiplier': multiplier, 'performance': performance, 'flow': flow,
                   'reference': str(reference), 'reference_kind': reference_kind,
                   'reference_sha256': hashlib.sha256(reference.read_bytes()).hexdigest(),
                   'candidate': str(candidate), **metrics}
            rows.append(row)
            (root / 'results.json').write_text(json.dumps(rows, indent=2) + '\n')
            print(f"{name}: {metrics['psnr']:.3f} dB, max {metrics['max_abs_diff']:.0f}/255, "
                  f">2 {metrics['pct_above_threshold']:.3f}%", flush=True)


if __name__ == '__main__':
    main()
