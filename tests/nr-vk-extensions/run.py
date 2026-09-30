#!/usr/bin/env python3
"""Compile the production DlssNr_VkExtensions.h against the real Vulkan headers and check the device
extension merge: VK_KHR_ and VK_EXT_buffer_device_address never leave it together."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

vulkan = root / 'external/vulkan/include'
assert (vulkan / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-vk-extensions-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(vulkan), '-I', str(root / 'OptiScaler'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
