#!/usr/bin/env python3
"""Compile the actual NGX core shutdown adapter and getter with strict host fakes."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/proxies/NVNGX_Proxy.h').read_text()


def between(start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


types = between('typedef NVSDK_NGX_Result (*PFN_D3D12_Shutdown)',
                'typedef NVSDK_NGX_Result (*PFN_D3D12_GetParameters)')
adapter = between('    static NVSDK_NGX_Result ShutdownDx12Device(', '    inline static void LogCallback(')
getter = between('    static PFN_D3D12_Shutdown1 D3D12_Shutdown1()', '    // Vulkan')
unit = """
#include <cassert>
#include <type_traits>
#include <iostream>
using NVSDK_NGX_Result = unsigned;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 1;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_FAIL_NotImplemented = 0xBAD00012;
struct ID3D12Device {};
""" + types + """
struct Module
{
    PFN_D3D12_Shutdown D3D12_Shutdown = nullptr;
    PFN_D3D12_CoreShutdown1 D3D12_Shutdown1 = nullptr;
};
class NVNGXProxy
{
  public:
    inline static Module _module;
    inline static bool _dx12Inited = false;
""" + adapter + getter + "};\n" + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-ngx-shutdown-') as directory:
    cpp = Path(directory) / 'shutdown.cpp'
    binary = Path(directory) / 'shutdown'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
