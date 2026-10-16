#!/usr/bin/env python3
"""Reject incomplete real-plugin evidence without needing a GPU or NVIDIA kit."""
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'fg-provider-d3d12/run.py', '--self-test'], check=True)
