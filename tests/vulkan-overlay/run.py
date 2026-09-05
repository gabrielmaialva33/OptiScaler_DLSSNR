#!/usr/bin/env python3
"""Build and run an isolated, fail-closed Vulkan overlay integration test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
BUILD_PREFIX = OUT / 'build-prefix'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def win(path):
    return 'Z:' + str(path.resolve()).replace('/', '\\')


def run(args, *, env=None, cwd=ROOT, logfile=None, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    if logfile:
        with open(logfile, 'w') as log:
            p = subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
        if p.returncode:
            print(Path(logfile).read_text(errors='replace')[-6000:], file=sys.stderr)
    else:
        p = subprocess.run(list(map(str, args)), cwd=cwd, env=env, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f'command failed ({p.returncode}); log: {logfile}')


def instrument(ref):
    name = 'OptiScaler/menu/menu_overlay_vk.cpp'
    source = subprocess.check_output(['git', 'show', f'{ref}:{name}'], cwd=ROOT, text=True)
    # Keep the rest of the DLL identical to the source revision being claimed.
    if subprocess.check_output(['git', 'diff', ref, '--', 'OptiScaler', 'external'], cwd=ROOT):
        raise RuntimeError('production tree differs from requested revision; refusing a mixed-version test')
    digest = hashlib.sha256(source.encode()).hexdigest()
    anchor = '// Vulkan overlay code adopted from here:'
    if source.count(anchor) != 1:
        raise RuntimeError('instrumentation include anchor changed')
    source = source.replace(anchor, f'#include "{(HERE / "probe.h").as_posix()}"\n\n' + anchor)
    for function, counter in [('void MenuOverlayVk::CreateSwapchain(', 'createCalls'),
                              ('void MenuOverlayVk::DestroyVulkanObjects(', 'destroyCalls'),
                              ('static bool DestroyVulkanObjectsLocked(bool shutdown)\n{', 'teardownCalls'),
                              ('bool MenuOverlayVk::QueuePresent(', 'presentCalls')]:
        if source.count(function) != 1:
            raise RuntimeError(f'instrumentation anchor changed: {function}')
        body = source.index('{', source.index(function)) + 1
        source = source[:body] + f'\n    ++VkLifetimeProbe::stats.{counter};' + source[body:]
    anchor = '    _frameFencePending[idx] = true;'
    if source.count(anchor) != 1:
        raise RuntimeError('overlay submit instrumentation anchor changed')
    source = source.replace(anchor, anchor + '\n    ++VkLifetimeProbe::stats.overlaySubmits;')
    source += r'''
// Test exports exist only in this generated translation unit, never in production.
extern void KeyUp(UINT vKey);
extern "C" __declspec(dllexport) void VkLifetimeGetStats(VkLifetimeStats* out)
{
    std::scoped_lock presentLock(_vkPresentMutex);
    std::scoped_lock cleanLock(_vkCleanMutex);
    *out = VkLifetimeProbe::stats;
    out->ready = _vulkanObjectsCreated;
    out->imageCount = _ImVulkan_Info.ImageCount;
}
extern "C" __declspec(dllexport) void VkLifetimeArmFailure(unsigned mode)
{
    VkLifetimeProbe::failure = mode;
    VkLifetimeProbe::framebufferCalls = 0;
}
extern "C" __declspec(dllexport) void VkLifetimeDestroy()
{
    MenuOverlayVk::DestroyVulkanObjects(false);
}
extern "C" __declspec(dllexport) void VkLifetimeOpenMenu()
{
    if (!MenuOverlayBase::IsVisible())
        KeyUp(Config::Instance()->ShortcutKey.value_or_default());
}
'''
    (OUT / 'menu_overlay_vk.instrumented.cpp').write_text(source)
    commit = subprocess.check_output(['git', 'rev-parse', ref], cwd=ROOT, text=True).strip()
    (OUT / 'source.json').write_text(json.dumps({'commit': commit, 'overlay_sha256': digest}, indent=2) + '\n')


def build(ref):
    OUT.mkdir(exist_ok=True)
    (OUT / 'run').mkdir(exist_ok=True)
    instrument(ref)
    if not BUILD_PREFIX.exists():
        run(['cp', '-a', '--reflink=auto', TOOLCHAIN_PREFIX, BUILD_PREFIX])
    # A late import replaces exactly one TU; all intermediates and output stay outside x64/Release.
    ns = 'http://schemas.microsoft.com/developer/msbuild/2003'
    project = ET.Element('Project', xmlns=ns)
    group = ET.SubElement(project, 'ItemGroup')
    ET.SubElement(group, 'ClCompile', Remove='menu\\menu_overlay_vk.cpp')
    item = ET.SubElement(group, 'ClCompile', Include=win(OUT / 'menu_overlay_vk.instrumented.cpp'))
    ET.SubElement(item, 'AdditionalIncludeDirectories').text = win(ROOT / 'OptiScaler/menu') + ';%(AdditionalIncludeDirectories)'
    ET.ElementTree(project).write(OUT / 'overlay.targets', encoding='utf-8', xml_declaration=True)
    env = dict(os.environ, WINEPREFIX=str(BUILD_PREFIX), WINEDEBUG='-all')
    run([MSVC / 'msbuild', ROOT / 'OptiScaler/OptiScaler.vcxproj', '/nologo', '/v:minimal',
         '/p:Configuration=Release', '/p:Platform=x64', '/p:CL_MPCount=16',
         '/p:PreBuildEventUseInBuild=false', '/p:PostBuildEventUseInBuild=false',
         '/p:SolutionDir=' + win(ROOT) + '\\', '/p:IntDir=' + win(OUT / 'obj') + '\\',
         '/p:OutDir=' + win(OUT / 'dll') + '\\',
         '/p:ForceImportBeforeCppTargets=' + win(OUT / 'overlay.targets')],
        env=env, logfile=OUT / 'build-dll.log', timeout=1200)
    shutil.copy2(OUT / 'dll/OptiScaler.dll', OUT / 'run/dxgi.dll')
    # Wine can leave the compiler prefix's server stuck after MSBuild. Reset ONLY our disposable
    # copy before the standalone compiler; never terminate the user's toolchain/game prefixes.
    run(['wineserver', '-k'], env=env)
    run(['wineserver', '-w'], env=env, timeout=30)
    run([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4',
         '/I' + str(ROOT / 'external/vulkan/include'), HERE / 'harness.cpp',
         '/Fo' + str(OUT / 'harness.obj'), '/Fe' + str(OUT / 'run/vk-overlay-harness.exe'),
         '/link', '/LIBPATH:' + str(ROOT / 'OptiScaler/library/vulkan'), 'vulkan-1.lib', 'user32.lib'],
        env=dict(env, WINE_MSVC_RAW_STDOUT='1'), logfile=OUT / 'build-app.log')
    # The harness does not install an upscaler/model/FG DLL or copy an entire game deployment.
    (OUT / 'run/OptiScaler.ini').write_text('[Menu]\nOverlayMenu=true\nShortcutKey=0x2D\n'
                                          '[DlssNr]\nEnabled=false\n'
                                          '[Log]\nLogToFile=true\nLogLevel=1\n')
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime not found')
    for dll in redists[-1].glob('*.dll'):
        shutil.copy2(dll, OUT / 'run' / dll.name)
    provenance = json.loads((OUT / 'source.json').read_text())
    provenance['binaries'] = {name: sha(OUT / 'run' / name)
                             for name in ('dxgi.dll', 'vk-overlay-harness.exe')}
    provenance['harness_sources'] = {p.name: sha(p) for p in HERE.iterdir()
                                      if p.suffix in ('.h', '.cpp', '.py')}
    (OUT / 'source.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Built isolated instrumentation of', ref, flush=True)


def execute():
    work = OUT / 'run'
    (OUT / 'results.json').write_text('{"status":"RUNNING"}\n')
    provenance = json.loads((OUT / 'source.json').read_text())
    for name, digest in provenance['binaries'].items():
        if sha(work / name) != digest:
            raise RuntimeError(f'binary changed since build: {name}')
    for name, digest in provenance['harness_sources'].items():
        if sha(HERE / name) != digest:
            raise RuntimeError(f'harness source changed since build: {name}; rebuild before running')
    if (work / 'optiscaler_skip_vulkan_hooks').exists():
        raise RuntimeError('marker present in harness directory; it will not be removed')
    if any(work.glob('sl.*.dll')) or any(work.glob('*dlssg*.dll')):
        raise RuntimeError('Streamline/DLSS-G found in harness directory')
    layer = Path('/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json')
    if not layer.exists():
        raise RuntimeError('Khronos validation layer missing; cannot claim a validated run')
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='dxgi=n,b;vcruntime140=n;vcruntime140_1=n;msvcp140=n',
               VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
               VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0')
    # Native layers are enabled through Wine's host loader. The rejected-fence negative control
    # must reach the error callback; finding the JSON or setting an environment variable is not proof.
    run(['wine', work / 'vk-overlay-harness.exe'], env=env, cwd=work,
        logfile=OUT / 'run.log', timeout=180)
    result = json.loads((work / 'result.json').read_text())
    if result.get('status') != 'PASS':
        raise RuntimeError('harness did not emit an explicit PASS result')
    # Exercise the silent-no-coverage failure mode in another test directory, without any marker.
    control = OUT / 'control'
    control.mkdir(exist_ok=True)
    if (control / 'optiscaler_skip_vulkan_hooks').exists():
        raise RuntimeError('marker present in negative-control directory; it will not be removed')
    if any(control.glob('sl.*.dll')) or any(control.glob('*dlssg*.dll')):
        raise RuntimeError('unexpected FG DLL in negative-control directory')
    for p in work.iterdir():
        if p.suffix.lower() in ('.dll', '.exe'):
            shutil.copy2(p, control / p.name)
    (control / 'OptiScaler.ini').write_text((work / 'OptiScaler.ini').read_text().replace('OverlayMenu=true', 'OverlayMenu=false'))
    with (OUT / 'negative-control.log').open('w') as log:
        negative = subprocess.run(['wine', str(control / 'vk-overlay-harness.exe')], cwd=control, env=env,
                                  stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=60)
    if negative.returncode != 1 or 'ZERO COVERAGE:' not in (OUT / 'negative-control.log').read_text():
        raise RuntimeError('disabled-overlay negative control did not fail explicitly on zero coverage')
    result['zero_coverage_control'] = 'PASS (disabled overlay rejected with exit 1)'
    result['source'] = provenance
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref', default='HEAD', help='commit whose overlay source is instrumented')
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(exist_ok=True)
    (OUT / 'results.json').write_text('{"status":"RUNNING"}\n')
    if not args.run_only:
        build(args.ref)
    if not args.build_only:
        execute()


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired, OSError, KeyError, ValueError) as error:
        if OUT.exists():
            (OUT / 'results.json').write_text(json.dumps({'status': 'FAIL', 'reason': str(error)}, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
