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


# --- Source guards ----------------------------------------------------------------------------------------------

# The headers are standard library only (the registry includes the transport header, the transport header the cold-NR
# one), so this suite compiles them as production does.
for text in (nr, owner):
    includes = sorted(l.strip() for l in text.splitlines() if l.startswith('#include'))
    assert all(re.fullmatch(r'#include <\w+>', l) for l in includes), includes
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
             'uint32_t Poll(', 'bool WaitSlot(', 'NrDecision DecideNr(', 'void StandDown(', 'void TakeModelLines(',
             'std::string MissingFormatFeatures(', 'void Observed(', 'void Housekeep(', 'void StartHandBack(',
             'void PollHandBack(', 'void EndHandBack('):
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
assert 'DlssNr::ShutdownPresentNgx(dev.device, forgetOnly, dev.lines);' in teardown
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
admit = code(function(feature_vk, 'VkPresentAdmission VkPresentModel::Admit(uint32_t width, uint32_t height)'))
assert 'std::clamp(DlssNr::WorkingScale(), 0.25f, 1.0f)' in admit  # never above native on zero guides
assert admit.index('pending.size() >= VkPresentHost::kNrInFlightMax') < admit.index('LoadForwarderShared()')
assert 'old.waitFor = m.pending;' in admit and 'DeviceWaitIdle' not in code(model)
completed = code(function(feature_vk, 'std::optional<double> VkPresentModel::Completed(uint64_t ticket)'))
assert 'm.FreeRetired();' in completed and 'VK_QUERY_RESULT_WAIT_BIT' not in completed
release = code(function(feature_vk, 'void VkPresentModel::Release()'))
assert 'Shutdown' not in release, 'NGX goes with the device (ShutdownPresentNgx), not with the model'
# The pass's slot ring: a frame is admitted only while the oldest one in flight is less than a ring behind it.
assert 'm.dispatches + 3 - oldest > kPresentPassSlots' in admit
assert release.index('if (!m.pending.empty())') < release.index('g_fwd.release(')
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
assert re.search(r'if \(dev\.handBack\.active\) PollHandBack\(dev\); else if \(dev\.model == nullptr && .*'
                 r'CouldRunModel\(dev\) && DlssNr::AfterUpscaleIdleOn\(dev\.device\)\) StartHandBack\(dev\);',
                 housekeep_flat), housekeep_flat
assert housekeep.index('StartHandBack(dev)') < housekeep.index('if (dev.model == nullptr)')
could_run = code(function(host, 'bool CouldRunModel(const DeviceTransport& dev)'))
for needed in ('DlssNrEnabled', 'dev.nrReady', 'dev.lost', 'sc.colour.sdr', 'sc.nrOff.empty()', '!sc.retired'):
    assert needed in could_run, needed
# Never waits: every queue's present and submit locks try-locked (a refusal tries again at the next present); the
# fences made before the hand-back begins, and submitted only after it began, each on an empty batch.
start_hb = code(function(host, 'void StartHandBack(DeviceTransport& dev)'))
assert start_hb.count('std::try_to_lock') == 2 and 'try_lock_for' not in start_hb and '.lock()' not in start_hb
assert start_hb.index('locks->present, std::try_to_lock') < start_hb.index('locks->submit, std::try_to_lock')
assert start_hb.index('try_to_lock') < start_hb.index('createFence(') < \
    start_hb.index('BeginAfterUpscaleHandBack(') < start_hb.index('queueSubmit(')
assert 'dev.fn.queueSubmit(queues[submitted].first, 0, nullptr, fences[submitted])' in start_hb
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
end_hb = code(function(host, 'void EndHandBack(DeviceTransport& dev, uint64_t budgetNs)'))
assert end_hb.index('CancelAfterUpscaleHandBack(') < end_hb.index('waitForFences(')
assert end_hb.index('!dev.lost') < end_hb.index('waitForFences(') < end_hb.index('destroyFence(')
for name, text in (('TeardownDevice', teardown), ('AbandonModel', abandon_model)):
    assert text.index('EndHandBack(dev') < text.index('AfterUpscaleDeviceGone(dev.device') < \
        text.index('ShutdownPresentNgx(dev.device'), name
assert 'EndHandBack(dev, 0);' in abandon_model

# The route's side (DlssNrFeature_Vk.cpp). Its lock never waited for under the host's; the token released before
# anything is freed, and nothing freed unless the hand-back finished; freed without a device-wide wait (the drain proved
# it idle), which ShutdownVk(true) still does first.
finish = code(function(feature_vk, 'AfterUpscaleHandBack FinishAfterUpscaleHandBack(VkDevice device, '))
assert 'std::unique_lock<std::mutex> route(g_vkMutex, std::try_to_lock);' in finish
assert finish.index('g_owner.FinishHandBack()') < finish.index('return AfterUpscaleHandBack::Cancelled;') < \
    finish.index('return AfterUpscaleHandBack::NotRetiring;') < finish.index('FreeRouteObjects();')
assert finish.count('FreeRouteObjects();') == 1 and 'DeviceWaitIdle' not in finish
free_route = code(function(feature_vk, 'void FreeRouteObjects()'))
assert 'DeviceWaitIdle' not in free_route and 'g_fwd.release(g_vk.feature)' in free_route
assert 'g_vk.floatSlotKnown = false;' in free_route
shutdown_vk = code(function(feature_vk, 'void ShutdownVk(bool deviceAlive)'))
assert shutdown_vk.index('vkDeviceWaitIdle(g_vk.device)') < shutdown_vk.index('FreeRouteObjects();')
assert code(feature_vk).count('FreeRouteObjects();') == 2
# The route records nothing before it asks: a hand-back it did not call off saw everything it recorded drained.
before_claim = after_upscale[:after_upscale.index('ClaimModelForAfterUpscale(device)')]
assert not re.search(r'vkCmd|cmdBuffer\)|\(void\*\) cmdBuffer|->Dispatch|g_fwd\.(create|evaluate)', before_claim[
    before_claim.index('std::lock_guard<std::mutex> lock(g_vkMutex);'):])
# Taking the token from nobody, the route finds the forwarder's float slot again for its own parameter block.
claim = code(function(feature_vk, 'bool ClaimModelForAfterUpscale(VkDevice device)'))
assert re.search(r'if \(!held\)\s*\{\s*g_vk\.floatSlotKnown = false;', claim)
# The device's destruction abandons the route's objects on it (ShutdownVk(false)) and gives its token back.
gone = code(function(feature_vk, 'void AfterUpscaleDeviceGone(VkDevice device, std::vector<std::string>& lines)'))
assert gone.index('ShutdownVk(false);') < gone.index('g_owner.DeviceGone(')
assert 'ShutdownVk(true)' not in code(feature_vk)

print('PASS: the way back (begun only with no model here and the route quiet; queues try-locked, fences made before '
      'and submitted after the hand-back began; the route freed only once every fence signalled and the token was '
      'released, without a device-wide wait; the route records nothing before it asks; the device going ends it)',
      flush=True)

print('PASS: stage-3 guards (decided before recording, counted on submit, seen complete on a fence, freed only idle, '
      'abandoned on loss; after-upscale route asks first; passthrough encode; semaphores freed after the destroy)',
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
             r'<ClInclude Include="..\retoque\dlssnr\DlssNr_VkModelOwner.h" />'):
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
