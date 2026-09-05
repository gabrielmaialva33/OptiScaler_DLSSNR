#!/usr/bin/env python3
"""Exercise the actual Config GPU timing transactions and INI expressions on the host."""
import pathlib
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

root = pathlib.Path(__file__).resolve().parents[2]
header = (root / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
implementation = (root / 'OptiScaler/Config.cpp').read_text()


def block(source, anchor):
    start = source.index(anchor)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def statement(source, anchor):
    start = source.index(anchor)
    opening = source.index('(', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '(') - (source[end] == ')')
        end += 1
    return source[start:end + 1]


optional = header[header.index('enum HasDefaultValue'):header.index('constexpr inline int UnboundKey')]
declarations = header[header.index('    struct DlssNrGpuTimingSettings'):header.index('    DlssNrPassSnapshot GetDlssNrPassSnapshot')]
methods = implementation[implementation.index('Config::DlssNrGpuTimingSettings Config::GetDlssNrGpuTimingSettings'):implementation.index('DlssNrResolvedPassSettings Config::DlssNrMasterSettingsUnlocked')]
read = '\n'.join(statement(implementation, f'DlssNr{key}.set_from_config(')
                 for key in ('GpuTiming', 'GpuTimingInterval'))
# The interval expressions contain a lambda; extract balanced SetValue calls rather than
# stopping at that lambda's semicolon. Nothing substitutes the production serializer.
save = []
for key in ('GpuTiming', 'GpuTimingInterval'):
    anchor = f'ini.SetValue("DlssNr", "{key}",'
    start = implementation.index(anchor)
    opening = implementation.index('(', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (implementation[end] == '(') - (implementation[end] == ')')
        end += 1
    save.append(implementation[start:end + 1])

source = '''#include <concepts>
#include <format>
#include <optional>
#include <mutex>
#include <thread>
#include <cassert>
#include <iostream>
#include "DlssNr_PassSettings.h"
#define SI_NO_CONVERSION
#include <SimpleIni.h>
''' + optional + '\n' + block(implementation, 'std::string GetBoolValue(') + '\n' + block(implementation, 'template <typename T> std::string GetIntValue(') + '''
class Config { public:
''' + declarations + '''
void Read(CSimpleIniA& ini) { const std::lock_guard lock(_dlssNrPassSettingsMutex);
''' + read + '''
}
void Save(CSimpleIniA& ini) { auto* config = this; const std::lock_guard lock(_dlssNrPassSettingsMutex);
''' + '\n'.join(save) + '''
}
private: mutable std::mutex _dlssNrPassSettingsMutex;
};
''' + methods + (root / 'tests/nr-gpu-timing-config/cases.cpp').read_text()

with tempfile.TemporaryDirectory(prefix='nr-gpu-timing-config-') as directory:
    directory = pathlib.Path(directory)
    (directory / 'test.cpp').write_text(source)
    subprocess.run(['g++', '-std=c++23', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                    '-isystem', str(root / 'external/simpleini'), '-I' + str(root / 'OptiScaler/dlssnr'),
                    str(directory / 'test.cpp'), '-o', str(directory / 'test')], check=True)
    subprocess.run([str(directory / 'test')], cwd=directory, check=True)

ini_text = (root / 'OptiScaler.ini').read_text()
for key, default in [('GpuTiming', 'false'), ('GpuTimingInterval', '30')]:
    assert re.search(r'CustomOptional<[^>]+> DlssNr' + key + r'\s*\{\s*' + default + r'\s*\}', declarations)
    assert f'{key}=auto' in ini_text
    assert f'"DlssNr", "{key}"' in read
    assert any(f'ini.SetValue("DlssNr", "{key}"' in line for line in save)

for name in ('OptiScaler.vcxproj', 'OptiScaler.vcxproj.filters'):
    project = ET.parse(root / 'OptiScaler' / name)
    entries = [node.attrib.get('Include') for node in project.iter()]
    for filename in ('DlssNr_GpuTiming.cpp', 'DlssNr_GpuTiming.h', 'DlssNr_GpuTimingModel.h'):
        assert entries.count('dlssnr\\' + filename) == 1, (name, filename)
print('PASS: four config points, actual INI round-trip and unique build registrations')
