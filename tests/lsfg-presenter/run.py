#!/usr/bin/env python3
"""The MFG presenter (study branch): LSFG at x3 and x4 through a fork of FFX's swapchain.

tools/lsfg-study/MFG_PRESENTER.md. Two parts:

1. test.cpp, under ASan/UBSan: retoque/lsfg/presenter/MfgPacing.h, the arithmetic the forked swapchain runs on
   (multiplier to generated frames, the present delta with x2 equal to FFX's formula, output-set double buffering
   and availability, a steady-state schedule of the presenter's timing model, the 10 s summary).
2. Source guards. The D3D12 half cannot run here, so what it must never do is checked in the text:
   - FSRFG_Dx12.cpp routes every FFX call that can reach a swapchain through Lsfg::Presenter, creates our swapchain
     only behind Wanted(), and keeps the one FFX hand-off;
   - the router forwards everything that is not ours to FfxApiProxy unchanged and tells FFX's FG context to leave
     the swapchain alone when it is ours;
   - our swapchain answers no FFX interface, so AMD's signed DLL can never cast it to its ABI types;
   - Lsfg::TakesFrame lowers numGeneratedFrames to 1 whenever FFX interpolates instead of LSFG;
   - the multiplier is read live through MfgLatch.h at every FG configure, and the menu row reads the latch from it
     (test.cpp holds MfgLatch.h to MFG_PRESENTER.md's started-at x chosen table);
   - every file copied from the FidelityFX SDK keeps AMD's MIT notice, and the shader bytecode headers are the SDK's
     bytes (when external/FidelityFX-SDK-v2 is checked out);
   - the GPU objects the fork owns are released only in shutdown(), after the threads have stopped.
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent
repo = root.parents[1]
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


def body(text, signature):
    """The body of the first function whose definition starts with `signature` (brace matched)."""
    start = text.find(signature)
    if start < 0:
        return None
    open_brace = text.find('{', start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
    return None


# 1. The arithmetic, compiled unmodified.
with tempfile.TemporaryDirectory(prefix='optiscaler-lsfg-presenter-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque'), str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

presenter = repo / 'retoque/lsfg/presenter'
fsrfg = (repo / 'OptiScaler/framegen/ffx/FSRFG_Dx12.cpp').read_text()
router = (presenter / 'LsfgPresenter.cpp').read_text()
swapchain = (presenter / 'MfgSwapChain_Dx12.cpp').read_text()
swapchain_h = (presenter / 'MfgSwapChain_Dx12.h').read_text()
lsfg = (repo / 'retoque/lsfg/Lsfg.cpp').read_text()
framegen = (repo / 'retoque/lsfg/LsfgFrameGen_Dx12.cpp').read_text()

# 2a. FSRFG_Dx12.cpp: nothing aimed at the swapchain context goes to FFX unrouted.
for call in ('D3D12_Configure(&_swapChainContext', 'D3D12_DestroyContext(&_swapChainContext'):
    check(('FfxApiProxy::' + call) not in fsrfg, f'FSRFG_Dx12.cpp still calls FfxApiProxy::{call}')
    check(('Lsfg::Presenter::' + call) in fsrfg, f'FSRFG_Dx12.cpp does not route {call} through Lsfg::Presenter')

# Every ffxConfigureDescFrameGeneration FSRFG configures names _swapChain: all of them through the router.
for desc in ('fgConfig', 'm_FrameGenerationConfig'):
    direct = re.findall(r'FfxApiProxy::D3D12_Configure\(&_fgContext, &' + desc + r'\.header\)', fsrfg)
    routed = re.findall(r'Lsfg::Presenter::D3D12_Configure\(&_fgContext, &' + desc + r'\.header\)', fsrfg)
    check(not direct, f'FSRFG_Dx12.cpp configures {desc} straight into FFX ({len(direct)}x)')
    check(routed, f'FSRFG_Dx12.cpp never routes {desc}')
check(len(re.findall(r'Lsfg::Presenter::D3D12_Configure\(&_fgContext, &fgConfig\.header\)', fsrfg)) == 3,
      'Dispatch, Activate and Deactivate route their FG configure')

# Both creation sites: ours only behind Wanted(), FFX's otherwise, with FFX's exact call.
creations = re.findall(r'Lsfg::Presenter::Wanted\(\)\s*\?\s*Lsfg::Presenter::CreateSwapChain1?\([^;]*?\)\s*:\s*'
                       r'FfxApiProxy::D3D12_CreateContext\(&_swapChainContext, &createSwapChainDesc\.header, '
                       r'nullptr\);', fsrfg, re.S)
check(len(creations) == 2, f'FSRFG_Dx12.cpp creation sites behind Wanted(): {len(creations)}, expected 2')
check(fsrfg.count('FfxApiProxy::D3D12_CreateContext(&_swapChainContext') == 2,
      'an FFX swapchain creation outside the two routed sites')
check(fsrfg.count('FfxApiProxy::D3D12_Dispatch(&_fgContext, &params->header)') == 1,
      'the FFX hand-off appears more than once')

# 2b. The router: not ours is forwarded verbatim; ours tells FFX's FG context to leave the swapchain alone.
configure = body(router, 'ffxReturnCode_t D3D12_Configure(ffxContext* context, const ffxConfigureDescHeader* desc)')
check(configure is not None, 'Lsfg::Presenter::D3D12_Configure not found')
if configure:
    check(configure.rstrip().endswith('return FfxApiProxy::D3D12_Configure(context, desc);'),
          'D3D12_Configure does not end by forwarding to FfxApiProxy unchanged')
    check('FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY' in configure,
          'the FG configure for our swapchain does not set NO_SWAPCHAIN_CONTEXT_NOTIFY')
    check(re.search(r'forFfx\.flags \|= FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;\s*\n\s*const auto result = '
                    r'FfxApiProxy::D3D12_Configure\(context, &forFfx\.header\);', configure),
          'FFX is configured with the original descriptor, not the flagged copy')
destroy = body(router, 'ffxReturnCode_t D3D12_DestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)')
check(destroy is not None and destroy.rstrip().endswith('return FfxApiProxy::D3D12_DestroyContext(context, memCb);'),
      'D3D12_DestroyContext does not end by forwarding to FfxApiProxy unchanged')

# The latch reads Enabled and Multiplier > 2, once (compare_exchange from undecided).
wanted = body(router, 'bool Wanted()')
check(wanted is not None and 'compare_exchange_strong' in wanted, 'Wanted() is not latched')
check(re.search(r'bool WantedNow\(\) \{ return Config::Instance\(\)->LsfgEnabled\.value_or_default\(\) && '
                r'Multiplier\(\) > 2; \}', router), 'WantedNow is not Enabled && Multiplier > 2')

# The multiplier is read at every FG configure (every real frame) through MfgLatch.h, so 2, 3 and 4 apply live with
# our swapchain; the menu reads the latch and the same header (MFG_PRESENTER.md 1.4, 5).
if configure:
    check(re.search(r'const uint32_t generated =\s*\n\s*LsfgMfg::GeneratedFramesLive\('
                    r'Config::Instance\(\)->LsfgEnabled\.value_or_default\(\), Multiplier\(\)\);', configure),
          'the FG configure does not take the generated-frame count from GeneratedFramesLive')
    check(configure.find('GeneratedFramesLive(') < configure.find('swapChain->setGeneratedFrames(generated);') and
          'swapChain->setGeneratedFrames(generated);' in configure,
          'the FG configure does not hand the live count to our swapchain')
latch = body(router, 'LsfgMfg::Latch CurrentLatch()')
check(latch is not None and re.search(r'case 0:\s*return LsfgMfg::Latch::Ffx;\s*case 1:\s*return LsfgMfg::Latch::Ours;'
                                      r'\s*default:\s*return LsfgMfg::Latch::Undecided;', latch),
      'CurrentLatch does not map g_latch 0/1/-1 to Ffx/Ours/Undecided')

menu = (repo / 'retoque/lsfg/LsfgMenu.cpp').read_text()
row = body(menu, 'void RenderMultiplier(Config* config, const Status& status, float menuResScale)')
check(row is not None, 'LsfgMenu.cpp has no RenderMultiplier')
if row:
    check('Presenter::CurrentLatch()' in row and 'LsfgMfg::ActiveMultiplier(latch, configured)' in row,
          'the Multiplier row does not read the latch and the active multiplier from MfgLatch.h')
    check(menu.count('config->LsfgMultiplier =') == 1 and
          re.search(r'if \(ImGui::Selectable\(item, multiplier == configured\) && multiplier != configured\)\s*\n\s*'
                    r'config->LsfgMultiplier = multiplier;', row),
          'LsfgMultiplier is written somewhere other than a changed selection (an untouched 2 must stay unset)')
text = body(menu, 'void MultiplierText(LsfgMfg::Latch latch, int multiplier, char* text, size_t size)')
check(text is not None and 'LsfgMfg::AppliesLive(latch, multiplier)' in text and 'after a restart' in text,
      'the combo items do not say which choices wait for a restart')
render = body(menu, 'void RenderMenu(float menuResScale)')
check(render is not None and render.find('ImGui::BeginDisabled(!enabled);') < render.find('RenderMultiplier(') <
      render.find('ImGui::EndDisabled();'), 'the Multiplier row is not disabled with LSFG off, as its siblings are')

# 2c. Our swapchain answers no FFX interface or IID.
def code_only(text):
    """The text without // comments (the fork's headers name what was removed)."""
    return '\n'.join(line.split('//', 1)[0] for line in text.splitlines())


fork_code = code_only(swapchain + swapchain_h + router).lower()
for ffx in ('IFrameInterpolationSwapChainDX12', 'IID_IFfxFrameInterpolationSwapChain', 'BEED74B2', '5f5fa2f5',
            'IID_AMDFFXABIVersion', 'IID_IFfxFrameInterpolationSwapChainResourceInfo'):
    check(ffx.lower() not in fork_code, f'the fork uses FFX interface {ffx}')
qi = body(swapchain, 'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::QueryInterface(REFIID riid, void** ppvObject)')
check(qi is not None, 'QueryInterface not found')
if qi:
    guids = re.findall(r'(IID_\w+|__uuidof\(\w+\))', qi)
    check(set(guids) == {'IID_IUnknown', 'IID_IDXGIObject', 'IID_IDXGIDeviceSubObject', 'IID_IDXGISwapChain',
                         'IID_IDXGISwapChain1', 'IID_IDXGISwapChain2', 'IID_IDXGISwapChain3', 'IID_IDXGISwapChain4',
                         '__uuidof(MfgSwapChainDX12)'}, f'QueryInterface answers {sorted(set(guids))}')

# The widened request: N outputs, numGeneratedFrames = N, and what came back is what is shown.
dispatch = body(swapchain, 'InterpolationResult MfgSwapChainDX12::dispatchInterpolationCommands()')
check(dispatch is not None, 'dispatchInterpolationCommands not found')
if dispatch:
    check('desc.numGeneratedFrames = wanted;' in dispatch, 'the callback is not asked for N frames')
    check('produced = std::min(desc.numGeneratedFrames, wanted);' in dispatch,
          'the count shown is not what the callback wrote')
    check(re.search(r'set\.availabilityFenceValue != 0\)\s*\n\s*presentInfo\.interpolationQueue->Wait\('
                    r'presentInfo\.compositionFenceGPU, set\.availabilityFenceValue\);', dispatch),
          'the interpolation queue does not wait for its output set')
    # One list per generated frame (MFG_PRESENTER.md 4.2): handed over only when there is more than one, each list
    # executed in its own submission with a fence value of its own, the unused ones dropped, all of them on failure.
    check(dispatch.find('presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU') <
          dispatch.find('frameGenerationCallback(&desc'), 'the output-set wait is not ahead of every list')
    check('desc.header.pNext = listCount > 1 ? &frameLists.header : nullptr;' in dispatch,
          'the per-frame lists are not handed to the callback (or are handed with one list)')
    check(re.search(r'for \(uint32_t i = 0; i < used; i\+\+\)\s*\{\s*commands\[i\]->execute\(true\);\s*'
                    r'presentInfo\.interpolationQueue->Signal\(presentInfo\.interpolationFence, '
                    r'\+\+interpolationFenceValue\);\s*result\.listFence\[i\] = interpolationFenceValue;', dispatch),
          'each interpolation list is not followed by a fence signal of its own')
    check(re.search(r'for \(uint32_t i = used; i < listCount; i\+\+\)\s*commands\[i\]->drop\(true\);', dispatch),
          'unused per-frame lists are not dropped (closed)')
    check(re.search(r'else\s*\{[^}]*for \(uint32_t i = 0; i < listCount; i\+\+\)\s*commands\[i\]->drop\(true\);',
                    dispatch), 'a failed callback leaves per-frame lists open')
    check('lists[used - 1]->ResolveQueryData(' in dispatch, 'the timestamps are not resolved by the last list')
present = body(swapchain, 'void MfgSwapChainDX12::presentInterpolated(UINT SyncInterval, UINT)')
check(present is not None, 'presentInterpolated not found')
if present:
    # Frame k waits for its own list, the entry for the first one only.
    check('fi.interpolationCompletedFenceValue = interpolation.listFence[ListForFrame(k, interpolation.lists)];'
          in present, 'a generated frame does not carry its own list\'s fence')
    check(re.search(r'entry\.interpolationCompletedFenceValue =\s*interpolation\.lists > 0 \? '
                    r'interpolation\.listFence\[0\] : interpolationFenceValue;', present),
          'the entry\'s first wait is not the first list\'s fence')
composite = body(swapchain, 'HRESULT compositeSwapChainFrame(PresentInfo* presenter, PacingData* pacingEntry, uint32_t frameIndex)')
check(composite is not None and re.search(r'if \(frameInfo\.generated\)\s*\n\s*presenter->presentQueue->Wait\('
                                          r'presenter->interpolationFence, frameInfo\.interpolationCompletedFenceValue\);',
                                          composite), 'a generated frame is composited without waiting for its list')

# FGHooks detours these by code address; with /OPT:ICF a forwarder identical to another class's would be folded with it
# and the detour would catch that class too. Each starts with the NOP that makes it unique.
for method in ('ULONG STDMETHODCALLTYPE MfgSwapChainDX12::Release()',
               'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetFullscreenState(',
               'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetFullscreenState(',
               'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::ResizeTarget(',
               'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetFullscreenDesc(',
               'HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::Present1('):
    b = body(swapchain, method)
    check(b is not None and b.strip().startswith('LSFG_MFG_UNFOLDABLE();'), f'{method} can be folded (no NOP first)')

# Outputs, replacement buffers and the UI composition objects are released only after the GPU is drained.
destroy_res = body(swapchain, 'bool MfgSwapChainDX12::destroyReplacementResources()')
check(destroy_res is not None and destroy_res.find('waitForPresents();') < destroy_res.find('SafeRelease(output);'),
      'outputs released before waitForPresents')
shutdown = body(swapchain, 'HRESULT MfgSwapChainDX12::shutdown()')
check(shutdown is not None, 'shutdown not found')
if shutdown:
    check(shutdown.find('killPresenterThread();') < shutdown.find('presentInfo.uiComposition.release();'),
          'UI composition objects released while the presenter thread may run')
    check(shutdown.find('destroyReplacementResources();') < shutdown.find('presentInfo.uiComposition.release();'),
          'UI composition objects released before the presents are drained')
    # The timestamp heap and readback: written by the interpolation lists, read by the presenter thread.
    check(shutdown.find('killPresenterThread();') < shutdown.find('releaseTiming();') and
          shutdown.find('waitForFenceValue(presentInfo.interpolationFence') < shutdown.find('releaseTiming();'),
          'timing objects released while the presenter thread or an interpolation list may use them')
check(swapchain.count('uiComposition.release()') == 1 and swapchain.count('debugPacing.release()') == 1,
      'the fork releases its GPU objects somewhere other than shutdown()')
check(swapchain.count('releaseTiming();') == 1, 'timing objects released somewhere other than shutdown()')
check(re.search(r'\bstatic\s+ID3D12', (presenter / 'MfgSwapChain_UiComposition.cpp').read_text() +
                (presenter / 'MfgSwapChain_DebugPacing.cpp').read_text()) is None,
      'UI composition or debug pacing keeps D3D12 objects in statics again')

# 2d. FFX interpolating instead of LSFG: one frame, never stale outputs.
takes = body(lsfg, 'bool TakesFrame(int fIndex, ffxDispatchDescFrameGeneration* params)')
check(takes is not None and re.search(r'if \(!lsfg && params != nullptr && params->numGeneratedFrames > 1\)\s*\n\s*'
                                      r'params->numGeneratedFrames = 1;', takes),
      'TakesFrame does not lower numGeneratedFrames to 1 on the FFX path')
check(takes is not None and re.search(r'if \(!lsfg && params != nullptr\)\s*\n\s*UnlinkFrameLists\(params\);', takes),
      'TakesFrame hands FFX the per-frame lists descriptor')

# 2d'. LSFG's side of the per-frame lists: frame k on its list, the list it leaves finished before the next starts.
ffx_dispatch = body(lsfg, 'uint32_t FfxDispatch(ffxDispatchDescFrameGeneration* params)')
check(ffx_dispatch is not None, 'FfxDispatch not found')
if ffx_dispatch:
    check('LsfgMfg::ListForFrame(k, lists)' in ffx_dispatch, 'FfxDispatch places frames other than by ListForFrame')
    check(re.search(r'O\(\)\.context->Record\(list, io, frame, schedule\);[^}]*frameLists->listsUsed = '
                    r'O\(\)\.context->LastListsUsed\(\);', ffx_dispatch, re.S),
          'FfxDispatch does not report the lists it recorded into')
record = body(framegen, 'void Context_Dx12::Record(ID3D12GraphicsCommandList* list, const FrameIo& io, const FrameConstants& frame,')
check(record is not None, 'Context_Dx12::Record not found')
if record:
    switch = re.search(r'if \(next != current\)\s*\{(.*?)\}', record, re.S)
    check(switch is not None, 'Record has no list switch')
    if switch:
        step = switch.group(1)
        check(step.find('_Flush(current);') >= 0 and step.find('_Flush(current);') < step.find('current = next;'),
              'what a frame leaves pending is not issued on its own list before the next list starts')
        check(step.find('current->SetDescriptorHeaps(1, heaps);') > step.find('current = next;'),
              'the next list does not get the descriptor heap')
        check('_boundPso = nullptr;' in step, 'the next list inherits a pipeline it never bound')
    check(re.search(r'_Flush\(current\);\s*StudyTimestamp\(current\);', record),
          'the last pending barriers are not issued on the last list')
    check('current->ResolveQueryData(' in record and 'list->ResolveQueryData(' not in record,
          'LSFG\'s GPU time is not resolved on the last list')
    check('_ClearHistory(list);' in record and '_Ingest(list, io.presentColor, io.presentState);' in record,
          'the clears or the ingest left the first list')

# 2e. AMD's notice on every copied file; the bytecode is the SDK's.
notice = ('Copyright (C) 2026 Advanced Micro Devices, Inc.', 'Permission is hereby granted, free of charge',
          'The above copyright notice and this permission notice shall be included in')
for path in sorted(presenter.glob('MfgSwapChain_*')):
    text = path.read_text()
    for line in notice:
        check(line in text, f'{path.name} lost the MIT notice line: {line!r}')
    check('Modified for Retoque' in text, f'{path.name} does not say it was modified')

sdk = repo / 'external/FidelityFX-SDK-v2/Kits/FidelityFX/framegeneration/fsr3/dx12'
shaders = sorted((presenter / 'precompile').glob('*.h'))
check(len(shaders) == 6, f'{len(shaders)} shader headers under presenter/precompile, expected 6')
if sdk.is_dir():
    for path in shaders:
        original = sdk / path.name
        check(original.is_file() and original.read_bytes() == path.read_bytes(),
              f'precompile/{path.name} differs from the SDK copy')

if failures:
    for f in failures:
        print('FAIL:', f)
    sys.exit(1)
print('source guards: ok')
