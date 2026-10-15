#!/usr/bin/env python3
"""Native D3D11 guides (docs/dlssnr/design/dx11-native-dlss-guides.md, option (c), step 3): the transport's decision
header compiled under ASan/UBSan, and source guards on the transport, its one call site, the capture core it shares
with the D3D12 route, the present host's half, and shutdown. No D3D11, no D3D12, no GPU, no game."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'


def read(relative):
    return (root / relative).read_text(encoding='utf-8')


def function(text, signature):
    """The body of the one definition that starts with `signature`, braces matched outside strings and comments."""
    assert text.count(signature) == 1, f'expected one definition: {signature!r} ({text.count(signature)})'
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
    return re.sub(r'//[^\n]*', '', text)


header = read('retoque/dlssnr/DlssNr_Dx11Guides.h')
transport = read('retoque/dlssnr/DlssNr_Dx11Guides.cpp')
renderer = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
layer = read('retoque/dlssnr/DlssNr_Dx11Upscaler.h')
dx11 = read('OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp')
feature = read('retoque/dlssnr/DlssNrFeature_Dx12.h')
upstream_bridge = read('OptiScaler/with_dx12/dx11_with_dx12.cpp')
depth_transfer = read('OptiScaler/shaders/depth_transfer/DT_Dx11.cpp')

# --- The header is pure ------------------------------------------------------------------------------------------
includes = sorted(l.strip() for l in header.splitlines() if l.startswith('#include'))
assert includes == sorted(['#include <array>', '#include <chrono>', '#include <cstddef>', '#include <cstdint>',
                           '#include <string>']), includes

# --- One call site, after the evaluate, through the route layer ------------------------------------------------
assert dx11.count('DlssNr::NoteDx11Evaluate(upscaler, InDevCtx, InParameters, upscaleResult);') == 1
evaluate = dx11[dx11.index('NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D11_EvaluateFeature('):]
assert evaluate.index('auto upscaleResult = deviceContext->Evaluate(InDevCtx, InParameters);') < \
    evaluate.index('DlssNr::NoteDx11Evaluate(upscaler, InDevCtx, InParameters, upscaleResult);')
assert 'CarryNativeDx11Guides' not in dx11, 'the transport is reached through NoteDx11Evaluate, not called directly'
note = function(layer, 'inline void NoteDx11Evaluate(')
assert note.index('if (!cfg.DlssNrEnabled.value_or_default())\n        return;') < \
    note.index('CarryNativeDx11Guides(context, params, route == Dx11Route::Kind::NativeDlss, evaluated);')

# --- Inert off its route: nothing looked at before the gate ----------------------------------------------------
carry = function(transport, 'void CarryNativeDx11Guides(')
gate = carry.index('if (Dx11Guides::Decide(in) == Reason::NotWanted || context == nullptr || params == nullptr)\n'
                   '        return;')
for touched in ('context->GetType()', 'Describe(', 'BridgePresentedExtent()', 'new Transport()', 'lock_guard'):
    assert gate < carry.index(touched), f'{touched} is reached before the NotWanted gate'
assert carry.index('in.presentHost = State::Instance().nrPresentHostActive;') < gate
assert carry.index('in.hookMethod = cfg.DlssNrHookMethod.value_or_default();') < gate
# No host to carry to: said once (the step 1 report points at this line), and nothing looked at either.
no_host = carry[carry.index('if (Dx11Guides::NoHost(in))'):gate]
assert 'LOG_INFO("{}", Dx11Guides::kNoHostText);' in no_host and 'return;' in no_host
assert 'exchange(true)' in no_host
for touched in ('context', 'params', 'Describe(', 'g_transport', 'lock_guard'):
    assert touched not in code(no_host), f'{touched} is reached on the no-host path'

# --- The carry: the order that keeps D3D11 off D3D12 and nothing freed or written under the GPU ----------------
locked = function(transport, 'Reason CarryLocked(')
order = ['const size_t slot = static_cast<size_t>(t.sequence % kSlots);',
         'if (!Dx11Guides::SlotFree(t.lastRead[slot], t.fence->GetCompletedValue()))\n        return Reason::SlotInFlight;',
         'Dx11WithDx12::Init(d3d11Device, context);',
         'if (!Dx11Guides::CacheFree(t.cache.Generation, Dx11WithDx12::CacheGeneration, t.fence->GetCompletedValue(),',
         'return Reason::SlotInFlight;\n    }',
         'const auto depthRoute = Dx11Guides::DepthCopy(in.depth, in.ntHandles);',
         'if (Dx11Guides::RunsShader(in.depth, in.ntHandles))\n            state.emplace(context, in.depth.depthStencil);',
         'context->CopySubresourceRegion(own.texture, 0, 0, 0, 0, gameDepth, 0, nullptr);',
         'Dx11WithDx12::PrepareUpscalerResources(',
         'return Reason::AlreadyShared;',
         'allocator->Reset()',
         'CaptureBridgedGuides(list, depth12, motion.Dx12Resource, D3D12_RESOURCE_STATE_COMMON,',
         'list->Close()',
         'queue->ExecuteCommandLists(1, lists);',
         't.lastRead[slot] = value;',
         'queue->Signal(t.fence, value)']
positions = [locked.index(step) for step in order]
assert positions == sorted(positions), list(zip(order, positions))
# The shared copies cross between the APIs in COMMON.
assert re.search(r'CaptureBridgedGuides\(list, depth12, motion\.Dx12Resource, D3D12_RESOURCE_STATE_COMMON, params,'
                 r'\s*in\.output\.width, in\.output\.height\);', locked)
# Upstream's transport with its own sync: motion, and depth only where upstream's copy is safe (DepthCopy), synced
# D3D11 -> D3D12 (the last argument). The own depth copy is recorded inside the same scope, before that sync's signal.
assert 'auto mask = Dx11WithDx12::ResourceMask::Mv;' in locked
assert ('if (depthRoute == Dx11Guides::DepthRoute::Upstream)\n        mask |= Dx11WithDx12::ResourceMask::Depth;'
        in locked)
assert re.search(r'PrepareUpscalerResources\(\s*t\.cache, params, mask, static_cast<UINT>\(slot\), '
                 r'Dx11WithDx12::NextUpscalerFrameId\(\),\s*Config::Instance\(\)->DontUseNTShared\.value_or_default\(\), '
                 r'false, true\);', locked)
# A copy of ours, or refused: a texture the game shares already would be read live.
assert 'if (!depth.OwnsSharedTexture && depth.DepthTransfer == nullptr)\n            return Reason::AlreadyShared;' in locked
assert 'if (!motion.OwnsSharedTexture)\n        return Reason::AlreadyShared;' in locked
# The own copy: one mip, a shader resource only, shared; rebuilt only for a set no list reads (after SlotFree).
own = function(transport, 'bool EnsureOwnDepth(')
assert 'desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;' in own and 'desc.MipLevels = 1;' in own
assert 'desc.Format = static_cast<DXGI_FORMAT>(format);' in own and 'Dx11Guides::OwnCopyFormat(depth.format)' in own
assert 'D3D11_BIND_DEPTH_STENCIL' not in code(own)
# Another D3D11 device (the game recreated its own) is another texture: copying across devices is not a copy.
assert 'own.d3d11 == d3d11' in own and 'own.d3d11 = d3d11;' in own
# Upstream releases a stale cache whole, so CacheFree comes after the Init that can make it stale, before any prepare;
# and the transport's sets are upstream's (the index Prepare uses is the transport's slot).
assert 'static_assert(kSlots == DX11_WITH_DX12_INPUT_FRAMES' in transport
assert 'if (cache.Generation != CacheGeneration)' in upstream_bridge and \
    'ResetUpscalerResourceCache(cache);' in upstream_bridge, 'upstream no longer releases a stale cache in Prepare'
assert 'const auto inputIndex = frameIndex % DX11_WITH_DX12_INPUT_FRAMES;' in upstream_bridge
# A list that closed but did not capture is not executed, and is abandoned, as one that failed to close is.
not_captured = locked[locked.index('if (!captured)'):locked.index('ExecuteCommandLists')]
assert 'Submission::Abandon(list);' in not_captured and 'return Reason::Ring;' in not_captured
assert code(locked).count('Submission::Abandon(list);') == 2
# D3D11 never waits on D3D12, and nothing waits on the GPU for a set: no context Wait, no fence wait on the carry.
for forbidden in ('->Wait(', 'WaitForSingleObject', 'SetEventOnCompletion', 'Flush()'):
    assert forbidden not in code(locked), f'the carry must not {forbidden}'
assert 'Wait(' not in code(carry)
# Bounded drains only where objects are released (a device change, shutdown).
assert code(transport).count('WaitForSingleObject(') == 1
drain = function(transport, 'bool DrainLocked(')
assert 'WaitForSingleObject(event, timeoutMs)' in drain
release = function(transport, 'void ReleaseD3D12Locked(')
assert release.index('if (!drained)') < release.index('->Release();')
ensure = function(transport, 'bool EnsureD3D12Locked(')
# On a device change the old objects are freed if already complete and left alive otherwise: the game's thread does
# not wait (DrainLocked with zero only looks). A failed build is not retried on every evaluate.
assert ensure.index('if (t.failedDevice == device && t.failedQueue == queue)\n        return false;') < \
    ensure.index('ReleaseD3D12Locked(t, DrainLocked(t, 0));') < ensure.index('CreateFence(')
assert 'timeoutMs != 0 && SUCCEEDED(t.fence->SetEventOnCompletion(' in drain
failed_build = ensure[ensure.index('if (!built)'):]
assert failed_build.index('ReleaseUnusedD3D12Locked(t);') < failed_build.index('t.failedDevice = device;')
assert 'D3D12_COMMAND_LIST_TYPE_DIRECT' in ensure and 't.lists[i]->Close();' in ensure
shutdown_transport = function(transport, 'void ShutdownNativeDx11Guides(')
assert 'ReleaseD3D12Locked(*t, DrainLocked(*t, 2000));' in shutdown_transport

# The game's D3D11 state is put back around the one step that changes it, upstream's DepthTransfer_Dx11: the compute
# bindings it sets, and the output merger it needs the depth off, render targets, depth-stencil view and unordered-
# access views together, append counters kept.
scoped = transport[transport.index('class ScopedDepthTransferState'):transport.index('void ReleaseOwnDepth(')]
for get, set_ in (('CSGetShader(', 'CSSetShader('), ('CSGetShaderResources(', 'CSSetShaderResources('),
                  ('CSGetUnorderedAccessViews(', 'CSSetUnorderedAccessViews('),
                  ('OMGetRenderTargetsAndUnorderedAccessViews(', 'OMSetRenderTargetsAndUnorderedAccessViews(')):
    assert get in scoped and set_ in scoped, (get, set_)
assert 'D3D11_1_UAV_SLOT_COUNT' in scoped and 'D3D11_PS_CS_UAV_REGISTER_COUNT' in scoped
assert 'keep.fill(static_cast<UINT>(-1));' in scoped
assert 'CSSetUnorderedAccessViews(0, _uavCount, _uavs.data(), keep.data());' in scoped
for untouched in ('ConstantBuffers', 'Samplers'):
    assert untouched not in scoped, f'{untouched}: nothing in the transport sets them, and a round trip loses offsets'
# What that holds on: DepthTransfer_Dx11::Dispatch sets the compute shader, one shader resource and one unordered-
# access view and nothing else, and upstream's transport otherwise only copies, signals and waits on the context.
dispatch = code(function(depth_transfer, 'bool DepthTransfer_Dx11::Dispatch('))
assert sorted(set(re.findall(r'InContext->(\w+)\(', dispatch))) == \
    ['CSSetShader', 'CSSetShaderResources', 'CSSetUnorderedAccessViews', 'Dispatch'], 'DepthTransfer_Dx11 changed'
assert set(re.findall(r'Dx11DeviceContext->(\w+)\(', code(upstream_bridge))) <= \
    {'Signal', 'Wait', 'CopyResource', 'Flush'}, 'upstream\'s D3D11 transport calls more on the context'

# --- The capture core both producers share ---------------------------------------------------------------------
bridged = function(renderer, 'bool CaptureBridgedGuides(')
assert 'return CaptureGuides(cmdList, source, GatherGuideFrame(params));' in bridged
assert 'source.output = EvaluateOutput(params, nullptr, outputWidth, outputHeight);' in bridged
assert 'GetResource(' not in bridged, 'a D3D11 block holds no D3D12 resources'
guide_frame = function(renderer, 'DlssNrFrameInfo GatherGuideFrame(')
for side_effect in ('g_nr.', 'LOG_', 'GetResource(', 'ColorBinding', 'static '):
    assert side_effect not in code(guide_frame), f'GatherGuideFrame must not have {side_effect}'
gather = function(renderer, 'DlssNrFrameInfo GatherFrame(')
assert 'DlssNrFrameInfo frame = GatherGuideFrame(params);' in gather

# The present host publishes the size it presents, before anything can return.
captured = function(renderer, 'bool CapturedPresentGuides(')
assert captured.index('NoteBridgePresentedExtent(presentWidth, presentHeight);') < captured.index('return false;')

# --- Shutdown, before the locks the capture takes after the transport's own ------------------------------------
shutdown = function(renderer, 'void Shutdown()')
assert shutdown.index('ShutdownNativeDx11Guides();') < shutdown.index('std::lock_guard<std::mutex> nrLock(g_nrMutex);')
assert renderer.count('ShutdownNativeDx11Guides();') == 1

# --- Declared once, registered in the project ------------------------------------------------------------------
for declaration in ('bool CaptureBridgedGuides(', 'unsigned long long BridgePresentedExtent();',
                    'DlssNrFrameInfo GatherGuideFrame(NVSDK_NGX_Parameter* params);',
                    'void CarryNativeDx11Guides(', 'bool NativeDx11GuidesCarriedRecently();',
                    'void ShutdownNativeDx11Guides();'):
    assert feature.count(declaration) == 1, declaration
project = (root / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (root / 'OptiScaler/OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
assert project.count('<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_Dx11Guides.cpp" />') == 1
assert filters.count('<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_Dx11Guides.cpp">') == 1
assert transport.startswith('#include "pch.h"')

print('PASS: source guards -- pure header; one call site after the evaluate, behind the NR gate; inert before the '
      'NotWanted gate, the missing host said once; slot and cache checks before any write, the D3D11 state put back '
      'around the depth conversion, COMMON across the APIs, close before execute, an uncaptured list abandoned, the '
      'set busy before the signal; no wait on the carry or a device change; the capture core shared with D3D12; the '
      'present size published; shutdown before the locks; registered in the project', flush=True)

flags = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-I', str(retoque)]
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-dx11-native-guides-') as directory:
    binary = Path(directory) / 'decide'
    subprocess.run(flags + [str(here / 'decide.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
