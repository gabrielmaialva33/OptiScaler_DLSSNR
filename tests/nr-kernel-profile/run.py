#!/usr/bin/env python3
import pathlib
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent

with tempfile.TemporaryDirectory(prefix="optiscaler-kernel-profile-") as tmp:
    tmp_path = pathlib.Path(tmp)
    bin_path = tmp_path / "test_bin"

    cmd = [
        "g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined",
        f"-I{ROOT / 'OptiScaler'}",
        str(HERE / "test.cpp"),
        "-o", str(bin_path)
    ]
    subprocess.run(cmd, check=True)
    res = subprocess.run([str(bin_path)], check=True, capture_output=True, text=True)
    print(res.stdout.strip())
