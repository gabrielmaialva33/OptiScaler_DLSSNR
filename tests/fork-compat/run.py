#!/usr/bin/env python3
"""Compile real INI transactions and signature hooks against host API fakes."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def function(source, anchor):
    start = source.index(anchor)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


header = (ROOT / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
config = (ROOT / 'OptiScaler/Config.cpp').read_text()
optional = header[header.index('enum HasDefaultValue'):header.index('constexpr inline int UnboundKey')]
declarations = '\n'.join(re.findall(r'^    CustomOptional<std::wstring, NoDefault> Xe(?:SS|FG|LL)Library;', header, re.M))
reads = '\n'.join(re.findall(r'^\s*\w+\.set_from_config\(readWString\("Libraries", "Xe(?:SS|FG|LL)Path"\)\);', config, re.M))
saves = '\n'.join(re.findall(r'ini\.SetValue\("Libraries", "Xe(?:SS|FG|LL)Path",.*?;', config, re.S))
config_source = '''#include <algorithm>
#include <cassert>
#include <cctype>
#include <concepts>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#define SI_NO_CONVERSION
#include <SimpleIni.h>
std::string wstring_to_string(const std::wstring& value) { return {value.begin(), value.end()}; }
std::wstring string_to_wstring(const std::string& value) { return {value.begin(), value.end()}; }
''' + optional + '''
class Config { public:
''' + declarations + '''
CSimpleIniA ini;
std::vector<std::string> _log;
static Config* current;
static Config* Instance() { return current; }
std::optional<std::wstring> readWString(std::string section, std::string key, bool lowercase = false);
void Read() {
''' + reads + '''
}
void Save() { current = this;
''' + saves + '''
}
};
Config* Config::current = nullptr;
''' + function(config, 'std::optional<std::wstring> Config::readWString(') + (HERE / 'config-cases.cpp').read_text()

hooks_source = (HERE / 'hook-fakes.h').read_text()
for filename, anchor in [('Crypt32_Hooks.h', 'static BOOL hkCryptQueryObject('),
                         ('Wintrust_Hooks.h', 'static LONG hkWinVerifyTrust(')]:
    hooks_source += '\n' + function((ROOT / 'OptiScaler/hooks' / filename).read_text(), anchor)
hooks_source += (HERE / 'hook-cases.cpp').read_text()

failed = []
with tempfile.TemporaryDirectory(prefix='fork-compat-') as folder:
    folder = Path(folder)
    for name, source in [('config', config_source), ('signature', hooks_source)]:
        translation_unit = folder / f'{name}.cpp'
        translation_unit.write_text(source)
        executable = folder / name
        subprocess.run(['g++', '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                        '-isystem', str(ROOT / 'external/simpleini'),
                        str(translation_unit), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], cwd=folder)
        if result.returncode:
            failed.append(name)
if failed:
    raise SystemExit('FAIL: ' + ', '.join(failed))
