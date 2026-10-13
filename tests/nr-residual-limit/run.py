#!/usr/bin/env python3
"""The residual limit (docs/dlssnr/design/residual-limit.md): the composition's own bound, sliced out of dlssnr.hlsl
and compiled under ASan/UBSan against HLSL's intrinsics, held against the guard alone; the host's clamp of the
configured value; and source guards on where the constant is read, set and shown. No GPU, no shader compile."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'

shader = (retoque / 'shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
common = (retoque / 'shaders/dlssnr/DlssNr_Common.h').read_text()
renderer = (retoque / 'shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
vulkan = (retoque / 'dlssnr/DlssNrFeature_Vk.cpp').read_text()
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()


def code(text):
    return re.sub(r'//[^\n]*', '', text)


# The shader reads the constant in one place only: the clamp after the guard. Not in the encode, not in the rebuilt
# proxy of the matched residual, not in the replace decodes (DEVELOPMENT.md rules 1 and 7).
live = code(shader)
assert live.count('gResidualLimit') == 4, live.count('gResidualLimit')
cbuffer = live[live.index('cbuffer Params'):]
cbuffer = cbuffer[cbuffer.index('{') + 1:cbuffer.index('};')]
assert re.findall(r'\b(?:uint|float)\s+(\w+)\s*;', cbuffer)[-1] == 'gResidualLimit', 'appended last'
gate = live.index('if (gResidualLimit > 0.0)')
assert live.index('gTransferStrength <= 0.0') < gate, 'strength zero returns the frame before the limit is read'
assert live.index('float boundedRatio = clamp(amplified, drop, lift);') < gate
assert gate < live.index('upgraded *= boundedRatio / max(lumaRatio, 1e-6);'), 'the correction carries it'
assert gate < live.index('const float3 lumaOnly = original * boundedRatio;'), 'and the colour blend keeps it'
assert gate < live.index('if (gReversibleMode == 2)'), 'the replace curves overwrite the composed result after it'

# The host sends the clamped value on both paths; the Vulkan resolve takes a copy of the encode's constants.
assert 'resolveParams.ResidualLimit = DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default());' in renderer
assert 'encode.ResidualLimit = DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default());' in vulkan
assert 'DlssNrConstants resolve = encode;' in vulkan
# A screenshot taken with it on can be told from a bug: the composition line carries it, and a change of it resets the
# output stabilizer (the same comparison) and the pulse metric's window.
compose = renderer[renderer.index('struct ComposeReport\n'):]
compose = compose[:compose.index('};')]
assert 'float residualLimit;' in compose and 'residualLimit == other.residualLimit' in compose
assert 'residual limit {}' in renderer
assert 'float residualLimit = 0.0f;' in renderer[renderer.index('struct PulseSession'):renderer.index('static PulseSession g_pulse;')]
# No control where nothing composes: the replace curves, and the D3D12 driver proxy (rule 2).
assert 'reversibleNow != 2 && reversibleNow != 4 && (vulkan || !config->DlssNrUseProxy.value_or_default())' in menu
assert 'kDlssNrResidualLimitMax' in menu

print('PASS: read once, after the guard and the strength-zero return, before the correction, the colour blend and '
      'the replace curves; appended last; sent clamped on D3D12 and Vulkan; logged, resetting the stabilizer and the '
      'pulse window; hidden where nothing composes', flush=True)

# The bound, twice: whole, and cut before the residual limit.
start = shader.index('    const float guard = max(gMaxRatio, 1.0);')
guard_end = shader.index('    float boundedRatio = clamp(amplified, drop, lift);\n') + \
    len('    float boundedRatio = clamp(amplified, drop, lift);\n')
limit_line = '        boundedRatio = clamp(boundedRatio, exp2(-gResidualLimit), exp2(gResidualLimit));\n'
limit_end = shader.index(limit_line) + len(limit_line)
floor = re.search(r'const float kRatioFloor = [^;]+;', shader).group(0)
bound = (f'static float GuardOnly(float amplified, float originalLuma, float gMaxRatio)\n{{\n    {floor}\n'
         f'{shader[start:guard_end]}    return boundedRatio;\n}}\n\n'
         f'static float Bound(float amplified, float originalLuma, float gMaxRatio, float gResidualLimit)\n{{\n'
         f'    {floor}\n{shader[start:limit_end]}    return boundedRatio;\n}}\n')

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-residual-limit-') as directory:
    (Path(directory) / 'bound.inc').write_text(bound)
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', directory, '-I', str(retoque), str(here / 'test.cpp'), '-o',
                    binary], check=True)
    subprocess.run([binary], check=True)
