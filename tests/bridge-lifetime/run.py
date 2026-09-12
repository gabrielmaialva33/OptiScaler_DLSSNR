#!/usr/bin/env python3
"""Execute production bridge lifetime functions with scripted COM/fence/Win32 fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp').read_text()


def function(signature):
    assert source.count(signature) == 1, signature
    begin = source.index(signature)
    start = source.index('{', begin)
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end] + '\n'


signatures = [
    'HRESULT WaitForBridgeFence(',
    'Dx11wDx12SC::~Dx11wDx12SC()',
    'ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()',
    'bool Dx11wDx12SC::_OwnsFgPresenter()',
    'bool Dx11wDx12SC::_OwnsPresenter()',
    'bool Dx11wDx12SC::_OwnsOverlay()',
    'bool Dx11wDx12SC::_DevicesRemoved()',
    'void Dx11wDx12SC::_DetachWrapperGlobals()',
    'void Dx11wDx12SC::_FinishRelease(',
    'void Dx11wDx12SC::_CollectRetired()',
    'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(',
    'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(',
    'HRESULT Dx11wDx12SC::_WaitForCopyAllocator(',
    'HRESULT Dx11wDx12SC::_WaitForCopyQueueIdle(',
    'HRESULT Dx11wDx12SC::_DrainForTeardown(',
    'void Dx11wDx12SC::_ResetTeardownDrain()',
    'void Dx11wDx12SC::_ReleaseInteropObjects()',
    'void Dx11wDx12SC::_ReleaseInteropBackBuffers()',
    'bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(',
]
unit = (here / 'fakes.h').read_text() + '\n'.join(map(function, signatures)) + (here / 'cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-bridge-lifetime-') as directory:
    cpp = Path(directory) / 'bridge.cpp'
    binary = Path(directory) / 'bridge'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
