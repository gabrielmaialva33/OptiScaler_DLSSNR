#!/usr/bin/env python3
"""Compile actual pre-upscale boundary functions with strict host fakes and ASan/UBSan."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
OUT.mkdir(exist_ok=True)
source = (ROOT / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
header = (ROOT / 'OptiScaler/dlssnr/DlssNrFeature_Dx12.h').read_text()


def between(text, start, end):
    assert text.count(start) == 1, f'instrumentation anchor changed: {start}'
    i = text.index(start)
    return text[i:text.index(end, i)]


declaration = between(header, 'class ScopedPreUpscale\n', '\n};') + '\n};\n'
allocation = between(source, 'ID3D12Resource* CreatePreUpscaleScratch(', '\nvoid Barrier(')
scope = between(source, 'ScopedPreUpscale::ScopedPreUpscale(', '// The pass. Resources in,')
after = between(source, 'void EvaluateAfterUpscale(', '// ---------------------------------------------------------------------------------------------')
coverage = between(source, 'std::mutex g_coverageMutex;', '// NGX result codes, by name.')
resources = between(source, 'struct RestoreResourceState\n', '// The upscaler\'s own names differ')
unit = '#include "fakes.h"\n' + coverage + allocation + resources
spatial = between(source, 'void ReportSpatialContract(', 'DlssNrFrameInfo GatherFrame(')
unit += '\nnamespace DlssNr {\n' + declaration + spatial + after + scope + '\n}\n#include "cases.h"\n'
(OUT / 'boundary.cpp').write_text(unit)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Wno-unused-variable',
                '-I' + str(ROOT), '-I' + str(HERE), str(OUT / 'boundary.cpp'),
                '-o', str(OUT / 'boundary')], check=True)
subprocess.run([str(OUT / 'boundary')], check=True)
