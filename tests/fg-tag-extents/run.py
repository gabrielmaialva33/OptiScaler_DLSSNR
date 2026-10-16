#!/usr/bin/env python3
"""Run the real Streamline resource-tag adapter with scripted FG consumers."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (ROOT / 'OptiScaler/inputs/FG/Streamline_Inputs.cpp').read_text()
header = (ROOT / 'OptiScaler/framegen/IFGFeature_Dx12.h').read_text()
resource = header[header.index('struct Dx12Resource'):header.index('struct LockedDx12Resource')]
fakes = (HERE / 'fakes.h').read_text().replace('// @resource@', resource)
unit = fakes + function(source, 'static std::optional<FG_ResourceType> GetResourceType(')
unit += function(source, 'bool Sl_Inputs::reportResource(') + (HERE / 'cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='fg-tag-extents-') as folder:
    cpp, exe = Path(folder) / 'test.cpp', Path(folder) / 'test'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(ROOT / 'external/streamline'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
