#!/usr/bin/env python3
"""The static-overlay mask's per-pixel rule (shaders/dlssnr/precompile/dlssnr_uimask_rule.h), the code the
shader runs, over synthetic frame sequences, compiled under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-uimask-rule-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', '-I', str(repo / 'OptiScaler'),
                    str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
