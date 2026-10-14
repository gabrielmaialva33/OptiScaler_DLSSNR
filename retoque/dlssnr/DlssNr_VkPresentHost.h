#pragma once

#include <windows.h>

// The Vulkan present host, stages 1 and 2: coverage, and the submission-only control
// (docs/dlssnr/design/nr-vulkan-present-host.md, "What settles the marker question" items 1 and 2, "A narrow NR-only
// hook path", "The queue and semaphore contract").
//
// A host for the NR pass below the game's own API, so a native Vulkan title and a D3D title translated by DXVK or
// vkd3d-proton would be reached the same way. This stage runs no NR and draws nothing. It installs an interception of
// its own on Vulkan creation, lifetime, queue calls and present, independent of the Vulkan menu and of the legacy
// vulkan-1.dll hook bundle, keeps the records DlssNr_VkPresentRegistry.h describes, and carries every eligible present
// through a round trip: the presented image copied into a work image of the host's and back unchanged, waiting on the
// application's present semaphores, with the present then waiting on the host's semaphore instead
// (DlssNr_VkPresentTransport.h decides when and on which queue). Two things of the application's calls change, and
// only then: the image usage of a swapchain whose surface offers TRANSFER_SRC/TRANSFER_DST (added, decided before the
// call), and the wait list of a present the round trip carried. Its present callback runs no Reflex, frame limiter,
// TickFrozenCheck or upscaler timing.
//
// [DlssNr] VulkanPresentHost with [DlssNr] Enabled, read when vulkan-1.dll is hooked. Off -- the default -- Install
// returns before touching anything, so no detour, record, line or submit exists. On, it installs also when [Vulkan]
// SkipHooks or the optiscaler_skip_vulkan_hooks marker keeps the bundle off: the marker stays in force for everything
// else (spoofing, extension and VRAM installers, the overlay), and this host is the one exception.
//
// Detours, each forwarding to its own trampoline, so nothing re-enters our hooks:
//   - vulkan-1.dll's exports vkCreateInstance, vkDestroyInstance, vkEnumeratePhysicalDevices, vkCreateDevice,
//     vkCreateWin32SurfaceKHR and vkDestroySurfaceKHR, installed by Install. Installed before the bundle's own on the
//     same exports, so a vkCreateDevice the bundle's vkGetInstanceProcAddr hands out (its own hook function) still
//     calls down through this one.
//   - per device, right after vkCreateDevice returns, what vkGetDeviceProcAddr returns for vkDestroyDevice,
//     vkGetDeviceQueue, vkGetDeviceQueue2 and the swapchain functions (create, destroy, get images, both acquires);
//     and, for the queue locks a borrowed queue needs, vkQueueSubmit, vkQueueSubmit2(KHR), vkQueueBindSparse and
//     vkQueueWaitIdle, by vkGetDeviceProcAddr's answer and by vulkan-1.dll's export, never an OptiScaler function;
//   - vkQueuePresentKHR when the device creates its first swapchain, which is after the bundle's own present hook (if
//     any) went in, so this one runs first: the order application -> NR -> overlay -> present the design asks for.
// One target per distinct address, up to four per function (one per ICD); a fifth is reported and left alone.
namespace DlssNr::VkPresentHost
{
// From VulkanHooks::Hook, on every call (startup, and each later load of vulkan-1.dll through the loader hook), before
// its own [Vulkan] SkipHooks / marker check. Installs once; a no-op unless the configuration above holds.
void Install(HMODULE vulkan1);

// For the bundle's vkQueuePresentKHR hook, which tells a DXVK or vkd3d-proton present from a native one by its caller:
// with this host's present detour on top, its own return address lies in OptiScaler. Inside a present that went
// through this host, the application's return address; otherwise -- the host off, or not installed -- `returnAddress`.
void* OriginalPresentCaller(void* returnAddress);
} // namespace DlssNr::VkPresentHost
