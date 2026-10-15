#!/usr/bin/env python3
"""Exercise DlssNr::Exposure::PlausibilityLatch rules, windowing, and WhitePoint/LiveWanted fallback."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-exposure-latch-') as directory:
    # Compile the production function, rather than a second implementation of its rule.
    shader = (root / 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
    start = shader.index('float WhitePoint()')
    end = shader.index('\n}', start) + 2
    (Path(directory) / 'white_point.h').write_text(shader[start:end])
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', directory, '-I', str(root / 'retoque'), '-I', str(root / 'OptiScaler'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-D', 'VK_MODE',
                    '-I', directory, '-I', str(root / 'retoque'), '-I', str(root / 'OptiScaler'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
