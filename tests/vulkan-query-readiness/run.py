#!/usr/bin/env python3
"""Run the real Vulkan timing class and methods against scripted host API fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
production = root / 'OptiScaler/upscaler_time'


def without_includes(path):
    # Keep all declarations and method bodies; replace only platform dependencies.
    return '\n'.join(line for line in path.read_text().splitlines()
                     if not line.startswith(('#include ', '#pragma once'))) + '\n'


unit = (here / 'fakes.h').read_text()
unit += without_includes(production / 'UpscalerTime_Vk.h')
unit += without_includes(production / 'UpscalerTime_Vk.cpp')
unit += (here / 'cases.cpp').read_text()
cases = ['inactive', 'not-ready-retry', 'not-ready-pending', 'partial-not-ready', 'errors',
         'success-once', 'invalid-duration', 'record-again']
with tempfile.TemporaryDirectory(prefix='optiscaler-vulkan-query-readiness-') as directory:
    cpp = Path(directory) / 'timing.cpp'
    binary = Path(directory) / 'timing'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    failed = []
    for case in cases:
        result = subprocess.run([str(binary), case], check=False)
        print(f'{"PASS" if result.returncode == 0 else "FAIL"} {case}', flush=True)
        if result.returncode:
            failed.append(case)
    if failed:
        raise SystemExit('Failed cases: ' + ', '.join(failed))
