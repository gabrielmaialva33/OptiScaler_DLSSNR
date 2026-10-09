#!/usr/bin/env python3
"""Exercise DlssNr::GetRenderSize resolution and fallback rules."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-render-size-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'OptiScaler'),
                    '-I', str(root / 'external/nvngx_dlss_sdk'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
