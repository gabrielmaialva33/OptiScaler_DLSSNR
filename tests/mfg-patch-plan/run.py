#!/usr/bin/env python3
"""Drive the MFG unlock's commit/rollback (MfgPatchPlan.h) over a fake memory with injected failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

with tempfile.TemporaryDirectory(prefix='optiscaler-mfg-patch-plan-') as directory:
    binary = str(Path(directory) / 'test')
    # The header under test is included straight from production (retoque/framegen/dlssg/), not copied.
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque/framegen/dlssg'),
                    str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
