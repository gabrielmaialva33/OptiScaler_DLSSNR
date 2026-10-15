#!/usr/bin/env python3
"""The HDR present conversion's maths (docs/dlssnr/design/hdr-present-conversion.md): the shader's conversion block,
sliced out of dlssnr_present_hdr.hlsl and compiled under ASan/UBSan against HLSL's intrinsics, held to the float64
reference in dlssnr/DlssNr_PresentHdr.h -- the PQ round trip over every code, the reference bypass on untouched texels
(bit for bit, HDR10 and scRGB), what the bypass can hide, an edit within one code, non-finite input. The constants of the
two texts are compared here, and the Rec.2020/Rec.709 matrices derived from the primaries. No GPU, no shader compile."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'

shader = (retoque / 'shaders/dlssnr/precompile/dlssnr_present_hdr.hlsl').read_text()
header = (retoque / 'dlssnr/DlssNr_PresentHdr.h').read_text()

begin_marker = '// --- the conversion maths:'
end_marker = '// --- end of the conversion maths ---'
assert shader.count(begin_marker) == 1 and shader.count(end_marker) == 1, 'the conversion block markers moved'
block = shader[shader.index(begin_marker):shader.index(end_marker)]
entry = shader[shader.index(end_marker):]

# --- the shader's own shape: one decode for both passes, the encode through the bypass, alpha from the original ---
assert entry.count('DecodeOriginal(original.rgb, gProfile)') == 1, 'the decode pass writes DecodeOriginal'
assert entry.count('EncodeTexel(processed, original.rgb, gProfile)') == 1, 'the encode pass goes through EncodeTexel'
assert 'KeepsOriginal(processed, DecodeOriginal(original, profile))' in block, 'the bypass compares the same decode'
assert entry.count('original.a)') == 1 and 'gOut[id.xy] = float4(EncodeTexel' in entry, 'alpha is the original\'s'
assert 'if (id.x >= gWidth || id.y >= gHeight)' in entry
# A binding the pass class does not give would be a descriptor nobody wrote: two SRVs, one UAV, one cbuffer, no sampler.
live = re.sub(r'//[^\n]*', '', shader)
assert sorted(re.findall(r'register\(([tubs])\d', live)) == ['b', 'b', 't', 't', 'u'], 'the bindings changed'
assert 'SamplerState' not in live

# --- constants: the shader's literals are the header's numbers ---


def number(expression):
    # A literal, arithmetic on literals, or other constants of the same header by name.
    expression = re.sub(r'\bk\w+\b', lambda m: repr(header_constant(m.group(0))), expression.strip())
    expression = re.sub(r'(\d)f\b', r'\1', expression)
    assert re.fullmatch(r'[0-9.eE+\-*/ ()]+', expression), expression
    return float(eval(expression))  # digits and arithmetic only, checked above


def header_constant(name):
    match = re.search(r'inline constexpr (?:double|float|uint32_t) ' + name + r' = ([^;]+);', header)
    assert match, f'{name} is not in DlssNr_PresentHdr.h'
    return number(match.group(1))


def shader_constant(name):
    match = re.search(r'static const (?:float|uint) ' + name + r' = ([^;]+);', block)
    assert match, f'{name} is not in the shader\'s conversion block'
    return number(match.group(1))


for name in ('kPqM1', 'kPqM2', 'kPqC1', 'kPqC2', 'kPqC3', 'kPqPeakScRgb', 'kBypassEps', 'kBypassFloor',
             'kProfileHdr10', 'kProfileScRgb', 'kModeDecode', 'kModeEncode'):
    assert header_constant(name) == shader_constant(name), (name, header_constant(name), shader_constant(name))

# The ST 2084 constants are exact fractions: hold both texts to them, not to each other only.
for name, value in (('kPqM1', 2610 / 16384), ('kPqM2', 2523 / 4096 * 128), ('kPqC1', 3424 / 4096),
                    ('kPqC2', 2413 / 4096 * 32), ('kPqC3', 2392 / 4096 * 32), ('kPqPeakScRgb', 10000 / 80)):
    assert shader_constant(name) == value, name
assert header_constant('kSdrWhiteNits') == 203.0 and abs(header_constant('kSdrWhiteScRgb') - 203 / 80) < 1e-12


def header_matrix(name):
    match = re.search(r'inline constexpr double ' + name + r'\[9\] = \{([^}]*)\};', header)
    assert match, name
    values = [float(v) for v in re.findall(r'-?\d+\.\d+', re.sub(r'//[^\n]*', '', match.group(1)))]
    assert len(values) == 9, name
    return [values[0:3], values[3:6], values[6:9]]


def shader_matrix(prefix):
    rows = []
    for row in 'RGB':
        match = re.search(r'static const float3 ' + prefix + row + r' = float3\(([^)]*)\);', block)
        assert match, prefix + row
        rows.append([float(v) for v in match.group(1).split(',')])
    return rows


# The matrices from the primaries (ITU-R BT.709 and BT.2020, D65), in float64.
def rgb_to_xyz(primaries, white):
    def column(x, y):
        return [x / y, 1.0, (1.0 - x - y) / y]

    m = [list(r) for r in zip(*[column(x, y) for x, y in primaries])]
    w = column(*white)
    scale = solve(m, w)
    return [[m[r][c] * scale[c] for c in range(3)] for r in range(3)]


def solve(m, v):
    inv = invert(m)
    return [sum(inv[r][c] * v[c] for c in range(3)) for r in range(3)]


def invert(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


def multiply(x, y):
    return [[sum(x[r][k] * y[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


d65 = (0.3127, 0.3290)
rec709 = rgb_to_xyz([(0.640, 0.330), (0.300, 0.600), (0.150, 0.060)], d65)
rec2020 = rgb_to_xyz([(0.708, 0.292), (0.170, 0.797), (0.131, 0.046)], d65)
derived = {'kRec2020To709': multiply(invert(rec709), rec2020), 'kRec709To2020': multiply(invert(rec2020), rec709)}

for name in derived:
    from_header = header_matrix(name)
    from_shader = shader_matrix(name)
    for r in range(3):
        for c in range(3):
            assert from_header[r][c] == from_shader[r][c], (name, r, c)
            assert abs(from_header[r][c] - derived[name][r][c]) < 1e-9, (name, r, c, derived[name][r][c])
    # Each row of Rec.709 -> Rec.2020 sums to 1 (white stays white), and so does each row of the inverse.
    for row in from_header:
        assert abs(sum(row) - 1.0) < 1e-9, (name, row)
identity = multiply(header_matrix('kRec2020To709'), header_matrix('kRec709To2020'))
for r in range(3):
    for c in range(3):
        assert abs(identity[r][c] - (1.0 if r == c else 0.0)) < 1e-6, identity

# --- source guards: the pass class, the project, and where the two hosts use it ---
opti = root / 'OptiScaler'
pass_h = (retoque / 'shaders/dlssnr/DlssNr_PresentHdr_Dx12.h').read_text()
pass_cpp = (retoque / 'shaders/dlssnr/DlssNr_PresentHdr_Dx12.cpp').read_text()
renderer = (retoque / 'shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
host = (retoque / 'dlssnr/DlssNr_PresentHost.cpp').read_text()
# The bytecode is optional: without it the conversion says it is unavailable rather than failing the build.
assert '#if __has_include("precompile/DlssNr_PresentHdr_Shader.h")' in pass_cpp and 'DlssNr_PresentHdr_cso' in pass_cpp
assert 'create_header.py DlssNr_PresentHdr_Shader.cso DlssNr_PresentHdr_Shader.h DlssNr_PresentHdr_cso' in shader
assert '-T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_present_hdr.hlsl' in shader
# Two SRVs, one UAV, one cbuffer, as the shader binds; a constant-buffer view over an alignas(256) struct.
assert 'SetupRootSignature(device, 2, 1, 1)' in pass_cpp
assert 'struct alignas(256) Constants' in pass_h and 'cbv.SizeInBytes = sizeof(Constants);' in pass_cpp
# Typed UAV store asked of the device, for the swapchain's format and the FP16 work texture.
assert 'D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE' in pass_cpp and 'D3D12_FORMAT_SUPPORT1_SHADER_LOAD' in pass_cpp
# The D3D12 route: the textures are made in one place, only for an HDR frame; an SDR frame only ever parks them.
assert renderer.count('CreateScratch(device, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height)') == 1
ensure = renderer[renderer.index('static const char* EnsurePresentHdr('):]
ensure = ensure[:ensure.index('\n}\n')]
assert 'g_hdrWork = CreateScratch(' in ensure and 'g_hdrOut = CreateScratch(device, format, width, height);' in ensure
assert renderer.count('EnsurePresentHdr(device, width, height, bbDesc.Format)') == 1
assert 'if (presentHdr)\n        hdrRefusal =' in renderer and '    else\n        ParkPresentHdrTextures();' in renderer
# With the switch off the decision is the one it always was; the white and the stabilizer unit stay 0.
assert 'return hdrPresent ? switchOn : ResolvePresentColour(format, record, false, legacy);' in renderer
assert 'hdrPresent ? switchOn : ResolvePresentColour(sourceDesc.Format, record, false, _colourSpace);' in host
assert 'const bool presentWhite = std::isfinite(frame.PresentWhite) && frame.PresentWhite > 0.0f;' in renderer
# Both hosts finish the timing sample before their list is closed or dropped.
assert renderer.count('hdrTiming.reset();') == 2
# Guards for native-only integration that the fake bridge cannot execute. These prove ordering/wiring,
# not Win32 display behavior (the GPU harness separately calls the real query).
present = renderer[renderer.index('void RunPresentPass('):renderer.index('void RunPresentPass(') + 45000]
assert present.index('DlssNr_PresentHdr_Dx12::TextureShape(bbDesc)') < present.index('EnsureBbCopy(')
assert 'presentHdr ? DlssNr::PresentHdr::DisplayWhiteForSwapchain(swapchain)' in present
assert 'frame.PresentWhite = presentHdr ? presentWhite.nits / 80.0f' in present
assert present.index('const bool executed = SubmitPresentList') < present.index('HDR conversion on, {}')
assert 'SDR white {:.1f} nits ({}) x Paper white; original codes bound for reconstruction' in re.sub(r'"\s*"', '', present)
assert host.index('_hdr->ValidResources(source, source,') < host.index('Transition(cmdList, source, sourceStateNow,')
display = (retoque / 'dlssnr/DlssNr_DisplayWhite.h').read_text()
assert 'GetContainingOutput(&output)' in display and 'path.targetInfo.adapterId' in display
assert 'DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL' in display
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()
assert 'on && config->DlssNrEnabled.value_or_default() && hdr.converted' in menu
assert "HDR present conversion is inactive; the game's original frame is shown." in menu
# The menu shows a decline reason through Localization::Tr(reason), which the localization suite's drift guard cannot
# see (it reads literals at Tr call sites): every reason the conversion gives has its pt-BR entry here instead.
colour = (retoque / 'dlssnr/DlssNr_PresentColour.h').read_text()
reasons = {name: ''.join(re.findall(r'"((?:\\.|[^"\\])*)"', literals)) for name, literals in
           re.findall(r'inline constexpr const char (k\w+)\[\] =\s*((?:"(?:\\.|[^"\\])*"\s*)+);', colour)}
assert len(reasons) >= 14, reasons.keys()
pack = (retoque / 'translations/optional/pt-BR.lang').read_text(encoding='utf-8')
keys = {line.split('\t')[0] for line in pack.splitlines() if line and not line.startswith('#')}
untranslated = [name for name, text in reasons.items() if text not in keys]
assert not untranslated, f'PresentColourText reasons without a pt-BR entry: {untranslated}'
# Registered in the project, once each.
project = (opti / 'OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (opti / 'OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_PresentHdr_Dx12.cpp" />',
             '<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_SwapchainColour.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_PresentHdr_Dx12.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_PresentHdr.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_SwapchainColour.h" />'):
    assert project.count(item) == 1, item
    assert filters.count(item.replace(' />', '>')) == 1, item

print('PASS: the shader\'s constants are the header\'s, ST 2084 exactly; both matrices are the derivation from the '
      'BT.709 and BT.2020 primaries to 1e-9 and inverses to 1e-6; one decode for both passes, alpha from the original, '
      'two SRVs, one UAV, one cbuffer; optional bytecode, typed UAV store asked, textures only for an HDR frame, the '
      'switch-off decision unchanged, timing finished on an open list, every reason translated, registered', flush=True)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-hdr-math-') as directory:
    (Path(directory) / 'conversion.inc').write_text(block)
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', directory, '-I', str(retoque), str(here / 'test.cpp'), '-o',
                    binary], check=True)
    subprocess.run([binary], check=True)
