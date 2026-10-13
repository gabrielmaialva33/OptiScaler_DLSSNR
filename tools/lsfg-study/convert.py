"""SPIR-V (from lsfg-vk.dll) -> HLSL (spirv-cross) -> DXIL (dxc under the msvc-wine prefix), for D3D12.

Vulkan bindings hold whole arrays (binding 32 = Input[7]), so spirv-cross's `register(t32)` + `register(t34)` overlap
once the arrays expand. Each binding gets its own register space instead: `register(<class>0, space<binding>)`, which a
D3D12 root signature serves as one descriptor table per binding. The map is written next to the HLSL for the harness.

usage: convert.py <spv dir> <out dir>
"""
import json
import os
import pathlib
import re
import subprocess
import sys

spv_dir = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
(out / 'hlsl').mkdir(parents=True, exist_ok=True)
(out / 'dxil').mkdir(parents=True, exist_ok=True)

repo = pathlib.Path(__file__).resolve().parents[2]
dxc = repo / 'OptiScaler/shaders/shader_tools/dxc.exe'
env = dict(os.environ, WINEPREFIX=os.path.expanduser('~/.local/opt/msvc-wineprefix'), WINEDEBUG='-all')

REG = re.compile(r'register\(([btsu])(\d+), space0\)')


def remap(hlsl):
    bindings = {}

    def sub(m):
        cls, binding = m.group(1), int(m.group(2))
        bindings.setdefault(binding, cls)
        return f'register({cls}0, space{binding})'

    return REG.sub(sub, hlsl), bindings


ok, failed, layout = 0, [], {}
for spv in sorted(spv_dir.glob('*.spv'), key=lambda p: int(p.stem)):
    rid = spv.stem
    hlsl = subprocess.run(['spirv-cross', '--hlsl', '--shader-model', '60', str(spv)], capture_output=True, text=True,
                          check=True).stdout
    hlsl, bindings = remap(hlsl)
    (out / 'hlsl' / f'{rid}.hlsl').write_text(hlsl)
    layout[rid] = {str(b): c for b, c in sorted(bindings.items())}
    res = subprocess.run(['wine', str(dxc), '-T', 'cs_6_0', '-E', 'main', '-O3', f'Z:{out / "hlsl" / (rid + ".hlsl")}',
                          '-Fo', f'Z:{out / "dxil" / (rid + ".cso")}'], capture_output=True, text=True, env=env)
    cso = out / 'dxil' / f'{rid}.cso'
    if res.returncode == 0 and cso.exists() and cso.stat().st_size > 0:
        ok += 1
    else:
        err = '\n'.join(l for l in (res.stdout + res.stderr).splitlines() if 'MESA' not in l and 'pci id' not in l)
        failed.append((rid, err.strip()[:300]))

(out / 'layout.json').write_text(json.dumps(layout, indent=1))
print(f'dxc ok={ok} failed={len(failed)}')
for rid, err in failed:
    print(f'FAIL {rid}: {err}')
