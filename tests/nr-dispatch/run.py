#!/usr/bin/env python3
"""Exercise the production composition constructor, dispatch and cleanup with host fakes."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
renderer = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
header = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.h').read_text()


def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


# The descriptor keys read a generation that lives above the constructor, so it comes along.
generation = between(renderer, 'uint64_t g_nrScratchGeneration', 'ID3D12Resource* CreateScratch(')

declaration = header[header.index('#define DLSSNR_NUM_OF_HEAPS'):]
implementation = between(renderer, 'DlssNr_Dx12::DlssNr_Dx12(', 'bool DlssNr_Dx12::Dispatch(')


def guard_descriptor_key(source):
    """The view arguments a descriptor is built from must also be the ones its key carries.

    The cache is only sound while those two agree, and nothing in the type system says they do: a
    parameter added to one and not the other compiles and silently serves a view built under the other
    policy. That is not hypothetical -- translateTypeless became a parameter in 97c94b05, after the
    first version of this cache was written. So the names are pinned here. A new argument at either
    call site fails this until it is also in the key.
    """
    policy = ('kViewFormat', 'kViewMipLevel', 'kViewTranslateTypeless')
    for call, expected in (('CreateShaderResourceView(_device,', ('kViewFormat', 'kViewTranslateTypeless')),
                           ('CreateUnorderedAccessView(_device,', ('kViewMipLevel', 'kViewTranslateTypeless'))):
        assert source.count(call) == 1, f'view creation call moved: {call}'
        arguments = source[source.index(call):]
        arguments = arguments[:arguments.index(';')]
        passed = tuple(name for name in policy if name in arguments)
        assert passed == expected, f'{call} passes {passed}, expected {expected}'

    keys = source.count('const BindKey key {')
    assert keys == 2, f'expected one key per view kind, found {keys}'
    for block in source.split('const BindKey key {')[1:]:
        block = block[:block.index('};')]
        missing = [name for name in policy if name not in block]
        assert not missing, f'BindKey does not carry {missing}; a view argument is outside its key'


def guard_key_members(source):
    """Every member of BindKey must appear in its comparison.

    A member that is in the struct but not in operator== is invisible to every behavioural test that
    can be written today, because the arguments it stands for are constants: drop translateTypeless
    from the comparison and nothing observable changes until the day someone varies it, which is the
    day the cache starts serving a view built under the other policy. So the struct is checked against
    its own comparison directly.
    """
    start = source.index('struct BindKey')
    body = source[start:source.index('\n    };', start)]
    comparison = body[body.index('bool operator=='):]
    members = re.findall(r'^        [A-Za-z_][\w:*<> ]*?\*? ?(\w+) = ', body, re.M)
    assert len(members) >= 8, f'BindKey members not found as expected: {members}'
    missing = [name for name in members if f'{name} == o.{name}' not in comparison]
    assert not missing, f'BindKey::operator== ignores {missing}; those keys cannot retire a descriptor'


guard_descriptor_key(implementation)
guard_key_members(declaration)

unit = (here / 'fakes.h').read_text() + '\n' + declaration + '\n' + generation + '\n' + implementation
unit += '\n' + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-dispatch-') as directory:
    source = Path(directory) / 'dispatch.cpp'
    binary = Path(directory) / 'dispatch'
    source.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
