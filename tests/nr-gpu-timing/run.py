#!/usr/bin/env python3
"""Compile production portable timing validation with ASan/UBSan; no GPU claims."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nr-gpu-timing-") as directory:
    executable = pathlib.Path(directory) / "cases"
    subprocess.run(["clang++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                    str(ROOT / "tests/nr-gpu-timing/cases.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

    runtime = pathlib.Path(directory) / "runtime"
    subprocess.run(["clang++", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/nr-gpu-timing/stubs"), "-I", str(ROOT),
                    str(ROOT / "tests/nr-gpu-timing/runtime.cpp"),
                    str(ROOT / "OptiScaler/dlssnr/DlssNr_GpuTiming.cpp"), "-o", str(runtime)], check=True)
    subprocess.run([str(runtime)], check=True)
    subprocess.run([str(runtime), "--cadence"], check=True)

source = (ROOT / "OptiScaler/dlssnr/DlssNr_Submission.cpp").read_text()
batch = source.split("Batch::Batch(", 1)[1].split("void Batch::Submitted()", 1)[0]
assert re.search(r"else\s+Detail::Model::CountExecution\(e\)", batch), "duplicate execution guard missing"
print("PASS duplicate-list occurrence guard at the production submission boundary")
