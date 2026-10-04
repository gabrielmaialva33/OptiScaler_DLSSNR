#!/usr/bin/env python3
"""Peripheral compression's layout and mapping (shaders/dlssnr/DlssNr_Periphery.h and the shared
precompile/dlssnr_periphery_warp.h the shader includes), compiled under ASan/UBSan, plus the text guards that
keep the shader, its constant block and the C++ in step. dlssnr/design/peripheral-compression.md."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]
dlssnr = repo / 'OptiScaler/shaders/dlssnr'

failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def body(text, tag):
    opening = text.index('{', text.index(tag))
    return text[opening + 1:text.index('};', opening)]


hlsl = (dlssnr / 'precompile/dlssnr_periphery.hlsl').read_text()
warp = (dlssnr / 'precompile/dlssnr_periphery_warp.h').read_text()
layout = (dlssnr / 'DlssNr_Periphery.h').read_text()
pass_cpp = (dlssnr / 'DlssNr_Periphery_Dx12.cpp').read_text()
composition = (dlssnr / 'precompile/dlssnr.hlsl').read_text()

# 1. The constant block is one ordered list in C++ and HLSL: sixteen 4-byte scalars, then the two axes.
cpp_fields = re.findall(r'\b(uint32_t|float|PeripheryAxis)\s+(\w+)\s*;',
                        strip_comments(body(layout, 'struct alignas(256) PeripheryConstants')))
hlsl_fields = re.findall(r'\b(uint|float|PeripheryAxis)\s+(\w+)\s*;', strip_comments(body(hlsl, 'cbuffer Params')))
cpp_list = [(t.replace('uint32_t', 'uint'), n.lower()) for t, n in cpp_fields]
hlsl_list = [(t, (n[1:] if n.startswith('g') else n).lower()) for t, n in hlsl_fields]
check(len(cpp_list) == 18, f'PeripheryConstants: expected 16 scalars and 2 axes, parsed {len(cpp_list)}')
check(cpp_list == hlsl_list, f'PeripheryConstants and cbuffer Params diverge:\n  C++  {cpp_list}\n  HLSL {hlsl_list}')

# The axis is shared text, so it cannot diverge; it must stay sixteen plain floats (no arrays: a cbuffer pads
# each array element to a row).
axis = strip_comments(body(warp, 'struct PeripheryAxis'))
check(len(re.findall(r'\bfloat\s+\w+\s*;', axis)) == 16 and '[' not in axis,
      'PeripheryAxis must be sixteen plain floats')

# 2. The colour pack reduces to mode 2 of dlssnr.hlsl when nothing is compressed; test.cpp holds the two
# together against a transcription of mode 2, which is only worth anything while mode 2 still reads this way.
mode2 = composition[composition.index('if (gMode == 2)'):composition.index('if (gMode == 0)')]
for anchor in ('const float x0 = ((float) id.x * (float) srcW) / (float) gWidth;',
               'const float x1 = ((float) (id.x + 1) * (float) srcW) / (float) gWidth;',
               'const float y0 = ((float) id.y * (float) srcH) / (float) gHeight;',
               'const float area = (x1 - x0) * (y1 - y0);',
               'const int i1 = (int) ceil(x1) - 1;',
               'acc += gSource.Load(int3(ii, jj, 0)).rgb * (max(bX - aX, 0.0) * wy);',
               'gTarget[id.xy] = float4(acc / area,'):
    check(anchor in mode2, f'mode 2 of dlssnr.hlsl no longer reads "{anchor}"; re-transcribe Mode2 in test.cpp')

# 3. The shader is the three passes the C++ loads, under the names the build comment gives them.
for define, stem in (('PERIPHERY_COLOUR', 'DlssNr_PeripheryColour'), ('PERIPHERY_GUIDES', 'DlssNr_PeripheryGuides'),
                     ('PERIPHERY_UNPACK', 'DlssNr_PeripheryUnpack')):
    check(f'defined({define})' in hlsl, f'dlssnr_periphery.hlsl has no {define} pass')
    check(f'-D {define} -Fo {stem}_Shader.cso' in hlsl, f'the build comment does not compile {define} to {stem}_Shader.cso')
    check(f'{stem}_Shader.cso {stem}_Shader.h {stem}_cso' in hlsl, f'the build comment does not make {stem}_Shader.h')
    check(f'__has_include("precompile/{stem}_Shader.h")' in pass_cpp, f'DlssNr_Periphery_Dx12.cpp does not look for {stem}_Shader.h')
    check(f'{stem}_cso' in pass_cpp, f'DlssNr_Periphery_Dx12.cpp does not load {stem}_cso')
for macro in ('PW_FN', 'PW_ABS', 'PW_FLOOR', 'PW_CEIL', 'PW_MINF', 'PW_MAXF', 'PW_MINI', 'PW_MAXI', 'PW_COLOUR',
              'PW_COLOUR_ZERO', 'PW_LOAD_COLOUR'):
    check(re.search(rf'#define {macro}\b', hlsl) is not None, f'dlssnr_periphery.hlsl does not define {macro}')
check(hlsl.index('#define PW_FN') < hlsl.index('#include "dlssnr_periphery_warp.h"') < hlsl.index('cbuffer Params'),
      'the shader must define the macros, then include the warp header, then declare the cbuffer that uses its axis')

# 4. The mapping core and its licence travel together.
for text, name in ((warp, 'dlssnr_periphery_warp.h'), (layout, 'DlssNr_Periphery.h')):
    check('BeliyG3' in text and 'MIT' in text and 'PeripheralWarp_LICENSE.txt' in text,
          f'{name} lost its PeripheralWarp attribution')
licence = repo / 'Licenses/PeripheralWarp_LICENSE.txt'
check(licence.exists() and 'Yuri Grib' in licence.read_text() and 'MIT License' in licence.read_text(),
      'Licenses/PeripheralWarp_LICENSE.txt is missing or is not the MIT text')

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-periphery-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', '-I', str(repo / 'OptiScaler'), str(here / 'test.cpp'), '-o',
                    binary], check=True)
    subprocess.run([binary], check=True)

if failures:
    for message in failures:
        print(f'FAIL: {message}')
    raise SystemExit(1)

print('PASS: constant block == cbuffer, mode 2 anchors, three passes named and loaded, attribution and licence')
