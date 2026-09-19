#!/usr/bin/env python3
"""Match the real MFG byte signatures against synthetic buffers; no patcher/DLL is run."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

# The real byte matcher: FindPattern is a free function in scanner.cpp, and unlike scanner::GetAddress
# it walks a raw range rather than a PE image, so it compiles and runs on the host untouched.
scanner = (repo / 'OptiScaler/scanner/scanner.cpp').read_text()
begin = scanner.index('uintptr_t FindPattern(')
find_pattern = scanner[begin:scanner.index('uintptr_t scanner::GetAddress', begin)].rstrip()

# The real signature strings, byte-for-byte from the module under test.
mfg = (repo / 'OptiScaler/framegen/dlssg/MfgUnlock.cpp').read_text()
patterns = re.findall(r'^constexpr std::string_view k\w+ = ".*?";', mfg, re.M)
assert len(patterns) == 5, f'expected 5 MFG signature constants, sliced {len(patterns)}'

slice_block = find_pattern + '\n\n' + '\n'.join(patterns) + '\n'
harness = (root / 'test.cpp').read_text()
assert '// @@PRODUCTION_SLICES@@' in harness, 'injection marker missing from test.cpp'
source = harness.replace('// @@PRODUCTION_SLICES@@', slice_block)

with tempfile.TemporaryDirectory(prefix='optiscaler-mfg-pattern-') as directory:
    generated = Path(directory) / 'generated.cpp'
    generated.write_text(source)
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    # scanner.cpp returns NULL as a uintptr_t on a miss; that production idiom is not
                    # ours to change, so tolerate only that one warning while keeping -Werror otherwise.
                    '-Wno-conversion-null',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    str(generated), '-o', binary], check=True)
    subprocess.run([binary], check=True)
