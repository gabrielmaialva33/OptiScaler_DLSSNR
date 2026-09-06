#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_overlay_vk.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>

#include <imgui/imgui_impl_vulkan.h>
#include <imgui/imgui_impl_win32.h>

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <misc/IdentifyGpu.h>

// Vulkan overlay code adopted from here:
// https://gist.github.com/mem99/0ec31ca302927457f86b1d6756aaa8c4
// Need to check resize & recreate fixes

static bool _isInited = false;

static bool _vulkanObjectsCreated = false;
static bool _vulkanDeviceLost = false;
static bool _vulkanBackendInited = false;
static std::mutex _vkCleanMutex;
static std::mutex _vkPresentMutex;

// imgui stuff
struct ImGui_ImplVulkan_InitInfo _ImVulkan_Info = {};
struct ImGui_ImplVulkanH_Frame* _ImVulkan_Frames = VK_NULL_HANDLE;
static VkSemaphore* _ImVulkan_Semaphores = VK_NULL_HANDLE;
static VkRenderPass _vkRenderPass = VK_NULL_HANDLE;
static uint32_t _scImageCount;

// Queue families each device was created with, recorded from vkCreateDevice.
//
// vkGetDeviceQueue on a family or index the device never created is undefined behaviour, and
// vkCreateDevice is the only place that knows. Keyed by device on purpose: vkd3d-proton creates
// several devices during startup, so a single family-to-count map would answer for whichever device
// happened to be created last and could hand out an index the swapchain's device does not have.
// Entries are never removed; the count is the handful of devices an application creates.
static std::unordered_map<VkDevice, std::unordered_map<uint32_t, uint32_t>> _deviceQueueCounts;
static std::mutex _deviceQueueCountsMutex;

// Every VkQueue of the device, mapped to the family it came from, plus that device's family
// properties. A command buffer may only be submitted to a queue of its pool's family, so the present
// hook needs the family of whatever queue it is handed.
static std::unordered_map<VkQueue, uint32_t> _queueFamilyOfQueue;
static std::vector<VkQueueFamilyProperties> _familyProps;

// The family the overlay's command pools were created from. vkd3d-proton does not present on the
// first graphics queue, so this is corrected to the presenting queue's family on first use.
static uint32_t _overlayQueueFamily = UINT32_MAX;

// Whether an overlay submit actually armed the fence of each frame. Waiting unconditionally made a
// single failed submit leave its fence unsignalled, and every later frame then paid the full timeout.
static bool _frameFencePending[8] = {};

static uint32_t CreatedQueueCount(VkDevice device, uint32_t family)
{
    std::scoped_lock lock(_deviceQueueCountsMutex);

    auto device_it = _deviceQueueCounts.find(device);

    if (device_it == _deviceQueueCounts.end())
        return 0u;

    auto it = device_it->second.find(family);
    return it == device_it->second.end() ? 0u : it->second;
}

static bool FamilyOfQueue(VkQueue queue, uint32_t* family)
{
    auto it = _queueFamilyOfQueue.find(queue);

    if (it == _queueFamilyOfQueue.end())
        return false;

    *family = it->second;
    return true;
}

// Move the overlay's command pools to another queue family.
//
// The family is fixed at pool creation and cannot be changed, so the pools and their command buffers
// are rebuilt. Called from the present hook the first time the presenting queue turns out to belong to
// a family other than the one guessed at swapchain creation.
static bool RebuildCommandPoolsForFamily(uint32_t family)
{
    VkDevice device = _ImVulkan_Info.Device;

    if (device == VK_NULL_HANDLE || _ImVulkan_Frames == nullptr)
        return false;

    // Nothing of ours may still be running when its pool is destroyed.
    auto idleResult = vkDeviceWaitIdle(device);

    if (idleResult != VK_SUCCESS)
    {
        LOG_ERROR("vkDeviceWaitIdle error: {0:X}", (UINT) idleResult);
        return false;
    }

    for (uint32_t i = 0; i < _scImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];

        if (fd->CommandBuffer != VK_NULL_HANDLE && fd->CommandPool != VK_NULL_HANDLE)
            vkFreeCommandBuffers(device, fd->CommandPool, 1, &fd->CommandBuffer);

        fd->CommandBuffer = VK_NULL_HANDLE;

        if (fd->CommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, fd->CommandPool, VK_NULL_HANDLE);

        fd->CommandPool = VK_NULL_HANDLE;

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;

        auto poolResult = vkCreateCommandPool(device, &poolInfo, NULL, &fd->CommandPool);

        if (poolResult != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateCommandPool error: {0:X}", (UINT) poolResult);
            return false;
        }

        VkCommandBufferAllocateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        bufferInfo.commandPool = fd->CommandPool;
        bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        bufferInfo.commandBufferCount = 1;

        auto bufferResult = vkAllocateCommandBuffers(device, &bufferInfo, &fd->CommandBuffer);

        if (bufferResult != VK_SUCCESS)
        {
            LOG_ERROR("vkAllocateCommandBuffers error: {0:X}", (UINT) bufferResult);
            return false;
        }
    }

    // The device is idle, so nothing is armed any more.
    for (auto& pending : _frameFencePending)
        pending = false;

    _overlayQueueFamily = family;
    _ImVulkan_Info.QueueFamily = family;

    return true;
}

void MenuOverlayVk::NoteDeviceQueues(VkDevice device, const VkDeviceCreateInfo* pCreateInfo)
{
    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || pCreateInfo->pQueueCreateInfos == nullptr)
        return;

    std::scoped_lock lock(_deviceQueueCountsMutex);
    auto& families = _deviceQueueCounts[device];
    families.clear();

    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
    {
        const auto& qci = pCreateInfo->pQueueCreateInfos[i];
        auto& slot = families[qci.queueFamilyIndex];
        slot = std::max(slot, qci.queueCount);
    }

    LOG_DEBUG("recorded {0} queue families for device {1:X}", families.size(), (UINT64) device);
}

static bool DestroyVulkanObjectsLocked(bool shutdown);

// These hooks see vkd3d-proton's presents as well as a native Vulkan game's. swapchainApi is DX12
// only once a wrapped DXGI swapchain has presented from a D3D12 queue, which under Proton means
// vkd3d-proton; MenuOverlayDx draws those titles on the game's own queue. Ordering holds: the game's
// first Present sets swapchainApi before vkd3d creates the VkSwapchain inside it.
static bool DxOverlayOwnsBackend()
{
    return State::Instance().swapchainApi == API::DX12 && IdentifyGpu::getPrimaryGpu().usesDxvk;
}

static void SetVkObjectName(VkDevice device, VkInstance instance, VkObjectType objectType, uint64_t objectHandle,
                            const char* name)
{
    static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;

    if (vkSetDebugUtilsObjectNameEXT == nullptr)
        vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));

    VkDebugUtilsObjectNameInfoEXT info {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = objectType;
    info.objectHandle = objectHandle;
    info.pObjectName = name;

    vkSetDebugUtilsObjectNameEXT(device, &info);
}

// ImGui_ImplVulkan_InitInfo::CheckVkResultFn. The backend calls it on every result, successes
// included; filter or the async logger's 8192-slot blocking queue stalls the present thread.
static void CheckVkResult(VkResult result)
{
    if (result == VK_SUCCESS)
        return;

    LOG_ERROR("ImGui Vulkan backend error: {0:X}", (UINT) result);
}

static void CreateVulkanObjects(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                const VkSwapchainCreateInfoKHR* pCreateInfo, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || *pSwapchain == VK_NULL_HANDLE)
    {
        LOG_WARN(
            "device({0:X}) == VK_NULL_HANDLE || pCreateInfo({1:X}) == nullptr || *pSwapchain({2:X}) == VK_NULL_HANDLE",
            (UINT64) device, (UINT64) pCreateInfo, (UINT64) *pSwapchain);
        return;
    }

    // Below the teardown, so a swapchain recreate still releases the objects of the old one. ImGui
    // holds one renderer backend at a time in io.BackendRendererUserData; leaving it unclaimed is what
    // lets MenuOverlayDx take it.
    if (DxOverlayOwnsBackend())
    {
        LOG_DEBUG("vkd3d-proton D3D12 swapchain, MenuOverlayDx draws the overlay");
        return;
    }
    // Initialize ImGui
    if (!MenuOverlayBase::IsInited() || MenuOverlayBase::Handle() != hwnd)
    {
        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        LOG_DEBUG("MenuOverlayBase::Init");
        MenuOverlayBase::Init(hwnd, false);
    }

    // Starts here, not above: MenuOverlayBase::Init reaches D3D12 device creation, which re-enters the
    // Vulkan hooks and DestroyVulkanObjects on this thread. Everything below is Vulkan object creation
    // and the ImGui backend, with no path back into the hooks.
    std::lock_guard<std::mutex> lock(_vkCleanMutex);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(pCreateInfo->imageExtent.width);
    io.DisplaySize.y = static_cast<float>(pCreateInfo->imageExtent.height);

    VkResult result;

    // Get swapchain image count.
    result = vkGetSwapchainImagesKHR(device, *pSwapchain, &_scImageCount, NULL);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
        return;
    }

    // images[] is a fixed eight and every per-image array below follows _scImageCount, so a swapchain
    // with more images than that would run past the end of all of them.
    constexpr uint32_t kMaxSwapchainImages = 8;

    if (_scImageCount > kMaxSwapchainImages)
    {
        LOG_WARN("swapchain reports {0} images, the overlay can track {1}; the menu is skipped on the rest",
                 _scImageCount, kMaxSwapchainImages);
        _scImageCount = kMaxSwapchainImages;
    }

    VkImage images[kMaxSwapchainImages];
    result = vkGetSwapchainImagesKHR(device, *pSwapchain, &_scImageCount, images);

    // VK_INCOMPLETE only reports that the clamp above took effect.
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
        return;
    }

    for (uint32_t i = 0; i < kMaxSwapchainImages; i++)
        _frameFencePending[i] = false;

    // Alloc ImGui frame structure/semaphores for every image.
    // For convenience, I am using ImGui_ImplVulkanH_Frame in imgui_impl_vulkan.h
    _ImVulkan_Frames = (ImGui_ImplVulkanH_Frame*) IM_ALLOC(sizeof(ImGui_ImplVulkanH_Frame) * _scImageCount);
    _ImVulkan_Semaphores = (VkSemaphore*) IM_ALLOC(sizeof(VkSemaphore) * _scImageCount);

    if (_ImVulkan_Frames == nullptr || _ImVulkan_Semaphores == nullptr)
    {
        // No Vulkan objects have been created in these arrays yet.
        IM_FREE(_ImVulkan_Frames);
        IM_FREE(_ImVulkan_Semaphores);
        _ImVulkan_Frames = nullptr;
        _ImVulkan_Semaphores = nullptr;
        LOG_ERROR("could not allocate Vulkan overlay frame data");
        return;
    }

    memset(_ImVulkan_Frames, 0, sizeof(ImGui_ImplVulkanH_Frame) * _scImageCount);
    memset(_ImVulkan_Semaphores, 0, sizeof(VkSemaphore) * _scImageCount);

    // Publish the owner and allocation count before any fallible Vulkan creation. Teardown must use
    // this count, never the next swapchain's image count, including on a partial initialization.
    _ImVulkan_Info.Device = device;
    _ImVulkan_Info.ImageCount = _scImageCount;

    struct CleanupOnFailure
    {
        ~CleanupOnFailure()
        {
            if (!_vulkanObjectsCreated)
                DestroyVulkanObjectsLocked(false);
        }
    } cleanupOnFailure;

    // Select queue family.
    //
    // It has to be a graphics family the application actually created queues for, because the family's
    // queues are enumerated below and vkGetDeviceQueue on one the device never created is undefined
    // behaviour.
    constexpr uint32_t kMaxQueueFamilies = 8;

    uint32_t queueFamily = 0;
    uint32_t count = 0;
    VkQueueFamilyProperties queues[kMaxQueueFamilies];

    {
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);

        if (count == 0)
        {
            LOG_WARN("PD Queue property count is 0!");
            return;
        }

        if (count > kMaxQueueFamilies)
            count = kMaxQueueFamilies;

        vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, queues);

        bool found = false;

        for (uint32_t i = 0; i < count; i++)
        {
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || CreatedQueueCount(device, i) == 0)
                continue;

            queueFamily = i;
            found = true;
            break;
        }

        // The device was created before the overlay could record its queues, or through a path that
        // does not pass our hook. Fall back to the first graphics family and to queue zero alone,
        // which is what this code did before the check existed.
        if (!found)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    queueFamily = i;
                    found = true;
                    LOG_WARN("no recorded queue count for a graphics family, assuming family {0} queue 0", i);
                    break;
                }
            }
        }

        if (!found)
        {
            LOG_WARN("no graphics queue family available for the overlay");
            return;
        }
    }

    // Map every queue of the device to its family, so the present hook can identify the queue it is
    // handed and move the command pools to that family when it belongs to another one.
    _queueFamilyOfQueue.clear();
    _familyProps.assign(queues, queues + count);
    _overlayQueueFamily = queueFamily;

    for (uint32_t f = 0; f < count; f++)
    {
        uint32_t created = CreatedQueueCount(device, f);

        // Only ask for queues the device was created with; the fallback above knows nothing, so it
        // may look at queue zero of its chosen family alone.
        if (created == 0)
            created = (f == queueFamily) ? 1 : 0;

        for (uint32_t i = 0; i < created; i++)
        {
            VkQueue q = VK_NULL_HANDLE;
            vkGetDeviceQueue(device, f, i, &q);

            if (q != VK_NULL_HANDLE)
                _queueFamilyOfQueue[q] = f;
        }
    }

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    if (queue == VK_NULL_HANDLE)
    {
        LOG_WARN("vkGetDeviceQueue returned no queue for family {0}", queueFamily);
        return;
    }

    LOG_DEBUG("overlay uses queue family {0}, mapped {1} queues across {2} families", queueFamily,
              _queueFamilyOfQueue.size(), count);

    // Create the render pool
    VkDescriptorPool pool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sampler_pool_size = {};
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = 8; // required by ImGui 1.92

        VkDescriptorPoolCreateInfo desc_pool_info = {};
        desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        desc_pool_info.maxSets = 8;
        desc_pool_info.poolSizeCount = 1;
        desc_pool_info.pPoolSizes = &sampler_pool_size;

        result = vkCreateDescriptorPool(device, &desc_pool_info, NULL, &pool);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateDescriptorPool error: {0:X}", (UINT) result);
            return;
        }

        _ImVulkan_Info.DescriptorPool = pool;
    }

    // Create the render pass
    {
        VkAttachmentDescription attachment_desc = {};

        attachment_desc.format = pCreateInfo->imageFormat;
        attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment_desc.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_attachment = {};
        color_attachment.attachment = 0;
        color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment_desc;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;

        result = vkCreateRenderPass(device, &render_pass_info, NULL, &_vkRenderPass);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateRenderPass error: {0:X}", (UINT) result);
            return;
        }
    }

    // Create The Image Views
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;

        info.format = pCreateInfo->imageFormat;
        info.components.r = VK_COMPONENT_SWIZZLE_R;
        info.components.g = VK_COMPONENT_SWIZZLE_G;
        info.components.b = VK_COMPONENT_SWIZZLE_B;
        info.components.a = VK_COMPONENT_SWIZZLE_A;

        VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        info.subresourceRange = image_range;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            fd->Backbuffer = images[i];
            info.image = fd->Backbuffer;

            result = vkCreateImageView(device, &info, NULL, &fd->BackbufferView);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateImageView error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_IMAGE_VIEW, (UINT64) fd->BackbufferView,
                            "ImGui Backbuffer View");
#endif
        }
    }

    // Create frame Buffer
    {
        VkImageView attachment[1];
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = _vkRenderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachment;

        info.width = pCreateInfo->imageExtent.width;
        info.height = pCreateInfo->imageExtent.height;

        info.layers = 1;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            attachment[0] = fd->BackbufferView;
            result = vkCreateFramebuffer(device, &info, NULL, &fd->Framebuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFramebuffer error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FRAMEBUFFER, (UINT64) fd->Framebuffer,
                            "ImGui Backbuffer Framebuffer");
#endif
        }
    }

    // Create command pools, command buffers, fences, and semaphores for every image
    for (uint32_t i = 0; i < _scImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
        VkSemaphore* fsd = &_ImVulkan_Semaphores[i];
        {
            VkCommandPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = queueFamily;
            result = vkCreateCommandPool(device, &info, NULL, &fd->CommandPool);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateCommandPool error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_POOL, (UINT64) fd->CommandPool,
                            "ImGui Backbuffer Command Pool");
#endif
        }

        {
            VkCommandBufferAllocateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandPool = fd->CommandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            result = vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkAllocateCommandBuffers error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_BUFFER, (UINT64) fd->CommandBuffer,
                            "ImGui Backbuffer Command Buffer");
#endif
        }

        {
            VkFenceCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(device, &info, NULL, &fd->Fence);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFence error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FENCE, (UINT64) fd->Fence, "ImGui Backbuffer Fence");
#endif
        }

        {
            VkSemaphoreCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(device, &info, NULL, fsd);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateSemaphore error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_SEMAPHORE, (UINT64) fsd, "ImGui Backbuffer Semaphore");
#endif
        }
    }

    // Initialize ImGui and upload fonts
    {
        _ImVulkan_Info.Instance = instance;
        _ImVulkan_Info.PhysicalDevice = pd;
        _ImVulkan_Info.Device = device;
        _ImVulkan_Info.QueueFamily = queueFamily;
        _ImVulkan_Info.Queue = queue;
        _ImVulkan_Info.DescriptorPool = pool;
        _ImVulkan_Info.Subpass = 0;
        _ImVulkan_Info.MinImageCount = pCreateInfo->minImageCount;
        _ImVulkan_Info.ImageCount = _scImageCount;
        _ImVulkan_Info.Allocator = NULL;
        _ImVulkan_Info.RenderPass = _vkRenderPass;
        _ImVulkan_Info.CheckVkResultFn = CheckVkResult;

        bool initResult = ImGui_ImplVulkan_Init(&_ImVulkan_Info);
        _vulkanBackendInited = ImGui::GetIO().BackendRendererUserData != nullptr;
        LOG_DEBUG("ImGui_ImplVulkan_Init result: {}", initResult);

        if (!initResult)
            return;

        // Upload Fonts
        // Use any command queue
        VkCommandPool command_pool = _ImVulkan_Frames[0].CommandPool;
        VkCommandBuffer command_buffer = _ImVulkan_Frames[0].CommandBuffer;
        result = vkResetCommandPool(device, command_pool, 0);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        // initResult = ImGui_ImplVulkan_CreateFontsTexture();
        // LOG_DEBUG("ImGui_ImplVulkan_CreateFontsTexture result: {}", initResult);

        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;

        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkEndCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        result = vkQueueSubmit(queue, 1, &end_info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) result);
            return;
        }

        result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkDeviceWaitIdle error: {0:X}", (UINT) result);
            return;
        }
    }

    _vulkanObjectsCreated = true;
    State::Instance().menuOverlayIsVulkan = true;
    LOG_FUNC_RESULT(_vulkanObjectsCreated);
}

// Caller holds _vkCleanMutex and _vkPresentMutex.
static bool DestroyVulkanObjectsLocked(bool shutdown)
{
    State::Instance().menuOverlayIsVulkan = false;

    // _ImVulkan_Info is zeroed at the tail under this lock; read it here, not before.
    if (_ImVulkan_Info.Device == VK_NULL_HANDLE)
        return !_vulkanDeviceLost;

    if (!shutdown)
        LOG_FUNC();

    _vulkanObjectsCreated = false;

    auto result = vkDeviceWaitIdle(_ImVulkan_Info.Device);
    if (result != VK_SUCCESS && !shutdown)
        LOG_WARN("vkDeviceWaitIdle error: {0:X}", (UINT) result);

    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
    {
        // The GPU may still be using these objects. Keep both handles and backing allocations so
        // a later teardown can retry; creation must not overwrite them in the meantime.
        return false;
    }

    _vulkanDeviceLost = result == VK_ERROR_DEVICE_LOST;

    // ImGui also owns Vulkan objects: shut it down only after a successful drain. On device loss its
    // backend is deliberately abandoned and this overlay stays disabled for the rest of the session.
    if (!_vulkanDeviceLost && _vulkanBackendInited)
    {
        ImGui_ImplVulkan_Shutdown(false);
        _vulkanBackendInited = false;
    }

    for (uint32_t i = 0; !_vulkanDeviceLost && i < _ImVulkan_Info.ImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];

        if (fd->Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(_ImVulkan_Info.Device, fd->Fence, VK_NULL_HANDLE);
            fd->Fence = VK_NULL_HANDLE;
        }

        if (fd->CommandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(_ImVulkan_Info.Device, fd->CommandPool, 1, &fd->CommandBuffer);
            fd->CommandBuffer = VK_NULL_HANDLE;
        }

        if (fd->CommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(_ImVulkan_Info.Device, fd->CommandPool, VK_NULL_HANDLE);
            fd->CommandPool = VK_NULL_HANDLE;
        }

        if (fd->BackbufferView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(_ImVulkan_Info.Device, fd->BackbufferView, VK_NULL_HANDLE);
            fd->BackbufferView = VK_NULL_HANDLE;
        }

        if (fd->Framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(_ImVulkan_Info.Device, fd->Framebuffer, VK_NULL_HANDLE);
            fd->Framebuffer = VK_NULL_HANDLE;
        }

        if (_ImVulkan_Semaphores[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(_ImVulkan_Info.Device, _ImVulkan_Semaphores[i], VK_NULL_HANDLE);
            _ImVulkan_Semaphores[i] = VK_NULL_HANDLE;
        }
    }

    if (!_vulkanDeviceLost)
    {
        if (_vkRenderPass)
            vkDestroyRenderPass(_ImVulkan_Info.Device, _vkRenderPass, VK_NULL_HANDLE);

        if (_ImVulkan_Info.DescriptorPool)
            vkDestroyDescriptorPool(_ImVulkan_Info.Device, _ImVulkan_Info.DescriptorPool, VK_NULL_HANDLE);
    }

    _vkRenderPass = VK_NULL_HANDLE;

    // Every child handle has now been destroyed on an idle device or abandoned on a lost device.
    // Free while the old ImageCount is still available, before clearing ownership for the next build.
    IM_FREE(_ImVulkan_Frames);
    IM_FREE(_ImVulkan_Semaphores);
    _ImVulkan_Frames = nullptr;
    _ImVulkan_Semaphores = nullptr;
    _scImageCount = 0;
    _ImVulkan_Info = {};

    _queueFamilyOfQueue.clear();
    _familyProps.clear();
    _overlayQueueFamily = UINT32_MAX;

    for (auto& pending : _frameFencePending)
        pending = false;

    return !_vulkanDeviceLost;
}

void MenuOverlayVk::DestroyVulkanObjects(bool shutdown)
{
    std::scoped_lock presentLock(_vkPresentMutex);
    std::scoped_lock cleanLock(_vkCleanMutex);
    DestroyVulkanObjectsLocked(shutdown);
}

bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();

    if (!_vulkanObjectsCreated)
        return true;

    if (!MenuOverlayBase::IsInited() || _ImVulkan_Info.Device == VK_NULL_HANDLE)
        return true;

    if (pPresentInfo->swapchainCount == 0)
        return false;

    // Streamline's DLSS-G pacer presents from a thread of its own while the game's render thread also
    // presents, so two threads reach this function at once. ImGui has a single global context and no
    // internal locking, and the per-image frame data below is shared, so the whole body is serialised.
    //
    // _vkCleanMutex is taken second and always in this order: it keeps a teardown from running the
    // destructors underneath a present already in flight. Nothing takes these two the other way round.
    std::scoped_lock presentLock(_vkPresentMutex);
    std::scoped_lock cleanLock(_vkCleanMutex);

    // The teardown may have run while this thread waited for the locks.
    if (!_vulkanObjectsCreated || _ImVulkan_Info.Device == VK_NULL_HANDLE)
        return true;

    LOG_DEBUG("rendering menu, swapchain count: {0}", pPresentInfo->swapchainCount);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    ImGui_ImplVulkan_NewFrame();

    if (State::Instance().delayMenuRenderBy > 0)
        State::Instance().delayMenuRenderBy--;

    if (!MenuOverlayBase::RenderMenu())
        return true;

    // From here on RenderMenu has produced a frame, and ImGui::Render must be called exactly once on
    // every path out of this function.
    if (State::Instance().delayMenuRenderBy != 0)
    {
        ImGui::Render();
        return true;
    }

    const uint32_t idx = pPresentInfo->pImageIndices[0];

    if (idx >= _scImageCount)
    {
        LOG_WARN("present image index {0} is outside the {1} frames the overlay created", idx, _scImageCount);
        ImGui::Render();
        return true;
    }

    // A command buffer may only be submitted to a queue of the family its pool was created from, and
    // the semaphore hand-over below only holds if the overlay and the present run on one queue.
    // vkd3d-proton does not present on the first graphics queue, so when the presenting queue belongs
    // to another family the pools are moved to it rather than the menu being dropped. Submitting
    // across families is what cost a device: it left the fence unsignalled and ended in DEVICE_LOST.
    uint32_t presentFamily = UINT32_MAX;

    if (!FamilyOfQueue(queue, &presentFamily))
    {
        static bool warnedUnknown = false;

        if (!warnedUnknown)
        {
            warnedUnknown = true;
            LOG_WARN("presenting queue {0:X} is not one of the device's queues, menu disabled on it", (UINT64) queue);
        }

        ImGui::Render();
        return true;
    }

    if (presentFamily != _overlayQueueFamily)
    {
        const bool graphics = presentFamily < _familyProps.size() &&
                              (_familyProps[presentFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

        if (!graphics)
        {
            static bool warnedNonGraphics = false;

            if (!warnedNonGraphics)
            {
                warnedNonGraphics = true;
                LOG_WARN("present happens on queue family {0} (flags {1:X}), which cannot run a render pass; "
                         "the Vulkan overlay is not possible on this swapchain",
                         presentFamily,
                         presentFamily < _familyProps.size() ? (UINT) _familyProps[presentFamily].queueFlags : 0u);
            }

            ImGui::Render();
            return true;
        }

        LOG_WARN("present happens on queue family {0}, not {1}; moving the overlay's command pools to it",
                 presentFamily, _overlayQueueFamily);

        if (!RebuildCommandPoolsForFamily(presentFamily))
        {
            LOG_ERROR("could not move the overlay's command pools to family {0}, menu disabled", presentFamily);
            ImGui::Render();
            return true;
        }

        _ImVulkan_Info.Queue = queue;
        LOG_WARN("overlay now records on queue family {0}", presentFamily);
    }

    // The overlay takes over whatever the present was going to wait on, and the present waits on the
    // overlay instead, so every semaphore keeps exactly one signal and one wait. pWaitDstStageMask
    // needs one entry per wait semaphore and none of them may be zero, so the array is sized to the
    // real count; dropping any of the game's semaphores would leave them unwaited, so an unexpectedly
    // long list skips the menu instead.
    constexpr uint32_t kMaxWaitSemaphores = 16;

    if (pPresentInfo->waitSemaphoreCount > kMaxWaitSemaphores)
    {
        LOG_WARN("present waits on {0} semaphores, more than the {1} the overlay can take over",
                 pPresentInfo->waitSemaphoreCount, kMaxWaitSemaphores);
        ImGui::Render();
        return true;
    }

    ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[idx];

    // Only wait on a fence an overlay submit actually armed.
    //
    // Bounded: an unsignalled fence (a present race with Streamline DLSS-G under vkd3d-proton, say)
    // must not hang the present thread forever. Skip the menu for this frame instead.
    if (_frameFencePending[idx])
    {
        auto fenceResult = vkWaitForFences(_ImVulkan_Info.Device, 1, &fd->Fence, VK_TRUE, 1000000000ull);

        if (fenceResult != VK_SUCCESS)
        {
            LOG_WARN("vkWaitForFences returned {0:X}, skipping menu render this frame", (UINT) fenceResult);
            ImGui::Render();
            return true;
        }

        _frameFencePending[idx] = false;
    }

    vkResetFences(_ImVulkan_Info.Device, 1, &fd->Fence);

    {
        vkResetCommandPool(_ImVulkan_Info.Device, fd->CommandPool, 0);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(fd->CommandBuffer, &info);
    }

    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = _vkRenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.x);
        info.renderArea.extent.height = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.y);
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    auto ecbResult = vkEndCommandBuffer(fd->CommandBuffer);

    if (ecbResult != VK_SUCCESS)
    {
        // The present itself is still valid: nothing has been taken from it yet. Let it through
        // without the menu rather than failing the frame.
        LOG_ERROR("vkEndCommandBuffer error: {0:X}", (UINT) ecbResult);
        return true;
    }

    LOG_DEBUG("waitSemaphoreCount: {0}", pPresentInfo->waitSemaphoreCount);

    VkPipelineStageFlags waitStages[kMaxWaitSemaphores];

    for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
        waitStages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    // Indexed by swapchain image, the same index as the fence and the command buffer above. Cycling
    // this on a frame counter instead let the index come round again while an earlier present was
    // still waiting on that semaphore, which is a hang with no diagnostic.
    VkSemaphore signalSemaphore = _ImVulkan_Semaphores[idx];

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &fd->CommandBuffer;
    submit_info.pWaitDstStageMask = waitStages;
    submit_info.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submit_info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &signalSemaphore;

    // On the queue that is presenting, not on the one captured at swapchain creation.
    auto qResult = vkQueueSubmit(queue, 1, &submit_info, fd->Fence);

    if (qResult != VK_SUCCESS)
    {
        LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) qResult);
        return true;
    }

    _frameFencePending[idx] = true;

    // The hook calls the real present after this function releases its locks. Do not lend it storage
    // in the swapchain arrays, which a concurrent teardown can now free. Each presenting thread keeps
    // its own copy until its next call; GPU semaphore/pacer synchronization is otherwise unchanged.
    static thread_local VkSemaphore presentWaitSemaphore = VK_NULL_HANDLE;
    presentWaitSemaphore = signalSemaphore;
    pPresentInfo->waitSemaphoreCount = 1;
    pPresentInfo->pWaitSemaphores = &presentWaitSemaphore;

    return true;
}

void MenuOverlayVk::CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                    const VkSwapchainCreateInfoKHR* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    std::scoped_lock presentLock(_vkPresentMutex);
    std::scoped_lock cleanLock(_vkCleanMutex);

    if (_vulkanDeviceLost)
        return;

    // Drain the old backend before changing its window/context as well as before replacing arrays.
    if (_ImVulkan_Info.Device != VK_NULL_HANDLE && !DestroyVulkanObjectsLocked(false))
        return;

    // The predicate also guards the shutdown below: where MenuOverlayDx owns ImGui, the renderer
    // backend behind io.BackendRendererUserData is a DX one and ImGui_ImplVulkan_Shutdown would free
    // it as if it were Vulkan. CreateVulkanObjects still runs, to release objects of its own.
    if (MenuOverlayBase::Handle() != hwnd && !DxOverlayOwnsBackend())
    {
        LOG_DEBUG("MenuOverlayBase::Handle() != _hwnd");

        if (MenuOverlayBase::IsInited())
        {
            ImGui_ImplVulkan_Shutdown(false);
            LOG_DEBUG("MenuOverlayBase::Shutdown();");
            MenuOverlayBase::Shutdown();
        }

        LOG_DEBUG("MenuOverlayBase::Init({0:X})", (UINT64) hwnd);
        MenuOverlayBase::Init(hwnd, false);
    }

    CreateVulkanObjects(device, pd, instance, hwnd, pCreateInfo, pSwapchain);

    if (_vulkanObjectsCreated)
    {
        _isInited = true;
        MenuOverlayBase::VulkanReady();
        LOG_DEBUG("Vulkan ready");
    }
}
