#!/usr/bin/env python3
"""Exercise FG lock transactions sliced verbatim from the production lifecycle methods."""
from pathlib import Path
import sys
import subprocess
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]


def function(signature, source):
    begin = source.index(signature)
    start = source.index('{', begin)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


source = (repo / 'OptiScaler/wrapped/wrapped_swapchain.cpp').read_text()
parts = []
for name in ('ResizeBuffers', 'ResizeBuffers1'):
    body = function('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::' + name + '(', source)
    # Only device work is replaced: acquisition, release and their original lexical scope stay verbatim.
    acquisition = body[body.index('#endif') + len('#endif'):body.index('    HRESULT result')]
    release = body[body.index('    LOG_DEBUG("result: {0:X}", (UINT) result);'):]
    params = 'IUnknown* const* ppPresentQueue' if name.endswith('1') else ''
    parts.append('HRESULT ' + name + '(' + params + ') {\n' + acquisition +
                 '\nHRESULT result = operation();\n' + release)

for name, path in [('FSRFG_Dx12', 'ffx'), ('DLSSG_Dx12', 'dlssg'), ('XeFG_Dx12', 'xefg')]:
    source = (repo / f'OptiScaler/framegen/{path}/{name}.cpp').read_text()
    parts.append(function('bool ' + name + '::ReleaseSwapchain(', source))
    if name == 'FSRFG_Dx12':
        dispatch = function('bool FSRFG_Dx12::Dispatch()', source)
        # Everything after the last dispatch branch: catches an unlock belonging to somebody else.
        tail = dispatch[dispatch.rindex('    }\n\n') + len('    }\n\n'):]
        parts.append('bool FSRFG_Dx12::DispatchTail() { auto* config = Config::Instance(); '
                     '(void)config; bool dispatchResult = true;\n' + tail)

mutex = (repo / 'OptiScaler/OwnedMutex.h').read_text()
mutex = mutex[mutex.index('class OwnedMutex'):]
fakes = (here / 'fakes.h').read_text().replace('// @owned-mutex@', mutex)
unit = fakes + '\n'.join(parts) + (here / 'cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-fg-locks-') as directory:
    cpp, binary = Path(directory) / 'test.cpp', Path(directory) / 'test'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    results = [subprocess.run([str(binary), case]).returncode for case in ("resize", "release", "dispatch")]
    if any(results):
        sys.exit(1)
