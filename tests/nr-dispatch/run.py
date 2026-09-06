#!/usr/bin/env python3
"""Exercise the production composition constructor, dispatch and cleanup with host fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
renderer = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
header = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.h').read_text()


def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


declaration = header[header.index('#define DLSSNR_NUM_OF_HEAPS'):]
implementation = between(renderer, 'DlssNr_Dx12::DlssNr_Dx12(', 'bool DlssNr_Dx12::Dispatch(')
unit = (here / 'fakes.h').read_text() + '\n' + declaration + '\n' + implementation
unit += '\n' + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-dispatch-') as directory:
    source = Path(directory) / 'dispatch.cpp'
    binary = Path(directory) / 'dispatch'
    source.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
