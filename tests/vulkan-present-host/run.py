#!/usr/bin/env python3
"""The Vulkan present host under Wine (docs/dlssnr/design/nr-vulkan-present-host.md, stages 1 to 3): the production
DLL, uninstrumented, loaded by a Win32 Vulkan application, with [DlssNr] VulkanPresentHost on and off, the skip marker
present and absent, the Vulkan menu on and off, and a surface that hides TRANSFER_SRC. Every line of the host in
OptiScaler.log is held against the facts the application wrote about itself -- which presents the round trip must have
carried, by which route, with how many waits and submits, and what it must have refused -- under the validation
layer's core and synchronization checks, and every presented image the application reads back must equal what it
made. With the key off, nothing of the host may appear and vulkan-1.dll's exports must be byte-for-byte untouched.

Stage 3's cases (nr*) run nr.cpp with the real model: the forwarder built with this DLL, NVIDIA's nvngx_dlssnr.dll
from the local kit (symlinked, never copied into the tree) and the driver's NGX core from /usr/lib/nvidia/wine, in a
prefix set up as Proton sets one up for NGX. Without those files they print SKIP. One of them, nr-route-quiet, is a game
that turns its upscaler on and off: the after-upscale route takes the model from the present host, goes quiet, and the
present host takes it back, twice."""
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
NR_EXE = 'vk-present-host-nr.exe'
MARKER = 'optiscaler_skip_vulkan_hooks'
WINDOW_LINE = 'DLSS-NR Vulkan present host (cold NR): window '
INSTALL_LINE = 'DLSS-NR Vulkan present host (cold NR on zero guides; stage 3'
HOST_TEXT = 'DLSS-NR Vulkan present host'
HIDE_LAYER = 'VK_LAYER_RETOQUE_hide_transfer_src'
LAYER_DIR = OUT / 'layer'

# Stage 3: the model's device extensions, as the host names them, and what a case without the model files says.
NR_EXTENSIONS = ['VK_NVX_binary_import', 'VK_NVX_image_view_handle', 'VK_KHR_push_descriptor',
                 'VK_KHR_buffer_device_address']
# The harness's devices are Vulkan 1.1 without shaderStorageImageWriteWithoutFormat: the composition pass's format-less
# storage writes need this one too.
FORMAT_FLAGS2 = 'VK_KHR_format_feature_flags2'
NO_FORWARDER = 'nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game'

# What the host detours: stage 1's creation, lifetime and present calls, and stage 2's queue calls (vkQueueSubmit,
# vkQueueWaitIdle), whose lock keeps a borrowed queue externally synchronised. vkCreateFence is the control: never.
HOST_HOOKED = ['vkCreateInstance', 'vkDestroyInstance', 'vkEnumeratePhysicalDevices', 'vkCreateDevice',
               'vkCreateWin32SurfaceKHR', 'vkDestroySurfaceKHR', 'vkDestroyDevice', 'vkGetDeviceQueue',
               'vkGetDeviceQueue2', 'vkCreateSwapchainKHR', 'vkDestroySwapchainKHR', 'vkGetSwapchainImagesKHR',
               'vkAcquireNextImageKHR', 'vkQueuePresentKHR', 'vkQueueSubmit', 'vkQueueWaitIdle']
NEVER_HOOKED = ['vkCreateFence']
DEVICE_LEVEL = {'vkDestroyDevice', 'vkGetDeviceQueue', 'vkGetDeviceQueue2', 'vkCreateSwapchainKHR',
                'vkDestroySwapchainKHR', 'vkGetSwapchainImagesKHR', 'vkAcquireNextImageKHR', 'vkQueuePresentKHR',
                'vkQueueSubmit', 'vkQueueWaitIdle'}

# The host's constants (retoque/dlssnr/DlssNr_VkPresentTransport.h): ring slots per swapchain, the bounded wait for
# the oldest one.
RING_MAX = 4
SLOT_WAIT_MS = 100.0

# name: (marker file, [Vulkan] SkipHooks, [DlssNr] Enabled, VulkanPresentHost, OverlayMenu, one window only,
#        surfaces hide TRANSFER_SRC)
#
# menu-on and key-off run the legacy bundle with its Vulkan menu. That overlay dies (a null read inside
# MenuOverlayVk::CreateSwapchain) when a second window's swapchain is created on a second device, with this host off
# as well, so they stop after window 1 (--one-window). See README.md.
CASES = {
    'marker': (True, False, True, True, True, False, False),
    'skiphooks': (False, True, True, True, True, False, False),
    'surface-no-src': (True, False, True, True, True, False, True),
    'marker-key-off': (True, False, True, False, True, False, False),
    'marker-master-off': (True, False, False, True, True, False, False),
    'menu-off': (False, False, True, True, False, False, False),
    'menu-on': (False, False, True, True, True, True, False),
    'key-off': (False, False, True, False, True, True, False),
}

# A D3D title under Proton's translation layers (translated.cpp), OptiScaler as winmm.dll: name: (API,
# VulkanPresentHost, marker). DXVK and vkd3d-proton come from a local Proton build. The key-off cases are the
# validation baseline: whatever the layers themselves trip is not the host's. The marker is present as deployed for
# such titles, except in the dxvk-bundle cases, which run the whole bundle beside the host (its menu on, LogLevel 0) to
# hold that the bundle still takes a DXVK present for a DXGI-backed one with the host's detour above its own.
TRANSLATED_CASES = {
    'dxvk': ('d3d11', True, True),
    'vkd3d-proton': ('d3d12', True, True),
    'dxvk-bundle': ('d3d11', True, False),
    'dxvk-key-off': ('d3d11', False, True),
    'vkd3d-proton-key-off': ('d3d12', False, True),
    'dxvk-bundle-key-off': ('d3d11', False, False),
}
TRANSLATED_CALLER = {'d3d11': 'DXVK', 'd3d12': 'vkd3d-proton'}
# Whether the validation layer runs under the layer. GE-Proton11-7's vkd3d-proton aborts (exit 3) in its device
# initialisation under the Khronos layer 1.4.363, with core validation alone and with OptiScaler's host off as well,
# so the D3D12 cases run without it, as stage 1's did; their transport is checked by the lines alone.
TRANSLATED_VALIDATION = {'d3d11': True, 'd3d12': False}
PROTON_LIBRARIES = {'dxvk': ['dxgi.dll', 'd3d11.dll', 'd3d10core.dll'], 'vkd3d-proton': ['d3d12.dll', 'd3d12core.dll']}

USAGE_TRANSFER_SRC, USAGE_TRANSFER_DST, USAGE_STORAGE = 0x1, 0x2, 0x8
QUEUE_GRAPHICS, QUEUE_COMPUTE, QUEUE_TRANSFER = 0x1, 0x2, 0x4
SKIP_ORDER = ['observe only', 'device off', 'multi-swapchain present', 'image unknown', 'no acquire seen',
              'too many waits', 'no route', 'work queue changed', 'ring busy', 'queue busy', 'setup failed',
              'record failed', 'submit failed']
ROUTE_ORDER = ['same queue', 'cross-family', 'concurrent']


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
    if b'(cold NR): window ' not in data:
        raise RuntimeError(f'{dll} has no stage-3 present host in it (stale build); run ./build-local.sh Release')
    # A source newer than the DLL means the DLL is not what the tree says.
    sources = [ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp', ROOT / 'OptiScaler/Config.h', ROOT / 'OptiScaler/Config.cpp',
               ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp', ROOT / 'retoque/dlssnr/DlssNr_VkModelOwner.h',
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
        run([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/O2',
             '/I' + str(ROOT / 'external/vulkan/include'), '/I' + str(ROOT / 'external/nvngx_dlss_sdk'), HERE / 'nr.cpp',
             '/Fo' + str(OUT / 'nr.obj'), '/Fe' + str(OUT / NR_EXE),
             '/link', '/LIBPATH:' + str(ROOT / 'OptiScaler/library/vulkan'), 'vulkan-1.lib', 'user32.lib'],
            env=env, logfile=OUT / 'build-nr.log', timeout=600)
    finally:
        # Leave no server running on the disposable compiler prefix; never touch any other prefix.
        subprocess.run(['wineserver', '-k'], env=env, stdin=subprocess.DEVNULL, capture_output=True)


def build_layer():
    """The native test layer that hides TRANSFER_SRC from surfaces, and its manifest, in artifacts/layer/."""
    LAYER_DIR.mkdir(parents=True, exist_ok=True)
    library = LAYER_DIR / 'libVkLayer_retoque_hide_transfer_src.so'
    run(['g++', '-std=c++20', '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror',
         '-I', ROOT / 'external/vulkan/include', HERE / 'hide_transfer_src.cpp', '-o', library],
        logfile=OUT / 'build-layer.log', timeout=300)
    (LAYER_DIR / 'VkLayer_retoque_hide_transfer_src.json').write_text(json.dumps({
        'file_format_version': '1.1.2',
        'layer': {'name': HIDE_LAYER, 'type': 'INSTANCE', 'library_path': str(library), 'api_version': '1.4.309',
                  'implementation_version': '1',
                  'description': 'tests/vulkan-present-host: hides TRANSFER_SRC from surface capabilities'}}) + '\n')


def runtime_env(hide=False):
    # The validation layer first (nearest the application), the hiding layer after it (nearest the driver).
    layers = 'VK_LAYER_KHRONOS_validation' + (f':{HIDE_LAYER}' if hide else '')
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='dxgi=n,b;vcruntime140=n;vcruntime140_1=n;msvcp140=n',
               VK_INSTANCE_LAYERS=layers,
               VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT',
               VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0')
    if hide:
        env['VK_ADD_LAYER_PATH'] = str(LAYER_DIR)
    return env


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


def translated_env(validation=False):
    # As Proton runs a D3D title: DXVK and vkd3d-proton are the system's dxgi/d3d11/d3d12, native first. With
    # `validation`, the validation layer (core and synchronization) runs under them; with no messenger of the
    # application's, it writes to stdout, which the run log holds.
    env = dict(os.environ, WINEPREFIX=str(OUT / 'wineprefix-translated'), WINEDEBUG='-all',
               WINEDLLOVERRIDES='winmm=n,b;dxgi,d3d11,d3d10core,d3d12,d3d12core=n;'
                                'vcruntime140=n;vcruntime140_1=n;msvcp140=n',
               VK_LOADER_LAYERS_DISABLE='~implicit~', ENABLE_GAMESCOPE_WSI='0', DXVK_LOG_LEVEL='info')
    if validation:
        env.update(VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation',
                   VK_LAYER_ENABLES='VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT')
    return env


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


def ini_for(skiphooks, enabled, key, menu, level=1):
    text = f'[Menu]\nOverlayMenu={str(menu).lower()}\nShortcutKey=0x2D\n'
    text += f'[DlssNr]\nEnabled={str(enabled).lower()}\nVulkanPresentHost={str(key).lower()}\n'
    if skiphooks:
        text += '[Vulkan]\nSkipHooks=true\n'
    text += f'[Log]\nLogToFile=true\nLogLevel={level}\n'
    return text


def validation_ids(run_log):
    return sorted(set(re.findall(r'\b(?:VUID|SYNC-HAZARD)-[\w-]+', run_log)) - {'VUID-VkFenceCreateInfo-flags-parameter'})


def execute_case(name, dll, redist):
    marker, skiphooks, enabled, key, menu, one_window, hide = CASES[name]
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
    args = []
    if one_window:
        args.append('--one-window')
    if menu and not (marker or skiphooks):
        # The legacy Vulkan overlay draws its splash into the presented images (host on or off): the read-backs are
        # counted, not compared. Its present hook then consumes the host's semaphore, which is the chain to validate.
        args.append('--overlay-draws')
    if hide:
        args.append('--no-transfer-src')
    elif enabled and key:
        args.append('--host-adds-src')
    with (OUT / f'{name}.log').open('w') as log:
        process = subprocess.run(['wine', str(work / EXE)] + args, cwd=work, env=runtime_env(hide),
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    run_log = (OUT / f'{name}.log').read_text(errors='replace')
    facts = json.loads((work / 'result.json').read_text())
    if not finished(process, facts, run_log, name) or facts.get('status') != 'PASS':
        reason = next((l for l in reversed(run_log.splitlines()) if l.startswith('FAIL: ')), 'no FAIL line')
        ids = validation_ids(run_log)
        raise RuntimeError(f'{name}: harness failed (exit {process.returncode}): {reason}'
                           + (f'; validation ids: {", ".join(ids)}' if ids else '') + f'; see {OUT / (name + ".log")}')
    if facts.get('validation_active') is not True or facts.get('validation_errors') != 0:
        raise RuntimeError(f'{name}: validation was not proved active and clean')
    if facts.get('readback_mismatches') != 0:
        raise RuntimeError(f'{name}: {facts.get("readback_mismatches")} read-backs differ from what was presented')
    log_path = work / 'OptiScaler.log'
    if not log_path.exists():
        raise RuntimeError(f'{name}: OptiScaler wrote no log; did the DLL load?')
    return facts, log_path.read_text(errors='replace')


# --- What the log must say ------------------------------------------------------------------------------------

def yes_no(flag):
    return 'yes' if flag else 'no'


def usage_names(bits):
    return '+'.join(n for bit, n in ((USAGE_TRANSFER_SRC, 'TRANSFER_SRC'), (USAGE_TRANSFER_DST, 'TRANSFER_DST'))
                    if bits & bit)


def new_stats():
    return dict(presents=0, transported=0, skipped=0, skips={}, waits=0, submits=0, completions=0, ring=0,
                max_wait_ms=0.0, routes={}, nr_refused=0, nr_skips={}, nr_first=None)


def add(stats, outcome):
    stats['presents'] += 1
    stats['waits'] += outcome['waits']
    stats['submits'] += outcome['submits']
    stats['completions'] += outcome['completions']
    stats['ring'] = max(stats['ring'], outcome['ring'])
    stats['max_wait_ms'] = max(stats['max_wait_ms'], outcome['wait_ms'])
    if outcome['transported']:
        stats['transported'] += 1
        stats['routes'][outcome['route']] = stats['routes'].get(outcome['route'], 0) + 1
        # Without the model's files every carried present is refused by the model, for that reason.
        stats['nr_refused'] += 1
        stats['nr_skips']['model unavailable'] = stats['nr_skips'].get('model unavailable', 0) + 1
        stats['nr_first'] = stats['nr_first'] or NO_FORWARDER
    else:
        stats['skipped'] += 1
        stats['skips'][outcome['skip']] = stats['skips'].get(outcome['skip'], 0) + 1


def model(facts, plan_of):
    """Every present's outcome and every window's lines, from the application's own record. `plan_of(swapchain)` gives
    (given usage, final usage, transport on, round-trip words)."""
    swapchains = {s['name']: s for s in facts['swapchains']}
    families = [f['flags'] for f in facts['families']]
    windows, acquires, lines = {}, {}, {1: [], 2: []}
    pending, per_swapchain = {}, {}
    for p in facts['presents']:
        s = swapchains[p['swapchain_name']]
        _, usage, transport, round_trip = plan_of(s)
        w = windows.setdefault(p['window'], dict(presents=0, swapchains=[], devices=[], next=10, last=None,
                                                 wmin=None, wmax=0, stats=new_stats(), previous=None))
        stats = per_swapchain.setdefault(s['name'], new_stats())

        # The swapchain this window presented before is gone by now (the application destroyed it before presenting
        # the next one): its last round trips were observed complete when it was retired.
        if w['previous'] is not None and w['previous'] != s['name'] and pending.get(w['previous']):
            w['stats']['completions'] += pending[w['previous']]
            per_swapchain[w['previous']]['completions'] += pending[w['previous']]
            pending[w['previous']] = 0
        w['previous'] = s['name']

        outcome = dict(transported=False, skip=None, route=None, waits=0, submits=0, completions=0, ring=0,
                       wait_ms=0.0)
        # Every earlier round trip of this swapchain is complete when the application had drained its own work.
        if p['gpu_idle']:
            outcome['completions'] = pending.get(s['name'], 0)
            pending[s['name']] = 0
        if not transport:
            outcome['skip'] = 'observe only'
        elif p['held_back'] >= RING_MAX:
            outcome.update(skip='ring busy', ring=RING_MAX, wait_ms=SLOT_WAIT_MS)
        else:
            graphics = families[p['family']] & QUEUE_GRAPHICS and families[p['family']] & QUEUE_COMPUTE
            outcome.update(transported=True, route='same queue' if graphics else 'cross-family', waits=p['waits'],
                           submits=1 if graphics else 3,
                           ring=p['held_back'] + 1 if p['held_back'] >= 0 else max(1, stats['ring']))
            pending[s['name']] = pending.get(s['name'], 0) + 1
        add(w['stats'], outcome)
        add(stats, outcome)
        p['outcome'] = outcome

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
        if w['presents'] == w['next']:
            w['next'] *= 10
            if not why:
                why = [f'{w["presents"]} presents']
        if not why:
            continue
        lines[p['window']].append(dict(
            why=', '.join(why), presents=w['presents'], swapchains=len(w['swapchains']), devices=len(w['devices']),
            swapchain=s['handle'], swapchain_name=s['name'], width=s['width'], height=s['height'], format=s['format'],
            color_space=s['color_space'], images=s['images'], usage=usage, given=plan_of(s)[0],
            surface_usage=s['surface_usage'], round_trip=round_trip, device=p['device'], device_name=p['device_name'],
            queue=p['queue'], waits=f'{w["wmin"]}-{w["wmax"]}', acquires=acquires[p['swapchain_name']],
            stats=json.loads(json.dumps(w['stats']))))
        w['last'] = now
        w['wmin'], w['wmax'] = None, 0
    # Retirement of what is left: every swapchain the application destroyed has all its round trips observed.
    for name, count in pending.items():
        if count and swapchains[name]['destroyed']:
            per_swapchain[name]['completions'] += count
    return lines, per_swapchain


LINE = re.compile(
    r'DLSS-NR Vulkan present host \(cold NR\): window (?P<window>0x[0-9A-F]+|unknown) \((?P<why>[^)]*)\); '
    r'presents (?P<presents>\d+), swapchains (?P<swapchains>\d+), devices (?P<devices>\d+); '
    r'swapchain (?P<swapchain>0x[0-9A-F]+) #(?P<swapchain_id>\d+) (?P<width>\d+)x(?P<height>\d+) '
    r'format (?P<format>\d+) colour-space (?P<color_space>\d+) images (?P<images>\d+) usage (?P<usage>0x[0-9A-F]+) '
    r'\(transfer-src (?P<src>yes|no), transfer-dst (?P<dst>yes|no), storage (?P<storage>yes|no)\)'
    r'(?: asked (?P<asked>0x[0-9A-F]+))?'
    r'(?: surface-usage (?P<surface_usage>0x[0-9A-F]+))? sharing (?P<sharing>EXCLUSIVE|CONCURRENT\[[\d,]*\])'
    r'(?P<retired> retired)?; round-trip (?P<round_trip>[^;]+); '
    r'device (?P<device>0x[0-9A-F]+) #(?P<device_id>\d+) physical (?P<physical>0x[0-9A-F]+) '
    r'instance #(?P<instance_id>\d+); '
    r'queue (?P<queue>0x[0-9A-F]+) family (?P<family>\d+) index (?P<index>\d+) flags (?P<flags>0x[0-9A-F]+) '
    r'\(graphics (?P<graphics>yes|no), compute (?P<compute>yes|no), transfer (?P<transfer>yes|no)\); '
    r'waits (?P<waits>\d+-\d+); acquires (?P<acquires>\d+)(?P<rest>.*?); '
    r'caller (?P<caller>.+?); transport: (?P<transport>.+?); (?P<nr>NR: .+)$')

TRANSPORT = re.compile(
    r'transported (?P<transported>\d+), skipped (?P<skipped>\d+)(?: \((?P<skips>[^)]*)\))?, '
    r'waits taken over (?P<waits>\d+), submits (?P<submits>\d+), completions (?P<completions>\d+), '
    r'ring (?P<ring>\d+), max CPU wait (?P<wait>[\d.]+) ms(?:, routes (?P<routes>[^;]+))?'
    r'(?:; first no-route reason: (?P<noroute>.+?))?'
    r'(?:; (?P<resources>.+))?$')


NR = re.compile(
    r'NR: evaluated (?P<evaluated>\d+), refused (?P<refused>\d+)(?: \((?P<skips>[^)]*)\))?, history resets '
    r'(?P<resets>\d+), pass GPU (?:not measured|last (?P<last>[\d.]+) ms, mean (?P<mean>[\d.]+) ms, max '
    r'(?P<max>[\d.]+) ms)(?:; first refusal: (?P<first>.+))?$')


def nr_building_problems(where, text, presents, first_at_size):
    """A swapchain the model ran on, built off the present path ("Off the present path" in the design note): the first
    swapchain at a size carries its first frames unchanged while the model is built (`model building`, at least one),
    every later one at a size already built none; everything else evaluated. Returns (problems, building)."""
    m = NR.match(text)
    if not m:
        return [f'{where}: NR text unparsed: {text!r}'], 0
    problems = []
    skips = counts(m.group('skips'))
    building = skips.get('model building', 0)
    if set(skips) - {'model building'}:
        problems.append(f'{where}: NR refusals other than building: {skips}')
    if int(m.group('refused')) != building or int(m.group('evaluated')) + building != presents:
        problems.append(f'{where}: NR evaluated {m.group("evaluated")} + building {building} != {presents} presents')
    if first_at_size and not 1 <= building < presents:
        problems.append(f'{where}: the first swapchain at its size carried {building} frames while the model was built')
    if not first_at_size and building != 0:
        problems.append(f'{where}: {building} frames carried for a build at a size already built')
    if building and not (m.group('first') or '').startswith('the model is being built for '):
        problems.append(f'{where}: NR first refusal {m.group("first")!r}')
    if m.group('last') is None:
        problems.append(f'{where}: no pass GPU time was measured')
    if int(m.group('resets')) < 1:
        problems.append(f'{where}: NR history resets {m.group("resets")}')
    return problems, building


def nr_problems(where, text, evaluated, refused, skips, first=None, resets=None, timed=None):
    """`text` is a line's "NR: ..." part."""
    m = NR.match(text)
    if not m:
        return [f'{where}: NR text unparsed: {text!r}']
    problems = []
    if int(m.group('evaluated')) != evaluated:
        problems.append(f'{where}: NR evaluated {m.group("evaluated")} != {evaluated}')
    if int(m.group('refused')) != refused:
        problems.append(f'{where}: NR refused {m.group("refused")} != {refused}')
    got = counts(m.group('skips'))
    got_first = m.group('first')
    # Off the present path ("Off the present path" in the design note): a model that cannot come up on a device says so
    # once its build is over; the frames before that are refused as building, a few per device at most.
    if 'model unavailable' in skips and 'model building' in got:
        building = got.pop('model building')
        got['model unavailable'] = got.get('model unavailable', 0) + building
        if building > 3:
            problems.append(f'{where}: {building} frames refused as building before the model was found unavailable')
        if (got_first or '').startswith('the model is being built for '):
            got_first = first
    if got != skips:
        problems.append(f'{where}: NR refusals {m.group("skips")!r} != {skips}')
    if first is not None and got_first != first:
        problems.append(f'{where}: NR first refusal {m.group("first")!r} != {first!r}')
    if first is None and refused == 0 and m.group('first') is not None:
        problems.append(f'{where}: an NR refusal where none was expected: {m.group("first")!r}')
    if resets is not None and not resets(int(m.group('resets'))):
        problems.append(f'{where}: NR history resets {m.group("resets")}')
    if timed is True and m.group('last') is None:
        problems.append(f'{where}: no pass GPU time was measured')
    return problems


def counts(text):
    out = {}
    for part in (text or '').split(', '):
        if part:
            name, _, count = part.rpartition(' ')
            out[name] = int(count)
    return out


def transport_problems(where, text, expected):
    """`text` is the transport part of a line; `expected` a model stats dict."""
    m = TRANSPORT.match(text)
    if not m:
        return [f'{where}: transport text unparsed: {text!r}']
    problems = []
    for field, key in (('transported', 'transported'), ('skipped', 'skipped'), ('waits', 'waits'),
                       ('submits', 'submits'), ('completions', 'completions'), ('ring', 'ring')):
        if int(m.group(field)) != expected[key]:
            problems.append(f'{where}: {field} {m.group(field)} != {expected[key]}')
    if counts(m.group('skips')) != expected['skips']:
        problems.append(f'{where}: skip reasons {m.group("skips")!r} != {expected["skips"]}')
    if counts(m.group('routes')) != expected['routes']:
        problems.append(f'{where}: routes {m.group("routes")!r} != {expected["routes"]}')
    wait = float(m.group('wait'))
    if expected['max_wait_ms'] == 0.0:
        if wait > 5.0:
            problems.append(f'{where}: a CPU wait of {wait} ms where the GPU was always done')
    elif not expected['max_wait_ms'] * 0.95 <= wait <= expected['max_wait_ms'] * 5:
        problems.append(f'{where}: max CPU wait {wait} ms, expected about {expected["max_wait_ms"]} ms')
    return problems


def check_host_on(name, facts, log, *, marker_off_bundle):
    """marker_off_bundle: the legacy bundle is installed too (no marker, no SkipHooks)."""
    problems = []
    hide = CASES[name][6]
    lower = log.lower()

    def need(condition, message):
        if not condition:
            problems.append(message)

    installs = [l for l in log.splitlines() if INSTALL_LINE in l]
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
    device_lines = [l for l in log.splitlines() if re.search(r'DLSS-NR Vulkan present host: device 0x[0-9A-F]+ #\d+ '
                                                             r'created by', l)]
    need(len(device_lines) == len(facts['devices']),
         f'expected {len(facts["devices"])} device lines, found {len(device_lines)}')
    for device, line in zip(facts['devices'], device_lines):
        m = re.search(r'device (0x[0-9A-F]+) #(\d+) created by (.+?); physical (0x[0-9A-F]+) instance #(\d+); '
                      r'queue families (.+?); requested(.*?); NR extensions: added (.+?); already enabled (.+?); not '
                      r'possible (.+)$', line)
        if not m:
            problems.append(f'device line unparsed: {line}')
            continue
        # The model's extensions: the host adds them under the marker; beside the bundle, the bundle's own device hook
        # (NR on) has added them before the call reaches the host.
        added, already = ', '.join(NR_EXTENSIONS + [FORMAT_FLAGS2]), 'none'
        if marker_off_bundle:
            added, already = FORMAT_FLAGS2, ', '.join(NR_EXTENSIONS)
        need((m.group(8), m.group(9), m.group(10)) == (added, already, 'none; bufferDeviceAddress enabled by the host'),
             f'{device["name"]}: NR extensions added {m.group(8)!r}, already {m.group(9)!r}, not possible '
             f'{m.group(10)!r}')
        requested = ''.join(f' family {f} x{c}' for f, c in device['requested'])
        need(m.group(1) == device['handle'], f'{device["name"]}: handle {m.group(1)} != {device["handle"]}')
        need(m.group(3) == creator, f'{device["name"]}: creator {m.group(3)!r}')
        need(m.group(4) == facts['physical'], f'{device["name"]}: physical {m.group(4)} != {facts["physical"]}')
        need(m.group(5) == instance_id, f'{device["name"]}: instance #{m.group(5)} != #{instance_id}')
        need(m.group(6) == families, f'{device["name"]}: families {m.group(6)!r} != {families!r}')
        need(m.group(7) == requested, f'{device["name"]}: requested {m.group(7)!r} != {requested!r}')
        device_ids[device['name']] = m.group(2)
        if device['destroyed']:
            gone = [l for l in log.splitlines() if f'DLSS-NR Vulkan present host: device {device["handle"]} '
                                                   f'#{m.group(2)} destroyed; ' in l]
            need(len(gone) == 1, f'{device["name"]}: expected one destruction line, found {len(gone)}')
            if gone:
                # Stage 3: the device's model (its forwarder missing here, so it never ran) is freed with it -- there is
                # one only where the round trip carried a present; and a destroyed swapchain's present semaphores went
                # with it, so none is left to retain.
                freed = '' if hide else '; the model freed'
                need(re.search(r'destroyed; freed: 0 swapchain\(s\) the application left, 0 retained present '
                               r'semaphore\(s\), 0 slot\(s\) kept from earlier swapchains' + re.escape(freed) + '$',
                               gone[0]) is not None, f'{device["name"]}: destruction line: {gone[0]}')
    need(len(set(device_ids.values())) == len(device_ids), f'device records alias: {device_ids}')
    if facts['third_reused_second']:
        need(device_ids.get('d2') != device_ids.get('d3'), 'd3 reused d2\'s handle and inherited its record')

    # The usage each swapchain was given, and what the round trip says about it.
    def plan_of(s):
        given = s['usage']
        # With the bundle and its menu, the overlay adds TRANSFER_SRC (menu blur) before the call reaches this host.
        if marker_off_bundle and CASES[name][4] and s['surface_usage'] & USAGE_TRANSFER_SRC:
            given |= USAGE_TRANSFER_SRC
        wanted = given | USAGE_TRANSFER_SRC | USAGE_TRANSFER_DST
        missing = wanted & ~given
        if missing & ~s['surface_usage']:
            return given, given, False, f'off, the surface does not offer {usage_names(missing & ~s["surface_usage"])}'
        return given, wanted, True, ('on as created' if not missing else f'on, {usage_names(missing)} added at creation')

    # One creation line per swapchain.
    swapchain_ids = {}
    for s in facts['swapchains']:
        given, usage, _, round_trip = plan_of(s)
        created = [l for l in log.splitlines() if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #' in l and
                   ' on device ' in l]
        need(len(created) == 1, f'{s["name"]}: expected one creation line, found {len(created)}')
        if created:
            m = re.search(r'swapchain (0x[0-9A-F]+) #(\d+) on device (0x[0-9A-F]+) #(\d+), window (0x[0-9A-F]+), '
                          r'(\d+)x(\d+) format (\d+): usage (0x[0-9A-F]+)(?: -> (0x[0-9A-F]+))? \(surface offers '
                          r'(0x[0-9A-F]+)\); round trip (.+?)(?:; the model (may run on it \(.+\)|will not run on it: '
                          r'.+))?$', created[0])
            if not m:
                problems.append(f'{s["name"]}: creation line unparsed: {created[0]}')
                continue
            swapchain_ids[s['name']] = m.group(2)
            # By the presents, not the handle: d3 may have d2's handle value.
            device_name = next(p['device_name'] for p in facts['presents'] if p['swapchain_name'] == s['name'])
            need(m.group(4) == device_ids.get(device_name), f'{s["name"]}: device #{m.group(4)}')
            need(m.group(5) == facts['windows'][s['window'] - 1], f'{s["name"]}: window {m.group(5)}')
            need((int(m.group(6)), int(m.group(7))) == (s['width'], s['height']), f'{s["name"]}: extent')
            need(int(m.group(9), 16) == given, f'{s["name"]}: asked {m.group(9)} != 0x{given:X}')
            need(int(m.group(10) or m.group(9), 16) == usage, f'{s["name"]}: given {m.group(10)} != 0x{usage:X}')
            need((m.group(10) is None) == (given == usage), f'{s["name"]}: "->" shown wrongly')
            need(int(m.group(11), 16) == s['surface_usage'], f'{s["name"]}: surface {m.group(11)}')
            need(m.group(12) == round_trip, f'{s["name"]}: round trip {m.group(12)!r} != {round_trip!r}')
            if round_trip.startswith('on'):
                need(m.group(13) == 'may run on it (B8G8R8A8_UNORM, sRGB non-linear)',
                     f'{s["name"]}: the model\'s word on it {m.group(13)!r}')

    # The window lines, window by window, against the model of the presents.
    expected, per_swapchain = model(facts, plan_of)
    actual = {1: [], 2: []}
    for line in log.splitlines():
        if WINDOW_LINE not in line:
            continue
        m = LINE.search(line)
        if not m:
            problems.append(f'window line unparsed: {line}')
            continue
        window = m.group('window')
        index = 1 if window == facts['windows'][0] else 2 if window == facts['windows'][1] else None
        if index is None:
            problems.append(f'window line for a window the application does not have: {line}')
            continue
        actual[index].append(m)
    families_flags = [f['flags'] for f in facts['families']]
    for window in (1, 2):
        need(len(actual[window]) == len(expected[window]),
             f'window {window}: {len(actual[window])} lines, expected {len(expected[window])} '
             f'({[e["why"] for e in expected[window]]} vs {[a.group("why") for a in actual[window]]})')
        for e, a in zip(expected[window], actual[window]):
            where = f'window {window} line "{e["why"]}"'
            for field in ('why', 'swapchain', 'device', 'queue', 'waits', 'round_trip'):
                need(a.group(field) == str(e[field]), f'{where}: {field} {a.group(field)!r} != {e[field]!r}')
            for field in ('presents', 'swapchains', 'devices', 'width', 'height', 'format', 'color_space', 'images',
                          'acquires'):
                need(int(a.group(field)) == e[field], f'{where}: {field} {a.group(field)} != {e[field]}')
            need(int(a.group('usage'), 16) == e['usage'], f'{where}: usage {a.group("usage")} != 0x{e["usage"]:X}')
            need((a.group('asked') and int(a.group('asked'), 16)) == (e['given'] if e['given'] != e['usage'] else None),
                 f'{where}: asked {a.group("asked")} (given 0x{e["given"]:X})')
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
            need(a.group('swapchain_id') == swapchain_ids.get(e['swapchain_name']),
                 f'{where}: swapchain record #{a.group("swapchain_id")} is not {e["swapchain_name"]}\'s')
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
            problems.extend(transport_problems(where, a.group('transport'), e['stats']))
            problems.extend(nr_problems(where, a.group('nr'), 0, e['stats']['nr_refused'], e['stats']['nr_skips'],
                                        e['stats']['nr_first'] if e['stats']['nr_refused'] else None))
    need(len(set(swapchain_ids.values())) == len(swapchain_ids), f'swapchain records alias: {swapchain_ids}')
    need('never saw created' not in log, 'a present of a swapchain the host never saw created')
    need('could not detour' not in log and 'not watched' not in lower, 'a detour failed or was refused')
    need('round trip off for it' not in log and 'submit returned' not in log, 'a round trip failed at run time')

    # The last line of every swapchain the application destroyed: its presents, all of its round trips seen complete,
    # its objects freed.
    for s in facts['swapchains']:
        if not s['destroyed'] or s['name'] not in per_swapchain:
            continue
        expect = per_swapchain[s['name']]
        gone = [l for l in log.splitlines() if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #'
                f'{swapchain_ids.get(s["name"])} destroyed after ' in l]
        need(len(gone) == 1, f'{s["name"]}: expected one destruction line, found {len(gone)}')
        if not gone:
            continue
        m = re.search(r'destroyed after (\d+) presents; (.+)$', gone[0])
        need(m is not None and int(m.group(1)) == expect['presents'], f'{s["name"]}: destruction line: {gone[0]}')
        if m:
            problems.extend(transport_problems(f'{s["name"]} destruction', m.group(2), expect))
            nr_text = gone[0][gone[0].index('; NR: ') + 2:] if '; NR: ' in gone[0] else ''
            problems.extend(nr_problems(f'{s["name"]} destruction', nr_text, 0, expect['nr_refused'],
                                        expect['nr_skips'], expect['nr_first'] if expect['nr_refused'] else None))
            if expect['transported']:
                need(expect['completions'] == expect['transported'], f'{s["name"]}: the model left round trips unseen')
            need(re.search(r'; freed after a drain of [\d.]+ ms; present semaphores \d+ freed with it, 0 kept until '
                           r'the device goes', gone[0]) is not None or not expect['transported'],
                 f'{s["name"]}: not freed: {gone[0]}')

    # Every transported present's image read back equal (the harness fails the run on any difference); here, that
    # there were read-backs at all where the host carried frames.
    transported = sum(1 for p in facts['presents'] if p['outcome']['transported'])
    if hide:
        need(transported == 0, f'{transported} presents modelled as transported with TRANSFER_SRC hidden')
        need(facts['readbacks'] == 0, 'read-backs on surfaces without TRANSFER_SRC')
    else:
        overlay = marker_off_bundle and CASES[name][4]
        # Under the legacy overlay the batch does not run (its present waits for the GPU on a new swapchain).
        need(overlay or facts['held_back_queued'] > RING_MAX,
             f'the held-back batch queued {facts["held_back_queued"]} presents, not more than the ring\'s {RING_MAX}')
        # The model has every batch present made while the block of GPU work ran: the run is only valid if it did.
        need(overlay or facts['held_back_gpu_ms'] > facts['held_back_presents_ms'] + 50,
             f'the block of GPU work ({facts["held_back_gpu_ms"]:.0f} ms) did not outlast the batch\'s presents '
             f'({facts["held_back_presents_ms"]:.0f} ms)')
        need(facts['readbacks'] >= transported // 2, f'only {facts["readbacks"]} read-backs for {transported} '
                                                     'transported presents')
        need(overlay or facts['overlay_mismatches'] == 0, 'read-backs set aside as the overlay\'s with no overlay')

    # The exports: the host's are patched, the control is not.
    for function in HOST_HOOKED:
        state = facts['prologues'][function]
        # Under Wine the export and vkGetDeviceProcAddr's answer are one winevulkan function, so the export's bytes are
        # the detour's. Should that stop holding, the export check below would prove nothing for this function.
        # With the bundle installed, its own vkGetDeviceProcAddr hands out its vkQueueSubmit hook (Vulkan_wDx12); the
        # host detours the export, which that hook calls down to.
        if function in DEVICE_LEVEL and not (marker_off_bundle and function.startswith('vkQueue') and
                                             function != 'vkQueuePresentKHR'):
            need(state['same_as_device_proc'], f'{function}: vkGetDeviceProcAddr no longer returns the export')
        need(state['patched'], f'{function} was not patched')
    if not marker_off_bundle:
        for function in NEVER_HOOKED:
            need(not facts['prologues'][function]['patched'], f'{function} was patched with the bundle off')
    summary = dict(transported=transported, skipped=len(facts['presents']) - transported,
                   readbacks=facts['readbacks'] - facts['overlay_mismatches'], overlay=facts['overlay_mismatches'],
                   held_back=facts['held_back_queued'])
    return problems, expected, summary


def execute_translated(name, dll, redist):
    api, key, marker = TRANSLATED_CASES[name]
    work = OUT / 'cases' / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy2(dll, work / 'winmm.dll')
    shutil.copy2(OUT / TRANSLATED_EXE, work / TRANSLATED_EXE)
    for library in redist:
        shutil.copy2(library, work / library.name)
    (work / 'OptiScaler.ini').write_text(ini_for(False, True, key, True, level=1 if marker else 0))
    if marker:
        (work / MARKER).write_text('')
    with (OUT / f'{name}.log').open('w') as log:
        process = subprocess.run(['wine', str(work / TRANSLATED_EXE), '--' + api], cwd=work,
                                 env=translated_env(TRANSLATED_VALIDATION[api]),
                                 stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, timeout=300)
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


def validation_errors(run_log):
    """The validation layer's own reports (it writes to stdout when no messenger of the application's exists)."""
    return [l for l in run_log.splitlines() if 'Validation Error' in l]


def check_translated(name, facts, log, run_log):
    """The translation layer's own Vulkan device and presents reach the host's hooks, under the marker or beside the
    bundle; its swapchain gets TRANSFER_SRC and every one of its presents is carried, under the validation layer."""
    api, _, marker = TRANSLATED_CASES[name]
    layer = TRANSLATED_CALLER[api]
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    need(log.count(INSTALL_LINE) == 1, 'expected one install line')
    skipped = 'Skipping vulkan-1.dll hooks, as [Vulkan] SkipHooks or the marker asks' in log
    if marker:
        need('this host is the one exception' in log, 'the install line does not name the skip')
        need(skipped, 'the bundle was not skipped')
    else:
        need('beside OptiScaler\'s own Vulkan hooks' in log, 'the install line does not say the bundle runs')
        need(not skipped, 'the bundle was skipped')
        # The bundle's present hook ran for every present, below the host's, and took each for a DXGI-backed one:
        # never the Vulkan overlay, which it only reaches for a native present (LogLevel 0's function trace).
        bundle = sum(1 for l in log.splitlines() if re.search(r'\bhkvkQueuePresentKHR\b', l))
        overlay = sum(1 for l in log.splitlines() if 'MenuOverlayVk::QueuePresent' in l)
        need(bundle >= facts['frames'], f'the bundle\'s present hook ran {bundle} times for {facts["frames"]} presents')
        need(overlay == 0, f'the bundle took a {layer} present for a native one {overlay} times')
    need(layer != 'DXVK' or 'DXVK' in run_log, 'DXVK did not announce itself: is it the dxgi/d3d11 that ran?')
    # The layer's devices: the title's, and any OptiScaler made through the layer for itself under ScopedSkipVulkanHooks
    # (IdentifyGpu makes a throwaway D3D12 device to detect vkd3d-proton), which must say so and never present here.
    devices, internal = [], []
    creator = re.escape(layer) if marker else r'OptiScaler\'s own Vulkan hook, called down to this one'
    for line in log.splitlines():
        m = re.search(r'DLSS-NR Vulkan present host: device (0x[0-9A-F]+) #(\d+) created by (\S+) \(' + creator +
                      r'\)', line)
        if m:
            (internal if 'OptiScaler\'s own device' in line else devices).append(m.groups())
    need(len(devices) >= 1, f'no device line for a device the title created through {layer}')
    # Its swapchains: TRANSFER_SRC added to what the layer asked for.
    created = [l for l in log.splitlines() if re.search(r'DLSS-NR Vulkan present host: swapchain 0x[0-9A-F]+ #\d+ on '
                                                        r'device', l)]
    need(len(created) >= 1, 'no swapchain creation line')
    for line in created:
        need(re.search(r': usage 0x12 -> 0x13 \(surface offers 0x[0-9A-F]+\); round trip on, TRANSFER_SRC added at '
                       r'creation(?:; the model .+)?$', line) is not None, f'swapchain not given TRANSFER_SRC: {line}')
    lines = [LINE.search(l) for l in log.splitlines() if WINDOW_LINE in l]
    need(all(lines), 'a window line did not parse')
    mine = [m for m in lines if m and m.group('window') == facts['window']]
    need(len(mine) >= 2, f'expected the first-present and 10-present lines for window {facts["window"]}, found '
                         f'{[m.group("why") for m in mine]}')
    transported = 0
    if mine:
        need(mine[0].group('why') == 'first present', f'first line is {mine[0].group("why")!r}')
        need(any(m.group('why') == '10 presents' for m in mine), 'no line at 10 presents: fewer presents reached the host')
        need(int(mine[-1].group('presents')) <= facts['frames'] + 1, 'more Vulkan presents than the application made')
        for m in mine:
            need(m.group('caller').endswith(f'({layer})'), f'caller {m.group("caller")!r} is not {layer}')
            need(any(m.group('device') == handle and m.group('device_id') == record for handle, record, _ in devices),
                 f'presenting device {m.group("device")} #{m.group("device_id")} is not one {layer} created')
            need(m.group('graphics') == 'yes' or m.group('compute') == 'yes', 'presenting family can run no work')
            t = TRANSPORT.match(m.group('transport'))
            need(t is not None, f'transport text unparsed: {m.group("transport")!r}')
            if t:
                need(int(t.group('transported')) == int(m.group('presents')) and int(t.group('skipped')) == 0,
                     f'line "{m.group("why")}": transported {t.group("transported")} of {m.group("presents")} presents, '
                     f'skipped {t.group("skipped")} ({t.group("skips")})')
                transported = int(t.group('transported'))
    need('never saw created' not in log, 'a present of a swapchain the host never saw created')
    need('round trip off for it' not in log and 'submit returned' not in log, 'a round trip failed at run time')
    # Every swapchain the layer destroyed: all of its round trips complete, its objects freed.
    destroyed_presents = carried = 0
    for line in log.splitlines():
        if re.search(r'DLSS-NR Vulkan present host: swapchain 0x[0-9A-F]+ #\d+ destroyed after', line):
            t = re.search(r'destroyed after (\d+) presents; transported (\d+), .*?completions (\d+)', line)
            need(t is not None and t.group(2) == t.group(3), f'not every round trip seen complete: {line}')
            need(t is not None and t.group(1) == t.group(2), f'a present of it was not carried: {line}')
            need('freed after a drain' in line or (t and t.group(2) == '0'), f'not freed: {line}')
            if t:
                destroyed_presents += int(t.group(1))
                carried += int(t.group(2))
    need(destroyed_presents == facts['frames'], f'the destroyed swapchains account for {destroyed_presents} presents, '
                                                f'not the {facts["frames"]} the application made')
    errors = validation_errors(run_log)
    if TRANSLATED_VALIDATION[api]:
        need('VALIDATION-SETTINGS' in run_log, 'the validation layer did not announce itself: was it loaded?')
    summary = (f'{layer}: device(s) {", ".join(f"{h} #{r} by {m}" for h, r, m in devices)}'
               + (f' (and {len(internal)} of OptiScaler\'s own, not reported)' if internal else '') + f'; {len(mine)} lines for '
               f'the window, caller {mine[0].group("caller") if mine else "?"}, family '
               f'{mine[0].group("family") if mine else "?"} flags {mine[0].group("flags") if mine else "?"}; '
               f'{carried} of {destroyed_presents} presents carried, ' +
               (f'{len(errors)} validation errors' if TRANSLATED_VALIDATION[api] else 'no validation layer'))
    return problems, summary, errors



# --- Stage 3: cold NR with the real model --------------------------------------------------------------------------

KIT = Path(os.environ.get('DLSSNR_KIT', str(Path.home() / '.local/opt/dlssnr-kit')))
NGX_CORE = Path('/usr/lib/nvidia/wine')

# name: (ApplyModel, WorkingScale or None, also present from the compute-only family, forwarder ('build': the one built
# beside this DLL, 'kit': the kit's, older than dlssnr_abi_version), model staged, device extensions hidden)
NR_CASES = {
    'nr-control': (False, None, True, 'build', True, None),
    'nr': (True, None, True, 'build', True, None),
    'nr-working-scale': (True, 0.5, False, 'build', True, None),
    'nr-no-extensions': (True, None, False, 'build', True, 'VK_NVX_binary_import'),
    'nr-old-forwarder': (True, None, False, 'kit', True, None),
    'nr-no-model': (True, None, False, 'build', False, None),
    # Off the present path (DOOM Eternal's startup hang, 2026-10-14): the stall watchdog names a present held 2.6 s
    # below the host's hook (the test layer); and the model's build slowed while presents and swapchain churn go on.
    'nr-stall-watch': (True, None, False, 'build', True, None),
    'nr-build-slow': (True, None, False, 'build', True, None),
}

# The test layer's timing knobs and the harness's arguments, for the cases that use them.
NR_TIMING = {
    'nr-stall-watch': ({'RETOQUE_STALL_PRESENT_AT': '30:2600'}, []),
    'nr-build-slow': ({'RETOQUE_SLOW_OFF_DEVICE_THREAD_MS': '400', 'RETOQUE_SLOW_OFF_DEVICE_THREAD_TOTAL_MS': '4000',
                       'RETOQUE_COUNT_OFF_THREAD_QUEUE': '1'}, ['--slow-build']),
}
# The longest a present may take in nr-build-slow, while the model's build takes seconds: the round trip, the validation
# layer and the call down at this harness's rate, nothing of the model's build.
SLOW_BUILD_PRESENT_MS = 300.0


def nr_proton():
    """The proton script the cold-NR cases run under, or None. Plain Wine brings the snippet up, but the driver's NGX core
    answers FAIL_PlatformError there (0xBAD00002, measured 2026-10-14 with the prefix set up by hand as Proton sets it
    up), as the D3D12 loopback found: the core needs Proton's own Wine. PROTON_DIR, else the newest GE-Proton (what
    DOOM Eternal ran under), else Valve's."""
    roots = [Path(os.environ['PROTON_DIR'])] if os.environ.get('PROTON_DIR') else []
    steam = Path.home() / '.local/share/Steam'
    roots += sorted((steam / 'compatibilitytools.d').glob('GE-Proton*'), reverse=True)
    roots += [steam / 'steamapps/common/Proton - Experimental', steam / 'steamapps/common/Proton Hotfix']
    return next((root / 'proton' for root in roots if (root / 'proton').exists()), None)


def nr_files(dll):
    """What the cold-NR cases need, and what of it is missing on this machine."""
    files = dict(model=KIT / 'nvngx_dlssnr.dll', old=KIT / 'nvngx.dll_dlssnr.dll', built=dll.parent / 'nvngx.dll_dlssnr.dll',
                 core=[NGX_CORE / '_nvngx.dll', NGX_CORE / 'nvngx.dll'], proton=nr_proton())
    missing = [str(f) for f in (files['model'], files['old'], files['built'], *files['core']) if not f.exists()]
    if files['proton'] is None:
        missing.append('a local Proton')
    return files, missing


def nr_env(hide=None, knobs=None):
    # Under Proton, which sets the prefix up for NGX itself: the driver's core and nvngx in system32, the NGXCore
    # registry key, dxvk-nvapi. Its own overrides are appended to these; the validation layer runs as in every case.
    env = runtime_env(False)
    del env['WINEPREFIX']
    env.update(STEAM_COMPAT_DATA_PATH=str(OUT / 'protonprefix-nr'),
               STEAM_COMPAT_CLIENT_INSTALL_PATH=str(Path.home() / '.local/share/Steam'), SteamGameId='0')
    if hide or knobs:
        # The test layer below the validation layer takes the extension out of what the device offers, or applies
        # its timing knobs; it leaves TRANSFER_SRC alone.
        env.update(VK_INSTANCE_LAYERS=f'VK_LAYER_KHRONOS_validation:{HIDE_LAYER}', VK_ADD_LAYER_PATH=str(LAYER_DIR),
                   RETOQUE_HIDE_TRANSFER_SRC='0')
    if hide:
        env.update(RETOQUE_HIDE_DEVICE_EXTENSIONS=hide)
    if knobs:
        env.update(knobs)
    return env


def prepare_nr_prefix(files):
    """Proton makes its prefix on first use; nothing else is set up by hand."""
    (OUT / 'protonprefix-nr').mkdir(parents=True, exist_ok=True)


def execute_nr(name, dll, redist, files):
    apply_model, scale, cross, forwarder, model, hide = NR_CASES[name]
    work = OUT / 'cases' / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy2(dll, work / 'dxgi.dll')
    shutil.copy2(OUT / NR_EXE, work / NR_EXE)
    for library in redist:
        shutil.copy2(library, work / library.name)
    shutil.copyfile(files['built'] if forwarder == 'build' else files['old'], work / 'nvngx.dll_dlssnr.dll')
    if model:
        # 165 MB of NVIDIA's model: linked, never copied, never in the tree.
        (work / 'nvngx_dlssnr.dll').symlink_to(files['model'])
    # Into the one [DlssNr] section, never a second one.
    extra = f'ApplyModel={str(apply_model).lower()}\n' + (f'WorkingScale={scale}\n' if scale is not None else '')
    ini = ini_for(False, True, True, True, level=1).replace('VulkanPresentHost=true\n', 'VulkanPresentHost=true\n' + extra)
    (work / 'OptiScaler.ini').write_text(ini)
    (work / MARKER).write_text('')
    with (OUT / f'{name}.log').open('w') as log:
        knobs, args = NR_TIMING.get(name, ({}, []))
        process = subprocess.run([str(files['proton']), 'run', str(work / NR_EXE)] + (['--cross'] if cross else []) +
                                 args, cwd=work, env=nr_env(hide, knobs), stdin=subprocess.DEVNULL, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=900)
    run_log = (OUT / f'{name}.log').read_text(errors='replace')
    facts = json.loads((work / 'result.json').read_text())
    log_path = work / 'OptiScaler.log'
    log = log_path.read_text(errors='replace') if log_path.exists() else ''
    if process.returncode != 0 or facts.get('status') != 'PASS':
        # Proton does not pass the application's output through: its own result.json says why.
        reason = facts.get('reason') or facts.get('first_validation') or next(
            (l for l in reversed(run_log.splitlines()) if l.startswith('FAIL: ')), 'no reason written')
        raise RuntimeError(f'{name}: application failed (exit {process.returncode}): {reason}; see '
                           f'{OUT / (name + ".log")} and {log_path}')
    if facts.get('validation_active') is not True or facts.get('validation_errors') != 0:
        raise RuntimeError(f'{name}: validation was not proved active and clean')
    return facts, log


# What the validation layer reports of NVIDIA's model's own commands, recorded into the host's command buffer: an image of
# its own (nv.ngx.dlssnr.resource) used in GENERAL at the first evaluate after each feature build without ever leaving
# UNDEFINED. Measured 2026-10-14 with 310.8.SF and the 615.78.08 core: the only report of a whole run under synchronization
# validation. Nothing else the model or the host does may be reported.
MODEL_VUIDS = {'VUID-vkCmdDraw-None-09600'}


def check_build_slow(name, facts, log):
    """nr-build-slow: the model's builds, slowed by the test layer on every thread but the one presenting, run while the
    application presents and churns its swapchains; no present waits on them. Frames are carried unchanged while a build
    runs, and edited once its product is adopted."""
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    lines = log.splitlines()
    names = [s['name'] for s in facts['swapchains']]
    need(names == ['once', 'same-size', 'resized', 'back', 'last'], f'swapchains {names}')
    if problems:
        return problems, ''

    building = {}
    seen = {}
    for s in facts['swapchains']:
        where = f'{s["name"]} ({s["width"]}x{s["height"]})'
        # A handle value comes back for a later swapchain here (once, then back): the k-th creation is this one.
        created = [re.search(r'swapchain 0x[0-9A-F]+ #(\d+) on device', l).group(1) for l in lines
                   if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #' in l and ' on device ' in l]
        k = seen.get(s['handle'], 0)
        seen[s['handle']] = k + 1
        record = created[k] if k < len(created) else '?'
        gone = [l for l in lines if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #{record} destroyed after'
                in l]
        need(len(gone) == 1, f'{where}: {len(gone)} destruction lines')
        if not gone:
            continue
        m = re.search(r'destroyed after (\d+) presents; transported (\d+), skipped (\d+)', gone[-1])
        need(m is not None and int(m.group(1)) == int(m.group(2)) == s['presents'] and m.group(3) == '0',
             f'{where}: not every present carried: {gone[-1]}')
        n = NR.match(gone[-1][gone[-1].index('; NR: ') + 2:]) if '; NR: ' in gone[-1] else None
        need(n is not None, f'{where}: no NR counters')
        if n is None:
            continue
        skips = counts(n.group('skips'))
        building[s['name']] = skips.get('model building', 0)
        need(set(skips) <= {'model building'}, f'{where}: refusals other than building: {skips}')
        need(int(n.group('evaluated')) + building[s['name']] == s['presents'], f'{where}: {gone[-1]}')
        # Carried frames come first in a swapchain, the model's after: only carried ones come back exactly.
        need(s['exact'] <= building[s['name']], f'{where}: {s["exact"]} exact read-backs, {building[s["name"]]} '
                                                f'frames carried')
        need(s['alpha_changed'] == 0 and s['black'] == 0, f'{where}: alpha {s["alpha_changed"]}, black {s["black"]}')

    # The build asked for at the one present runs on through the churn: every frame of it carried, the resize included.
    need(building.get('once') == 1, f'the one-present swapchain was not carried while the build ran: {building}')
    need(all(building.get(n) == s['presents'] for n, s in zip(names[1:3], facts['swapchains'][1:3])),
         f'frames of the churn reached the model while it was being built: {building}')
    back = facts['swapchains'][3]
    need(back['readbacks'] > back['exact'] and back['mae_mean'] > 0, f'the model never ran after the churn: {back}')
    # The device destroyed one present after a build was asked for at a last size: its destruction waited for it, and
    # the model's objects were freed with nothing left to the device (validation is clean of live children at destroy).
    need(building.get('last') == 1, f'the last swapchain was not carried while its build ran: {building}')
    waited = [re.search(r'waited (\d+) ms for the model.s build under way before the device goes', l) for l in lines]
    waited = [int(m.group(1)) for m in waited if m]
    need(len(waited) == 1 and waited[0] >= 1000, f'the device did not wait for the slowed build under way: {waited}')

    # The builds: asked for on the present path, made off it, slowly (the layer), adopted when over.
    asked = [l for l in lines if 'the model is being built for ' in l and 'off the present path; frames are carried' in l]
    ready = [re.search(r'the model is ready at (\d+x\d+), built off the present path in (\d+) ms', l) for l in lines]
    ready = [(m.group(1), int(m.group(2))) for m in ready if m]
    need(len(asked) >= 1 and len(ready) >= 1 and len(ready) <= len(asked), f'builds asked {len(asked)}, ready '
                                                                           f'{len(ready)}')
    slowest = max((ms for _, ms in ready), default=0)
    need(slowest >= 2000, f'no build was slowed enough to prove anything (slowest {slowest} ms)')
    # Behind the host's back on the queues: the builder's one submit per build, no idle wait (NGX's own included).
    offthread = (Path(OUT / 'cases' / name / 'layer-offthread.txt').read_text().split()
                 if (OUT / 'cases' / name / 'layer-offthread.txt').exists() else [])
    need(len(offthread) == 4 and int(offthread[3]) == 0 and int(offthread[1]) == len(ready),
         f'queue calls off the presenting thread: {offthread} for {len(ready)} builds')
    longest = max(s['max_present_ms'] for s in facts['swapchains'])
    need(longest < SLOW_BUILD_PRESENT_MS, f'a present took {longest:.1f} ms while builds took up to {slowest} ms')
    for text in ('the model is off on this device', 'called off', 'faulted'):
        need(not any(text in l for l in lines), f'a line with {text!r}')
    # The watchdog names a long build (it holds no host lock), and vkDestroyDevice waiting for one; never a present.
    stalls = [l[l.index('STALL: ') + 7:] for l in lines if 'DLSS-NR Vulkan present host: STALL: ' in l]
    need(all((' in model build ' in s and s.endswith('holds no host lock')) or
             (' in vkDestroyDevice ' in s and "waiting for the model's build under way (bounded)" in s) for s in stalls),
         f'stall lines other than the build and the destroy waiting for it: {stalls}')
    gone = [l for l in lines if f'DLSS-NR Vulkan present host: device {facts["device"]} #' in l and 'destroyed;' in l]
    need(len(gone) == 1 and gone[0].endswith('; the model freed'), f'device destruction: {gone}')
    vuids = set(v for v in facts.get('model_vuids', '').split(',') if v)
    need(vuids <= MODEL_VUIDS, f'validation reports of the model\'s own objects beyond those known: {sorted(vuids)}')

    summary = (f'{sum(s["presents"] for s in facts["swapchains"])} presents over the churn (one present, same size, '
               f'resized, back), {sum(building.values())} carried while {len(ready)} builds ran off the present path '
               f'(slowest {slowest} ms, layer-slowed), the longest present {longest:.1f} ms; '
               f'{len(stalls)} watchdog line(s), all for the build; vkDestroyDevice waited '
               f'{waited[0] if waited else "?"} ms for a build under way; off the presenting thread '
               f'{offthread[1] if offthread else "?"} submits (the builder\'s), '
               f'{offthread[3] if offthread else "?"} idle waits; 0 validation errors of the host\'s')
    return problems, summary


def check_nr(name, facts, log):
    """The model ran on every SDR present it should have, or was refused for the case's reason; what the application
    read back is what that means: exact where the model did not run or ApplyModel is off, changed but close where it
    ran; the device's extensions and the model's lifecycle in the log."""
    apply_model, scale, cross, forwarder, model, hide = NR_CASES[name]
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    lines = log.splitlines()
    need(sum(1 for l in lines if INSTALL_LINE in l) == 1, 'expected one install line')
    # The stall watchdog: silent, but in nr-stall-watch, where the test layer holds the 30th present 2.6 s below the
    # host's hook: then one line naming the present, its stage, and the present lock it holds.
    stalls = [l[l.index('STALL: ') + 7:] for l in lines if 'DLSS-NR Vulkan present host: STALL: ' in l]
    if name == 'nr-stall-watch':
        need(len(stalls) == 1 and re.fullmatch(r'thread \d+ in vkQueuePresentKHR \(0x[0-9A-F]+\) for 2\.\d s: calling '
                                               r'down vkQueuePresentKHR; holds the present lock', stalls[0]),
             f'the stalled present was not named as expected: {stalls}')
    else:
        need(not stalls, f'stall lines: {stalls}')
    device = [l for l in lines if f'DLSS-NR Vulkan present host: device {facts["device"]} #' in l and 'created by' in l]
    need(len(device) == 1, f'expected one line for the device, found {len(device)}')
    if device:
        if hide:
            need(f'not possible {hide} (the device does not offer it)' in device[0], f'device line: {device[0]}')
        else:
            need(device[0].endswith('; NR extensions: added ' + ', '.join(NR_EXTENSIONS + [FORMAT_FLAGS2]) +
                                    '; already enabled none; not possible none; bufferDeviceAddress enabled by the '
                                    'host'), f'device line: {device[0]}')

    refusal = None
    if hide:
        refusal = ('device extensions', f'the device was created without the model\'s extensions: {hide} (the device '
                                        f'does not offer it)')
    elif forwarder == 'kit':
        refusal = ('model unavailable', 'nvngx.dll_dlssnr.dll is from a different build')
    elif not model:
        refusal = ('model unavailable', 'nvngx_dlssnr.dll was not found beside OptiScaler or the game')
    runs = refusal is None

    evaluated_total = timed = 0
    seen = {}
    built_sizes = set()
    building = {}
    for s in facts['swapchains']:
        where = f'{s["name"]} ({s["format"]}, colour space {s["color_space"]}, {s["width"]}x{s["height"]})'
        # A handle value can come back for a later swapchain: the k-th creation line of it is this one's record.
        created = [re.search(r'swapchain 0x[0-9A-F]+ #(\d+) on device', l).group(1) for l in lines
                   if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #' in l and ' on device ' in l]
        k = seen.get(s['handle'], 0)
        seen[s['handle']] = k + 1
        record = created[k] if k < len(created) else '?'
        gone = [l for l in lines if f'DLSS-NR Vulkan present host: swapchain {s["handle"]} #{record} destroyed after'
                in l]
        need(len(gone) == 1, f'{where}: expected one destruction line, found {len(gone)}')
        if gone:
            m = re.search(r'destroyed after (\d+) presents; transported (\d+), skipped (\d+)', gone[0])
            need(m is not None and int(m.group(1)) == int(m.group(2)) == s['presents'] and m.group(3) == '0',
                 f'{where}: not every present carried: {gone[0]}')
            nr_text = gone[0][gone[0].index('; NR: ') + 2:] if '; NR: ' in gone[0] else ''
            if not s['sdr']:
                problems.extend(nr_problems(where, nr_text, 0, s['presents'], {'colour': s['presents']}))
            elif runs:
                size = (s['width'], s['height'])
                found, building[s['name']] = nr_building_problems(where, nr_text, s['presents'],
                                                                  size not in built_sizes)
                problems.extend(found)
                built_sizes.add(size)
                evaluated_total += s['presents'] - building[s['name']]
                timed += 1
            else:
                problems.extend(nr_problems(where, nr_text, 0, s['presents'], {refusal[0]: s['presents']}, refusal[1]))
        need(s['readbacks'] >= 1, f'{where}: nothing read back')
        if not s['sdr'] or not runs or not apply_model:
            need(s['exact'] == s['readbacks'], f'{where}: {s["readbacks"] - s["exact"]} of {s["readbacks"]} read-backs '
                                               f'changed where the picture had to come back exactly (largest change '
                                               f'{s["max_diff"]}, mean {s["mae_mean"]:.3f})')
        else:
            # The model ran and its edit landed: changed, close, opaque, not black. Only the frames carried while it was
            # built (the first ones) may come back exactly.
            need(s['exact'] <= building.get(s['name'], 0) and s['exact'] < s['readbacks'],
                 f'{where}: {s["exact"]} of {s["readbacks"]} read-backs unchanged with the model applied '
                 f'({building.get(s["name"], 0)} frames carried while it was built)')
            need(0 < s['mae_mean'] and s['mae_max'] <= 25.0, f'{where}: mean change {s["mae_mean"]:.3f}, largest '
                                                             f'mean {s["mae_max"]:.3f} (eight-bit steps)')
            need(s['alpha_changed'] == 0 and s['black'] == 0, f'{where}: alpha changed {s["alpha_changed"]}, black '
                                                              f'{s["black"]}')

    if runs:
        for words in ('the model initialised on device', "the model's images built for", "the model's feature is up at",
                      'the model runs at present on'):
            need(any(words in l for l in lines), f'no line saying {words!r}')
        need(not any('the model is off on this device' in l for l in lines), 'the model went off: ' +
             next((l for l in lines if 'the model is off on this device' in l), ''))
        gone = [l for l in lines if f'DLSS-NR Vulkan present host: device {facts["device"]} #' in l and 'destroyed;' in l]
        need(len(gone) == 1 and gone[0].endswith('; the model freed'), f'device destruction: {gone}')
        need(any('NGX shut down on the device before it goes (returned 1)' in l for l in lines),
             'NGX was not shut down on the device before it went')
        if scale is not None:
            odd = next(s for s in facts['swapchains'] if s['name'] == 'odd')
            want = f'working {int(odd["width"] * scale + 0.5)}x{int(odd["height"] * scale + 0.5)})'
            need(any("the model's images built for" in l and want in l for l in lines),
                 f'no rebuild at the working size {want}')
        if cross and facts['compute_family'] >= 0:
            crossed = next((s for s in facts['swapchains'] if s['name'] == 'cross'), None)
            need(crossed is not None and crossed['family'] == facts['compute_family'], 'no cross-family swapchain')
    else:
        need(not any("the model's feature is up at" in l for l in lines), 'a feature was built where none could be')

    builds = sum(1 for l in lines if "the model's feature is up at" in l)
    vuids = set(v for v in facts.get('model_vuids', '').split(',') if v)
    need(vuids <= MODEL_VUIDS, f'validation reports of the model\'s own objects beyond those known: {sorted(vuids)}')
    need(facts.get('model_messages', 0) <= builds, f'{facts.get("model_messages")} reports of the model\'s own objects '
                                                    f'for {builds} feature builds')

    sdr = [s for s in facts['swapchains'] if s['sdr']]
    summary = (f'{len(facts["swapchains"])} swapchains ({", ".join(s["name"] for s in facts["swapchains"])}), '
               f'{sum(s["presents"] for s in facts["swapchains"])} presents, '
               + (f'the model ran on {evaluated_total} SDR presents ({sum(building.values())} carried while it was '
                  f'built off the present path, {len(building and [v for v in building.values() if v])} build(s)), '
                  + ('every read-back exact (ApplyModel off)' if not apply_model else
                     f'SDR read-backs changed by a mean of '
                     f'{sum(s["mae_mean"] for s in sdr) / max(len(sdr), 1):.2f} eight-bit steps (largest frame mean '
                     f'{max((s["mae_max"] for s in sdr), default=0):.2f})') if runs else
                  f'the model refused ({refusal[0]}: {refusal[1]}), every read-back exact')
               + f', 0 validation errors of the host\'s ({facts.get("model_messages", 0)} of the model\'s own '
               f'{",".join(sorted(vuids)) or "objects"} set aside)')
    return problems, summary


# --- The way back: an after-upscale route that goes quiet (DOOM Eternal, 2026-10-14) ----------------------------

# nr.cpp --route: one swapchain throughout; OptiScaler's own FSR 2.2 behind its NGX exports for 30 frames, the
# after-upscale route running the model on its output; then the feature released and nothing evaluated until 14 s after
# the last evaluate, longer than the present host waits (kAfterUpscaleIdleMs); twice. The phases' read-backs say who
# ran the model when: until 9 s after the last evaluate the route (frames unedited), from 12 s the present host.
ROUTE_CASES = {'nr-route-quiet', 'nr-route-quiet-wdx12', 'nr-route-toggle-wdx12'}
ROUTE_IDLE_S = 10  # retoque/dlssnr/DlssNr_VkModelOwner.h, kAfterUpscaleIdleMs


def execute_route(name, dll, redist, files):
    work = OUT / 'cases' / name
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    shutil.copy2(dll, work / 'dxgi.dll')
    shutil.copy2(OUT / NR_EXE, work / NR_EXE)
    for library in redist:
        shutil.copy2(library, work / library.name)
    shutil.copyfile(files['built'], work / 'nvngx.dll_dlssnr.dll')
    (work / 'nvngx_dlssnr.dll').symlink_to(files['model'])
    # OptiScaler's own FSR 2.2 as the game's upscaler: nothing but the model to supply, and the evaluate's path to the
    # after-upscale route is the one every OptiScaler upscaler takes. DLSS stays on, so NGX initialises through the
    # driver's core, as a game's does.
    wdx12 = name.endswith('-wdx12')
    toggle = name == 'nr-route-toggle-wdx12'
    ini = ini_for(False, True, True, toggle or not wdx12, level=1) + '[Upscalers]\nVulkanUpscaler=fsr22\n'
    (work / 'OptiScaler.ini').write_text(ini)
    if not wdx12:
        (work / MARKER).write_text('')
    env = nr_env()
    # Do not inherit a diagnostic override that silently removes the chain this case must exercise.
    env.pop('OPTISCALER_DISABLE_VULKAN_WDX12_HOOKS', None)
    if toggle:
        ini = ini.replace('VulkanPresentHost=true\n', 'VulkanPresentHost=true\nToggleKey=0x76\n')
        (work / 'OptiScaler.ini').write_text(ini)
    args = ['--route'] + (['--wdx12'] if wdx12 else []) + (['--nr-toggle'] if toggle else [])
    with (OUT / f'{name}.log').open('w') as log:
        process = subprocess.run([str(files['proton']), 'run', str(work / NR_EXE), *args], cwd=work,
                                 env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                 timeout=900)
    run_log = (OUT / f'{name}.log').read_text(errors='replace')
    facts = json.loads((work / 'result.json').read_text())
    log_path = work / 'OptiScaler.log'
    log = log_path.read_text(errors='replace') if log_path.exists() else ''
    if process.returncode != 0 or facts.get('status') != 'PASS':
        reason = facts.get('reason') or facts.get('first_validation') or next(
            (l for l in reversed(run_log.splitlines()) if l.startswith('FAIL: ')), 'no reason written')
        raise RuntimeError(f'{name}: application failed (exit {process.returncode}): {reason}; see '
                           f'{OUT / (name + ".log")} and {log_path}')
    if facts.get('validation_active') is not True or facts.get('validation_errors') != 0:
        raise RuntimeError(f'{name}: validation was not proved active and clean')
    return facts, log


def check_route(name, facts, log):
    """Twice: the present host stands down for the route, the route runs the model, goes quiet, and kAfterUpscaleIdleMs
    later the present host takes the model back, the route's objects freed; never earlier, never called off."""
    problems = []

    def need(condition, message):
        if not condition:
            problems.append(message)

    wdx12 = name.endswith('-wdx12')
    toggle = name == 'nr-route-toggle-wdx12'
    need(facts.get('wdx12_probed') is wdx12, 'wDx12 chain probe did not match the case')
    # One rejection is the application's deliberate, Vulkan-valid zero-batch probe. No host drain may add one.
    need(log.count('Invalid parameters to hk_vkQueueSubmit') == int(wdx12),
         'unexpected wDx12 submit rejections (only the chain probe may be rejected)')
    if wdx12:
        need('Skipping vulkan-1.dll hooks' not in log, 'the Vulkan bundle was skipped')
        need('Skipping Vulkan_wDx12 hooks' not in log, 'wDx12 was disabled')
    lines = log.splitlines()
    stalls = [l for l in lines if 'DLSS-NR Vulkan present host: STALL: ' in l]
    need(not stalls, f'stall lines: {stalls}')
    phases = {p['name']: p for p in facts.get('phases', [])}
    expected = ['cold-build', 'cold', 'on', 'off-waiting', 'off-handing-back', 'off-back', 'on-again', 'off-again-waiting',
                'off-again-handing-back', 'off-again-back', 'on-at-exit']
    if toggle:
        expected = ['cold-build', 'cold', 'on', 'toggle-off', 'off-waiting', 'nr-disabled', 'toggle-on', 'off-handing-back',
                    'off-back', 'on-again', 'toggle-off-again', 'off-again-waiting', 'nr-disabled-again',
                    'toggle-on-again', 'off-again-handing-back', 'off-again-back', 'on-at-exit']
        blocker = 'hand-back due but not started: NR off ([DlssNr] Enabled=false)'
        need(log.count(blocker) == 2, 'NR off must block each due hand-back exactly once')
        need(log.count('Neural Rendering toggle key pressed, setting DlssNrEnabled to ') == 4, 'four production F7 toggles were not observed')
        events = [(i, line) for i, line in enumerate(lines)
                  if 'Neural Rendering toggle key pressed, setting DlssNrEnabled to ' in line]
        starts = [i for i, line in enumerate(lines) if 's: taking it back, once the device' in line]
        blockers = [i for i, line in enumerate(lines) if blocker in line]
        def time_ms(line):
            m = re.match(r'\[(\d+):(\d+):(\d+)\.(\d+)\]', line)
            return ((int(m[1]) * 60 + int(m[2])) * 60 + int(m[3])) * 1000 + float('0.' + m[4]) * 1000
        if len(events) == 4 and len(starts) == 2 and len(blockers) == 2:
            for cycle in range(2):
                off, on = events[2 * cycle], events[2 * cycle + 1]
                need(off[1].endswith('false') and on[1].endswith('true'), 'F7 order must be off then on')
                need(off[0] < blockers[cycle] < on[0] < starts[cycle], 'hand-back started while NR was off')
                elapsed = (time_ms(lines[starts[cycle]]) - time_ms(on[1])) % (24 * 3600000)
                need(elapsed < 2000, f'hand-back did not start promptly after NR on: {elapsed:.1f} ms')
        for phase_name in ('nr-disabled', 'nr-disabled-again'):
            p = phases.get(phase_name, {})
            need(p.get('readbacks', 0) > 0 and p.get('exact') == p.get('readbacks'),
                 f'{phase_name}: NR ran while disabled, or no read-backs: {p}')
    need(list(phases) == expected, f'phases {list(phases)}')
    if problems:
        return problems, ''

    # Who ran the model, from what came back: the present host edits the frames it runs on, the route's run is on its
    # own output, never presented.
    for name_ in ('cold', 'off-back', 'off-again-back'):
        p = phases[name_]
        need(p['readbacks'] >= 5 and p['exact'] == 0 and p['mae_mean'] > 0,
             f'{name_}: the present host did not run the model on every frame: {p}')
    # The model's first build, off the present path: frames carried unchanged while it ran (the overlay's splash, in
    # the toggle case, keeps these frames from being compared).
    if not toggle:
        p = phases['cold-build']
        need(p['readbacks'] >= 1 and p['exact'] >= 1, f'cold-build: no frame carried while the model was built: {p}')
    for name_, evaluates in (('on', 30), ('on-again', 30), ('on-at-exit', 10)):
        p = phases[name_]
        need(p['evaluates'] == evaluates and p['evaluates'] == p['presents'], f'{name_}: evaluates {p}')
        need(p['readbacks'] >= 5 and p['exact'] == p['readbacks'], f'{name_}: the present host did not stand down '
                                                                   f'for the route: {p}')
    for name_ in ('off-waiting', 'off-again-waiting'):
        p = phases[name_]
        # This phase ends at a fixed deadline; the enabled bundle can make it produce fewer than five read-backs.
        # Require actual coverage, and reject any edited frame. A frame-count floor here would test throughput.
        need(p['readbacks'] > 0, f'{name_}: no read-back before the hand-back deadline: {p}')
        need(p['exact'] == p['readbacks'], f'{name_}: the model went back to the present host '
                                         f'before {ROUTE_IDLE_S} s: {p}')

    def count(text):
        return sum(1 for l in lines if text in l)

    host = f'DLSS-NR Vulkan present host: device {facts["device"]} #'
    route_waits = 'DLSS-NR Vulkan: this route waits: the Vulkan present host holds the model'
    stood_down = 'the model stood down and was freed'
    route_has = 'DLSS-NR Vulkan: this route has the model again (the present host stood down)'
    taking = f'the after-upscale route has not asked for the model for {ROUTE_IDLE_S} s: taking it back'
    freed = "DLSS-NR Vulkan: the after-upscale route's model was freed for the hand-back (its feature at 1280x720"
    again = "the model is the present host's again"
    exit_freed = "DLSS-NR Vulkan: the after-upscale route's objects on the device freed before it goes"
    for text, times in ((route_waits, 3), (stood_down, 3), (route_has, 3), (taking, 2), (freed, 2), (again, 2),
                        ('the model initialised on device', 3), ('DLSS-NR Vulkan: running natively at 1280x720', 1),
                        (exit_freed, 1)):
        need(count(text) == times, f'expected {times} line(s) with {text!r}, found {count(text)}')
    for text in ('called off', 'the model is off on this device', 'drain is still waiting', 'faulted', 'abandoned'):
        need(count(text) == 0, f'a line with {text!r}: ' + next((l for l in lines if text in l), ''))
    for text in (stood_down, taking, again):
        need(all(host in l for l in lines if text in l), f'{text!r} not said for the harness\'s device')

    # In order, twice: stand down, the route takes it, goes quiet, the drain, the route's objects freed, the host's; then
    # once more the stand-down, and at exit the route's objects freed before NGX is shut down on the device.
    ngx_down = 'NGX shut down on the device before it goes (returned 1)'
    kinds_text = (route_waits, stood_down, route_has, taking, freed, again, exit_freed, ngx_down)
    order = [i for i, l in enumerate(lines) for text in kinds_text if text in l]
    kinds = [next(k for k, text in enumerate(kinds_text) if text in lines[i]) for i in order]
    need(kinds == [0, 1, 2, 3, 4, 5] * 2 + [0, 1, 2, 6, 7], f'lines out of order: {kinds}')

    # The host had stood down for the route before the device went: no model of its own to free then.
    gone = [l for l in lines if host in l and 'destroyed;' in l]
    need(len(gone) == 1 and 'the model' not in gone[0], f'device destruction: {gone}')

    builds = sum(1 for l in lines if "the model's feature is up at" in l)
    route_builds = sum(1 for l in lines if 'DLSS-NR Vulkan: feature up at 1280x720' in l)
    need(builds == 3 and route_builds == 3, f'feature builds: {builds} of the present host\'s, {route_builds} of the '
                                            'route\'s')
    vuids = set(v for v in facts.get('model_vuids', '').split(',') if v)
    need(vuids <= MODEL_VUIDS, f'validation reports of the model\'s own objects beyond those known: {sorted(vuids)}')
    need(facts.get('model_messages', 0) <= builds + route_builds,
         f'{facts.get("model_messages")} reports of the model\'s own objects for {builds + route_builds} feature builds')

    drains = [re.search(r'the drain took (\d+) ms', l) for l in lines if again in l]
    handback_when = ('after F7 re-enabled NR (one due NR-off blocker per cycle, start within 2 s)' if toggle else
                     f'{ROUTE_IDLE_S} s after the route went quiet')
    changed_when = 'after re-enabling NR and draining' if toggle else 'from 12 s'
    summary = (f'twice: the present host stood down for the route ({phases["on"]["evaluates"]} and '
               f'{phases["on-again"]["evaluates"]} evaluates, every read-back exact), took the model back '
               f'{handback_when} (drains of '
               f'{", ".join(m.group(1) for m in drains if m)} ms; {phases["off-waiting"]["readbacks"]} and '
               f'{phases["off-again-waiting"]["readbacks"]} read-backs exact until 9 s after the last evaluate, every one '
               f'changed {changed_when}), the route\'s feature freed each time and at exit, before NGX went; {builds} feature builds of the present host\'s, '
               f'{route_builds} of the route\'s; '
               f'0 validation errors of the host\'s ({facts.get("model_messages", 0)} of the model\'s own '
               f'{",".join(sorted(vuids)) or "objects"} set aside)')
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
    if not (args.skip_build and (OUT / EXE).exists() and (OUT / TRANSLATED_EXE).exists() and (OUT / NR_EXE).exists()):
        build_harness()
    build_layer()

    cases = (args.only.split(',') if args.only else
             list(CASES) + list(TRANSLATED_CASES) + list(NR_CASES) + sorted(ROUTE_CASES))
    unknown = [c for c in cases if c not in CASES and c not in TRANSLATED_CASES and c not in NR_CASES and
               c not in ROUTE_CASES]
    if unknown:
        raise RuntimeError(f'unknown case(s): {unknown}')
    results = {'dll': str(dll), 'dll_sha256': sha(dll), 'harness_sha256': sha(OUT / EXE), 'cases': {}}
    failures = []
    wine = proton_libraries() if any(c in TRANSLATED_CASES for c in cases) else None
    baseline = {}
    for name in sorted([c for c in cases if c in TRANSLATED_CASES], key=lambda c: TRANSLATED_CASES[c][1]):
        if wine is None:
            results['cases'][name] = {'status': 'SKIP', 'problems': ['no local Proton with DXVK and vkd3d-proton']}
            print(f'SKIP: {name}: no local Proton with DXVK and vkd3d-proton (set PROTON_DIR)', flush=True)
            continue
        try:
            prepare_translated_prefix(wine)
            facts, log, run_log = execute_translated(name, dll, redist)
            api, key, marker = TRANSLATED_CASES[name]
            if key:
                problems, summary, errors = check_translated(name, facts, log, run_log)
                # The layers' own reports, in the key-off run of the same API, are not the host's.
                own = baseline.get((api, marker))
                ids = set(validation_ids('\n'.join(errors)))
                if not TRANSLATED_VALIDATION[api]:
                    pass
                elif own is None:
                    problems.append('no key-off baseline ran for the validation comparison')
                elif ids - own:
                    problems.append(f'validation errors only with the host: {sorted(ids - own)}')
                if errors and own is not None and not ids - own:
                    summary += ' (all of them the layer\'s own, also with the host off)'
            else:
                problems = check_host_off(name, facts, log, marker=marker)
                errors = validation_errors(run_log)
                baseline[(api, marker)] = set(validation_ids('\n'.join(errors)))
                summary = (f'no host line ({TRANSLATED_CALLER[api]} presented {facts["frames"]} frames); ' +
                           (f'{len(errors)} validation errors of the layer\'s own' if TRANSLATED_VALIDATION[api]
                            else 'no validation layer'))
            results['cases'][name] = {'status': 'FAIL' if problems else 'PASS', 'problems': problems,
                                      'proton': str(wine.parents[2]), 'validation_errors': len(errors)}
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
        marker, skiphooks, enabled, key, menu, one_window, hide = CASES[name]
        try:
            facts, log = execute_case(name, dll, redist)
            host_on = enabled and key
            if host_on:
                problems, expected, s = check_host_on(name, facts, log, marker_off_bundle=not (marker or skiphooks))
                summary = (f'{sum(len(v) for v in expected.values())} window lines, '
                           f'{len(facts["presents"])} presents ({s["transported"]} carried, {s["skipped"]} not), '
                           f'{s["readbacks"]} read-backs equal'
                           + (f' ({s["overlay"]} more carrying the overlay\'s splash)' if s['overlay'] else '') +
                           f', {s["held_back"]} held back, '
                           f'{len(facts["devices"])} device(s)'
                           f'{" (d3 reused d2" + chr(39) + "s handle)" if facts["third_reused_second"] else ""}, '
                           f'second queue family {facts["second_queue_family"]}, 0 validation errors, '
                           f'{facts["wine_injected_extension_messages"]} Wine dma_buf messages set aside')
            else:
                problems = check_host_off(name, facts, log, marker=marker or skiphooks)
                summary = ('no host line' + (', every watched export byte-identical' if marker or skiphooks else '') +
                           f', {facts["readbacks"] - facts["overlay_mismatches"]} read-backs equal'
                           + (f' ({facts["overlay_mismatches"]} more carrying the overlay\'s splash)'
                              if facts['overlay_mismatches'] else '') + ', 0 validation errors')
            results['cases'][name] = {'status': 'FAIL' if problems else 'PASS', 'problems': problems,
                                      'third_reused_second': facts['third_reused_second'],
                                      'compute_family': facts['compute_family'], 'readbacks': facts['readbacks'],
                                      'held_back_queued': facts.get('held_back_queued')}
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

    nr_cases = [c for c in cases if c in NR_CASES or c in ROUTE_CASES]
    files, missing = nr_files(dll) if nr_cases else ({}, [])
    if nr_cases and not missing:
        prepare_nr_prefix(files)
    for name in nr_cases:
        if missing:
            results['cases'][name] = {'status': 'SKIP', 'problems': [f'missing: {", ".join(missing)}']}
            print(f'SKIP: {name}: missing {", ".join(missing)}', flush=True)
            continue
        try:
            if name in ROUTE_CASES:
                facts, log = execute_route(name, dll, redist, files)
                problems, summary = check_route(name, facts, log)
            else:
                facts, log = execute_nr(name, dll, redist, files)
                checker = check_build_slow if name == 'nr-build-slow' else check_nr
                problems, summary = checker(name, facts, log)
            results['cases'][name] = {'status': 'FAIL' if problems else 'PASS', 'problems': problems,
                                      'swapchains': facts['swapchains'], 'offered': facts['offered'],
                                      'phases': facts.get('phases')}
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
