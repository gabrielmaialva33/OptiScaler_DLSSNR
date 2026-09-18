#!/usr/bin/env python3
"""Exercise the production zero-guide provider against strict host D3D12 fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/dlssnr/DlssNr_ZeroGuides.cpp').read_text()
header = (root / 'OptiScaler/dlssnr/DlssNr_ZeroGuides.h').read_text()

# The production translation unit, minus the includes the fakes stand in for. Taking the body rather
# than a copy is the point: a change to the provider is compiled by this suite or it is not tested.
for line in ('#include "pch.h"', '#include "DlssNr_ZeroGuides.h"', '#include <Logger.h>', '#include <d3d12.h>',
             '#include <cstdint>', '#pragma once'):
    assert line in source or line in header, f'production include boundary changed: {line}'

def strip_includes(text):
    return '\n'.join(l for l in text.splitlines() if not l.startswith('#include') and l != '#pragma once')

unit = (here / 'fakes.h').read_text().replace('#pragma once\n', '', 1)
unit += '\n' + strip_includes(header)
unit += '\n' + strip_includes(source)
unit += '\n' + (here / 'cases.h').read_text()

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-zero-guides-') as directory:
    path = Path(directory) / 'guides.cpp'
    binary = Path(directory) / 'guides'
    path.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
