#!/usr/bin/env python3
"""Host unit test suite for LSFG runtime shader library.

Exercises:
- Windows Steam discovery via registry HKCU\\Software\\Valve\\Steam\\SteamPath and libraryfolders.vdf
- Wine / Proton Z: drive path discovery across Steam Linux prefixes and XDG_DATA_HOME
- INI override paths and Lossless.dll folder mapping
- Resource ID resolution formulas for Quality/Performance x FP32/FP16
- PE RT_RCDATA SPIR-V resource extraction and validation
- Real parsing of user's installed lsfg-vk.dll if present
- Content-hash-keyed shader cache management and invalidation
"""

from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

with tempfile.TemporaryDirectory(prefix="optiscaler-lsfg-shader-lib-") as directory:
    binary = str(Path(directory) / "test")
    cmd = [
        "g++",
        "-std=c++20",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        "-g",
        "-I",
        str(root / "stubs"),
        "-I",
        str(repo / "retoque"),
        str(root / "test.cpp"),
        str(repo / "retoque" / "lsfg" / "LsfgShaderLibrary.cpp"),
        str(repo / "retoque" / "lsfg" / "LsfgShaderSource.cpp"),
        "-o",
        binary,
    ]
    res = subprocess.run(cmd)
    if res.returncode != 0:
        sys.exit(res.returncode)

    res_run = subprocess.run([binary, directory])
    if res_run.returncode != 0:
        sys.exit(res_run.returncode)
