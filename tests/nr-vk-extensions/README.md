# NR Vulkan device extensions

Run `python3 tests/nr-vk-extensions/run.py`. The runner compiles the production
`OptiScaler/dlssnr/DlssNr_VkExtensions.h` as it is, against the real Vulkan headers in
`external/vulkan` (the submodule CI checks out), with g++ C++20, ASan/UBSan and `-Werror`.

## What this covers

DLSS-NR's `vkCreateDevice` hook (`hooks/Vulkan_Hooks.cpp`) appends the device extensions the model
names, `VK_KHR_buffer_device_address` among them, after OptiScaler's own spoofing has added
`VK_EXT_buffer_device_address` on NVIDIA. The spec forbids enabling both
(VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328). `DropConflictingBufferDeviceAddress` removes
the one the game did not ask for: the EXT one, unless the game asked for the EXT one itself, in which
case the KHR one is not added. A game that asked for both keeps its own list. The cases walk each of
those, plus lists with one of them or neither, which must come back unchanged.

## What this does not cover

No Vulkan device is created. Whether the driver accepts the merged list, and whether the model runs
with only the EXT extension when a game insists on it, is for a game run (DOOM Eternal).
