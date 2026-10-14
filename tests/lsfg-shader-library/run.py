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
- tools/lsfg-study/convert.py's Float16 detection (the --fp16-native study conversion): synthetic modules, and on
  the user's own lsfg-vk.dll exactly the FP16 variant ids (51..98), never base shader 2
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


# convert.py (study tool): which modules get a native 16-bit conversion. Synthetic SPIR-V first, then the user's
# own DLL when present. Nothing extracted is written anywhere.
sys.dont_write_bytecode = True
sys.path.insert(0, str(repo / "tools" / "lsfg-study"))
import convert  # noqa: E402

import struct  # noqa: E402


def spirv(*instructions):
    words = [0x07230203, 0x00010000, 0, 16, 0]
    for opcode, operands in instructions:
        words.append(((1 + len(operands)) << 16) | opcode)
        words.extend(operands)
    return struct.pack(f"<{len(words)}I", *words)


OP_CAPABILITY, OP_MEMORY_MODEL = 17, 14
SHADER, FLOAT16, IMAGE_QUERY, FLOAT64 = 1, 9, 50, 10

checks = [
    (spirv((OP_CAPABILITY, [SHADER]), (OP_CAPABILITY, [FLOAT16]), (OP_MEMORY_MODEL, [0, 1])), True),
    (spirv((OP_CAPABILITY, [SHADER]), (OP_CAPABILITY, [IMAGE_QUERY]), (OP_MEMORY_MODEL, [0, 1])), False),
    (spirv((OP_CAPABILITY, [FLOAT64]), (OP_MEMORY_MODEL, [0, 1])), False),
    # capabilities open a module: a Float16 operand after another instruction is not a capability declaration
    (spirv((OP_CAPABILITY, [SHADER]), (OP_MEMORY_MODEL, [0, 1]), (OP_CAPABILITY, [FLOAT16])), False),
    (b"", False),
    (spirv((OP_CAPABILITY, [FLOAT16]))[:-2], False),  # not a whole number of words
]
for index, (blob, expected) in enumerate(checks):
    if convert.declares_float16(blob) != expected:
        print(f"FAIL convert.declares_float16 case {index}: expected {expected}")
        sys.exit(1)
print(f"convert.declares_float16: {len(checks)} synthetic cases")

dll = Path.home() / ".local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll"
if dll.is_file():
    modules = convert.extract_spv_from_pe(dll.read_bytes())
    fp16 = sorted(rid for rid, blob in modules.items() if convert.declares_float16(blob))
    # VariantRid (retoque/lsfg/LsfgGraph.h): core shaders 3..26, +24 performance, +48 FP16. generate_16bit (2) is
    # the 16-bit-output generate, not an FP16 module.
    if fp16 != list(range(51, 99)):
        print(f"FAIL real lsfg-vk.dll: Float16 modules are {fp16}, expected 51..98")
        sys.exit(1)
    print("real lsfg-vk.dll: Float16 declared by exactly ids 51..98")
else:
    print("real lsfg-vk.dll: skipped (not installed)")
