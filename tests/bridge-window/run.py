#!/usr/bin/env python3
"""Compile the bridge's window takeover header (retoque/with_dx12/Dx11wDx12_Window.h) and run it, under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-bridge-window-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(repo / 'retoque'), str(here / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
