#!/usr/bin/env python3
"""Build the production portable codec and exact Config transaction methods under sanitizers."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
config_h = (root / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
optional = config_h[config_h.index('enum HasDefaultValue'):config_h.index('constexpr inline int UnboundKey')]
methods = config_cpp[config_cpp.index('DlssNrResolvedPassSettings Config::DlssNrMasterSettingsUnlocked'):config_cpp.index('bool Config::Reload(')]
api = config_h[config_h.index('    DlssNrPassSnapshot GetDlssNrPassSnapshot'):config_h.index('    // Which depth convention')]
fields = '\n'.join(line for line in config_h.splitlines() if any(f' {name} ' in line for name in
    ('DlssNrIntensity', 'DlssNrLocalStructure', 'DlssNrLocalTone', 'DlssNrSkinStructure', 'DlssNrStyle',
     'DlssNrPreset', 'DlssNrAutoMask', 'DlssNrPasses', 'DlssNrIndividualPassSettings')) and 'CustomOptional<' in line)
source = '''#include <concepts>
#include <optional>
#include <mutex>
#include "DlssNr_PassSettings.h"
#define SI_NO_CONVERSION
#include <SimpleIni.h>
''' + optional + '\nclass Config { public:\n' + fields + '\n' + api + '''
private:
mutable std::mutex _dlssNrPassSettingsMutex;
DlssNr::PassConfig::Overrides _dlssNrPassOverrides;
DlssNrResolvedPassSettings DlssNrMasterSettingsUnlocked() const;
};
''' + methods + (root / 'tests/nr-pass-config/cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='nr-pass-config-') as temp:
    temp = pathlib.Path(temp)
    (temp / 'test.cpp').write_text(source)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                    '-isystem', str(root / 'external/simpleini'), '-I' + str(root / 'OptiScaler/dlssnr'),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], cwd=temp, check=True)
# Four config points and scoped parsing: tests may not substitute generic parser rewrites.
for key in ('Passes', 'IndividualPassSettings'):
    assert f'DlssNr{key} {{ ' in config_h
    assert f'"DlssNr", "{key}"' in config_cpp[:config_cpp.index('bool Config::SaveIni()')]
    assert f'ini.SetValue("DlssNr", "{key}"' in config_cpp
    assert f'{key}=auto' in (root / 'OptiScaler.ini').read_text()
print('PASS: both master keys have declaration/default, read, save and shipped INI entries')
