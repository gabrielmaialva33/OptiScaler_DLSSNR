#!/usr/bin/env python3
"""Compile actual pass-menu control flow with scripted ImGui events; no GPU/UI claims."""
from pathlib import Path
import subprocess
import tempfile
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
source = (root / 'OptiScaler/dlssnr/DlssNr_Menu.cpp').read_text()
start = source.index('static bool DeferredSlider(')
end = source.index('void RenderMenu(', start)
actual = source[start:end]
# A control writes a short Config transaction, never a public optional used by snapshots.
for field in ('Preset', 'Style', 'Intensity', 'LocalStructure', 'LocalTone', 'SkinStructure', 'AutoMask'):
    assert not re.search(r'config->DlssNr' + field + r'\s*=', source), field
    assert f'config->SetDlssNrMasterSetting(&Config::DlssNr{field},' in source, field
assert 'RetryAfterFailure' not in actual
# Register the runtime translation units, once, in both project files.
for name in ('OptiScaler.vcxproj', 'OptiScaler.vcxproj.filters'):
    tree = ET.parse(root / 'OptiScaler' / name)
    includes = [item.attrib.get('Include') for item in tree.iter()]
    for path in ('misc\\Localization.cpp', 'dlssnr\\DlssNr_Submission.cpp'):
        assert includes.count(path) == 1, (name, path)
with tempfile.TemporaryDirectory(prefix='nr-menu-') as directory:
    generated = Path(directory) / 'menu.cpp'
    generated.write_text((root / 'tests/nr-menu/fakes.h').read_text() + '\nnamespace DlssNr {\n' +
                         actual + '\n}\n' + (root / 'tests/nr-menu/cases.h').read_text())
    binary = Path(directory) / 'menu'
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'OptiScaler'), str(generated), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: actual menu state transitions, short Config writes and project registration')
