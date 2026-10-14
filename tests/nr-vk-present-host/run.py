#!/usr/bin/env python3
"""The Vulkan present host, stages 1 and 2 (docs/dlssnr/design/nr-vulkan-present-host.md): the production registry and
transport headers compiled on the host and driven through their state machines and decisions, and source guards that
the host is inert when off and, when on, changes nothing but an eligible swapchain's usage and a transported present's
waits. No Vulkan device, no Wine; tests/vulkan-present-host runs the real DLL."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
dlssnr = root / 'retoque/dlssnr'

registry = (dlssnr / 'DlssNr_VkPresentRegistry.h').read_text()
transport = (dlssnr / 'DlssNr_VkPresentTransport.h').read_text()
host = (dlssnr / 'DlssNr_VkPresentHost.cpp').read_text()
hooks = (root / 'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()
project = (root / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')


def function(text, signature):
    """The body of the one definition that starts with `signature`, braces matched outside strings and comments."""
    assert text.count(signature) == 1, f'expected one definition: {signature!r}'
    i = text.index('{', text.index(signature))
    depth, j = 0, i
    while True:
        c = text[j]
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        if c in '"\'':
            j += 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1


def code(text):
    """Without comments and string literals, for guards about what the code calls."""
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def flat(text):
    return re.sub(r'\s+', ' ', text)


# --- Source guards ----------------------------------------------------------------------------------------------

# The headers are standard library only (the registry includes the transport header), so this suite compiles them as
# production does.
includes = sorted(l.strip() for l in transport.splitlines() if l.startswith('#include'))
assert all(re.fullmatch(r'#include <\w+>', l) for l in includes), includes
includes = sorted(l.strip() for l in registry.splitlines() if l.startswith('#include'))
assert includes[0] == '#include "DlssNr_VkPresentTransport.h"', includes
assert all(re.fullmatch(r'#include <\w+>', l) for l in includes[1:]), includes

# Inert when off. The one call site is the first statement of VulkanHooks::Hook, ahead of the marker/SkipHooks check;
# Install's first statement is the configuration gate, before any lookup, lock, detour or record.
calls = [p for p in (root / 'OptiScaler').rglob('*.cpp') if 'VkPresentHost::Install(' in p.read_text(errors='ignore')]
calls += [p for p in (root / 'retoque').rglob('*.cpp')
          if p.name != 'DlssNr_VkPresentHost.cpp' and 'VkPresentHost::Install(' in p.read_text(errors='ignore')]
assert calls == [root / 'OptiScaler/hooks/Vulkan_Hooks.cpp'], calls
hook = code(function(hooks, 'void VulkanHooks::Hook(HMODULE vulkan1)'))
statements = [s.strip() for s in hook.strip('{} \n').split(';') if s.strip()]
assert statements[0] == 'DlssNr::VkPresentHost::Install(vulkan1)', statements[0]
assert hook.index('VkPresentHost::Install') < hook.index('vulkanHooksSkipped')
# The include, the call, and one read in the bundle's present hook: nothing else of the bundle changed for it.
assert code(hooks).count('VkPresentHost') == 3, code(hooks).count('VkPresentHost')
# That read: the bundle tells a DXVK/vkd3d-proton present from a native one by its caller, which with the host's detour
# on top is OptiScaler; the host hands it the application's. Off, it is the address it was given.
bundle_present = code(function(hooks, 'VALIDATE_HOOK(hkvkQueuePresentKHR, PFN_vkQueuePresentKHR)'))
assert ('IsDxgiBackedCaller(DlssNr::VkPresentHost::OriginalPresentCaller(_ReturnAddress()))' in bundle_present)
original = flat(code(function(host, 'void* DlssNr::VkPresentHost::OriginalPresentCaller(void* returnAddress)')))
assert original == '{ return t_presentCaller != nullptr ? t_presentCaller : returnAddress; }', original
assert '#include <dlssnr/DlssNr_VkPresentHost.h>' in hooks

install = flat(code(function(host, 'void DlssNr::VkPresentHost::Install(HMODULE vulkan1)')))
gate = ('{ if (vulkan1 == nullptr || !Config::Instance()->DlssNrEnabled.value_or_default() || '
        '!Config::Instance()->DlssNrVulkanPresentHost.value_or_default()) return;')
assert install.startswith(gate), install[:300]
# Under the gate, the marker decides nothing: vulkanHooksSkipped is read only for the log line.
assert host.count('vulkanHooksSkipped') == 1
assert re.search(r'LOG_INFO\([^;]*State::Instance\(\)\.vulkanHooksSkipped', code(host), re.S)

# What the host never does, on or off: none of the bundle's present work (Reflex, the frame limiter, the frozen-feature
# tick, upscaler timing, the overlay, spoofing), no NR, no NGX, and no device-wide idle wait (DEVELOPMENT.md's drain is
# done on the host's own fences, which needs no queue).
body = code(host)
for forbidden in ('ReflexHooks', 'FrameLimit', 'TickFrozenCheck', 'ReadUpscalerTime', 'upscaleTimes', 'MenuOverlay',
                  'VulkanSpoofing', 'InputCommon', 'EvaluateAfterUpscaleVk', 'DlssNr_Vk', 'ShutdownVk', 'swapchainApi',
                  'NGX', 'NVSDK', 'DeviceWaitIdle', 'memcpy', 'localCreateInfo', 'imageUsage |='):
    assert forbidden not in body, f'the present host must not use {forbidden}'

# Its own Vulkan calls are the five read-only queries resolved from vulkan-1.dll at install, and the device functions
# resolved per device into DeviceFns (dev.fn./dev->fn.); no prototype is called directly.
direct = re.search(r'(?<![\w.>])vk[A-Z]\w*\(', body)
assert direct is None, direct

# OptiScaler's own Vulkan work is ScopedSkipVulkanHooks alone. creatingD3DDevice is also set around the game's own
# D3D12CreateDevice, CreateDXGIFactory and D3D11 device creation, inside which vkd3d-proton and DXVK make the very
# instance and device this host exists for; reading it as "internal" hid every translated title's presents.
assert 'creatingD3DDevice' not in body
assert re.search(r'bool Internal\(\) \{ return ScopedSkipVulkanHooks::Active\(\); \}', body)

own_calls = set(re.findall(r'\b(g_\w+)\(', body))
assert own_calls == {'g_getDeviceProcAddr', 'g_getQueueFamilies', 'g_getSurfaceCapabilities', 'g_getMemoryProperties',
                     'g_getImageFormatProperties'}, own_calls

# Every hook hands its arguments down exactly once on each path, and only two things are ever edited on the way.
for call, times in (('o_CreateInstance(info, allocator, instance)', 1), ('o_DestroyInstance(instance, allocator)', 1),
                    ('o_EnumeratePhysicalDevices(instance, count, devices)', 1),
                    ('o_CreateDevice(physical, info, allocator, device)', 1),
                    ('o_CreateWin32Surface(instance, info, allocator, surface)', 1),
                    ('o_DestroySurface(instance, surface, allocator)', 1), ('next(device, allocator)', 1),
                    ('next(device, family, index, queue)', 1), ('next(device, info, queue)', 1),
                    ('next(device, info, allocator, swapchain)', 1), ('next(device, down, allocator, swapchain)', 1),
                    ('next(device, swapchain, allocator)', 1), ('next(device, swapchain, count, images)', 1),
                    ('next(device, swapchain, timeout, semaphore, fence, index)', 1), ('next(device, info, index)', 1),
                    ('next(queue, info)', 2), ('next(queue, &local)', 1), ('next(queue, args...)', 2)):
    assert body.count(call) == times, f'expected the pass-through {call} {times}x, found {body.count(call)}'
for hook_name in ('OnPresent', 'OnCreateSwapchain', 'OnGetQueue', 'OnGetQueue2', 'OnDestroyDevice',
                  'OnDestroySwapchain', 'OnGetImages', 'OnAcquire', 'OnAcquire2'):
    assert re.search(r'\b' + hook_name + r'\(PFN_vk\w+ next,', host), hook_name

# 1. The usage: only imageUsage of a copy, and that copy goes down only when the plan says the round trip runs and
#    added something; decided before the real call, never by retrying it.
swapchain_hook = code(function(host, 'VkResult OnCreateSwapchain('))
assert re.findall(r'\bedited\.(\w+)\s*=', swapchain_hook) == ['imageUsage'], swapchain_hook
assert 'edited.imageUsage = plan.usage;' in swapchain_hook
assert 'const VkSwapchainCreateInfoKHR* down = plan.transport && plan.added != 0 ? &edited : info;' in swapchain_hook
assert swapchain_hook.index('PlanUsage(query)') < swapchain_hook.index('next(device, down, allocator, swapchain)')

# 2. The present: the caller's own pointer unless the round trip ran; then a copy whose waits, and nothing else, are
#    the host's semaphore. The pNext chain (present fences, modes, ids) goes down by pointer.
present = code(function(host, 'VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, '
                              'const VkPresentInfoKHR* info, void* returnAddress)'))
assert re.findall(r'\blocal\.(\w+)\s*=', present) == ['waitSemaphoreCount', 'pWaitSemaphores'], present
assert 'local.waitSemaphoreCount = 1;' in present and 'local.pWaitSemaphores = &prep.wait;' in present
assert 'if (prep.wait == VK_NULL_HANDLE) return next(queue, info);' in flat(present)

# The application's waits are consumed only by a submit that ran; everything that can refuse is decided before it.
prepare = code(function(host, 'Prepared Prepare(VkQueue queue, const VkPresentInfoKHR& info, const PresentView* view)'))
# Lock order: the presenting queue's present lock (the hook, first), the transport's, then a borrowed queue's present and
# submit locks, try-locked only after the ring and the recording, so no wait above holds them.
assert prepare.index('std::lock_guard lock(g_transportMutex)') < prepare.index('try_lock_for')
assert prepare.index('RecordWork(') < prepare.index('try_lock_for')
assert prepare.index('WaitSlot(') < prepare.index('try_lock_for')
assert prepare.index('borrowedPresent.try_lock_for') < prepare.index('borrowedSubmit.try_lock_for')
assert prepare.index('try_lock_for') < prepare.index('submit(workQueue, info.waitSemaphoreCount')
present_hook = code(function(host, 'VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, '
                                   'const VkPresentInfoKHR* info, void* returnAddress)'))
assert present_hook.index('presentLock = std::unique_lock(locks->present)') < present_hook.index('Prepare(')
# The round trip's own submits go below every hook on vkQueueSubmit (the host's trampoline), and its device functions
# are resolved below OptiScaler's hooks: no other hook's lock is ever taken inside the transport's.
assert 'dev->fn.queueSubmit = below;' in code(function(host, 'VkResult VKAPI_CALL hkCreateDevice('))
assert 'out = reinterpret_cast<Pfn>(VulkanHooks::GetDeviceProcAddr(device, name));' in body
# A cross-family chain cut short after the release still hands the present a valid image: the recovery is recorded
# with the rest, before any wait is consumed.
assert prepare.index('RecordRecovery(') < prepare.index('submit(queue, info.waitSemaphoreCount')
assert prepare.index('RecordWork(') < prepare.index('submit(workQueue, info.waitSemaphoreCount')
assert prepare.index('RecordHandOver(') < prepare.index('submit(queue, info.waitSemaphoreCount')
assert prepare.count('info.pWaitSemaphores') == 2  # one submit per route takes them
# No line written and nothing called down while the transport's lock is held.
for name in ('Prepared Prepare(', 'void AfterPresent(', 'Retired RetireSwapchain(', 'std::string TeardownDevice(',
             'uint32_t Poll(', 'bool WaitSlot('):
    held = code(function(host, name))
    assert 'LOG_' not in held and 'next(' not in held, name

# Nothing that can reach Vulkan, or OptiScaler's delay-loaded import of vulkan-1.dll (whose first use runs the loader
# hook and Install, which takes the same lock), is called with g_installMutex held.
for name in ('void HookDevice(VkDevice device)', 'void HookPresent(VkDevice device)\n{'):
    held = code(function(host, name))
    held = held[held.index('std::lock_guard lock(g_installMutex);'):]
    for forbidden in ('GetDeviceProcAddr(', 'QueueCall(device', 'get("', 'GetProcAddress_'):
        assert forbidden not in held, (name, forbidden)
created = code(function(host, 'VkResult VKAPI_CALL hkCreateDevice('))
for block in re.findall(r'std::lock_guard lock\(g_installMutex\);(.*?)\n\s*\}', created, re.S):
    assert 'GetDeviceProcAddr' not in block, block

# DEVELOPMENT.md rule 8: nothing of a lost device is destroyed, and retirement drains before it frees.
for name in ('Retired RetireSwapchain(', 'std::string TeardownDevice('):
    retire = code(function(host, name))
    first_destroy = min(retire.index(x) for x in ('DestroySlot(', 'destroySemaphore(') if x in retire)
    assert retire.index('if (dev.lost)') < first_destroy, name
    assert retire.index('WaitSlot(') < first_destroy or name.startswith('std::string'), name

# Destroys retire the record before the downstream call, while the handle cannot be handed out again.
for name, record, down in (('OnDestroyDevice', 'OnDeviceDestroyed', 'next(device, allocator)'),
                           ('OnDestroySwapchain', 'OnSwapchainDestroyed', 'next(device, swapchain, allocator)'),
                           ('hkDestroyInstance', 'OnInstanceDestroyed', 'o_DestroyInstance('),
                           ('hkDestroySurfaceKHR', 'OnSurfaceDestroyed', 'o_DestroySurface(')):
    start = host.index(name + '(')
    segment = host[start:host.index('\n}\n', start)]
    assert segment.index(record) < segment.index(down), name

# The registry lock is never held across a call into Vulkan or the log: every lock_guard on it lives in a block
# whose only calls are the registry's.
blocks = re.findall(r'std::lock_guard lock\(g_registryMutex\);(.*?)\n\s*\}', host, re.S)
assert len(blocks) == host.count('std::lock_guard lock(g_registryMutex);') >= 14, len(blocks)
for block in blocks:
    assert 'LOG_' not in block and 'next(' not in block and 'o_' not in block and 'g_get' not in block, block

# The present hook goes in at a device's swapchain creation, after the bundle's own (installed in its vkCreateDevice),
# so it runs first: application -> NR -> overlay -> present.
assert 'HookPresent(device);' in code(function(host, 'VkResult OnCreateSwapchain('))
assert 'vkQueuePresentKHR' not in code(function(host, 'void HookDevice(VkDevice device)'))

# Config: the key, its four points, and an ini comment that says what it does today.
assert 'CustomOptional<bool> DlssNrVulkanPresentHost { false };' in config_h
assert 'DlssNrVulkanPresentHost.set_from_config(readBool("DlssNr", "VulkanPresentHost"));' in config_cpp
assert re.search(r'ini\.SetValue\("DlssNr", "VulkanPresentHost",\s*GetBoolValue\(Instance\(\)->DlssNrVulkanPresentHost',
                 config_cpp)
section = ini[ini.index('[DlssNr]'):]
following = re.search(r'^\[', section[1:], re.M)
section = section[:following.start() + 1] if following else section
assert re.search(r'^VulkanPresentHost=auto$', section, re.M)
comment = section[:section.index('VulkanPresentHost=auto')]
comment = comment[comment.rindex('\n\n'):]
for words in ('Experiment', 'runs no NR', 'copies the presented image', 'back unchanged', 'TRANSFER_SRC',
              'Default (auto) is false'):
    assert words in comment, f'the ini comment should say {words!r}'

# The project builds it.
for item in (r'<ClCompile Include="..\retoque\dlssnr\DlssNr_VkPresentHost.cpp" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentHost.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentRegistry.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentTransport.h" />'):
    assert item in project, item

print('PASS: source guards (one gated call site ahead of the skip; only the usage and the waits edited, each where the '
      'round trip decided it; no refusal after a wait is spent; locks in order; drains before frees, nothing destroyed '
      'on a lost device; destroys before the call down; config and project)', flush=True)

# --- The registry on the host -----------------------------------------------------------------------------------

vulkan = root / 'external/vulkan/include'
assert (vulkan / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-vk-present-host-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I', str(vulkan), '-I', str(dlssnr),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
