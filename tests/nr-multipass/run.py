#!/usr/bin/env python3
"""Run portable production helpers; no GPU/NGX execution is claimed."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-chain-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
