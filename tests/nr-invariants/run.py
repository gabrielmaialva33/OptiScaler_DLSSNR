#!/usr/bin/env python3
"""The mechanical guards of dlssnr/design/DEVELOPMENT.md section 4, against the live tree.

Four checks, each for an invariant a script can catch so review time goes to the ones it cannot:

1. Config round-trip (rule 4). Every CustomOptional DlssNr* member in Config.h is read in Config.cpp,
   saved there under the same key, and present in the [DlssNr] section of the shipped OptiScaler.ini.
   Keys that are deliberately absent from the ini are listed below with the reason, and a listed key
   that no longer exists fails too, so the list cannot rot.
2. Struct == cbuffer (rule 5). DlssNrConstants (C++) and the Params cbuffer (HLSL) are one ordered list
   of 4-byte scalars, same types, same names.
3. Shader headers are the committed bytecode (rule 6, the half a host can check). Each precompiled
   header is exactly what create_header.py makes from the .cso / .spv beside it. Whether the bytecode
   matches the .hlsl needs dxc and stays with the person who edits the shader.
4. Retired identifiers. Things removed on purpose must not come back as live code.

Pure Python: no compiler, no Wine prefix, safe to run beside a build.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def fail(message):
    failures.append(message)


# --- 1. Config round-trip -------------------------------------------------------------------------

# Read and saved, deliberately not in the shipped ini. Each reason is the code's own.
NOT_IN_INI = {
    'WhitePointFromExposure': 'legacy switch, migrated into WhitePointSource on load (Config.cpp)',
    'ScanAnchorValue': 'legacy single anchor, migrated into ScanAnchors then unused (Config.h)',
    'ScanAnchorWhitePoint': 'legacy single anchor, migrated into ScanAnchors then unused (Config.h)',
    'ProbeD3D11': 'diagnostic that initialises NGX on the live D3D11 device; opt-in by hand only (Config.h)',
    'ProxyProbe': 'diagnostic for the driver-core route, "not a feature" (Config.h)',
    'ScanExposure': 'diagnostic exposure scan that only watches and reports (Config.h)',
}

config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()

start = ini.index('[DlssNr]')
following = re.search(r'^\[', ini[start + 1:], re.M)
section = ini[start:start + 1 + following.start()] if following else ini[start:]

members = re.findall(r'CustomOptional<[^;]*?>\s+(DlssNr\w+)\s*\{', config_h)
if len(members) < 40:
    fail(f'found only {len(members)} DlssNr members in Config.h; the parser no longer matches the file')


def statements_mentioning(member):
    for match in re.finditer(re.escape(member) + r'\b', config_cpp):
        begin = config_cpp.rfind(';', 0, match.start()) + 1
        yield config_cpp[begin:config_cpp.find(';', match.end())]


checked = 0
for member in members:
    keys = set()
    for statement in statements_mentioning(member):
        if 'set_from_config' in statement:
            keys.update(re.findall(r'"DlssNr",\s*"(\w+)"', statement))

    if not keys:
        fail(f'{member}: never read from [DlssNr] in Config.cpp')
        continue

    for key in sorted(keys):
        if not re.search(r'SetValue\(\s*"DlssNr",\s*"' + key + r'"', config_cpp):
            fail(f'{member}: key {key} is read but never saved')
        in_ini = re.search(r'^' + key + r'=', section, re.M) is not None
        if key in NOT_IN_INI:
            if in_ini:
                fail(f'{key} is listed as deliberately absent from OptiScaler.ini but is there; drop it from the list')
        elif not in_ini:
            fail(f'{member}: key {key} is missing from [DlssNr] in OptiScaler.ini')
        checked += 1

for key in NOT_IN_INI:
    if f'"DlssNr", "{key}"' not in config_cpp:
        fail(f'{key} is listed as deliberately absent from OptiScaler.ini but is no longer a key; drop it')

if not failures:
    print(f'PASS: {len(members)} DlssNr members, {checked} keys read and saved; '
          f'{checked - len(NOT_IN_INI)} shipped in OptiScaler.ini, {len(NOT_IN_INI)} deliberately not, each with its reason')

# --- 2. Struct == cbuffer ----------------------------------------------------------------------------

before = len(failures)


def body(source, tag):
    opening = source.index('{', source.index(tag))
    return source[opening + 1:source.index('};', opening)]


common = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Common.h').read_text()
hlsl_path = root / 'OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl'
hlsl = hlsl_path.read_text()

cpp_fields = re.findall(r'\b(uint32_t|float)\s+(\w+)\s*;',
                        re.sub(r'//[^\n]*', '', body(common, 'struct alignas(256) DlssNrConstants')))
hlsl_fields = re.findall(r'\b(uint|float)\s+(\w+)\s*;', re.sub(r'//[^\n]*', '', body(hlsl, 'cbuffer Params')))

cpp_list = [(t.replace('uint32_t', 'uint'), n.lower()) for t, n in cpp_fields]
hlsl_list = [(t, n[1:].lower() if n.startswith('g') else n.lower()) for t, n in hlsl_fields]

if not cpp_list:
    fail('no fields parsed from DlssNrConstants')
elif cpp_list != hlsl_list:
    for i, (c, h) in enumerate(zip(cpp_list, hlsl_list)):
        if c != h:
            fail(f'DlssNrConstants and Params diverge at scalar {i}: C++ {c} vs HLSL {h}')
            break
    else:
        fail(f'DlssNrConstants has {len(cpp_list)} scalars, Params has {len(hlsl_list)}')

if len(failures) == before:
    print(f'PASS: {len(cpp_list)} ordered 4-byte constants match between DlssNrConstants and the Params cbuffer')

# --- 3. Headers are the committed bytecode --------------------------------------------------------

before = len(failures)
precompile = root / 'OptiScaler/shaders/dlssnr/precompile'
create_header = root / 'OptiScaler/shaders/shader_tools/create_header.py'
targets = [
    ('DlssNr_Shader.cso', 'DlssNr_Shader.h', 'DlssNr_cso', b'DXBC'),
    ('DlssNr_Shader_Vk.spv', 'DlssNr_Shader_Vk.h', 'dlssnr_spv', bytes.fromhex('03022307')),
]

with tempfile.TemporaryDirectory() as scratch:
    for binary, header, array, magic in targets:
        blob = (precompile / binary).read_bytes()
        if not blob.startswith(magic):
            fail(f'{binary} does not start with its container magic {magic!r}')
        regenerated = Path(scratch) / header
        subprocess.run([sys.executable, str(create_header), str(precompile / binary), str(regenerated), array],
                       check=True, capture_output=True)
        if regenerated.read_bytes() != (precompile / header).read_bytes():
            fail(f'{header} is not what create_header.py makes from {binary}: regenerate it (array {array})')

if len(failures) == before:
    print('PASS: DlssNr_Shader.h and DlssNr_Shader_Vk.h are byte-identical to create_header.py over the '
          'committed .cso and .spv')

# --- 4. Retired identifiers -------------------------------------------------------------------------

before = len(failures)
sources = [p for p in (root / 'OptiScaler').rglob('*') if p.suffix in ('.cpp', '.h')
           and 'external' not in p.parts and 'include' not in p.parts]
forwarder = (root / 'OptiScaler/dlssnr/forwarder/dlssnr_forwarder.cpp').read_text()

RETIRED = [
    # The present-time pass sat behind this macro, which nothing defined, so LTCG dropped it from every
    # build (CLAUDE.md, "The present-time pass had never shipped").
    (re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif)\b[^\n]*\bDLSS_NEURAL_RENDERING\b', re.M), sources,
     'a preprocessor gate on DLSS_NEURAL_RENDERING'),
    # Folded into HookMethod=2 by e7ca0209; a saved ini that still has it must stay inert.
    (re.compile(r'"DlssNr",\s*"Dx11BridgeHost"'), [root / 'OptiScaler/Config.cpp'],
     'the Dx11BridgeHost key being read or saved'),
]

for pattern, files, what in RETIRED:
    for path in files:
        text = path.read_text(errors='replace') if path.is_file() else ''
        match = pattern.search(text)
        if match:
            line = text.count('\n', 0, match.start()) + 1
            fail(f'{path.relative_to(root)}:{line}: {what} is back')

# The model's vocabulary has no GlobalToneStrength; writing it filled a map nothing reads.
if re.search(r'set\w*\([^;]*"DLSSNR\.GlobalToneStrength"', forwarder):
    fail('dlssnr_forwarder.cpp writes DLSSNR.GlobalToneStrength again, which the model never reads')

if len(failures) == before:
    print(f'PASS: no retired identifier is live ({len(sources)} sources scanned)')

if failures:
    for message in failures:
        print(f'FAIL: {message}')
    sys.exit(1)
