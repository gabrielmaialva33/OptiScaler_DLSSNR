#!/usr/bin/env python3
"""The pulse metric (docs/dlssnr/design/pulse-metric.md): the production CPU half compiled under ASan/UBSan and driven
over synthetic readbacks, plus source guards on the GPU half and on the renderer that wires them. No GPU, no shader
compile."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
opti = root / 'OptiScaler'
retoque = root / 'retoque'


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


header = (retoque / 'dlssnr/DlssNr_Pulse.h').read_text()
shader = (retoque / 'shaders/dlssnr/precompile/dlssnr_pulse.hlsl').read_text()
pass_h = (retoque / 'shaders/dlssnr/DlssNr_Pulse_Dx12.h').read_text()
pass_cpp = (retoque / 'shaders/dlssnr/DlssNr_Pulse_Dx12.cpp').read_text()
renderer = (retoque / 'shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()

# The constants and the cbuffer are one ordered list of 4-byte scalars, padded to the 256 bytes of a view.
struct_body = pass_h[pass_h.index('struct alignas(256) Params'):]
struct_body = struct_body[struct_body.index('{') + 1:struct_body.index('};')]
cpp_fields = [(t.replace('uint32_t', 'uint'), n.lower())
              for t, n in re.findall(r'\b(uint32_t|float)\s+(\w+)\s*=', strip_comments(struct_body))]
cbuffer = shader[shader.index('cbuffer Params'):]
cbuffer = cbuffer[cbuffer.index('{') + 1:cbuffer.index('};')]
hlsl_fields = [(t, n[1:].lower()) for t, n in re.findall(r'\b(uint|float)\s+(\w+)\s*;', strip_comments(cbuffer))]
assert cpp_fields and cpp_fields == hlsl_fields, (cpp_fields, hlsl_fields)

# Every register the shader declares is a slot the root signature has.
srv = sorted(int(n) for n in re.findall(r'register\(t(\d+)\)', shader))
uav = sorted(int(n) for n in re.findall(r'register\(u(\d+)\)', shader))
assert srv == list(range(int(re.search(r'kSrvCount = (\d+)', pass_h).group(1))))
assert uav == list(range(int(re.search(r'kUavCount = (\d+)', pass_h).group(1))))

# The grid the shader writes is the grid the CPU reads, and the dispatch covers it.
grid = int(re.search(r'constexpr unsigned kGrid = (\d+);', header).group(1))
assert int(re.search(r'static const uint kGrid = (\d+);', shader).group(1)) == grid
assert 'cmdList->Dispatch(kGrid, kGrid, 1);' in pass_cpp
assert 'gGrid[groupId.xy] = float4(gSum[0], 0.0);' in shader
assert '[numthreads(8, 8, 1)]' in shader and 'groupshared float3 gSum[64];' in shader

# Its bytecode is optional: included only behind __has_include, and named as the build comment says.
assert '#if __has_include("precompile/DlssNr_Pulse_Shader.h")' in pass_cpp
assert 'DlssNr_Pulse_cso' in pass_cpp
assert 'create_header.py DlssNr_Pulse_Shader.cso DlssNr_Pulse_Shader.h DlssNr_Pulse_cso' in shader
assert '-T cs_6_0 -E CSMain' in shader

# Off builds and records nothing: the pass is made in one place, inside UpdatePulse, after the early return that the
# key and the bytecode decide; and the menu offers the key only where the bytecode is.
update = renderer[renderer.index('static void UpdatePulse('):]
update = update[:update.index('\n}\n')]
assert renderer.count('new DlssNr_Pulse_Dx12(') == 1 and 'new DlssNr_Pulse_Dx12(' in update
off = update.index('if (!enabled || !DlssNr_Pulse_Dx12::Available())')
assert off < update.index('StopPulseLocked();', off) < update.index('new DlssNr_Pulse_Dx12(')
assert renderer.count('UpdatePulse(cmdList, device, cfg.DlssNrPulseMetric.value_or_default(),') == 1
assert renderer.count('g_pulse.pass->Record(') == 1
# One measurement per present, and a present it cannot measure -- an early return included -- still took its
# sequence number: counted at the top of Dispatch, before its first early return.
assert 'if (present != 0 && present == g_pulse.lastPresent)' in update
assert update.index('const uint64_t seq = g_pulse.entered;') < update.index('if (wrote && target != nullptr')
dispatch = renderer[renderer.index('bool DlssNr_Dx12::Dispatch('):]
noted = dispatch.index('NotePulsePresent(State::Instance().frameCount);')
assert noted < dispatch.index('return false;') < dispatch.index('UpdatePulse(cmdList, device,')
assert renderer.count('NotePulsePresent(') == 2  # its definition and its one call
# Readbacks recorded before what is measured changed are collected, never counted.
assert 'slot.stale = slot.pending;' in update and 'if (slot.stale)' in update
# The target is read in the state the pass leaves it in, and handed back so.
record = update[update.index('g_pulse.pass->Record(') - 300:update.index('g_pulse.pass->Record(') + 300]
record = re.sub(r'\s+', ' ', record)
assert ('Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)'
        in record)
assert ('Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)'
        in record)
# Parked, never freed under the GPU, and freed with the rest.
assert 'delete retired.pulse;' in renderer and 'delete r.pulse;' in renderer
assert 'ParkNrPulse(g_pulse.pass);' in renderer
assert 'DlssNr::PulseAvailable() && !config->DlssNrUseProxy.value_or_default()' in menu

# Registered in the project, once each.
project = (opti / 'OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (opti / 'OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Pulse_Dx12.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Pulse_Dx12.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_Pulse.h" />'):
    assert project.count(item) == 1, item
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Pulse_Dx12.cpp">',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_Pulse.h">'):
    assert filters.count(item) == 1, item

print('PASS: constants == cbuffer, registers == root signature, one grid on both sides, optional bytecode, off builds '
      'and records nothing, one sample per present, stale readbacks dropped, target state kept, parked, registered',
      flush=True)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-pulse-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(retoque), str(here / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
