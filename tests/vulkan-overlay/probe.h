#pragma once
#include "probe_api.h"
#include <unordered_map>
#include <algorithm>

// Included AFTER the real overlay's headers in a generated copy of that translation unit.
// These wrappers forward to real Vulkan/ImGui except for explicitly armed, one-shot failures.
namespace VkLifetimeProbe
{
inline VkLifetimeStats stats;
inline std::unordered_map<void*, size_t> allocations;
inline unsigned failure = 0;
inline unsigned framebufferCalls = 0;

inline void* Alloc(size_t size)
{
    void* p = ImGui::MemAlloc(size);
    if (p)
    {
        allocations.emplace(p, size);
        ++stats.allocations;
        stats.liveBytes += size;
        stats.peakBytes = std::max(stats.peakBytes, stats.liveBytes);
    }
    return p;
}
inline void Free(void* p)
{
    if (p)
    {
        auto it = allocations.find(p);
        if (it == allocations.end())
            std::abort();
        stats.liveBytes -= it->second;
        allocations.erase(it);
        ++stats.releases;
    }
    ImGui::MemFree(p);
}
inline VkResult WaitIdle(VkDevice device)
{
    if (failure == 1)
    {
        failure = 0;
        ++stats.drainFailures;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return vkDeviceWaitIdle(device);
}
#define PROBE_CREATE(Name, Info, Handle) \
inline VkResult Name(VkDevice d, const Info* i, const VkAllocationCallbacks* a, Handle* h) \
{ \
    auto r = ::Name(d, i, a, h); \
    if (r == VK_SUCCESS) ++stats.objectsCreated; \
    return r; \
}
PROBE_CREATE(vkCreateImageView, VkImageViewCreateInfo, VkImageView)
PROBE_CREATE(vkCreateRenderPass, VkRenderPassCreateInfo, VkRenderPass)
PROBE_CREATE(vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool)
PROBE_CREATE(vkCreateCommandPool, VkCommandPoolCreateInfo, VkCommandPool)
PROBE_CREATE(vkCreateFence, VkFenceCreateInfo, VkFence)
PROBE_CREATE(vkCreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore)
#undef PROBE_CREATE
inline VkResult vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo* i,
                                   const VkAllocationCallbacks* a, VkFramebuffer* h)
{
    if (failure == 2 && ++framebufferCalls == 2)
    {
        failure = 0;
        *h = VK_NULL_HANDLE;
        ++stats.partialFailures;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    auto r = ::vkCreateFramebuffer(d, i, a, h);
    if (r == VK_SUCCESS) ++stats.objectsCreated;
    return r;
}
inline VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* b)
{
    auto r = ::vkAllocateCommandBuffers(d, i, b);
    if (r == VK_SUCCESS) stats.objectsCreated += i->commandBufferCount;
    return r;
}
#define PROBE_DESTROY(Name, Handle) \
inline void Name(VkDevice d, Handle h, const VkAllocationCallbacks* a) \
{ \
    if (h) ++stats.objectsDestroyed; \
    ::Name(d, h, a); \
}
PROBE_DESTROY(vkDestroyImageView, VkImageView)
PROBE_DESTROY(vkDestroyRenderPass, VkRenderPass)
PROBE_DESTROY(vkDestroyDescriptorPool, VkDescriptorPool)
PROBE_DESTROY(vkDestroyCommandPool, VkCommandPool)
PROBE_DESTROY(vkDestroyFence, VkFence)
PROBE_DESTROY(vkDestroySemaphore, VkSemaphore)
PROBE_DESTROY(vkDestroyFramebuffer, VkFramebuffer)
#undef PROBE_DESTROY
inline void vkFreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t count, const VkCommandBuffer* b)
{
    stats.objectsDestroyed += count;
    ::vkFreeCommandBuffers(d, p, count, b);
}
}
#undef IM_ALLOC
#undef IM_FREE
#define IM_ALLOC VkLifetimeProbe::Alloc
#define IM_FREE VkLifetimeProbe::Free
#define vkDeviceWaitIdle VkLifetimeProbe::WaitIdle
#define vkCreateImageView VkLifetimeProbe::vkCreateImageView
#define vkCreateFramebuffer VkLifetimeProbe::vkCreateFramebuffer
#define vkCreateRenderPass VkLifetimeProbe::vkCreateRenderPass
#define vkCreateDescriptorPool VkLifetimeProbe::vkCreateDescriptorPool
#define vkCreateCommandPool VkLifetimeProbe::vkCreateCommandPool
#define vkCreateFence VkLifetimeProbe::vkCreateFence
#define vkCreateSemaphore VkLifetimeProbe::vkCreateSemaphore
#define vkAllocateCommandBuffers VkLifetimeProbe::vkAllocateCommandBuffers
#define vkDestroyImageView VkLifetimeProbe::vkDestroyImageView
#define vkDestroyFramebuffer VkLifetimeProbe::vkDestroyFramebuffer
#define vkDestroyRenderPass VkLifetimeProbe::vkDestroyRenderPass
#define vkDestroyDescriptorPool VkLifetimeProbe::vkDestroyDescriptorPool
#define vkDestroyCommandPool VkLifetimeProbe::vkDestroyCommandPool
#define vkDestroyFence VkLifetimeProbe::vkDestroyFence
#define vkDestroySemaphore VkLifetimeProbe::vkDestroySemaphore
#define vkFreeCommandBuffers VkLifetimeProbe::vkFreeCommandBuffers
