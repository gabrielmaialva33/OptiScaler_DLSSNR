#!/usr/bin/env python3
"""The Vulkan present host's stage 1 under Wine (docs/dlssnr/design/nr-vulkan-present-host.md): the production DLL,
uninstrumented, loaded by a Win32 Vulkan application, with [DlssNr] VulkanPresentHost on and off, the skip marker
present and absent, the Vulkan menu on and off. The observe lines in OptiScaler.log are held against the facts the
application wrote about itself; with the key off, nothing of the host may appear and vulkan-1.dll's exports must be
byte-for-byte untouched."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
MSVC = Path(os.environ.get('MSVC_BIN', str(Path.home() / '.local/opt/msvc/bin/x64')))
TOOLCHAIN_PREFIX = Path(os.environ.get('WINEPREFIX', str(Path.home() / '.local/opt/msvc-wineprefix')))
BUILD_PREFIX = OUT / 'build-prefix'
EXE = 'vk-present-host-harness.exe'
TRANSLATED_EXE = 'vk-present-host-translated.exe'
MARKER = 'optiscaler_skip_vulkan_hooks'
OBSERVE = 'DLSS-NR Vulkan present host (observe only): window '
HOST_TEXT = 'DLSS-NR Vulkan present host'

# What the host detours. vkQueueSubmit and vkCreateFence are controls: the host never hooks them.
HOST_HOOKED = ['vkCreateInstance', 'vkDestroyInstance', 'vkEnumeratePhysicalDevices', 'vkCreateDevice',
               'vkCreateWin32SurfaceKHR', 'vkDestroySurfaceKHR', 'vkDestroyDevice', 'vkGetDeviceQueue',
               'vkGetDeviceQueue2', 'vkCreateSwapchainKHR', 'vkDestroySwapchainKHR', 'vkGetSwapchainImagesKHR',
               'vkAcquireNextImageKHR', 'vkQueuePresentKHR']
NEVER_HOOKED = ['vkQueueSubmit', 'vkCreateFence']
DEVICE_LEVEL = {'vkDestroyDevice', 'vkGetDeviceQueue', 'vkGetDeviceQueue2', 'vkCreateSwapchainKHR',
                'vkDestroySwapchainKHR', 'vkGetSwapchainImagesKHR', 'vkAcquireNextImageKHR', 'vkQueuePresentKHR'}

# name: (marker file, [Vulkan] SkipHooks, [DlssNr] Enabled, VulkanPresentHost, OverlayMenu, one window only)
#
# The last two run the legacy bundle with its Vulkan menu. That overlay dies (a null read inside
# MenuOverlayVk::CreateSwapchain) when a second window's swapchain is created on a second device, with this host off
# as well, so they stop after window 1 (--one-window). See README.md.
CASES = {
    'marker': (True, False, True, True, True, False),
    'skiphooks': (False, True, True, True, True, False),
    'marker-key-off': (True, False, True, False, True, False),
    'marker-master-off': (True, False, False, True, True, False),
    'menu-off': (False, False, True, True, False, False),
    'menu-on': (False, False, True, True, True, True),
    'key-off': (False, False, True, False, True, True),
}

# A D3D title under Proton's translation layers (translated.cpp), the marker present as deployed, OptiScaler as winmm.dll:
# name: (API, VulkanPresentHost). DXVK and vkd3d-proton come from a local Proton build.
TRANSLATED_CASES = {
    'dxvk': ('d3d11', True),
    'vkd3d-proton': ('d3d12', True),
    'dxvk-key-off': ('d3d11', False),
}
TRANSLATED_CALLER = {'d3d11': 'DXVK', 'd3d12': 'vkd3d-proton'}
PROTON_LIBRARIES = {'dxvk': ['dxgi.dll', 'd3d11.dll', 'd3d10core.dll'], 'vkd3d-proton': ['d3d12.dll', 'd3d12core.dll']}

USAGE_TRANSFER_SRC, USAGE_TRANSFER_DST, USAGE_STORAGE = 0x1, 0x2, 0x8
QUEUE_GRAPHICS, QUEUE_COMPUTE, QUEUE_TRANSFER = 0x1, 0x2, 0x4


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, *, env=None, cwd=ROOT, logfile=None, timeout=600):
    print('+', ' '.join(map(str, args)), flush=True)
    with open(logfile, 'w') as log:
        p = subprocess.run(list(map(str, args)), cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log,
                           stderr=subprocess.STDOUT, timeout=timeout)
    if p.returncode:
        print(Path(logfile).read_text(errors='replace')[-4000:], file=sys.stderr)
        raise RuntimeError(f'command failed ({p.returncode}); log: {logfile}')


def finished(process, facts, run_log, name):
    """The application's exit: 0, or a SIGKILL during Wine's process teardown after it wrote its facts and its PASS line
    (seen once in a dozen runs, with the host not even installed). A kill at exit cannot make a check pass -- an
    unflushed log only fails one -- so it is reported and accepted only then."""
    if process.returncode == 0:
        return True
    if (process.returncode == -9 and facts.get('status') == 'PASS' and
            any(l.startswith('PASS: ') for l in run_log.splitlines())):
        print(f'NOTE: {name}: the application was killed during Wine\'s teardown after its PASS; its facts are complete',
              flush=True)
        return True
    return False


def check_dll(dll):
    if not dll.exists():
        raise RuntimeError(f'{dll} does not exist; run ./build-local.sh Release first')
    data = dll.read_bytes()
    if OBSERVE.encode() not in data:
        raise RuntimeError(f'{dll} has no present host in it (stale build); run ./build-local.sh Release')
    # A source newer than the DLL means the DLL is not what the tree says.
    sources = [ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp', ROOT / 'OptiScaler/Config.h', ROOT / 'OptiScaler/Config.cpp',
               *sorted((ROOT / 'retoque/dlssnr').glob('DlssNr_VkPresent*'))]
    stale = [str(s.relative_to(ROOT)) for s in sources if s.stat().st_mtime > dll.stat().st_mtime]
    if stale:
        raise RuntimeError(f'{dll} is older than {", ".join(stale)}; rebuild before testing')


def build_harness():
    OUT.mkdir(exist_ok=True)
    if not BUILD_PREFIX.exists():
        subprocess.run(['cp', '-a', '--reflink=auto', str(TOOLCHAIN_PREFIX), str(BUILD_PREFIX)], check=True)
    env = dict(os.environ, WINEPREFIX=str(BUILD_PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    try:
        run([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/O2',
             '/I' + str(ROOT / 'external/vulkan/include'), HERE / 'harness.cpp',
             '/Fo' + str(OUT / 'harness.obj'), '/Fe' + str(OUT / EXE),
             '/link', '/LIBPATH:' + str(ROOT / 'OptiScaler/library/vulkan'), 'vulkan-1.lib', 'user32.lib'],
            env=env, logfile=OUT / 'build-harness.log', timeout=600)
        run([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/O2', HERE / 'translated.cpp',
             '/Fo' + str(OUT / 'translated.obj'), '/Fe' + str(OUT / TRANSLATED_EXE), '/link', 'user32.lib'],
            env=env, logfile=OUT / 'build-translated.log', timeout=600)
    finally:
        # Leave no server running on the disposable compiler prefix; never touch any other prefix.
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)


def runtime_env():
    return dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
                WINEDLLOVERRIDES='dxgi=n,b;vcruntime140=n;vcruntime140_1=n;msvcp140=n',
                VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
                VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0')


def proton_libraries():
    """The directory holding DXVK's and vkd3d-proton's x86_64 DLLs of a local Proton, or None."""
    roots = [Path(os.environ['PROTON_DIR'])] if os.environ.get('PROTON_DIR') else []
    steam = Path.home() / '.local/share/Steam'
    roots += sorted((steam / 'compatibilitytools.d').glob('GE-Proton*'), reverse=True)
    roots += sorted((steam / 'steamapps/common').glob('Proton*'), reverse=True)
    for proton in roots:
        wine = proton / 'files/lib/wine'
        if all((wine / layer / 'x86_64-windows' / dll).exists() for layer, dlls in PROTON_LIBRARIES.items() for dll in dlls):
            return wine
    return None


def translated_env():
    # As Proton runs a D3D title: DXVK and vkd3d-proton are the system's dxgi/d3d11/d3d12, native first. No validation
    # layer: these cases are about which hooks the translation layers reach, not about their own Vulkan usage.
    return dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix-translated'), WINEDEBUG='-all',
                WINEDLLOVERRIDES='winmm=n,b;dxgi,d3d11,d3d10core,d3d12,d3d12core=n;'
                                 'vcruntime140=n;vcruntime140_1=n;msvcp140=n',
                VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0', DXVK_LOG_LEVEL='info')


def prepare_translated_prefix(wine):
    env = translated_env()
    prefix = Path(env['WINEPREFIX'])
    if not (prefix / 'drive_c/windows/system32').exists():
        subprocess.run(['wine', 'cmd', '/c', 'exit'], env=env, stdin=subprocess.DEVNULL, capture_output=True,
                       timeout=300)
    system32 = prefix / 'drive_c/windows/system32'
    for layer, dlls in PROTON_LIBRARIES.items():
        for dll in dlls:
            # Proton ships them read-only; copy the bytes, not the mode, so the next run can replace them.
            target = system32 / dll
            if target.exists():
                target.chmod(0o644)
                target.unlink()
            shutil.copyfile(wine / layer / 'x86_64-windows' / dll, target)


def ini_for(skiphooks, enabled, key, menu):
    text = f'[Menu]\nOverlayMenu={str(menu).lower()}\nShortcutKey=0x2D\n'
    text += f'[DlssNr]\nEnabled={str(enabled).lower()}\nVulkanPresentHost={str(key).lower()}\n'
    if skiphooks:
        text += '[Vulkan]\nSkipHooks=true\n'
    text += '[Log]\nLogToFile=true\nLogLevel=1\n'
    return text


def execute_case(name, dll, redist):
    marker, skiphooks, enabled, key, menu, one_window = CASES[name]
    work = OUT / 'cases' / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy2(dll, work / 'dxgi.dll')
    shutil.copy2(OUT / EXE, work / EXE)
    for library in redist:
        shutil.copy2(library, work / library.name)
    (work / 'OptiScaler.ini').write_text(ini_for(skiphooks, enabled, key, menu))
    if marker:
        (work / MARKER).write_text('')
    with (OUT / f'{name}.log').open('w') as log:
        process = subprocess.run(['wine', str(work / EXE)] + (['--one-window'] if one_window else []), cwd=work, env=runtime_env(), stdin=subprocess.DEVNULL,
                                 stdout=log, stderr=subprocess.STDOUT, timeout=180)
    run_log = (OUT / f'{name}.log').read_text(errors='replace')
    facts = json.loads((work / 'result.json').read_text())
    if not finished(process, facts, run_log, name) or facts.get('status') != 'PASS':
        reason = next((l for l in reversed(run_log.splitlines()) if l.startswith('FAIL: ')), 'no FAIL line')
        ids = sorted(set(re.findall(r'\b(?:VUID|SYNC-HAZARD)-[\w-]+', run_log)) -
                     {'VUID-VkFenceCreateInfo-flags-parameter'})
        raise RuntimeError(f'{name}: harness failed (exit {process.returncode}): {reason}'
                           + (f'; validation ids: {", ".join(ids)}' if ids else '') + f'; see {OUT / (name + ".log")}')
    if facts.get('validation_active') is not True or facts.get('validation_errors') != 0:
        raise RuntimeError(f'{name}: validation was not proved active and clean')
    log_path = work / 'OptiScaler.log'
    if not log_path.exists():
        raise RuntimeError(f'{name}: OptiScaler wrote no log; did the DLL load?')
    return facts, log_path.read_text(errors='replace')


# --- What the log must say ------------------------------------------------------------------------------------

def yes_no(flag):
    return 'yes' if flag else 'no'


def round_trip(usage, surface_usage):
    missing = (USAGE_TRANSFER_SRC | USAGE_TRANSFER_DST) & ~usage
    if not missing:
        return 'legal as created'
    names = '+'.join(n for bit, n in ((USAGE_TRANSFER_SRC, 'TRANSFER_SRC'), (USAGE_TRANSFER_DST, 'TRANSFER_DST'))
                     if missing & bit)
    if surface_usage & missing == missing:
        return f'needs {names} at creation, the surface offers it'
    return f'not possible, the surface does not offer {names}'


def expected_lines(facts, effective_usage):
    """The observe lines the host must write, per window, from the order of the application's presents: one at a
    window's first present, one whenever its swapchain, device, queue or caller changes, one at 10, 100, ... presents."""
    swapchains = {s['name']: s for s in facts['swapchains']}
    windows, acquires, lines = {}, {}, {1: [], 2: []}
    for p in facts['presents']:
        w = windows.setdefault(p['window'], dict(presents=0, swapchains=[], devices=[], next=10, last=None,
                                                 wmin=None, wmax=0))
        acquires[p['swapchain_name']] = acquires.get(p['swapchain_name'], 0) + 1
        w['presents'] += 1
        for listing, item in (('swapchains', p['swapchain_name']), ('devices', p['device_name'])):
            if item not in w[listing]:
                w[listing].append(item)
        w['wmin'] = p['waits'] if w['wmin'] is None else min(w['wmin'], p['waits'])
        w['wmax'] = max(w['wmax'], p['waits'])
        now = (p['swapchain_name'], p['device_name'], p['queue'])
        if w['last'] is None:
            why = ['first present']
        else:
            why = [reason for reason, changed in (('new swapchain', now[0] != w['last'][0]),
                                                  ('new device', now[1] != w['last'][1]),
                                                  ('new queue', now[2] != w['last'][2])) if changed]
        decade = w['presents'] == w['next']
        if decade:
            w['next'] *= 10
            if not why:
                why = [f'{w["presents"]} presents']
        if not why:
            continue
        s = swapchains[p['swapchain_name']]
        usage = effective_usage(s)
        lines[p['window']].append(dict(
            why=', '.join(why), presents=w['presents'], swapchains=len(w['swapchains']), devices=len(w['devices']),
            swapchain=s['handle'], swapchain_name=s['name'], width=s['width'], height=s['height'], format=s['format'],
            color_space=s['color_space'], images=s['images'], usage=usage, surface_usage=s['surface_usage'],
            round_trip=round_trip(usage, s['surface_usage']), device=p['device'], device_name=p['device_name'],
            queue=p['queue'], waits=f'{w["wmin"]}-{w["wmax"]}', acquires=acquires[p['swapchain_name']]))
        w['last'] = now
        w['wmin'], w['wmax'] = None, 0
    return lines


LINE = re.compile(
    r'DLSS-NR Vulkan present host \(observe only\): window (?P<window>0x[0-9A-F]+|unknown) \((?P<why>[^)]*)\); '
    r'presents (?P<presents>\d+), swapchains (?P<swapchains>\d+), devices (?P<devices>\d+); '
    r'swapchain (?P<swapchain>0x[0-9A-F]+) #(?P<swapchain_id>\d+) (?P<width>\d+)x(?P<height>\d+) '
    r'format (?P<format>\d+) colour-space (?P<color_space>\d+) images (?P<images>\d+) usage (?P<usage>0x[0-9A-F]+) '
    r'\(transfer-src (?P<src>yes|no), transfer-dst (?P<dst>yes|no), storage (?P<storage>yes|no)\)'
    r'(?: surface-usage (?P<surface_usage>0x[0-9A-F]+))? sharing (?P<sharing>EXCLUSIVE|CONCURRENT\[[\d,]*\])'
    r'(?P<retired> retired)?; round-trip (?P<round_trip>[^;]+); '
    r'device (?P<device>0x[0-9A-F]+) #(?P<device_id>\d+) physical (?P<physical>0x[0-9A-F]+) '
    r'instance #(?P<instance_id>\d+); '
    r'queue (?P<queue>0x[0-9A-F]+) family (?P<family>\d+) index (?P<index>\d+) flags (?P<flags>0x[0-9A-F]+) '
    r'\(graphics (?P<graphics>yes|no), compute (?P<compute>yes|no), transfer (?P<transfer>yes|no)\); '
    r'waits (?P<waits>\d+-\d+); acquires (?P<acquires>\d+)(?P<rest>.*?); '
    r'caller (?P<caller>.+)$')


def check_host_on(name, facts, log, *, marker_off_bundle):
    """marker_off_bundle: the legacy bundle is installed too (no marker, no SkipHooks)."""
    problems = []
    lower = log.lower()

    def need(condition, message):
        if not condition:
            problems.append(message)

    installs = [l for l in log.splitlines() if 'DLSS-NR Vulkan present host (observe only; stage 1' in l]
    need(len(installs) == 1, f'expected one install line, found {len(installs)}')
    if installs:
        if marker_off_bundle:
            need('beside OptiScaler\'s own Vulkan hooks' in installs[0], 'install line does not say the bundle runs')
        else:
            need('this host is the one exception' in installs[0], 'install line does not name the skip')
    skip = 'Skipping vulkan-1.dll hooks, as [Vulkan] SkipHooks or the marker asks'
    need((skip in log) != marker_off_bundle, 'the bundle skip line does not match the case')
    if facts['late_load']:
        again = [l for l in log.splitlines() if 'vulkan-1.dll reached the hook again (the same module)' in l]
        need(len(again) == 1, f'the late load through the loader hook was not seen exactly once ({len(again)})')

    # Instance and devices.
    instance_lines = [l for l in log.splitlines() if f'DLSS-NR Vulkan present host: instance {facts["instance"]} #' in l]
    need(len(instance_lines) == 1, f'expected one line for the application\'s instance, found {len(instance_lines)}')
    instance_id = re.search(r'instance 0x[0-9A-F]+ #(\d+)', instance_lines[0]).group(1) if instance_lines else None
    # With the bundle installed after this host on the same exports, the bundle's hook runs first and calls down here.
    creator = ('dxgi.dll (OptiScaler\'s own Vulkan hook, called down to this one)' if marker_off_bundle
               else f'{EXE} (native Vulkan)')
    if instance_lines:
        need(creator in instance_lines[0], f'instance creator wrong: {instance_lines[0]}')

    families = ' '.join(f'{i}:0x{f["flags"]:X}x{f["count"]}' for i, f in enumerate(facts['families']))
    device_ids = {}
    device_lines = [l for l in log.splitlines() if 'DLSS-NR Vulkan present host: device 0x' in l]
    need(len(device_lines) == len(facts['devices']),
         f'expected {len(facts["devices"])} device lines, found {len(device_lines)}')
    for device, line in zip(facts['devices'], device_lines):
        m = re.search(r'device (0x[0-9A-F]+) #(\d+) created by (.+?); physical (0x[0-9A-F]+) instance #(\d+); '
                      r'queue families (.+?); requested(.*)$', line)
        if not m:
            problems.append(f'device line unparsed: {line}')
            continue
        requested = ''.join(f' family {f} x{c}' for f, c in device['requested'])
        need(m.group(1) == device['handle'], f'{device["name"]}: handle {m.group(1)} != {device["handle"]}')
        need(m.group(3) == creator, f'{device["name"]}: creator {m.group(3)!r}')
        need(m.group(4) == facts['physical'], f'{device["name"]}: physical {m.group(4)} != {facts["physical"]}')
        need(m.group(5) == instance_id, f'{device["name"]}: instance #{m.group(5)} != #{instance_id}')
        need(m.group(6) == families, f'{device["name"]}: families {m.group(6)!r} != {families!r}')
        need(m.group(7) == requested, f'{device["name"]}: requested {m.group(7)!r} != {requested!r}')
        device_ids[device['name']] = m.group(2)
    need(len(set(device_ids.values())) == len(device_ids), f'device records alias: {device_ids}')
    if facts['third_reused_second']:
        need(device_ids.get('d2') != device_ids.get('d3'), 'd3 reused d2\'s handle and inherited its record')

    # The observe lines, window by window, against the model of the presents.
    swapchains = {s['name']: s for s in facts['swapchains']}

    def effective_usage(s):
        # With the bundle and its menu, the overlay adds TRANSFER_SRC (menu blur) before the call reaches this host,
        # which sees the create info the driver gets.
        usage = s['usage']
        if marker_off_bundle and CASES[name][4] and s['surface_usage'] & USAGE_TRANSFER_SRC:
            usage |= USAGE_TRANSFER_SRC
        return usage

    expected = expected_lines(facts, effective_usage)
    actual = {1: [], 2: []}
    for line in log.splitlines():
        if OBSERVE not in line:
            continue
        m = LINE.search(line)
        if not m:
            problems.append(f'observe line unparsed: {line}')
            continue
        window = m.group('window')
        index = 1 if window == facts['windows'][0] else 2 if window == facts['windows'][1] else None
        if index is None:
            problems.append(f'observe line for a window the application does not have: {line}')
            continue
        actual[index].append(m)
    families_flags = [f['flags'] for f in facts['families']]
    swapchain_ids = {}
    for window in (1, 2):
        need(len(actual[window]) == len(expected[window]),
             f'window {window}: {len(actual[window])} observe lines, expected {len(expected[window])} '
             f'({[e["why"] for e in expected[window]]} vs {[a.group("why") for a in actual[window]]})')
        for e, a in zip(expected[window], actual[window]):
            where = f'window {window} line "{e["why"]}"'
            for field in ('why', 'swapchain', 'device', 'queue', 'waits', 'round_trip'):
                need(a.group(field) == str(e[field]), f'{where}: {field} {a.group(field)!r} != {e[field]!r}')
            for field in ('presents', 'swapchains', 'devices', 'width', 'height', 'format', 'color_space', 'images',
                          'acquires'):
                need(int(a.group(field)) == e[field], f'{where}: {field} {a.group(field)} != {e[field]}')
            need(int(a.group('usage'), 16) == e['usage'], f'{where}: usage {a.group("usage")} != 0x{e["usage"]:X}')
            need((a.group('src'), a.group('dst'), a.group('storage')) ==
                 (yes_no(e['usage'] & USAGE_TRANSFER_SRC), yes_no(e['usage'] & USAGE_TRANSFER_DST),
                  yes_no(e['usage'] & USAGE_STORAGE)), f'{where}: usage words wrong')
            need(a.group('surface_usage') is not None and int(a.group('surface_usage'), 16) == e['surface_usage'],
                 f'{where}: surface usage {a.group("surface_usage")} != 0x{e["surface_usage"]:X}')
            need(a.group('sharing') == 'EXCLUSIVE', f'{where}: sharing {a.group("sharing")}')
            need(a.group('physical') == facts['physical'], f'{where}: physical {a.group("physical")}')
            need(a.group('instance_id') == instance_id, f'{where}: instance #{a.group("instance_id")}')
            need(a.group('device_id') == device_ids.get(e['device_name']),
                 f'{where}: device #{a.group("device_id")} is not {e["device_name"]}\'s #{device_ids.get(e["device_name"])}')
            previous = swapchain_ids.setdefault(e['swapchain_name'], a.group('swapchain_id'))
            need(previous == a.group('swapchain_id'), f'{where}: swapchain record id changed')
            family = int(a.group('family'))
            expected_family = facts['second_queue_family'] if e['queue'] == facts['second_queue'] else facts['graphics_family']
            need(family == expected_family, f'{where}: family {family} != {expected_family}')
            need(a.group('index') == '0', f'{where}: index {a.group("index")}')
            flags = families_flags[family] if family < len(families_flags) else -1
            need(int(a.group('flags'), 16) == flags, f'{where}: flags {a.group("flags")} != 0x{flags:X}')
            need((a.group('graphics'), a.group('compute'), a.group('transfer')) ==
                 (yes_no(flags & QUEUE_GRAPHICS), yes_no(flags & QUEUE_COMPUTE), yes_no(flags & QUEUE_TRANSFER)),
                 f'{where}: queue capability words wrong')
            need(a.group('rest') == '', f'{where}: unexpected extras {a.group("rest")!r}')
            need(a.group('caller') == f'{EXE} (native Vulkan)', f'{where}: caller {a.group("caller")!r}')
    need(len(set(swapchain_ids.values())) == len(swapchain_ids), f'swapchain records alias: {swapchain_ids}')
    need('never saw created' not in log, 'a present of a swapchain the host never saw created')
    need('could not detour' not in log and 'not watched' not in lower, 'a detour failed or was refused')

    # The exports: the host's are patched, the controls are not.
    for function in HOST_HOOKED:
        state = facts['prologues'][function]
        # Under Wine the export and vkGetDeviceProcAddr's answer are one winevulkan function, so the export's bytes are
        # the detour's. Should that stop holding, the export check below would prove nothing for this function.
        if function in DEVICE_LEVEL:
            need(state['same_as_device_proc'], f'{function}: vkGetDeviceProcAddr no longer returns the export')
        need(state['patched'], f'{function} was not patched')
    if not marker_off_bundle:
        for function in NEVER_HOOKED:
            need(not facts['prologues'][function]['patched'], f'{function} was patched with the bundle off')
    return problems, expected


def execute_translated(name, dll, redist):
    api, key = TRANSLATED_CASES[name]
    work = OUT / 'cases' / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy2(dll, work / 'winmm.dll')
    shutil.copy2(OUT / TRANSLATED_EXE, work / TRANSLATED_EXE)
    for library in redist:
        shutil.copy2(library, work / library.name)
    (work / 'OptiScaler.ini').write_text(ini_for(False, True, key, True))
    (work / MARKER).write_text('')
    with (OUT / f'{name}.log').open('w') as log:
        process = subprocess.run(['wine', str(work / TRANSLATED_EXE), '--' + api], cwd=work, env=translated_env(),
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=180)
    run_log = (OUT / f'{name}.log').read_text(errors='replace')
    facts = json.loads((work / 'result.json').read_text())
    if not finished(process, facts, run_log, name) or facts.get('status') != 'PASS':
        reason = next((l for l in reversed(run_log.splitlines()) if l.startswith('FAIL: ')), 'no FAIL line')
        raise RuntimeError(f'{name}: application failed (exit {process.returncode}): {reason}; '
                           f'see {OUT / (name + ".log")}')
    log_path = work / 'OptiScaler.log'
    if not log_path.exists():
        raise RuntimeError(f'{name}: OptiScaler wrote no log; did the DLL load?')
    return facts, log_path.read_text(errors='replace'), run_log


def check_translated(name, facts, log, run_log):
    """The translation layer's own Vulkan device and presents reach the host's hooks, under the marker, and the line
    names the layer."""
    api, _ = TRANSLATED_CASES[name]
    layer = TRANSLATED_CALLER[api]
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    need(log.count('DLSS-NR Vulkan present host (observe only; stage 1') == 1, 'expected one install line')
    need('this host is the one exception' in log, 'the install line does not name the skip')
    need('Skipping vulkan-1.dll hooks, as [Vulkan] SkipHooks or the marker asks' in log, 'the bundle was not skipped')
    need(layer != 'DXVK' or 'DXVK' in run_log, 'DXVK did not announce itself: is it the dxgi/d3d11 that ran?')
    # The layer's devices: the title's, and any OptiScaler made through the layer for itself under ScopedSkipVulkanHooks
    # (IdentifyGpu makes a throwaway D3D12 device to detect vkd3d-proton), which must say so and never present here.
    devices, internal = [], []
    for line in log.splitlines():
        m = re.search(r'DLSS-NR Vulkan present host: device (0x[0-9A-F]+) #(\d+) created by (\S+) \(' + re.escape(layer) +
                      r'\)', line)
        if m:
            (internal if 'OptiScaler\'s own device' in line else devices).append(m.groups())
    need(len(devices) >= 1, f'no device line for a device the title created through {layer}')
    lines = [LINE.search(l) for l in log.splitlines() if OBSERVE in l]
    need(all(lines), 'an observe line did not parse')
    mine = [m for m in lines if m and m.group('window') == facts['window']]
    need(len(mine) >= 2, f'expected the first-present and 10-present lines for window {facts["window"]}, found '
                         f'{[m.group("why") for m in mine]}')
    if mine:
        need(mine[0].group('why') == 'first present', f'first line is {mine[0].group("why")!r}')
        need(any(m.group('why') == '10 presents' for m in mine), 'no line at 10 presents: fewer presents reached the host')
        need(int(mine[-1].group('presents')) <= facts['frames'] + 1, 'more Vulkan presents than the application made')
        for m in mine:
            need(m.group('caller').endswith(f'({layer})'), f'caller {m.group("caller")!r} is not {layer}')
            need(any(m.group('device') == handle and m.group('device_id') == record for handle, record, _ in devices),
                 f'presenting device {m.group("device")} #{m.group("device_id")} is not one {layer} created')
            need(m.group('graphics') == 'yes' or m.group('compute') == 'yes', 'presenting family can run no work')
    need('never saw created' not in log, 'a present of a swapchain the host never saw created')
    summary = (f'{layer}: device(s) {", ".join(f"{h} #{r} by {m}" for h, r, m in devices)}'
               + (f' (and {len(internal)} of OptiScaler\'s own, not reported)' if internal else '') + f'; {len(mine)} lines for '
               f'the window, caller {mine[0].group("caller") if mine else "?"}, family '
               f'{mine[0].group("family") if mine else "?"} flags {mine[0].group("flags") if mine else "?"}')
    return problems, summary


def check_host_off(name, facts, log, *, marker):
    problems = []
    if HOST_TEXT in log:
        problems.append('the host wrote to the log with its key or the master off: ' +
                        next(l for l in log.splitlines() if HOST_TEXT in l))
    skip = 'Skipping vulkan-1.dll hooks, as [Vulkan] SkipHooks or the marker asks'
    if (skip in log) != marker:
        problems.append('the bundle skip line does not match the case')
    if marker and 'prologues' in facts:
        # Nothing at all is detoured: the exports are byte-for-byte what they were before OptiScaler loaded.
        patched = [f for f, state in facts['prologues'].items() if state['patched']]
        if patched:
            problems.append(f'exports patched with the bundle skipped and the host off: {patched}')
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dll', default=str(ROOT / 'x64/out/OptiScaler.dll'),
                        help='the production OptiScaler.dll to test (default: x64/out, where build-local.sh copies it)')
    parser.add_argument('--only', help='comma-separated cases')
    parser.add_argument('--skip-build', action='store_true', help='reuse the harness already in artifacts/')
    args = parser.parse_args()

    OUT.mkdir(exist_ok=True)
    (OUT / 'results.json').write_text('{"status":"RUNNING"}\n')
    dll = Path(args.dll).resolve()
    check_dll(dll)
    if not Path('/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json').exists():
        raise RuntimeError('Khronos validation layer missing; cannot claim a validated run')
    redists = sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))
    if not redists:
        raise RuntimeError('native MSVC x64 runtime not found')
    redist = sorted(redists[-1].glob('*.dll'))
    if not (args.skip_build and (OUT / EXE).exists() and (OUT / TRANSLATED_EXE).exists()):
        build_harness()

    cases = args.only.split(',') if args.only else list(CASES) + list(TRANSLATED_CASES)
    unknown = [c for c in cases if c not in CASES and c not in TRANSLATED_CASES]
    if unknown:
        raise RuntimeError(f'unknown case(s): {unknown}')
    results = {'dll': str(dll), 'dll_sha256': sha(dll), 'harness_sha256': sha(OUT / EXE), 'cases': {}}
    failures = []
    wine = proton_libraries() if any(c in TRANSLATED_CASES for c in cases) else None
    for name in [c for c in cases if c in TRANSLATED_CASES]:
        if wine is None:
            results['cases'][name] = {'status': 'SKIP', 'problems': ['no local Proton with DXVK and vkd3d-proton']}
            print(f'SKIP: {name}: no local Proton with DXVK and vkd3d-proton (set PROTON_DIR)', flush=True)
            continue
        try:
            prepare_translated_prefix(wine)
            facts, log, run_log = execute_translated(name, dll, redist)
            if TRANSLATED_CASES[name][1]:
                problems, summary = check_translated(name, facts, log, run_log)
            else:
                problems = check_host_off(name, facts, log, marker=True)
                summary = f'no host line ({TRANSLATED_CALLER[TRANSLATED_CASES[name][0]]} presented {facts["frames"]} frames)'
            results['cases'][name] = {'status': 'FAIL' if problems else 'PASS', 'problems': problems,
                                      'proton': str(wine.parents[2])}
            if problems:
                failures.append(name)
                print(f'FAIL: {name}:', file=sys.stderr)
                for problem in problems:
                    print(f'  - {problem}', file=sys.stderr)
                print(f'  see {OUT / "cases" / name / "OptiScaler.log"}', file=sys.stderr)
            else:
                print(f'PASS: {name}: {summary}', flush=True)
        except (RuntimeError, subprocess.TimeoutExpired, OSError, KeyError, ValueError) as error:
            failures.append(name)
            results['cases'][name] = {'status': 'FAIL', 'problems': [str(error)]}
            print(f'FAIL: {name}: {error}', file=sys.stderr)
    for name in [c for c in cases if c in CASES]:
        marker, skiphooks, enabled, key, menu, one_window = CASES[name]
        try:
            facts, log = execute_case(name, dll, redist)
            host_on = enabled and key
            if host_on:
                problems, expected = check_host_on(name, facts, log, marker_off_bundle=not (marker or skiphooks))
                summary = (f'{sum(len(v) for v in expected.values())} observe lines, '
                           f'{len(facts["presents"])} presents, {len(facts["devices"])} device(s)'
                           f'{" (d3 reused d2" + chr(39) + "s handle)" if facts["third_reused_second"] else ""}, '
                           f'second queue family {facts["second_queue_family"]}, '
                           f'{facts["wine_injected_extension_messages"]} Wine dma_buf messages set aside')
            else:
                problems = check_host_off(name, facts, log, marker=marker or skiphooks)
                summary = 'no host line' + (', every watched export byte-identical' if marker or skiphooks else '')
            results['cases'][name] = {'status': 'FAIL' if problems else 'PASS', 'problems': problems,
                                      'third_reused_second': facts['third_reused_second'],
                                      'compute_family': facts['compute_family']}
            if problems:
                failures.append(name)
                print(f'FAIL: {name}:', file=sys.stderr)
                for problem in problems:
                    print(f'  - {problem}', file=sys.stderr)
                print(f'  see {OUT / "cases" / name / "OptiScaler.log"}', file=sys.stderr)
            else:
                print(f'PASS: {name}: {summary}', flush=True)
        except (RuntimeError, subprocess.TimeoutExpired, OSError, KeyError, ValueError) as error:
            failures.append(name)
            results['cases'][name] = {'status': 'FAIL', 'problems': [str(error)]}
            print(f'FAIL: {name}: {error}', file=sys.stderr)

    results['status'] = 'FAIL' if failures else 'PASS'
    (OUT / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    if failures:
        raise RuntimeError(f'{len(failures)} of {len(cases)} cases failed: {", ".join(failures)}')
    print(f'PASS: all {len(cases)} cases')


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired, OSError) as error:
        if OUT.exists():
            current = OUT / 'results.json'
            try:
                data = json.loads(current.read_text())
            except (OSError, ValueError):
                data = {}
            data.update(status='FAIL', reason=str(error))
            current.write_text(json.dumps(data, indent=2) + '\n')
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
