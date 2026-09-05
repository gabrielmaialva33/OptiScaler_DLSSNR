#!/usr/bin/env python3
"""Run portable production helpers; no GPU/NGX execution is claimed."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-chain-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

source = (root.parents[1] / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
dispatch = source[source.index('bool DlssNr_Dx12::Dispatch('):source.index('namespace DlssNr', source.index('bool DlssNr_Dx12::Dispatch('))]
assert dispatch.count('g_nr.superDown->Dispatch(cmdList, finalOutput, g_nr.outputNative)') == 1
assert 'DlssNr::Chain::Routing<ID3D12Resource*> chain(modelInput, g_nr.output, g_nr.passPing)' in dispatch
assert 'DlssNr::Chain::Resolve(g_nr.colorCopy, modelInput, g_nr.outputNative, finalOutput, superDownOk)' in dispatch
print('PASS: renderer uses tested routing/resolve; one supersampling down-leg after finalOutput selection')
