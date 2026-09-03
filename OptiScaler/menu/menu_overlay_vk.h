#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.hpp>

namespace MenuOverlayVk
{
void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);

// Records which queue families a device was created with. vkGetDeviceQueue on a family or index the
// device never created is undefined behaviour, and vkCreateDevice is the only place that knows.
void NoteDeviceQueues(VkDevice device, const VkDeviceCreateInfo* pCreateInfo);
void DestroyVulkanObjects(bool shutdown);
} // namespace MenuOverlayVk
