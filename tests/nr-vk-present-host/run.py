#!/usr/bin/env python3
"""The Vulkan present host, stage 1 (docs/dlssnr/design/nr-vulkan-present-host.md): the production registry header
compiled on the host and driven through its state machine, and source guards that the host is inert when off and only
observes when on. No Vulkan device, no Wine; tests/vulkan-present-host runs the real DLL."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
dlssnr = root / 'retoque/dlssnr'

registry = (dlssnr / 'DlssNr_VkPresentRegistry.h').read_text()
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

# The registry is standard library only, so this suite compiles it as production does.
includes = sorted(l.strip() for l in registry.splitlines() if l.startswith('#include'))
assert all(re.fullmatch(r'#include <\w+>', l) for l in includes), includes

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
assert code(hooks).count('VkPresentHost') == 2  # the include and the call: nothing else of the bundle changed for it
assert '#include <dlssnr/DlssNr_VkPresentHost.h>' in hooks

install = flat(code(function(host, 'void DlssNr::VkPresentHost::Install(HMODULE vulkan1)')))
gate = ('{ if (vulkan1 == nullptr || !Config::Instance()->DlssNrEnabled.value_or_default() || '
        '!Config::Instance()->DlssNrVulkanPresentHost.value_or_default()) return;')
assert install.startswith(gate), install[:300]
# Under the gate, the marker decides nothing: vulkanHooksSkipped is read only for the log line.
assert host.count('vulkanHooksSkipped') == 1
assert re.search(r'LOG_INFO\([^;]*State::Instance\(\)\.vulkanHooksSkipped', code(host), re.S)

# Observation only. Nothing in the host creates, submits, waits, records or changes; none of the bundle's present work
# (Reflex, the frame limiter, the frozen-feature tick, upscaler timing, the overlay) is called; no NR is evaluated.
body = code(host)
for forbidden in ('vkQueueSubmit', 'vkCreateImage', 'vkCreateBuffer', 'vkCreateCommandPool', 'vkCreateSemaphore',
                  'vkCreateFence', 'vkAllocate', 'vkCmd', 'vkDeviceWaitIdle', 'vkQueueWaitIdle', 'vkWaitForFences',
                  'vkBeginCommandBuffer', 'ReflexHooks', 'FrameLimit', 'TickFrozenCheck', 'ReadUpscalerTime',
                  'upscaleTimes', 'MenuOverlay', 'VulkanSpoofing', 'InputCommon', 'EvaluateAfterUpscaleVk', 'DlssNr_Vk',
                  'ShutdownVk', 'swapchainApi', 'localCreateInfo', 'localPresentInfo', 'imageUsage |=', 'memcpy'):
    assert forbidden not in body, f'the present host must not use {forbidden}'

# OptiScaler's own Vulkan work is ScopedSkipVulkanHooks alone. creatingD3DDevice is also set around the game's own
# D3D12CreateDevice, CreateDXGIFactory and D3D11 device creation, inside which vkd3d-proton and DXVK make the very
# instance and device this host exists for; reading it as "internal" hid every translated title's presents.
assert 'creatingD3DDevice' not in body
assert re.search(r'bool Internal\(\) \{ return ScopedSkipVulkanHooks::Active\(\); \}', body)

# The Vulkan calls it makes of its own are the three read-only queries; everything else goes down a trampoline.
own_calls = set(re.findall(r'\b(g_\w+)\(', body))
assert own_calls == {'g_getDeviceProcAddr', 'g_getQueueFamilies', 'g_getSurfaceCapabilities'}, own_calls

# Every hook hands its own arguments down, unchanged, exactly once.
for call in ('o_CreateInstance(info, allocator, instance)', 'o_DestroyInstance(instance, allocator)',
             'o_EnumeratePhysicalDevices(instance, count, devices)', 'o_CreateDevice(physical, info, allocator, device)',
             'o_CreateWin32Surface(instance, info, allocator, surface)', 'o_DestroySurface(instance, surface, allocator)',
             'next(device, allocator)', 'next(device, family, index, queue)', 'next(device, info, queue)',
             'next(device, info, allocator, swapchain)', 'next(device, swapchain, allocator)',
             'next(device, swapchain, count, images)', 'next(device, swapchain, timeout, semaphore, fence, index)',
             'next(device, info, index)', 'next(queue, info)'):
    assert body.count(call) == 1, f'expected the pass-through {call} once, found {body.count(call)}'
for hook_name in ('OnPresent', 'OnCreateSwapchain', 'OnGetQueue', 'OnGetQueue2', 'OnDestroyDevice',
                  'OnDestroySwapchain', 'OnGetImages', 'OnAcquire', 'OnAcquire2'):
    assert re.search(r'\b' + hook_name + r'\(PFN_vk\w+ next,', host), hook_name
# The present callback calls down with the caller's own pointer: no copy to edit.
present = code(function(host, 'VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, '
                              'const VkPresentInfoKHR* info, void* returnAddress)'))
assert present.rstrip('} \n').endswith('return next(queue, info);'), present[-200:]

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
assert len(blocks) == host.count('std::lock_guard lock(g_registryMutex);') >= 12, len(blocks)
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
for words in ('Experiment', 'only watches', 'runs no NR', 'submits nothing', 'Default (auto) is false'):
    assert words in comment, f'the ini comment should say {words!r}'

# The project builds it.
for item in (r'<ClCompile Include="..\retoque\dlssnr\DlssNr_VkPresentHost.cpp" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentHost.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentRegistry.h" />'):
    assert item in project, item

print('PASS: source guards (one gated call site ahead of the skip, observe-only host, pass-through hooks, '
      'destroys before the call down, the registry lock never across a call, config and project)', flush=True)

# --- The registry on the host -----------------------------------------------------------------------------------

vulkan = root / 'external/vulkan/include'
assert (vulkan / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-vk-present-host-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I', str(vulkan), '-I', str(dlssnr),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
