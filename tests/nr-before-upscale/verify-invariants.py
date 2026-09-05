#!/usr/bin/env python3
"""Check this port's unchanged shader contract and four-point Stage configuration."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base', default='660303ec', help='baseline before the Stage port')
args = parser.parse_args()


def original(path):
    return subprocess.check_output(['git', 'show', f'{args.base}:{path}'], cwd=ROOT)


def body(source, tag):
    start = source.index('{', source.index(tag))
    return source[start + 1:source.index('};', start)]


common = 'OptiScaler/shaders/dlssnr/DlssNr_Common.h'
tag = 'struct alignas(256) DlssNrConstants'
cpp = body((ROOT / common).read_text(), tag)
assert cpp == body(original(common).decode(), tag), 'shader constants changed from baseline'
shader = 'OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl'
for path in [shader, 'OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h',
             'OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader_Vk.h']:
    assert (ROOT / path).read_bytes() == original(path), f'shader changed: {path}'
hlsl = body((ROOT / shader).read_text(), 'cbuffer Params')
cpp = re.sub(r'//[^\n]*', '', cpp)
hlsl = re.sub(r'//[^\n]*', '', hlsl)
c = re.findall(r'\b(uint32_t|float)\s+(\w+)\s*;', cpp)
h = re.findall(r'\b(uint|float)\s+(\w+)\s*;', hlsl)
assert len(c) > 0
assert [(t.replace('uint32_t', 'uint'), n.lower()) for t, n in c] == [(t, n[1:].lower()) for t, n in h]
config = (ROOT / 'OptiScaler/Config.h').read_text()
implementation = (ROOT / 'OptiScaler/Config.cpp').read_text()
ini = (ROOT / 'OptiScaler.ini').read_text()
assert re.search(r'CustomOptional<uint32_t> DlssNrStage\s*\{ 0 \}', config)
assert 'DlssNrStage.set_from_config(readUInt("DlssNr", "Stage"))' in implementation
assert 'ini.SetValue("DlssNr", "Stage", GetIntValue(Instance()->DlssNrStage.value_for_config()).c_str())' in implementation
assert re.search(r'^Stage=auto$', ini[ini.index('[DlssNr]'):], re.M)
print(f'PASS: {len(c)} ordered 4-byte constants match HLSL; shader and both compiled headers unchanged; Stage has all four config points and defaults to 0')
