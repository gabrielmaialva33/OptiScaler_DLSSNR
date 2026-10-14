#pragma once

#include <windows.h>

// The Vulkan present host, stage 1: coverage and no-op control (docs/dlssnr/design/nr-vulkan-present-host.md, "What
// settles the marker question" item 1 and "A narrow NR-only hook path").
//
// A host for the NR pass below the game's own API, so a native Vulkan title and a D3D title translated by DXVK or
// vkd3d-proton would be reached the same way. This stage runs no NR. It installs an interception of its own on
// Vulkan creation, lifetime and present, independent of the Vulkan menu and of the legacy vulkan-1.dll hook bundle,
// keeps the records DlssNr_VkPresentRegistry.h describes, and logs per window what presents and through what. It
// forwards every call unchanged: it submits nothing, creates nothing, edits no create info, and its present callback
// runs no Reflex, frame limiter, TickFrozenCheck or upscaler timing.
//
// [DlssNr] VulkanPresentHost with [DlssNr] Enabled, read when vulkan-1.dll is hooked. Off -- the default -- Install
// returns before touching anything, so no detour, record or line exists. On, it installs also when [Vulkan]
// SkipHooks or the optiscaler_skip_vulkan_hooks marker keeps the bundle off: the marker stays in force for everything
// else (spoofing, extension and VRAM installers, the overlay), and this host is the one exception.
//
// Detours, each forwarding to its own trampoline, so nothing re-enters our hooks:
//   - vulkan-1.dll's exports vkCreateInstance, vkDestroyInstance, vkEnumeratePhysicalDevices, vkCreateDevice,
//     vkCreateWin32SurfaceKHR and vkDestroySurfaceKHR, installed by Install. Installed before the bundle's own on the
//     same exports, so a vkCreateDevice the bundle's vkGetInstanceProcAddr hands out (its own hook function) still
//     calls down through this one.
//   - per device, what vkGetDeviceProcAddr returns for vkDestroyDevice, vkGetDeviceQueue, vkGetDeviceQueue2 and the
//     swapchain functions (create, destroy, get images, both acquires), right after vkCreateDevice returns;
//   - vkQueuePresentKHR when the device creates its first swapchain, which is after the bundle's own present hook (if
//     any) went in, so this one runs first: the order application -> NR -> overlay -> present the design asks for.
// One target per distinct address, up to four per function (one per ICD); a fifth is reported and left alone.
namespace DlssNr::VkPresentHost
{
// From VulkanHooks::Hook, on every call (startup, and each later load of vulkan-1.dll through the loader hook), before
// its own [Vulkan] SkipHooks / marker check. Installs once; a no-op unless the configuration above holds.
void Install(HMODULE vulkan1);
} // namespace DlssNr::VkPresentHost
