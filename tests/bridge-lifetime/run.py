#!/usr/bin/env python3
"""Execute production bridge lifetime functions with scripted COM/fence/Win32 fakes."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp').read_text()


def function(signature, source=source):
    assert source.count(signature) == 1, signature
    begin = source.index(signature)
    start = source.index('{', begin)
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end] + '\n'


signatures = [
    'Dx11wDx12SC::~Dx11wDx12SC()',
    'ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()',
    'bool Dx11wDx12SC::_OwnsFgPresenter()',
    'bool Dx11wDx12SC::_OwnsPresenter()',
    'bool Dx11wDx12SC::_OwnsOverlay()',
    'bool Dx11wDx12SC::_DevicesRemoved()',
    'void Dx11wDx12SC::_DetachWrapperGlobals()',
    'void Dx11wDx12SC::_FinishRelease(',
    'void Dx11wDx12SC::_CollectRetired()',
    'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(',
    'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(',
    'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(',
    'bool Dx11wDx12SC::_EmulatesFullscreen()',
    'HRESULT Dx11wDx12SC::_RecoverPresenter()',
    'HRESULT Dx11wDx12SC::_ResizePresenterToMatch()',
    'HRESULT Dx11wDx12SC::_ResizePresenter(',
    'void Dx11wDx12SC::_RetryRefusedPresenterResize()',
    'bool Dx11wDx12SC::_PresenterDrawsOverlay()',
    'void Dx11wDx12SC::_ReleaseOverlayForResize()',
    'HRESULT Dx11wDx12SC::_WaitForCopyAllocator(',
    'HRESULT Dx11wDx12SC::_WaitForCopyQueueIdle(',
    'HRESULT Dx11wDx12SC::_DrainForTeardown(',
    'void Dx11wDx12SC::_ResetTeardownDrain()',
    'void Dx11wDx12SC::_ReleaseInteropObjects()',
    'void Dx11wDx12SC::_ReleaseInteropBackBuffers()',
    'bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(',
    'ID3D12CommandQueue* Dx11wDx12SC::_PresentQueueForFrame()',
]
wait_source = (root / 'OptiScaler/with_dx12/with_dx12.cpp').read_text()
waiter = function('HRESULT WaitForBridgeFence(', wait_source)
# What the bridge is built for, and whether a host rides along: the production predicates, verbatim.
predicates = 'namespace Dx11wDx12\n{\n' + ''.join(map(function, [
    'bool WantedForFrameGeneration()',
    'bool WantedForNeuralRendering()',
    'bool WantedIdleForNeuralRendering()',
    'void ResolveZeroExtent(HWND gameWindow,',
    'bool PresenterExtent(IDXGISwapChain1* presenter,',
    'UINT PresenterResizeFlags(IDXGISwapChain1* presenter,',
    'void MatchHiddenToPresenter(IDXGISwapChain* hidden,',
])) + '} // namespace Dx11wDx12\n'
# Upstream's skip-resize test lives in the production file's anonymous namespace; verbatim here too.
helpers = 'namespace\n{\n' + function('bool IsSame(IDXGISwapChain* swapchain,') + '} // namespace\n'
# Production's retry budget for a refused presenter resize, verbatim from the header, into the fake class.
bridge_header = (root / 'OptiScaler/with_dx12/dx11_with_dx12_sc.h').read_text()
retries = re.search(r'^\s*static constexpr UINT kRefusedResizeRetries = \d+;$', bridge_header, re.M).group(0)
fakes = (here / 'fakes.h').read_text()
assert fakes.count('// @kRefusedResizeRetries@') == 1
fakes = fakes.replace('// @kRefusedResizeRetries@', retries.strip())
unit = (fakes + waiter + predicates + helpers + '\n'.join(map(function, signatures)) +
        (here / 'cases.cpp').read_text())
with tempfile.TemporaryDirectory(prefix='optiscaler-bridge-lifetime-') as directory:
    cpp = Path(directory) / 'bridge.cpp'
    binary = Path(directory) / 'bridge'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# The D3D12 overlay's render-target lifetime, which the bridge's resize contract leans on: the production
# CreateRenderTargetDx12 and CleanupRenderTargetDx12 from menu_overlay_dx.cpp, over scripted swapchain buffers
# whose public reference counts decide a vkd3d-proton-style ResizeBuffers.
overlay_source = (root / 'OptiScaler/menu/menu_overlay_dx.cpp').read_text()
overlay_unit = ((here / 'overlay_fakes.h').read_text() +
                function('static void CreateRenderTargetDx12(', overlay_source) +
                function('static void CleanupRenderTargetDx12(', overlay_source) +
                (here / 'overlay_cases.cpp').read_text())
with tempfile.TemporaryDirectory(prefix='optiscaler-overlay-targets-') as directory:
    cpp = Path(directory) / 'overlay.cpp'
    binary = Path(directory) / 'overlay'
    cpp.write_text(overlay_unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Compile the actual upscaler Init, allocator preparation and inline IsInited.
# Only the surrounding COM/feature machinery is fake. The evaluate submission
# block is compiled intact inside a test-only adapter (no production test seam).
upscaler = (root / 'OptiScaler/upscalers/IFeature_Dx11wDx12.cpp').read_text()
header = (root / 'OptiScaler/upscalers/IFeature_Dx11wDx12.h').read_text()
submit_start = upscaler.index('    if (dx12EvalResult)\n    {\n        ID3D12CommandList*')
submit_end = upscaler.index('    auto evalResult = false;', submit_start)
base_fakes = (here / 'fakes.h').read_text().split('struct Dx11Context')[0]
# Production's ring sizes, verbatim: the command ring (upscaler header) and the resource ring (cache).
cache_header = (root / 'OptiScaler/with_dx12/dx11_with_dx12.h').read_text()
defines = ''.join(re.search(rf'^#define {name} \d+\n', text, re.M).group(0) for name, text in (
    ('DX11WDX12_COMMAND_BUFFER_COUNT', header), ('DX11_WITH_DX12_CACHED_FRAMES', cache_header)))
upscaler_unit = base_fakes + defines + waiter
upscaler_unit += (here / 'upscaler_fakes.h').read_text()
upscaler_unit += function('bool IsInited() override', header)
upscaler_unit += """
    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* DeviceContext = nullptr;
    ID3D12Device* _dx11on12Device = nullptr;
    ID3D12CommandQueue* Dx12CommandQueue = nullptr;
    ID3D12CommandAllocator* Dx12CommandAllocator[DX11WDX12_COMMAND_BUFFER_COUNT] {};
    ID3D12GraphicsCommandList* Dx12CommandList[DX11WDX12_COMMAND_BUFFER_COUNT] {};
    ID3D12Fence* Dx12Fence = nullptr;
    HANDLE Dx12FenceEvent = nullptr;
    UINT64 Dx12FenceValue = 0, Dx12CommandAllocatorFenceValue[DX11WDX12_COMMAND_BUFFER_COUNT] {};
    std::unique_ptr<GpuTime_Dx11> UpscalerTime;
    unsigned _frameCount = 0;
    struct FakeHandle { unsigned Id = 0; } handle;
    FakeHandle* Handle() { return &handle; }
    bool AutoExposure() { return true; }
    bool BaseInit(ID3D11Device*, ID3D11DeviceContext*, NVSDK_NGX_Parameter*) { return true; }
    void SetInitParameters(NVSDK_NGX_Parameter*) {}
    bool Init(ID3D11Device*, ID3D11DeviceContext*, NVSDK_NGX_Parameter*);
    bool ProcessDx11Textures(const NVSDK_NGX_Parameter*);
    bool SubmitEvaluateForTest(UINT frame)
    {
        const auto commandFrame = frame;
        auto cmdList = Dx12CommandList[commandFrame];
        bool dx12EvalResult = true, commandListExecuted = false;
        HRESULT result;
""" + upscaler[submit_start:submit_end] + """
        return dx12EvalResult && commandListExecuted;
    }
};
"""
upscaler_unit += function('bool IFeature_Dx11wDx12::Init(', upscaler)
upscaler_unit += function('bool IFeature_Dx11wDx12::ProcessDx11Textures(', upscaler)
upscaler_unit += (here / 'upscaler_cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-upscaler-fence-') as directory:
    cpp = Path(directory) / 'upscaler.cpp'
    binary = Path(directory) / 'upscaler'
    cpp.write_text(upscaler_unit)
    # -Wno-unused-variable: the log macros compile out here, and production keeps locals (fence values
    # read before and after a wait) whose only reader is a LOG_WARN.
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
