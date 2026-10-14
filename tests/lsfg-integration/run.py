#!/usr/bin/env python3
"""LSFG inside FSR-FG (study branch): the host-testable half of retoque/lsfg/, plus source guards.

1. test.cpp, under ASan/UBSan: the DXIL PSV0 reader the root signatures come from (synthetic containers,
   every truncation and single-byte corruption), the pass graph (pass counts, dispatch extents against
   PIPELINE_V3_1.md, pools, one writer per image, variants, flow scale, refusals), the constant-buffer
   table, which frames LSFG takes, the history schedule, the barrier waves and which FFX outputs generate
   may write itself. With LSFG_DXIL_DIR set to a folder of the user's own converted modules (<rid>.cso),
   all 99 are read and held against the graph as well.
2. [LSFG] round trip, the four points (DEVELOPMENT.md rule 4; nr-invariants checks only DlssNr keys):
   declared in Config.h, read and saved in Config.cpp, present in OptiScaler.ini, documented in Config.md.
3. The FSRFG_Dx12.cpp call sites come as a set: a latch without the prepare skip, or a prepare skip without
   the callback hand-off, would run FFX's interpolation on a frame it never prepared.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent
repo = root.parents[1]
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


# 1. The headers, compiled unmodified.
with tempfile.TemporaryDirectory(prefix='optiscaler-lsfg-integration-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque'), str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary, os.environ.get('LSFG_DXIL_DIR', '')], check=True)

# 2. [LSFG] round trip.
config_h = (repo / 'OptiScaler/Config.h').read_text()
config_cpp = (repo / 'OptiScaler/Config.cpp').read_text()
ini = (repo / 'OptiScaler.ini').read_text()
config_md = (repo / 'Config.md').read_text()

keys = {  # ini key -> (member, read helper, default as declared)
    'Enabled': ('LsfgEnabled', 'readBool', 'false'),
    'FlowScale': ('LsfgFlowScale', 'readFloat', '1.0f'),
    'PerformanceMode': ('LsfgPerformanceMode', 'readBool', 'false'),
    'ShaderDir': ('LsfgShaderDir', 'readWString', None),
    'DllPath': ('LsfgDllPath', 'readWString', None),
}

section = re.search(r'^\[LSFG\]\n(.*?)(?=^; -{20,}\n\[|\Z)', ini, re.M | re.S)
check(section is not None, 'OptiScaler.ini has no [LSFG] section')
section = section.group(1) if section else ''
md_section = re.search(r'### LSFG as FSR FG\'s interpolator.*?(?=^### )', config_md, re.M | re.S)
check(md_section is not None, 'Config.md has no LSFG section')
md_section = md_section.group(0) if md_section else ''

for key, (member, reader, default) in keys.items():
    decl = re.search(r'CustomOptional<[^;]*>\s+' + member + r'\b\s*(\{\s*([^}]*)\})?\s*;', config_h)
    check(decl is not None, f'Config.h does not declare {member}')
    if decl and default is not None:
        check(decl.group(2) is not None and decl.group(2).strip() == default,
              f'{member} default is {decl.group(2)!r}, expected {default}')
    check(re.search(member + r'\.set_from_config\(\s*' + reader + r'\("LSFG", "' + key + r'"\)\)', config_cpp),
          f'Config.cpp does not read [LSFG] {key} into {member} with {reader}')
    check(re.search(r'ini\.SetValue\("LSFG", "' + key + r'",[^;]*' + member + r'\.value_for_config', config_cpp,
                    re.S), f'Config.cpp does not save [LSFG] {key} from {member}')
    lines = re.findall(r'^' + key + r'=(.*)$', section, re.M)
    check(lines == ['auto'], f'OptiScaler.ini [LSFG] {key}: {lines}, expected exactly one "auto"')
    check(re.search(r'^' + key + r'=', md_section, re.M), f'Config.md does not show [LSFG] {key}')

# Every Lsfg member is one of the four: a fifth would have escaped the round trip above.
members = set(re.findall(r'CustomOptional<[^;]*>\s+(Lsfg\w+)', config_h))
check(members == {m for m, _, _ in keys.values()}, f'Lsfg members in Config.h: {sorted(members)}')

# 3. The call sites.
fsrfg = (repo / 'OptiScaler/framegen/ffx/FSRFG_Dx12.cpp').read_text()
sites = {
    'latch': r'const bool lsfgFrame = Lsfg::BeginFrame\(fIndex, willDispatchFrame\);',
    'gate skipped': r'if \(!lsfgFrame && \(!_resourceReady\[fIndex\]',
    'prepare skipped': r'if \(retCode == FFX_API_RETURN_OK && _isActive && lsfgFrame\)\s*dispatchResult = true;'
                       r'[^\n]*\n\s*else if \(retCode == FFX_API_RETURN_OK && _isActive\)',
    'hand-off': r'Lsfg::TakesFrame\(fIndex, params\)\s*\?\s*Lsfg::FfxDispatch\(params\)[^:]*:\s*'
                r'FfxApiProxy::D3D12_Dispatch\(&_fgContext, &params->header\)',
    'release': r'State::Instance\(\)\.currentFGSwapchain = nullptr;\s*Lsfg::OnSwapchainReleased\(\);',
}
found = {name: re.search(pattern, fsrfg) is not None for name, pattern in sites.items()}
check(all(found.values()), f'FSRFG_Dx12.cpp call sites: {found}')
check(fsrfg.count('Lsfg::') == 4, f'FSRFG_Dx12.cpp has {fsrfg.count("Lsfg::")} Lsfg:: calls, expected 4')
check(fsrfg.count('FfxApiProxy::D3D12_Dispatch(&_fgContext, &params->header)') == 1,
      'the FFX hand-off appears more than once: a path that bypasses the latch')

# The disabled path takes no lock and touches nothing but the latch (invariant 1, default-identical).
lsfg = (repo / 'retoque/lsfg/Lsfg.cpp').read_text()
begin = re.search(r"bool BeginFrame\(int fIndex, uint64_t frameId\)\n\{(.*?)\n\}", lsfg, re.S)
check(begin is not None, 'BeginFrame not found')
if begin:
    body = begin.group(1)
    off = body.find('if (!Config::Instance()->LsfgEnabled.value_or_default())')
    check(0 <= off < body.find('std::lock_guard'), 'BeginFrame takes the lock before the Enabled check')
takes = re.search(r'bool TakesFrame\(int fIndex, ffxDispatchDescFrameGeneration\* params\)\n\{(.*?)\n\}', lsfg, re.S)
check(takes is not None, 'TakesFrame not found')
if takes:
    body = takes.group(1)
    check(body.find('LsfgEnabled.value_or_default()') < body.find('std::lock_guard'),
          'TakesFrame takes the lock before the Enabled check')

if failures:
    for f in failures:
        print('FAIL:', f)
    sys.exit(1)
print('source guards: ok')
