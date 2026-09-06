#!/usr/bin/env python3
"""Exercise production timing metadata/UI with host fakes, and guard marker placement."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
out = here / 'artifacts'
out.mkdir(exist_ok=True)
renderer = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
menu = (root / 'OptiScaler/dlssnr/DlssNr_Menu.cpp').read_text()

def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]

metadata = between(renderer, 'DlssNr::GpuTiming::Metadata TimingMetadata(', '// Writes matched before/after frames')
controls = between(menu, 'static void RenderGpuTiming(', '// A slider that only writes')
dispatch = between(renderer, 'bool DlssNr_Dx12::Dispatch(', '\nnamespace DlssNr\n{')
assert 'GpuTime_Dx12' not in renderer
assert 'DLSS-NR cost:' not in renderer
assert 'std::optional<double> LastGpuTime() { return std::nullopt; }' in renderer
assert dispatch.index('GpuTiming::Evaluation timing(') < dispatch.index('Barrier(cmdList, target, outputArrival,')
assert dispatch.index('timing.SetMetadata(') < dispatch.index('timing.ModelBegin()')
assert dispatch.count('timing.ModelBegin();') == dispatch.count('timing.ModelEnd();') == 2
first_begin = dispatch.index('timing.ModelBegin();')
first_end = dispatch.index('timing.ModelEnd();')
assert dispatch[first_begin:first_end].count('g_nr.evaluate(') == 1  # shared single/chain first pass
second_begin = dispatch.index('timing.ModelBegin();', first_end)
second_end = dispatch.index('timing.ModelEnd();', second_begin)
assert dispatch[second_begin:second_end].count('g_nr.evaluate(') == 1  # repeated per extra pass
assert dispatch.count('g_nr.evaluate(') == 2  # no unmeasured forwarder calls outside the markers
assert second_begin > dispatch.index('for (unsigned i = 1; i < passSnapshot.Count; ++i)')
assert dispatch.index('timing.FinishOnScopeExit(') > dispatch.index('colorRead.Restore();')
assert 'wrote && result == 1 && chain.completed == passSnapshot.Count' in dispatch
header = (root / 'OptiScaler/dlssnr/DlssNr_GpuTiming.h').read_text()
records = between(header, 'struct Metadata\n', '// Collection never waits')
unit = (here / 'fakes.h').read_text()
unit += '\nnamespace DlssNr::GpuTiming {\n' + records + '\n}\n'
unit += 'DlssNr::GpuTiming::Snapshot published; int snapshotReads=0;\n'
unit += 'namespace DlssNr::GpuTiming { void SetEnabled(bool) {} Snapshot GetSnapshot() { ++snapshotReads; return published; } }\n'
unit += metadata
unit += '\nnamespace DlssNr {\n' + controls + '\n}\n' + (here / 'cases.h').read_text()
(out / 'boundary.cpp').write_text(unit)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
                '-I' + str(root), str(out / 'boundary.cpp'), '-o', str(out / 'boundary')], check=True)
subprocess.run([str(out / 'boundary')], check=True)

print("PASS production timing placement guards")
