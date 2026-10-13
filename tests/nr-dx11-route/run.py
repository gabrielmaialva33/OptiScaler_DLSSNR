#!/usr/bin/env python3
"""D3D11 DLSS route (docs/dlssnr/design/dx11-native-dlss-guides.md, steps 1 and 2): the decision header and the
production layer compiled under ASan/UBSan, the second against a minimal Config stub, plus source guards on the
call sites, on what the stub mirrors, and on the pt-BR entries for the panel's text."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8')


def require(condition, message):
    if not condition:
        failures.append(message)


def function_body(source, signature):
    """The text from a function's signature to the next function at column zero."""
    start = source.find(signature)
    if start < 0:
        return ''
    end = source.find('\nNVSDK_NGX_API ', start + len(signature))
    return source[start:end if end >= 0 else len(source)]


# --- Call sites: one line each, where the decision is right -------------------------------------------------

dx11 = read('OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp')
require('#include <dlssnr/DlssNr_Dx11Upscaler.h>' in dx11, 'NVNGX_DLSS_Dx11.cpp does not include the route layer')
require(dx11.count('DlssNr') == 3, 'NVNGX_DLSS_Dx11.cpp should name DlssNr exactly three times: the include and two '
                                   f'one-line calls (found {dx11.count("DlssNr")})')

create = function_body(dx11, 'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_CreateFeature(')
default_at = create.find('upscalerChoice = Upscaler::DLSS;')
call_at = create.find('upscalerChoice = DlssNr::Dx11DefaultUpscaler(upscalerChoice);')
key_at = create.find('if (Config::Instance()->Dx11Upscaler.has_value())')
require(0 <= default_at < call_at < key_at,
        'CreateFeature must call Dx11DefaultUpscaler after its own DLSS default and before the Dx11Upscaler key, '
        'so an explicit key still wins (cases.cpp mirrors that order)')
require(create.count('Dx11DefaultUpscaler') == 1 and dx11.count('Dx11DefaultUpscaler') == 1,
        'Dx11DefaultUpscaler is called once, in CreateFeature')
supersampling_at = create.find('if (InFeatureID == NVSDK_NGX_Feature_SuperSampling)')
rr_at = create.find('else if (InFeatureID == NVSDK_NGX_Feature_RayReconstruction)')
require(0 <= supersampling_at < call_at < rr_at, 'the default is replaced for super resolution only')

evaluate = function_body(dx11, 'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature(')
type_at = evaluate.find('auto upscaler = deviceContext->GetUpscalerType();')
note_at = evaluate.find('DlssNr::NoteDx11Evaluate(upscaler);')
require(0 <= type_at < note_at, 'EvaluateFeature must note each evaluate with the feature\'s own upscaler type')
require(evaluate.find('deviceContext->Evaluate(InDevCtx, InParameters);') < note_at,
        'the evaluate is noted after it ran, not before')
require(dx11.count('NoteDx11Evaluate') == 1, 'NoteDx11Evaluate is called once, in EvaluateFeature')

# --- The production layer: inert with NR off ------------------------------------------------------------------

layer = read('retoque/dlssnr/DlssNr_Dx11Upscaler.h')
for marker in ('inline ::Upscaler Dx11DefaultUpscaler(', 'inline void NoteDx11Evaluate(',
               'inline Dx11Route::Report Dx11RouteReport('):
    require(marker in layer, f'DlssNr_Dx11Upscaler.h no longer has {marker!r}; update this runner')
note = layer[layer.find('inline void NoteDx11Evaluate('):layer.find('inline Dx11Route::Report Dx11RouteReport(')]
gate = note.find('if (!cfg.DlssNrEnabled.value_or_default())\n        return;')
require(0 <= gate < note.find('g_lastRoute.store') and gate < note.find('LOG_'),
        'NoteDx11Evaluate must return with NR off before it stores or logs anything')
choose = layer[layer.find('inline ::Upscaler Dx11DefaultUpscaler('):layer.find('inline void NoteDx11Evaluate(')]
require('cfg.DlssNrEnabled.value_or_default()' in choose and 'cfg.Dx11Upscaler.has_value()' in choose,
        'Dx11DefaultUpscaler decides from DlssNrEnabled and whether Dx11Upscaler is set')
require(re.search(r'case ::Upscaler::DLSS:\s*return Dx11Route::Kind::NativeDlss;', layer) and
        re.search(r'case ::Upscaler::DLSS_on12:\s*return Dx11Route::Kind::BridgedDlss;', layer),
        'KindOf maps dlss to native DLSS and dlss_12 to the bridge')

pure = read('retoque/dlssnr/DlssNr_Dx11Route.h')
includes = re.findall(r'^#include\s+(\S+)', pure, re.M)
require(all(i.startswith('<') and '/' not in i and '.' not in i for i in includes),
        f'DlssNr_Dx11Route.h must include only standard headers (found {includes})')

# --- What the stub mirrors --------------------------------------------------------------------------------------

def enumerators(source):
    match = re.search(r'enum class Upscaler\s*\{(.*?)\};', source, re.S)
    if not match:
        return []
    body = re.sub(r'//[^\n]*', '', match.group(1))
    return [name.strip() for name in body.split(',') if name.strip()]


stub = read('tests/nr-dx11-route/stubs/Config.h')
real = enumerators(read('OptiScaler/OptiTypes.h'))
require(real and enumerators(stub) == real, 'stubs/Config.h has a different Upscaler enum than OptiTypes.h')
config_h = read('OptiScaler/Config.h')
for declaration, stubbed in (('CustomOptional<bool> DlssNrEnabled { false };', 'DlssNrEnabled { false }'),
                             ('CustomOptional<uint32_t> DlssNrHookMethod { 1 };', 'DlssNrHookMethod { 1 }'),
                             ('CustomOptional<Upscaler, SoftDefault> Dx11Upscaler { Upscaler::FSR22 };',
                              'Dx11Upscaler { Upscaler::FSR22 }')):
    require(declaration in config_h, f'Config.h no longer declares {declaration!r}; update stubs/Config.h')
    require(stubbed in stub, f'stubs/Config.h does not mirror {declaration!r}')

# --- The panel ---------------------------------------------------------------------------------------------------

menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
require('const auto routeReport = enabled ? DlssNr::Dx11RouteReport() : DlssNr::Dx11Route::Report::None;' in menu
        and 'const char* routeNotice = DlssNr::Dx11Route::ReportText(routeReport);' in menu,
        'the NR panel does not ask for the route line, or asks with NR off')
require('Localization::Tr(routeNotice)' in menu, 'the route line is not translated')
require(re.search(r'if \(!vkOnly && routeReport != DlssNr::Dx11Route::Report::NoPass\)\s*\{\s*'
                  r'const char\* status = DlssNr::HookStatus\(\);', menu),
        'the "upscaled: active" hook status must not contradict the after-upscaler route line')
require(re.search(r'else if \(enabled && routeNotice == nullptr\)\s*ImGui::TextUnformatted\(Localization::Tr\('
                  r'"Waiting for the upscaler to run\."\)\);', menu),
        '"Waiting for the upscaler to run." must give way to the route line')

# --- Build and run -----------------------------------------------------------------------------------------------

pack = read('retoque/translations/optional/pt-BR.lang')
pack_keys = set()
for line in pack.splitlines():
    if line and not line.startswith('#'):
        key = line.split('\t')[0]
        pack_keys.add(re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\'}[m.group(1)], key))

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-dx11-route-') as directory:
    binary = Path(directory) / 'cases'
    # stubs/ stands in for OptiScaler/, which is deliberately not on the include path: <Config.h> is the stub.
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(here / 'stubs'), '-I', str(root / 'retoque'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    texts = subprocess.run([str(binary), '--texts'], check=True, capture_output=True, text=True).stdout.splitlines()
    require(len(texts) == 2, f'expected two report texts, got {len(texts)}')
    for text in texts:
        require(text in pack_keys, f'pt-BR.lang has no entry for the panel line {text[:70]!r}...')
    cases = subprocess.run([str(binary)])

if failures:
    for failure in failures:
        print('FAIL:', failure)
if failures or cases.returncode != 0:
    raise SystemExit(1)

print('PASS: D3D11 route source guards')
