#!/usr/bin/env python3
"""The NR model loaders' byte-level decisions, the forwarder/direct drift guard and the wiring.

Runs no model, no NGX and no GPU. See README.md.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]
dlssnr = repo / 'OptiScaler/dlssnr'


def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    sys.exit(1)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def body(source, signature, path):
    """The text of the function whose definition starts at `signature`, braces balanced."""
    start = source.find(signature)
    if start < 0:
        fail(f'{signature!r} not found in {path}')
    open_brace = source.index('{', start)
    depth = 0
    for i in range(open_brace, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    fail(f'unbalanced braces after {signature!r} in {path}')


# --- 1. The byte scanners and the version predicates, compiled against synthetic images -----------

ngx_info = (dlssnr / 'DlssNr_NgxInfo.cpp').read_text()
driver_from_loader = body(ngx_info, 'std::string DriverFromLoaderVersion(', 'DlssNr_NgxInfo.cpp')

harness = (root / 'test.cpp').read_text()
marker = '// @@PRODUCTION_SLICES@@'
if marker not in harness:
    fail('injection marker missing from test.cpp')

# Real DLLs from this machine, when present: evidence about the files NR actually runs against. CI has
# neither, and the suite passes there on the synthetic cases alone.
loader = os.environ.get('NR_LOADER_DLL', '/usr/lib/nvidia/wine/_nvngx.dll')
model = os.environ.get('NR_MODEL_DLL', str(Path.home() / '.local/share/Steam/steamapps/common/'
                                                         'Crimson Desert/bin64/nvngx_dlssnr.dll'))
local_args = []
for flag, path in (('--loader', loader), ('--model', model)):
    if Path(path).is_file():
        local_args += [flag, path]
    else:
        print(f'local {flag[2:]} not present ({path}); synthetic cases only')

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-model-loader-') as directory:
    generated = Path(directory) / 'generated.cpp'
    generated.write_text(harness.replace(marker, driver_from_loader))
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++23', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', '-I', str(dlssnr), str(generated), '-o', binary], check=True)
    subprocess.run([binary, *local_args], check=True)

# --- 2. Drift: the direct runtime writes what the forwarder writes, in the same order --------------

forwarder_path = 'OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp'
direct_path = 'OptiScaler/dlssnr/DlssNr_DirectRuntime.cpp'
forwarder = strip_comments((repo / forwarder_path).read_text())
direct = strip_comments((repo / direct_path).read_text())

write = re.compile(r'\b(set(?:UInt|Float|Resource))\(\s*capabilityParams\s*,\s*("[^"]+")\s*,\s*([^;]+?)\s*\);')
guarded = re.compile(r'Guarded\(([^;]+?)\);')

pairs = [
    ('dlssnr_query_scaling_ratio(', 'int QueryScalingRatio('),
    ('dlssnr_call_create(', 'void* Create('),
    ('dlssnr_call_evaluate(', 'int Evaluate('),
    ('dlssnr_call_set_extras(', 'void SetExtras('),
    ('dlssnr_call_release(', 'void Release('),
]
total = 0
for forwarder_sig, direct_sig in pairs:
    a = body(forwarder, forwarder_sig, forwarder_path)
    b = body(direct, direct_sig, direct_path)
    writes_a = [(m[0], m[1], ' '.join(m[2].split())) for m in write.findall(a)]
    writes_b = [(m[0], m[1], ' '.join(m[2].split())) for m in write.findall(b)]
    if writes_a != writes_b:
        for i, (x, y) in enumerate(zip(writes_a + [None] * len(writes_b), writes_b + [None] * len(writes_a))):
            if x != y:
                fail(f'{direct_sig} drifted from {forwarder_sig} at write {i}: forwarder {x}, direct {y}')
    # The calls into the model: same entry point, same application id, same arguments.
    calls_a = [' '.join(c.split()) for c in guarded.findall(a)]
    calls_b = [' '.join(c.split()) for c in guarded.findall(b)]
    if calls_a != calls_b:
        fail(f'{direct_sig} calls the model differently from {forwarder_sig}: {calls_a} vs {calls_b}')
    total += len(writes_a)

# The setters behind those writes: same vtable slots on the capability block, same casts.
for setter in ('void setUInt(', 'void setFloat(', 'void setResource('):
    if ' '.join(body(forwarder, setter, forwarder_path).split()) != ' '.join(body(direct, setter, direct_path).split()):
        fail(f'{setter} differs between the forwarder and the direct runtime')
for constant in ('constexpr int VT_SET_UINT = 3;', 'int g_floatSlot = 1;'):
    if constant not in forwarder or constant not in direct:
        fail(f'{constant!r} must be the same in both')
for export in ('"NVSDK_NGX_D3D12_Init_Ext"', '"NVSDK_NGX_D3D12_CreateFeature"', '"NVSDK_NGX_D3D12_EvaluateFeature"',
               '"NVSDK_NGX_D3D12_ReleaseFeature"', '"NVSDK_NGX_D3D12_PopulateParameters_Impl"'):
    if export not in forwarder or export not in direct:
        fail(f'both loaders must resolve {export}')

# Fault containment: the filter that decides what counts as a fault is the forwarder's, verbatim.
filter_a = ' '.join(body(forwarder, 'int FaultFilter(', forwarder_path).split())
filter_b = ' '.join(body(direct, 'int FaultFilter(', direct_path).split())
if filter_a != filter_b:
    fail('FaultFilter differs between the forwarder and the direct runtime')
if 'constexpr int kFaulted = static_cast<int>(0xBAD0FA17u);' not in direct:
    fail('the direct runtime lost the forwarder\'s kFaulted value')
# The two properties that make a call pass the caller check: a real call (no tail call) and the alias
# scoped to exactly that call.
guarded_b = body(direct, 'template <typename Fn, typename... Args> int Guarded(', direct_path)
if not re.search(r't_callerAlias = ThisModule\(\);.*__try.*t_callerAlias = previousAlias;', guarded_b, re.S):
    fail('Guarded no longer scopes the caller alias around the call')
if re.search(r'return\s+Guarded\(', direct):
    fail('a Guarded call is returned directly: a tail call would leave this module\'s frame behind')

print(f'PASS: direct runtime matches the forwarder -- {total} parameter writes, model calls and fault filter')

# --- 3. Wiring -------------------------------------------------------------------------------------

host = strip_comments((repo / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text())
ensure = body(host, 'bool EnsureForwarder()', 'DlssNr_Dx12.cpp')
direct_at = ensure.find('NrModelLoader::Direct')
load_at = ensure.find('LoadLibraryW(')
if direct_at < 0 or load_at < 0 or direct_at > load_at:
    fail('EnsureForwarder must choose ModelLoader=direct before it loads the forwarder')
if host.find('DlssNr::NgxInfo::Describe(*snippet);') < 0 or \
        host.find('DlssNr::NgxInfo::Describe(*snippet);') > host.find('DlssNr::ModelLog::Install(*snippet);'):
    fail('the driver/loader/model line must be logged before the first feature build')
probe = body(host, 'void ProbeD3D11(', 'DlssNr_Dx12.cpp')
if probe.find('g_nr.forwarder == nullptr') < 0 or probe.find('g_nr.forwarder == nullptr') > probe.find('GetProcAddress('):
    fail('ProbeD3D11 must not GetProcAddress a null forwarder (that searches the game executable)')

proxy = strip_comments((dlssnr / 'DlssNr_Proxy.cpp').read_text())
guard_at = proxy.find('LoaderRouteKnownToFault(')
create_at = proxy.find('SetCreationParameters(g_proxy.params')
if guard_at < 0 or create_at < 0 or guard_at > create_at:
    fail('the UseProxy route must refuse the known-faulting pairing before it creates feature 18')

config_h = (repo / 'OptiScaler/Config.h').read_text()
if not re.search(r'CustomOptional<NrModelLoader> DlssNrModelLoader \{ NrModelLoader::Forwarder \};', config_h):
    fail('ModelLoader must default to the forwarder')

print('PASS: wiring -- loader choice precedes the forwarder load, NGX line precedes the build, '
      'probe and proxy guards in place, forwarder is the default')
