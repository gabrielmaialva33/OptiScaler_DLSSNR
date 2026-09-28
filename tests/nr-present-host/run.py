#!/usr/bin/env python3
"""Exercise the production present host (what it builds, and when) against host fakes.

The unit is DlssNr_PresentHost.h and .cpp as they are in the tree, with only their includes swapped
for fakes.h. `--source <path>` compiles another copy of the .cpp instead, which is how a fix is shown
to be what the cases detect: the cases must fail against the code from before it.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, help='compile this DlssNr_PresentHost.cpp instead of the tree\'s')
args = parser.parse_args()

source = (args.source or root / 'OptiScaler/dlssnr/DlssNr_PresentHost.cpp').read_text()
header = (root / 'OptiScaler/dlssnr/DlssNr_PresentHost.h').read_text()

# The includes fakes.h stands in for. If production grows a new one, this suite has to learn it rather
# than compile past it.
expected = {'#include "pch.h"', '#include "DlssNr_PresentHost.h"', '#include "DlssNrFeature_Dx12.h"',
            '#include "DlssNr_Identity.h"', '#include <shaders/format_transfer/FT_Dx12.h>', '#include <Config.h>',
            '#include <Logger.h>', '#include "DlssNr_ZeroGuides.h"', '#include <d3d12.h>', '#include <chrono>',
            '#include <cstdint>', '#include <memory>'}
found = {l.strip() for l in (source + '\n' + header).splitlines() if l.startswith('#include')}
assert found == expected, f'production include boundary changed: {sorted(found ^ expected)}'


def strip_includes(text):
    return '\n'.join(l for l in text.splitlines() if not l.startswith('#include') and l != '#pragma once')


unit = (here / 'fakes.h').read_text().replace('#pragma once\n', '', 1)
unit += '\n' + strip_includes(header)
unit += '\n' + strip_includes(source)
unit += '\n' + (here / 'cases.h').read_text()

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-present-host-') as directory:
    path = Path(directory) / 'host.cpp'
    binary = Path(directory) / 'host'
    path.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
