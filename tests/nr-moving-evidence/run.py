#!/usr/bin/env python3
"""Exercise the moving GPU fixture's evidence checks without Wine or a model."""
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'dlssnr-loopback/test_moving.py'], check=True)
