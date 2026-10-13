#!/usr/bin/env python3
"""Run the 16 requested LSFG D3D12/native Vulkan cases sequentially and save evidence."""
import argparse
import csv
import hashlib
import itertools
import json
from pathlib import Path
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from run import DEFAULT, HERE, idle


def telemetry():
    p = subprocess.run(['nvidia-smi', '--query-gpu=name,driver_version,clocks.gr,clocks.mem,temperature.gpu,power.draw,utilization.gpu',
                        '--format=csv,noheader'], capture_output=True, text=True, check=True)
    return p.stdout.strip()


def run(command, logfile, cwd):
    idle()
    with logfile.open('w') as log:
        subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                       stdin=subprocess.DEVNULL, cwd=cwd, timeout=300, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--artifacts', type=Path, default=DEFAULT)
    ap.add_argument('--iterations', type=int, default=400)
    ap.add_argument('--warmup', type=int, default=100)
    ap.add_argument('--duration', type=int, default=5)
    args = ap.parse_args()
    base = args.artifacts.resolve()
    output = base / 'out/benchmarks'
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    manifest = {'started': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'gpu_before': telemetry(),
                'clocks': 'unlocked; no clock or scheduling changes',
                'exe_sha256': hashlib.sha256((base / 'harness/lsfg-harness.exe').read_bytes()).hexdigest(),
                'iterations': args.iterations, 'warmup': args.warmup, 'native_duration_s': args.duration}
    for (w, h), perf, half, flow in itertools.product([(1920, 1080), (3440, 1440)], (False, True), (False, True), (1.0, .5)):
        name = f'{w}x{h}-{"performance" if perf else "quality"}-fp{16 if half else 32}-flow{flow:g}'
        case = output / name
        case.mkdir(exist_ok=True)
        print(name, flush=True)
        command = [sys.executable, HERE / 'run.py', '--run-only', '--benchmark', '--artifacts', base,
                   '--output', case, '--width', w, '--height', h, '--flow-scale', flow,
                   '--iterations', args.iterations, '--warmup', args.warmup]
        if perf:
            command += ['--performance']
        if half:
            command += ['--fp16']
        before = telemetry()
        run(command, case / 'driver.log', HERE)
        d3d = json.loads((case / 'timing.json').read_text())
        assert len(d3d['passes']) == 100
        assert d3d['iterations'] == args.iterations
        assert d3d['total']['p95_ms'] >= d3d['total']['median_ms'] > 0
        assert all(p['p95_ms'] >= p['median_ms'] >= 0 for p in d3d['passes'])
        native = ['lsfg-vk-cli', 'benchmark', '-w', w, '-h', h, '-m', 2, '-t', args.duration, '-f', flow]
        if perf:
            native += ['-p']
        if not half:
            native += ['-a']
        run(native, case / 'vulkan.log', case)
        match = re.search(r'Time per iteration\s*:\s*([\d.]+) ms', (case / 'vulkan.log').read_text())
        if not match:
            raise RuntimeError(f'no native timing in {case}')
        row = {'case': name, 'width': w, 'height': h, 'mode': 'performance' if perf else 'quality',
               'precision': 'FP16' if half else 'FP32', 'flow': flow,
               'd3d_gpu_median_ms': d3d['total']['median_ms'], 'd3d_gpu_p95_ms': d3d['total']['p95_ms'],
               'd3d_generation_median_ms': d3d['generation']['median_ms'],
               'd3d_instrumented_median_ms': d3d['instrumented_total']['median_ms'],
               'd3d_wall_mean_ms': d3d['wall_mean_ms'], 'vulkan_wall_mean_ms': float(match[1]),
               'gpu_before': before, 'gpu_after': telemetry(), 'vulkan_command': list(map(str, native))}
        rows.append(row)
        (output / 'results.json').write_text(json.dumps({'manifest': manifest, 'cases': rows}, indent=2) + '\n')
        print(f"  D3D GPU {row['d3d_gpu_median_ms']:.3f}/{row['d3d_gpu_p95_ms']:.3f} median/p95 ms; "
              f"Vulkan wall {row['vulkan_wall_mean_ms']:.2f} ms", flush=True)
    fields = [k for k in rows[0] if k not in ('gpu_before', 'gpu_after', 'vulkan_command')]
    with (output / 'results.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)
    table = ['| Resolution | Mode | Variant | Flow | D3D GPU median / p95 | D3D wall mean | Vulkan wall mean |',
             '|---|---|---|---:|---:|---:|---:|']
    for r in rows:
        table.append(f"| {r['width']}×{r['height']} | {r['mode']} | {r['precision']} | {r['flow']:g} | "
                     f"{r['d3d_gpu_median_ms']:.3f} / {r['d3d_gpu_p95_ms']:.3f} | {r['d3d_wall_mean_ms']:.3f} | {r['vulkan_wall_mean_ms']:.2f} |")
    (output / 'table.md').write_text('\n'.join(table) + '\n')


if __name__ == '__main__':
    main()
