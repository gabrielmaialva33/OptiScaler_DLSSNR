#!/usr/bin/env python3
"""Run production DLSSG option caching against the real Streamline SDK types."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]


def function(signature, source):
    begin = source.index(signature)
    start = source.index('{', begin)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


source = (repo / 'OptiScaler/framegen/dlssg/DLSSG_Common.cpp').read_text()
header = (repo / 'OptiScaler/framegen/dlssg/DLSSG_Common.h').read_text()
hooks = (repo / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text()
# Use production cache declarations, not a separately maintained fake cache layout.
members = header[header.index('    // The DLSSG and Reflex options'):header.index('    sl::ViewportHandle')]
pending = next(line for line in header.splitlines() if 'bool _mfgReinitPending' in line)
unit = (here / 'fakes.h').read_text().replace('// @cache-members@', pending + '\n' + members)
unit += '\n' + function('void StreamlineHooks::applyMenuDlssgInterlock(', hooks)
unit += '\n' + function('bool DLSSG_Common::SetOptions(', source)
unit += '\n' + (here / 'cases.cpp').read_text()
cases = ('unchanged', 'dlssg-retry', 'reflex-retry', 'generations', 'dlssg-generation-retry',
         'reflex-generation-retry', 'mfg-transition', 'mfg-off-retry', 'mfg-count-reversal', 'mfg-latest-count-retry',
         'mfg-menu-transition', 'menu', 'dynamic-reflex')
with tempfile.TemporaryDirectory(prefix='retoque-fg-dlssg-options-') as directory:
    cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(repo / 'external/streamline'), str(cpp), '-o', str(binary)], check=True)
    results = [subprocess.run([str(binary), case]).returncode for case in cases]
    if any(results):
        sys.exit(1)
