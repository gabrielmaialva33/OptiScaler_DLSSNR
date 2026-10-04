#!/usr/bin/env python3
"""Compile the production Streamline log repeat filter against a fake clock, under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-sl-log-repeat-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'OptiScaler'), str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
