#!/usr/bin/env python3
"""The Vulkan present host, stages 1 to 3 (docs/dlssnr/design/nr-vulkan-present-host.md): the production registry,
transport and cold-NR headers and the model's one-owner arbiter compiled on the host and driven through their state
machines and decisions, and source guards that the host is inert when off and, when on, changes nothing but an
eligible swapchain's usage, a transported present's waits and a carried device's extension list, and that the model's
frames are counted, freed and handed over only on completion. No Vulkan device, no Wine; tests/vulkan-present-host runs
the real DLL."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
dlssnr = root / 'retoque/dlssnr'

registry = (dlssnr / 'DlssNr_VkPresentRegistry.h').read_text()
transport = (dlssnr / 'DlssNr_VkPresentTransport.h').read_text()
nr = (dlssnr / 'DlssNr_VkPresentNr.h').read_text()
build = (dlssnr / 'DlssNr_VkPresentBuild.h').read_text()
owner = (dlssnr / 'DlssNr_VkModelOwner.h').read_text()
model_h = (dlssnr / 'DlssNr_VkPresentModel.h').read_text()
feature_vk = (dlssnr / 'DlssNrFeature_Vk.cpp').read_text()
forwarder = (dlssnr / 'forwarder/dlssnr_forwarder.cpp').read_text()
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


def empty_submit_calls(text):
    """Find literal zero-count/null-array QueueSubmit(2/KHR) calls, including dispatch-table spellings.

    This is a source guard, not data-flow analysis: forwarded application arguments are deliberately untouched.
    Match argument nesting so a queue lookup or cast cannot hide the second/third argument.
    """
    text = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m: re.sub(r'[^\n]', ' ', m[0]), text, flags=re.S)
    for match in re.finditer(r'\b\w*queuesubmit(?:2(?:khr)?)?\s*\(', text, re.I):
        args, start, depth = [], match.end(), 0
        for i in range(start, len(text)):
            c = text[i]
            if c == ')' and depth == 0:
                args.append(text[start:i].strip())
                break
            if c == ',' and depth == 0:
                args.append(text[start:i].strip())
                start = i + 1
            elif c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
        if len(args) != 4:
            continue
        count, submits = (re.sub(r'[()\s]', '', a) for a in args[1:3])
        zero = r'(?:0[xX])?0+[uUlL]*'
        if re.fullmatch(zero, count) or re.fullmatch(r'(?:nullptr|NULL|VK_NULL_HANDLE|' + zero + r')', submits):
            yield text.count('\n', 0, match.start()) + 1, args


# Prove the scanner catches either bad argument, across both APIs and dispatch-table names; comments do not count.
for api in ('vkQueueSubmit', 'dev.fn.queueSubmit', 'vkQueueSubmit2', 'dev->fn.queueSubmit2KHR'):
    for args in ('q, 0, &info, f', 'getQueue(dev, index), 0U, &info, f', 'q, 1, nullptr, f',
                 'q, 1, NULL, f', 'q, 1, (VK_NULL_HANDLE), f', 'q, 1, 0, f'):
        assert list(empty_submit_calls(f'{api}({args});')), (api, args)
    assert not list(empty_submit_calls(f'{api}(q, 1, &info, VK_NULL_HANDLE);'))
    assert not list(empty_submit_calls(f'// {api}(q, 0, nullptr, f);\n/* {api}(q, 0, nullptr, f); */'))

bad_submits = []
for path in sorted((root / 'retoque').rglob('*')):
    if path.suffix in ('.cpp', '.h', '.hpp', '.cc', '.cxx', '.c'):
        bad_submits += [f'{path.relative_to(root)}:{line}: {args}'
                        for line, args in empty_submit_calls(path.read_text())]
assert not bad_submits, 'fork submits need a non-null, non-empty submit array:\n' + '\n'.join(bad_submits)
print('PASS: every fork QueueSubmit/QueueSubmit2/KHR call avoids literal zero count and null submit arrays')


# --- Source guards ----------------------------------------------------------------------------------------------

# The headers are standard library only (the registry includes the transport header, the transport header the cold-NR
# one, the cold-NR one the build's), so this suite compiles them as production does.
for text in (build, owner):
    includes = sorted(l.strip() for l in text.splitlines() if l.startswith('#include'))
    assert all(re.fullmatch(r'#include <\w+>', l) for l in includes), includes
includes = sorted(l.strip() for l in nr.splitlines() if l.startswith('#include'))
assert includes[0] == '#include "DlssNr_VkPresentBuild.h"', includes
assert all(re.fullmatch(r'#include <\w+>', l) for l in includes[1:]), includes
includes = sorted(l.strip() for l in transport.splitlines() if l.startswith('#include'))
assert includes[0] == '#include "DlssNr_VkPresentNr.h"', includes
assert all(re.fullmatch(r'#include <\w+>', l) for l in includes[1:]), includes
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
# tick, upscaler timing, the overlay, spoofing), no NGX of its own (the model is DlssNrFeature_Vk.cpp's, reached through
# DlssNr_VkPresentModel.h), not the after-upscale route's state, and no device-wide idle wait (DEVELOPMENT.md's drain is
# done on the host's own fences, which needs no queue).
body = code(host)
for forbidden in ('ReflexHooks', 'FrameLimit', 'TickFrozenCheck', 'ReadUpscalerTime', 'upscaleTimes', 'MenuOverlay',
                  'VulkanSpoofing', 'InputCommon', 'EvaluateAfterUpscaleVk', 'DlssNr_Vk(', 'ShutdownVk', 'swapchainApi',
                  'NGX', 'NVSDK', 'DeviceWaitIdle', 'memcpy', 'localCreateInfo', 'imageUsage |=', 'g_vk'):
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
                     'g_getImageFormatProperties', 'g_getFormatProperties', 'g_enumerateDeviceExtensions',
                     'g_getInstanceProcAddr'}, own_calls

# Every hook hands its arguments down exactly once on each path, and only two things are ever edited on the way.
for call, times in (('o_CreateInstance(info, allocator, instance)', 1), ('o_DestroyInstance(instance, allocator)', 1),
                    ('o_EnumeratePhysicalDevices(instance, count, devices)', 1),
                    ('o_CreateDevice(physical, edited ? &withExtensions : info, allocator, device)', 1),
                    ('o_CreateWin32Surface(instance, info, allocator, surface)', 1),
                    ('o_DestroySurface(instance, surface, allocator)', 1), ('next(device, allocator)', 1),
                    ('next(device, family, index, queue)', 1), ('next(device, info, queue)', 1),
                    ('next(device, info, allocator, swapchain)', 1), ('next(device, down, allocator, swapchain)', 1),
                    ('next(device, swapchain, allocator)', 2), ('next(device, swapchain, count, images)', 1),
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

# 3. The device (stage 3): the caller's create info unless the model's extensions were added; then a copy whose
#    extension list, and nothing else, is the caller's plus those the device offers, decided before the call (never by
#    retrying it), and only for a device the host will carry presents on.
device_hook = code(function(host, 'VkResult VKAPI_CALL hkCreateDevice('))
assert re.findall(r'\bwithExtensions\.(\w+)\s*=', device_hook) == ['enabledExtensionCount', 'ppEnabledExtensionNames',
                                                                     'pNext', 'pNext']
assert 'withExtensions = *info;' in device_hook
# The pNext edit: one feature struct of the host's, prepended, the application's chain behind it unchanged; decided by
# PlanBufferDeviceAddress (never beside a struct of the chain that could conflict), only where the extensions are ready.
assert device_hook.count('.pNext = const_cast<void*>(info->pNext);') == 2
assert device_hook.index('if (extensions.ready)') < device_hook.index('PlanBufferDeviceAddress(')
assert device_hook.index('PlanDeviceExtensions(') < device_hook.index('o_CreateDevice(')
assert device_hook.index('if (!internal && info != nullptr)') < device_hook.index('PlanDeviceExtensions(')
assert device_hook.count('o_CreateDevice(') == 1

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
assert prepare.index('TransportLock lock;') < prepare.index('try_lock_for')
assert prepare.index('RecordWork(') < prepare.index('try_lock_for')
assert prepare.index('WaitSlot(') < prepare.index('try_lock_for')
assert prepare.index('borrowedPresent.try_lock_for') < prepare.index('borrowedSubmit.try_lock_for')
assert prepare.index('try_lock_for') < prepare.index('submit(workQueue, info.waitSemaphoreCount')
present_hook = code(function(host, 'VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, '
                                   'const VkPresentInfoKHR* info, void* returnAddress)'))
assert present_hook.index('presentLock = std::unique_lock(locks->present)') < present_hook.index('Prepare(')
# The round trip's own submits use the host's trampoline. With the bundle installed the next layer can still be
# Vulkan_wDx12, so the submit-array guard above applies even to fence-only drains.
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
             'uint32_t Poll(', 'bool WaitSlot(', 'NrDecision DecideNr(', 'void StandDown(', 'void TakeModelLines(',
             'std::string MissingFormatFeatures(', 'void Observed(', 'void Housekeep(', 'void StartHandBack(',
             'void PollHandBack(', 'bool EndHandBack('):
    held = code(function(host, name))
    assert 'LOG_' not in held and 'next(' not in held, name
assert 'LOG_' not in code(function(host, 'bool DrainForDestroy('))

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

# --- Stage 3: the model inside the round trip ---------------------------------------------------------------------

# Decided before anything is recorded and before any wait is spent: the decision, then the recording (the model's pass
# inside the work buffer, between the blit in and the blit out), then the queue locks and the submits.
assert prepare.index('DecideNr(') < prepare.index('RecordWork(') < prepare.index('try_lock_for')
record = code(function(host, 'Recorded RecordWork('))
assert record.index('cmdCopyImage(buffer, image,') < record.index('cmdBlitImage(buffer, work,') < \
    record.index('dev.model->Record(*nr)') < record.index('cmdBlitImage(buffer, nr->image,') < \
    record.index('cmdCopyImage(buffer, work,')
# A model frame counts only once its work buffer was submitted: exactly the two submits that carry it, each on success.
assert prepare.count('recording.Submitted(slot);') == 2
same_queue = prepare[prepare.index('if (!cross)'):prepare.index('else', prepare.index('if (!cross)'))]
assert same_queue.index('if (result != VK_SUCCESS)') < same_queue.index('recording.Submitted(slot);')
assert re.search(r'if \(second == VK_SUCCESS\)\s*\{\s*chain = slot\.toPresent;\s*recording\.Submitted\(slot\);', prepare)
# Anything else discards it (the recording's destructor), and a pass that failed while recording is replaced by the
# plain round trip.
assert '~NrRecording() { Discard(); }' in flat(code(host))
assert re.search(r'if \(work == Recorded::NrFailed\)\s*\{\s*recording\.Discard\(\);', prepare)
# A model frame is seen complete only where a slot's fence was: Observed, from Poll and WaitSlot.
for name in ('uint32_t Poll(', 'bool WaitSlot('):
    assert 'Observed(dev, slot, times);' in code(function(host, name)), name
assert code(host).count('->Completed(') == 1
# The model is freed only with nothing of it in flight, and abandoned (nothing destroyed) on a lost device or when
# frames never completed; the owner token goes back after either.
stand_down = code(function(host, 'void StandDown(DeviceTransport& dev)'))
assert stand_down.index('dev.lost') < stand_down.index('Release()') < stand_down.index('ReleaseModelFromPresentHost')
assert stand_down.index('InFlight() != 0') < stand_down.index('Release()')
teardown = code(function(host, 'std::string TeardownDevice('))
assert teardown.index('InFlight()') < teardown.index('Release()') < teardown.index('ShutdownPresentNgx(')
assert teardown.count('AbandonModel(dev);') == 2 and teardown.count('model->Abandon()') == 1
# The token goes back and NGX on the device goes with it whatever became of the model (stood down earlier included);
# on a lost device or with frames never completed, NGX is only forgotten.
assert 'DlssNr::ShutdownPresentNgx(dev.device, unproven, dev.lines);' in teardown
abandon_model = code(function(host, 'void AbandonModel(DeviceTransport& dev)'))
assert 'ShutdownPresentNgx(dev.device, true, dev.lines)' in abandon_model and 'ReleaseModelFromPresentHost' in abandon_model
# Whatever becomes of a present, a model on its device stands down when NR is off or another route asks: before the
# refusals that return without looking at the transport.
assert prepare.index('Housekeep(*dev);') < prepare.index('return skip(Skip::MultiSwapchain)')
housekeep = code(function(host, 'void Housekeep(DeviceTransport& dev)'))
assert 'DlssNrEnabled' in housekeep and 'PresentHostMustYield' in housekeep and 'StandDown(dev)' in housekeep
# A discard on a lost device abandons the model rather than releasing what the recording built.
assert re.search(r'if \(dev->lost\)\s*dev->model->Abandon\(\);\s*else\s*dev->model->Discarded\(ticket\);', flat(code(host)))
# Live off, and another route asking: stand down. The claim comes after every cheap refusal.
decide = code(function(host, 'NrDecision DecideNr('))
# A swapchain holds the model only once the model was admitted for it.
assert decide.index('->Admit(') < decide.index('dev.modelSwapchain = sc.id;')
assert decide.index('DlssNrEnabled') < decide.index('StandDown(dev)')
assert decide.index('PresentHostMustYield') < decide.index('ClaimModelForPresentHost') < decide.index('->Admit(')
for cheap in ('sc.colour.sdr', 'dev.nrReady', 'MissingFormatFeatures('):
    assert decide.index(cheap) < decide.index('ClaimModelForPresentHost'), cheap
# Present semaphores of a destroyed swapchain are freed after the destroy returned, never before it.
destroy = host[host.index('void OnDestroySwapchain('):]
destroy = destroy[:destroy.index('\n}\n')]
assert destroy.index('next(device, swapchain, allocator);') < destroy.index('destroySemaphore(')

# The model (DlssNrFeature_Vk.cpp): per owner, its own state, the forwarder shared; the after-upscale route asks the
# arbiter where it commits to a frame; the present model's encode is passthrough everywhere it is reproduced.
model = feature_vk[feature_vk.index('struct VkPresentModel::Impl'):]
after_upscale = code(function(feature_vk, 'void EvaluateAfterUpscaleVk('))
assert after_upscale.index('ClaimModelForAfterUpscale(device)') < after_upscale.index('g_vk.instance = instance;')
assert after_upscale.index('colour == nullptr || depth == nullptr || motion == nullptr') < \
    after_upscale.index('ClaimModelForAfterUpscale(device)')
assert 'g_vk.' not in code(model), 'the present model must not touch the after-upscale route\'s state'
record_model = code(function(feature_vk, 'bool VkPresentModel::Record(const VkPresentNrFrame& frame)'))
assert 'encode.Passthrough = 1u;' in record_model and 'encode.HighlightRecovery = 0u;' in record_model
# One set of constants, the encode's; the downsample and the resolve are copies of it, so the passthrough gate holds in
# every place the shader reproduces the encode.
assert re.findall(r'DlssNrConstants (\w+)', record_model) == ['encode', 'down', 'resolve'], record_model
assert 'DlssNrConstants down = encode;' in record_model and 'DlssNrConstants resolve = encode;' in record_model
admit = code(function(feature_vk, 'VkPresentAdmission VkPresentModel::Admit(uint32_t width, uint32_t height, '))
assert 'std::clamp(DlssNr::WorkingScale(), 0.25f, 1.0f)' in admit  # never above native on zero guides
retire = code(function(feature_vk, '    void RetireProduct()'))
assert 'old.waitFor = pending;' in retire and 'DeviceWaitIdle' not in code(model)
completed = code(function(feature_vk, 'std::optional<double> VkPresentModel::Completed(uint64_t ticket)'))
assert 'm.FreeRetired();' in completed and 'VK_QUERY_RESULT_WAIT_BIT' not in completed
release = code(function(feature_vk, 'void VkPresentModel::Release()'))
assert 'Shutdown' not in release, 'NGX goes with the device (ShutdownPresentNgx), not with the model'
# The pass's slot ring: a frame is admitted only while the oldest one in flight is less than a ring behind it.
assert 'm.dispatches + 3 - oldest > kPresentPassSlots' in admit
# A queued job is called off first; a running one, frames in flight or a build-lost device abandon instead of freeing.
assert release.index('m.CancelQueued();') < \
    release.index('if (!m.pending.empty() || m.JobState() == BuildState::Running || m.deviceLost)') < \
    release.index('g_fwd.release(')
abandon = code(function(feature_vk, 'void VkPresentModel::Abandon()'))
assert not re.search(r'\bvk[A-Z]\w*\(|g_fwd\.|Destroy', abandon), abandon
# The model's calls go through the forwarder (its __try isolation), and its fault ends it.
assert 'FaultLatched()' in admit and 'FaultLatched()' in code(function(feature_vk, 'bool VkPresentModel::Record('))
assert 'dlssnr_vk_shutdown(void* device)' in forwarder
# NGX's device-specific shutdown takes an out count (as the D3D12 one does); called without one, the core wrote through a
# null pointer at vkDestroyDevice (2026-10-14).
assert 'Guarded(g_vk.shutdown1, device, &remaining)' in forwarder
assert 'using PFN_NrVkShutdown1 = int(__cdecl*)(void*, unsigned int*);' in forwarder
assert 'using PFN_Shutdown1 = NVSDK_NGX_Result (*)(VkDevice, unsigned int*);' in feature_vk
assert 'VULKAN_Shutdown1()(' not in code(model) and 'shutdown(device, &remaining);' in feature_vk

# The way back from the after-upscale route (DOOM Eternal, 2026-10-14; DlssNr_VkModelOwner.h). Begun only with no
# model of the host's on the device and the route quiet there; followed at every present, before any refusal.
housekeep_flat = flat(housekeep)
assert housekeep.index('AfterUpscaleIdleOn(dev.device)') < housekeep.index('HandBackBlockReason(') < \
    housekeep.index('StartHandBack(dev);')
assert 'HandBackBlocked(dev, reason);' in housekeep and 'dev.handBackDiagnostics.Reset();' in housekeep
assert 'handBackNotBefore =' not in housekeep  # F7 off does not postpone the already-due hand-back
could_run = code(function(host, 'std::string ModelUnavailable(DeviceTransport& dev)'))
for needed in ('dev.nrReady', 'dev.lost', 'sc.colour.sdr', 'sc.nrOff.empty()', 'sc.retired',
               'PresentHostMustYield(dev.device)', 'MissingFormatFeatures(dev, sc.colour.raw)'):
    assert needed in could_run, needed
assert 'DlssNrEnabled.value_or_default()' in housekeep
# The real try-only lock helper is shared with host tests; StartHandBack's RAII vector releases earlier locks.
start_hb = code(function(host, 'void StartHandBack(DeviceTransport& dev)'))
try_locks = code(function(nr, 'inline const char* TryHandBackQueueLocks('))
assert try_locks.count('std::try_to_lock') == 2 and 'try_lock_for' not in try_locks and '.lock()' not in try_locks
assert try_locks.index('locks.present, std::try_to_lock') < try_locks.index('locks.submit, std::try_to_lock')
assert start_hb.index('TryHandBackQueueLocks(') < start_hb.index('createFence(') < \
    start_hb.index('BeginAfterUpscaleHandBack(') < start_hb.index('queueSubmit(')
assert 'HandBackBlocked(dev,' in start_hb and '1000);' in start_hb
assert 'fence creation returned' in function(host, 'void StartHandBack(DeviceTransport& dev)')
assert prepare.index('Housekeep(*dev);') < prepare.index('prep.warnings.push_back(std::move(line));', prepare.index('Housekeep(*dev);')) < \
    prepare.index('return skip(Skip::MultiSwapchain);')
assert 'call.nowMs = PresentNowMs();' in present_hook
assert 'dev.fn.queueSubmit(queues[submitted].first, 1, &submitInfo, fences[submitted])' in start_hb
assert start_hb.index('queueSubmit(') < start_hb.index('held.clear();')
# A failed drain submit calls the hand-back off; a lost device destroys nothing, fences included.
failed = start_hb[start_hb.index('if (failure != VK_SUCCESS)'):]
assert failed.index('CancelAfterUpscaleHandBack(') < failed.index('MarkLost(dev);') < failed.index('destroyFrom(submitted)')
# Freed only once every drain fence was seen signalled, on a device not lost, and never after a failed drain submit.
poll_hb = code(function(host, 'void PollHandBack(DeviceTransport& dev)'))
assert poll_hb.index('if (dev.lost)') < poll_hb.index('getFenceStatus(')
assert poll_hb.index('while (!hb.fences.empty())') < poll_hb.index('if (hb.calledOff)') < \
    poll_hb.index('FinishAfterUpscaleHandBack(')
assert poll_hb.count('FinishAfterUpscaleHandBack(') == 1 and 'waitForFences' not in poll_hb
assert re.search(r'if \(status == VK_NOT_READY\)\s*\{.*?return;\s*\}', poll_hb, re.S)
# The device going: the drain's fences within the drain's bound, the hand-back off, then the after-upscale route's
# objects on the device abandoned and its token released, before NGX on it is shut down; lost, nothing waited for.
end_hb = code(function(host, 'bool EndHandBack(DeviceTransport& dev, uint64_t budgetNs)'))
assert end_hb.index('CancelAfterUpscaleHandBack(') < end_hb.index('waitForFences(')
assert end_hb.index('!dev.lost') < end_hb.index('waitForFences(') < end_hb.index('destroyFence(')
for name, text in (('TeardownDevice', teardown), ('AbandonModel', abandon_model)):
    assert text.index('EndHandBack(dev') < text.index('AfterUpscaleDeviceGone(dev.device') < \
        text.index('ShutdownPresentNgx(dev.device'), name
assert 'EndHandBack(dev, 0);' in abandon_model and 'AfterUpscaleDeviceGone(dev.device, true, dev.lines);' in abandon_model
# The route's objects are freed, and NGX shut down, at the device's destruction only where the GPU is proven done:
# nothing of the host's unproven and every queue drained now behind fences of its own, within a bound (no device-wide
# wait, which a hung GPU would make unbounded under the transport's lock).
assert ('const bool unproven = dev.lost || forgetOnly || abandoned != 0 || !drained || !DrainForDestroy(dev, kDrainNs);'
        in flat(teardown))
assert teardown.index('DrainForDestroy(') < teardown.index('AfterUpscaleDeviceGone(dev.device, unproven, dev.lines);') \
    < teardown.index('ShutdownPresentNgx(dev.device, unproven, dev.lines);')
drain_destroy = flat(code(function(host, 'bool DrainForDestroy(DeviceTransport& dev, uint64_t budgetNs)')))
assert 'if (dev.lost || queues.empty()) return false;' in drain_destroy
assert drain_destroy.index('queueSubmit(queue, 1, &submitInfo, fence)') < drain_destroy.index('waitForFences(')
# Fences destroyed only once seen signalled (or never submitted): a failed wait keeps them.
after_wait = drain_destroy[drain_destroy.index('waitForFences('):]
assert after_wait.index('if (result != VK_SUCCESS) return false;') < after_wait.index('destroyFence(')

# The route's side (DlssNrFeature_Vk.cpp). Its lock never waited for under the host's; the token released before
# anything is freed, and nothing freed unless the hand-back finished; freed without a device-wide wait (the drain proved
# it idle), which ShutdownVk(true) still does first.
finish = code(function(feature_vk, 'AfterUpscaleHandBack FinishAfterUpscaleHandBack(VkDevice device, '))
assert 'std::unique_lock<std::mutex> route(g_vkMutex, std::try_to_lock);' in finish
assert finish.index('g_owner.FinishHandBack(reinterpret_cast<uint64_t>(device))') < finish.index('return AfterUpscaleHandBack::Cancelled;') < \
    finish.index('return AfterUpscaleHandBack::NotRetiring;') < finish.index('FreeRouteObjects();')
assert finish.count('FreeRouteObjects();') == 1 and 'DeviceWaitIdle' not in finish
free_route = code(function(feature_vk, 'void FreeRouteObjects()'))
assert 'DeviceWaitIdle' not in free_route and 'g_fwd.release(g_vk.feature)' in free_route
assert 'g_vk.floatSlotKnown = false;' in free_route
shutdown_vk = code(function(feature_vk, 'void ShutdownVk(bool deviceAlive)'))
assert shutdown_vk.index('vkDeviceWaitIdle(g_vk.device)') < shutdown_vk.index('FreeRouteObjects();')
assert code(feature_vk).count('FreeRouteObjects();') == 3
# The idle clock counts from the end of the route's recording: a guard right after its claim stamps it when the evaluate
# returns, whatever path it takes.
claimed = flat(after_upscale)
assert claimed.index('std::lock_guard<std::mutex> lock(g_vkMutex);') < \
    claimed.index('if (!ClaimModelForAfterUpscale(device)) return;') < claimed.index('} recordingDone;') < \
    claimed.index('g_vk.instance = instance;')  # destroyed before the route's lock: the stamp is under it
assert re.search(r'~RecordingDone\(\)\s*\{\s*std::lock_guard<std::mutex> lock\(g_ownerMutex\);\s*'
                 r'g_owner\.AfterUpscaleDone\(OwnerNowMs\(\)\);', after_upscale)
# The route records nothing before it asks: a hand-back it did not call off saw everything it recorded drained.
before_claim = after_upscale[:after_upscale.index('ClaimModelForAfterUpscale(device)')]
assert not re.search(r'vkCmd|cmdBuffer\)|\(void\*\) cmdBuffer|->Dispatch|g_fwd\.(create|evaluate)', before_claim[
    before_claim.index('std::lock_guard<std::mutex> lock(g_vkMutex);'):])
# Taking the token from nobody, the route finds the forwarder's float slot again for its own parameter block.
claim = code(function(feature_vk, 'bool ClaimModelForAfterUpscale(VkDevice device)'))
assert re.search(r'if \(!held\)\s*\{\s*g_vk\.floatSlotKnown = false;', claim)
# The device's destruction frees the route's objects on it (the host drained the device first; before NGX is shut down
# on it), or only forgets them (ShutdownVk(false)) when it is lost or its GPU was not proven done; then the token goes
# back. Nothing in the file waits for a whole device but ShutdownVk(true), which nothing calls.
gone = code(function(feature_vk, 'void AfterUpscaleDeviceGone(VkDevice device, bool forgetOnly, '))
assert re.search(r'if \(forgetOnly\)\s*\{\s*ShutdownVk\(false\);', gone), gone
assert gone.index('ShutdownVk(false);') < gone.index('FreeRouteObjects();') < gone.index('g_owner.DeviceGone(')
assert 'DeviceWaitIdle' not in gone and code(feature_vk).count('ShutdownVk(true)') == 0

print('PASS: the way back (begun only with no model here and the route quiet since its last recording ended; queues '
      'try-locked, fences made before and submitted after the hand-back began; the route freed only once every fence '
      'signalled and the token was released, without a device-wide wait; the route records nothing before it asks; '
      'the device going ends it, the route freed only where the GPU is proven done)', flush=True)

# --- Off the present path (DOOM Eternal's startup hang, 2026-10-14) -------------------------------------------------
# Nothing opaque or unbounded runs under the host's locks: the present path neither initialises NGX, brings the core up,
# creates the pass, allocates the model's images nor creates a feature; the builder thread does, and takes no lock of the
# host's or the route's. While a job is queued or running, the present path makes no NGX call for the model.
for name, text in (('Admit', admit), ('Record', record_model)):
    for forbidden in ('g_fwd.init', 'g_fwd.create', 'g_fwd.probe', 'NVNGXProxy::', 'CreateImageOn(', 'DlssNr_Vk>(',
                      'vkCreate', 'vkAllocate', 'LoadForwarderShared', 'vkCmdClearColorImage'):
        assert forbidden not in text, (name, forbidden)
assert admit.index('DecideBuild(') < admit.index('m.Adopt();') and 'm.Request(want);' in admit
assert admit.index('DecideBuild(') < admit.index('pending.size() >= VkPresentHost::kNrInFlightMax')
assert re.search(r'return \{ NrSkip::Building,', admit)
assert 'm.Building()' in record_model.split('return false;')[0]  # no evaluate while a job runs
free_retired = code(function(feature_vk, '    void FreeRetired()'))
assert re.match(r'\{\s*if \(Building\(\)\)\s*return;', free_retired), free_retired
builder_part = code(feature_vk[feature_vk.index("// --- The model's builds, off the present path"):
                               feature_vk.index('struct VkPresentModel::Impl')])
run_build = code(function(feature_vk, 'void RunBuild(PresentBuildJob& job)'))
for needed in ('g_fwd.init(', 'NVNGXProxy::InitVulkan(', 'g_fwd.create(', 'CreateImageOn(', 'job.dev.submit(',
               'make_unique<DlssNr_Vk>(', 'job.pass->Prepare(cmd)', 'vkCmdClearColorImage('):
    assert needed in run_build, needed
# Its only CPU wait for the GPU is the host's submit (bounded); it never touches the route's state or locks.
for forbidden in ('g_vk.', 'g_vkMutex', 'g_ownerMutex', 'DeviceWaitIdle', 'QueueWaitIdle', 'WaitForFences', 'Sleep('):
    assert forbidden not in builder_part, forbidden
assert builder_part.count('std::thread(') == 1 and 'std::thread(' in code(function(feature_vk, '    void Post('))
# Orphaned (the device lost or destroyed): what the job made is forgotten, never freed.
assert re.search(r"if \(job\.orphaned\.load\(std::memory_order_acquire\) \|\| !free\)\s*ForgetBuild\(job\);",
                 run_build)
# The GPU's own work is waited for before the product is handed over; a timeout or a lost device frees nothing.
assert re.search(r'if \(submitted == VK_TIMEOUT\)\s*return stop\([^;]*,\s*false\);', run_build, re.S)
assert re.search(r'if \(submitted == VK_ERROR_DEVICE_LOST\)\s*\{\s*job\.lost = true;\s*return stop\([^;]*, false\);',
                 run_build, re.S)
assert re.search(r'if \(submitted == VK_INCOMPLETE\)\s*return stop\([^;]*, false\);', run_build, re.S)
# The model's state is the present path's; the builder's product is handed over only once the job's state says so.
adopt = code(function(feature_vk, '    bool Adopt()'))
assert "over->state.load(std::memory_order_acquire) == BuildState::Failed" in adopt
assert 'job->state.store(job->failure.empty() ? BuildState::Done : BuildState::Failed,' in flat(builder_part)
# Releasing and abandoning respect a job under way: never freed under the builder; abandoned orphans it.
abandon_model_fn = code(function(feature_vk, 'void VkPresentModel::Abandon()'))
assert 'm.job->orphaned.store(true, std::memory_order_release);' in abandon_model_fn
assert 'return _impl->pending.size() + (_impl->JobState() == BuildState::Running ? 1 : 0);' in flat(feature_vk)
# A job is forgotten, never destroyed, when its last reference goes; an orphaned one's product is forgotten by the
# builder and never handed over.
assert 'PresentBuildJob::~PresentBuildJob() { ForgetBuild(*this); }' in feature_vk
run_builder = code(function(feature_vk, '    void Run()'))
assert run_builder.index('RunBuild(*job);') < run_builder.index('ForgetBuild(*job);')
# The NGX device records' lock is never held across an NGX call.
for block in re.findall(r'std::lock_guard<std::mutex> lock\(g_presentNgxMutex\);(.*?)\n\s*\}', run_build, re.S):
    assert 'NVNGXProxy::' not in block and 'ShutdownPresentCore' not in block and 'g_fwd.' not in block, block

# The host: the builder's submit takes the work queue's locks as a borrower, bounded, never the transport lock, and
# waits for its fence only after giving them back; at vkDestroyDevice a build under way is waited for, bounded, before
# anything of the model's is freed; the model is asked with the round trip's work family.
build_submit = code(function(host, 'VkResult BuildSubmit(const DeviceFns& fn, VkDevice device, VkCommandBuffer cmd,'))
# Short tries: the present lock bounded per slice, the submit lock never waited for while the present lock is held,
# nothing taken blocking, never the transport lock; the fence waited for in slices after both locks are given back,
# and the orphan flag checked between slices.
assert 'present.try_lock_for(std::chrono::milliseconds(kBuildSliceMs))' in build_submit
assert 'submit.try_lock()' in build_submit and 'submit.try_lock_for' not in build_submit
assert '.lock()' not in build_submit and 'TransportLock' not in build_submit and 'g_transportMutex' not in build_submit
assert build_submit.index('submit.try_lock()') < build_submit.index('fn.queueSubmit(queue, 1, &info, fence)') < \
    build_submit.index('present.unlock();') < build_submit.index('waitForFences(')
assert re.search(r'if \(cancelled\.load\(std::memory_order_acquire\)\)\s*return VK_INCOMPLETE;', build_submit)
assert 'fn.waitForFences(device, 1, &fence, VK_TRUE, slice)' in build_submit
assert re.search(r'if \(result != VK_ERROR_DEVICE_LOST\)\s*fn\.destroyFence', build_submit)
assert re.search(r'if \(result == VK_SUCCESS\)\s*fn\.destroyFence\(device, fence, nullptr\);\s*return result;',
                 build_submit)
assert 'dev.model->Admit(sc.extent.width, sc.extent.height, workFamily)' in decide
# vkDestroyDevice: a build under way is ended -- waited for, then orphaned and waited for again -- before anything,
# the lost branch included; a build that lost the device marks it lost there and in DecideNr.
end_build = code(function(host, 'void EndModelBuild(DeviceTransport& dev)'))
assert end_build.index('WaitBuild(kBuildDestroyWaitMs)') < end_build.index('Orphan();') < \
    end_build.index('WaitBuild(kBuildOrphanWaitMs)')
assert teardown.index('EndModelBuild(dev);') < teardown.index('if (dev.lost)') < teardown.index('InFlight()')
assert re.search(r'if \(dev\.model->DeviceLost\(\)\)\s*MarkLost\(dev\);', decide)

# The stall watchdog: started only once Install is past its gate and installed; it reads the activity table and logs,
# and takes no lock.
install_body = code(function(host, 'void DlssNr::VkPresentHost::Install(HMODULE vulkan1)'))
assert install_body.index('DlssNrVulkanPresentHost.value_or_default()') < install_body.index('g_installed = true;') < \
    install_body.index('std::thread(')
watchdog = install_body[install_body.index('std::thread('):]
for forbidden in ('lock', 'Lock', 'Mutex', 'mutex'):
    assert forbidden not in watchdog, forbidden
assert 'Activities().Check(ActivityNowMs())' in watchdog and 'Sleep(500)' in watchdog
# Every entry point that can hold or wait for a host lock records an activity.
for name in ('VkResult OnPresent(', 'void OnDestroyDevice(', 'void OnDestroySwapchain(', 'VkResult OnCreateSwapchain(',
             'VkResult LockedQueueCall('):
    assert 'HostActivity activity(' in code(function(host, name)), name
assert 'g_transportMutex.lock()' in code(function(host, '    TransportLock()'))
assert 'std::lock_guard lock(g_transportMutex)' not in code(host)

print('PASS: off the present path (Admit and Record build nothing; the builder does it all, takes no lock of the '
      "host's or the route's, hands over only once its GPU work is seen complete; no NGX call while a job runs; the "
      "build's submit borrows the work queue's locks bounded; vkDestroyDevice waits for a build; the watchdog takes "
      'no lock and starts only installed)', flush=True)

print('PASS: stage-3 guards (decided before recording, counted on submit, seen complete on a fence, freed only idle, '
      'abandoned on loss; after-upscale route asks first; passthrough encode; semaphores freed after the destroy)',
      flush=True)

# --- Stage 5: synthesized motion at the present ------------------------------------------------------------------
# The estimator (SynthMotion_Vk.*) is made off the present path like the rest of the product -- its pipelines and its
# images by the builder, their initialisation in the build's own command buffer -- and only recorded on it: before the
# encode reads the working colour, its field the model's motion guide once Ready, its history advanced on submit,
# dropped on a discard, its scene-change bit read when the frame's fence was seen.
synth_vk = (root / 'retoque/shaders/synth_motion/SynthMotion_Vk.cpp').read_text()
synth_vk_h = (root / 'retoque/shaders/synth_motion/SynthMotion_Vk.h').read_text()
for forbidden in ('LOG_', 'QueueSubmit', 'WaitIdle', 'WaitForFences', 'vkGetFenceStatus', 'Sleep('):
    assert forbidden not in code(synth_vk), forbidden
for name, text in (('Admit', admit), ('Record', record_model)):
    for forbidden in ('CreateVkImages(', 'CreateVkPipelines(', 'VkUnsupported('):
        assert forbidden not in text, (name, forbidden)
assert 'SynthWanted(Config::Instance()->DlssNrSynthMotion.value_or_default(), !m.synthOff.empty(), width, height)' \
    in flat(admit)
assert 'const BuildKey want { width, height, workWidth, workHeight, family, synth };' in admit
assert 'job.synthOff = SynthMotion::VkUnsupported(pd, job.dev.apiVersion);' in run_build
assert run_build.index('CreateVkPipelines(d, job.synthPipelines') < run_build.index('vkBeginCommandBuffer(cmd')
assert run_build.index('vkBeginCommandBuffer(cmd') < \
    run_build.index('SynthMotion::CreateVkImages(d, pd, k.width, k.height, cmd, job.synthImages, job.synthOff)') < \
    run_build.index('g_fwd.create(') < run_build.index('job.dev.submit(')
free_build = code(function(feature_vk, 'void FreeBuild(PresentBuildJob& job)'))
assert 'DestroyVkImages(d, job.synthImages);' in free_build and 'DestroyVkPipelines(job.synthPipelines);' in free_build
forget_build = code(function(feature_vk, 'void ForgetBuild(PresentBuildJob& job)'))
assert 'job.synthImages = SynthMotion::VkImages {};' in forget_build and 'Destroy' not in forget_build
assert 'old.synth = synthImages;' in retire and 'estimator.Restart();' in retire
free_set = code(function(feature_vk, '    void Free(RetiredSet& set)'))
assert 'SynthMotion::DestroyVkImages(dev.device, set.synth);' in free_set
# The estimator reads the working colour before the encode, the model then gets the field or the zero guide.
assert record_model.index('m.estimator.Record(cmd, m.synthPipelines, m.synthImages, frame.view, VK_IMAGE_LAYOUT_GENERAL,') \
    < record_model.index('m.pass->Dispatch(cmd, encode,') < record_model.index('ChooseMotionGuide(') < \
    record_model.index('g_fwd.evaluate(')
assert 'SynthReset(m.reset, frame.cut, m.lastSynthMs, now)' in record_model
assert 'NrResetWithMotion(m.reset, frame.cut, sceneCut,' in flat(record_model)
assert 'motionGuide, &m.output.ngx' in flat(record_model) and 'guide.width, guide.height, 0, 0, 0, 0' in flat(record_model)
assert 'guide.scaleX, guide.scaleY);' in flat(record_model)
# Confirmed with the submit, abandoned with a discard, its scene-change bit read only at completion.
submitted = code(function(feature_vk, 'void VkPresentModel::Submitted(uint64_t ticket)'))
discarded = code(function(feature_vk, 'void VkPresentModel::Discarded(uint64_t ticket)'))
assert 'm.estimator.ConfirmExecuted();' in submitted and 'AbandonRecording' not in submitted
assert 'm.estimator.AbandonRecording();' in discarded and 'ConfirmExecuted' not in discarded
assert 'm.estimator.Completed(ticket, m.synthImages);' in completed
assert 'DestroyVkImages(m.dev.device, m.synthImages);' in release and 'DestroyVkPipelines(m.synthPipelines);' in release
assert 'm.synthImages = SynthMotion::VkImages {};' in abandon and 'm.synthPipelines = SynthMotion::VkPipelines {};' in abandon
# The host counts a frame on the field, and hands the model the instance's version (the estimator needs 1.1).
assert 'out.nrSynthesized = dev->model != nullptr && dev->model->LastFrameSynthesized();' in host
assert 'device.apiVersion = dev.apiVersion;' in host
# The project builds the estimator, and its SPIR-V is the committed one (nr-invariants checks the headers).
for item in (r'<ClCompile Include="..\retoque\shaders\synth_motion\SynthMotion_Vk.cpp" />',
             r'<ClInclude Include="..\retoque\shaders\synth_motion\SynthMotion_Vk.h" />'):
    assert item in project, item
assert 'kVkReadbackSlots = 8;' in synth_vk_h and 'kNrInFlightMax = 4;' in nr  # a slot per frame in flight, and more

print('PASS: stage-5 guards (the estimator made by the builder, its images initialised in the build\'s buffer; recorded '
      'before the encode, its field the motion guide once Ready; confirmed on submit, dropped on discard, its cut read '
      'at completion; freed with the product, forgotten on abandon; it logs, submits and waits for nothing)',
      flush=True)

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
for words in ('Experiment', 'zero guides', 'copies the presented image', 'SDR', 'TRANSFER_SRC', 'extensions',
              'Default (auto) is false'):
    assert words in comment, f'the ini comment should say {words!r}'

# The project builds it.
for item in (r'<ClCompile Include="..\retoque\dlssnr\DlssNr_VkPresentHost.cpp" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentHost.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentRegistry.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentTransport.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentNr.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentModel.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkModelOwner.h" />',
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkPresentBuild.h" />'):
    assert item in project, item

print('PASS: source guards (one gated call site ahead of the skip; only the usage and the waits edited, each where the '
      'round trip decided it; no refusal after a wait is spent; locks in order; drains before frees, nothing destroyed '
      'on a lost device; destroys before the call down; config and project)', flush=True)

# --- The registry on the host -----------------------------------------------------------------------------------

vulkan = root / 'external/vulkan/include'
assert (vulkan / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-vk-present-host-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-pthread', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-I', str(vulkan), '-I', str(dlssnr),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
