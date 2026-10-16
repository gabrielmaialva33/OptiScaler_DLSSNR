#!/usr/bin/env python3
"""Compile real INI transactions and signature hooks against host API fakes."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutations', action='store_true', help='verify eight binding regression controls fail')
args = parser.parse_args()

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

# Keep production binding, bundled-hook early returns, and API-selection guards intact.
# Only InitStreamline's DLL/bootstrap side effects are replaced by a scripted boundary.
proxy = (ROOT / 'OptiScaler/proxies/Streamline_Proxy.h').read_text()
binding_source = (HERE / 'binding-fakes.h').read_text()
pointer_names = ('slGetFeatureFunction', 'slSetD3DDevice', 'slSetVulkanInfo',
                 'slGetPluginFunction', 'slDLSSGSetOptions', 'slDLSSGGetState',
                 'slReflexGetState', 'slReflexSleep', 'slReflexSetOptions',
                 'slReflexSetCameraData', 'slReflexGetPredictedCameraData',
                 'slPCLGetState', 'slPCLSetMarker', 'slPCLSetOptions')
aliases = []
fields = []
for name in pointer_names:
    aliases.append(re.search(r'^    typedef .*\bPFN_' + name + r'\b.*;$', proxy, re.M).group(0))
    fields.extend(re.findall(r'^    inline static PFN_' + name + r' _\w+ = nullptr;$', proxy, re.M))
for name in ('_isD3D12Inited', '_isD3D12Requested', '_isVulkanInited', '_isVulkanRequested'):
    fields.append(re.search(r'^    inline static bool ' + name + r' = false;$', proxy, re.M).group(0))
binding_source = binding_source.replace('// @aliases@', '\n'.join(aliases))
binding_source = binding_source.replace('// @fields@', '\n'.join(fields))
anchors = ('    template <typename T>\n    static bool ResolveActiveFeature(',
           '    static bool BindActiveFeatures()',
           '    static HMODULE HookStreamlineDLSSG()',
           '    static HMODULE HookStreamlineReflex()',
           '    static HMODULE HookStreamlinePCL()',
           '    static sl::Result SetD3DDeviceAndBind(',
           '    static bool DisableReflex()',
           '    static bool InitWithD3D12(',
           '    static bool InitWithVulkan()',
           '    static bool SetVulkanInfo(')
production_binding = '\n'.join(function(proxy, anchor) for anchor in anchors)
binding_template = binding_source
binding_tail = (HERE / 'binding-cases.cpp').read_text()
binding_source = binding_template.replace('// @production@', production_binding) + binding_tail

failed = []
with tempfile.TemporaryDirectory(prefix='fork-compat-') as folder:
    folder = Path(folder)
    def compile_and_run(name, source, expect_failure=False):
        translation_unit = folder / f'{name}.cpp'
        translation_unit.write_text(source)
        executable = folder / name
        subprocess.run(['g++', '-std=c++20' if name.startswith('binding') else '-std=c++23', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-type-limits',
                        '-isystem', str(ROOT / 'external/simpleini'),
                        '-isystem', str(ROOT / 'external/streamline'),
                        '-isystem', str(ROOT / 'external/vulkan/include'),
                        str(translation_unit), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], cwd=folder, capture_output=expect_failure)
        if expect_failure:
            if result.returncode == 0:
                raise SystemExit('FAIL: regression control survived: ' + name)
        elif result.returncode:
            failed.append(name)

    for name, source in [('config', config_source), ('signature', hooks_source), ('binding', binding_source)]:
        compile_and_run(name, source)

    if args.mutations and not failed:
        controls = []
        for group, pointer in [('dlssg', '_slDLSSGSetOptions'), ('reflex', '_slReflexGetState'),
                               ('pcl', '_slPCLSetMarker')]:
            before = '            ' + pointer + ' = nullptr;'
            assert production_binding.count(before) == 1
            controls.append(('uncleared-' + group, production_binding.replace(before, '')))
        before = ' &&\n            State::Instance().currentD3D11Device == nullptr'
        assert production_binding.count(before) == 1
        controls.append(('defer-bridge', production_binding.replace(before, '')))
        before = '        if (_isVulkanRequested)\n            return false;'
        assert production_binding.count(before) == 1
        controls.append(('interop-steals-vulkan', production_binding.replace(before, '        if (false)\n            return false;')))
        before = 'result != sl::Result::eOk || !_isD3D12Requested'
        assert production_binding.count(before) == 1
        controls.append(('bind-failed-device', production_binding.replace(before, '!_isD3D12Requested')))
        before = 'return _slReflexSetOptions != nullptr && _slReflexSetOptions(options)'
        assert production_binding.count(before) == 1
        controls.append(('null-reflex-call', production_binding.replace(before, 'return _slReflexSetOptions(options)')))
        binding = function(proxy, '    static bool BindActiveFeatures()')
        # The final required-pointer completeness check, reverting the integration defect seen RED.
        incomplete = re.sub(r'        return [^;]+;\s*}\Z', '        return true;\n    }', binding)
        assert incomplete != binding
        controls.append(('incomplete-vulkan-success', production_binding.replace(binding, incomplete)))
        for name, mutated in controls:
            compile_and_run('binding-' + name,
                            binding_template.replace('// @production@', mutated) + binding_tail, True)
        print(f'PASS: {len(controls)} Streamline regression controls rejected')
if failed:
    raise SystemExit('FAIL: ' + ', '.join(failed))
