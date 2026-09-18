#!/usr/bin/env python3
"""Execute production bridge lifetime functions with scripted COM/fence/Win32 fakes."""
from pathlib import Path
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
unit = (here / 'fakes.h').read_text() + waiter + '\n'.join(map(function, signatures)) + (here / 'cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-bridge-lifetime-') as directory:
    cpp = Path(directory) / 'bridge.cpp'
    binary = Path(directory) / 'bridge'
    cpp.write_text(unit)
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
upscaler_unit = base_fakes + waiter
upscaler_unit += (here / 'upscaler_fakes.h').read_text()
upscaler_unit += function('bool IsInited() override', header)
upscaler_unit += """
    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* DeviceContext = nullptr;
    ID3D12Device* _dx11on12Device = nullptr;
    ID3D12CommandQueue* Dx12CommandQueue = nullptr;
    ID3D12CommandAllocator* Dx12CommandAllocator[2] {};
    ID3D12GraphicsCommandList* Dx12CommandList[2] {};
    ID3D12Fence* Dx12Fence = nullptr;
    HANDLE Dx12FenceEvent = nullptr;
    UINT64 Dx12FenceValue = 0, Dx12CommandAllocatorFenceValue[2] {};
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
        auto cmdList = Dx12CommandList[frame];
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
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
