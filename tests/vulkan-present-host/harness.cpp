// A Win32 Vulkan application for the Vulkan present host's stage 1 (docs/dlssnr/design/nr-vulkan-present-host.md).
//
// It loads the production OptiScaler.dll as dxgi.dll, uninstrumented, and drives real Vulkan under Wine through the
// objects the host has to keep apart: two windows, three devices (the third created after the second is destroyed,
// so a reused handle value is possible), a swapchain recreated through oldSwapchain, a present from a non-graphics
// compute family when the surface allows one, presents with zero, one and two wait semaphores, vkGetDeviceQueue and
// vkGetDeviceQueue2. It never calls NGX or an upscaler.
//
// It asserts nothing about the host itself. It writes every fact it controls -- handles, families, usages, image
// counts, the order of its presents and their waits -- to result.json, and whether vulkan-1.dll's exports were
// patched, by comparing their first bytes before OptiScaler loaded with the same bytes after the first swapchain.
// run.py holds OptiScaler.log against those facts. Its own checks are about the run being valid: validation active
// (proved by a deliberately invalid fence the layer must reject), zero validation errors, every call succeeding.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::atomic<unsigned> validationErrors { 0 };
static std::atomic<unsigned> wineInjected { 0 };
static std::atomic<bool> validationActive { false };
static std::atomic<bool> validationProbe { false };

static void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    const char* text = data->pMessage ? data->pMessage : "";
    if (validationProbe && data->pMessageIdName && !strcmp(data->pMessageIdName, "VUID-VkFenceCreateInfo-flags-parameter"))
    {
        validationActive = true;
        std::fprintf(stderr, "VALIDATION PROBE (expected): %s\n", text);
        return VK_TRUE; // the layer skips the invalid call
    }
    // winevulkan (11.19) appends VK_EXT_external_memory_dma_buf to the host-side device it creates for every
    // vkCreateDevice, without VK_KHR_external_memory_fd, and the native layer sees Wine's list rather than ours: the
    // index it names is past the one extension this application asks for. Counted and reported, not an error of
    // the application or of OptiScaler, which passes the create info down unchanged.
    if (data->pMessageIdName && !strcmp(data->pMessageIdName, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387") &&
        strstr(text, "VK_EXT_external_memory_dma_buf") != nullptr && strstr(text, "ppEnabledExtensionNames[0]") == nullptr)
    {
        ++wineInjected;
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        ++validationErrors;
        std::fprintf(stderr, "VALIDATION: %s\n", text);
    }
    return VK_FALSE;
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    return DefWindowProcW(window, message, w, l);
}

static void Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static std::string Hex(uint64_t value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

template <typename T> static uint64_t H(T handle) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle)); }

// --- Export prologues ----------------------------------------------------------------------------------------------

static const char* const kWatched[] = {
    "vkCreateInstance",        "vkDestroyInstance",     "vkEnumeratePhysicalDevices", "vkCreateDevice",
    "vkCreateWin32SurfaceKHR", "vkDestroySurfaceKHR",   "vkDestroyDevice",            "vkGetDeviceQueue",
    "vkGetDeviceQueue2",       "vkCreateSwapchainKHR",  "vkDestroySwapchainKHR",      "vkGetSwapchainImagesKHR",
    "vkAcquireNextImageKHR",   "vkQueuePresentKHR",     "vkQueueSubmit",              "vkCreateFence",
};
constexpr size_t kWatchedCount = sizeof(kWatched) / sizeof(kWatched[0]);

struct Prologue
{
    void* address = nullptr;
    std::array<unsigned char, 16> before {};
    std::array<unsigned char, 16> after {};
    bool sameAsDeviceProc = false;
    bool deviceProcChecked = false;
};

static std::array<Prologue, kWatchedCount> prologues;

static void SnapshotPrologues(bool after)
{
    HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
    Require(vulkan != nullptr, "vulkan-1.dll is not loaded");
    for (size_t i = 0; i < kWatchedCount; ++i)
    {
        void* address = reinterpret_cast<void*>(GetProcAddress(vulkan, kWatched[i]));
        Require(address != nullptr, "vulkan-1.dll lacks a watched export");
        if (!after)
            prologues[i].address = address;
        Require(prologues[i].address == address, "an export's address moved");
        std::memcpy(after ? prologues[i].after.data() : prologues[i].before.data(), address, 16);
    }
}

// --- The application -------------------------------------------------------------------------------------------

struct Present
{
    int window = 0; // 1 or 2
    uint64_t swapchain = 0;
    uint64_t queue = 0;
    uint64_t device = 0;
    uint32_t waits = 0;
    uint32_t image = 0;
    std::string swapchainName;
    std::string deviceName;
};

struct SwapchainFacts
{
    std::string name;
    uint64_t handle = 0;
    uint64_t device = 0;
    int window = 0;
    uint32_t usage = 0;
    uint32_t format = 0;
    uint32_t colorSpace = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t images = 0;
    uint32_t sharing = 0;
    uint32_t surfaceUsage = 0;
};

struct DeviceFacts
{
    std::string name;
    uint64_t handle = 0;
    std::vector<std::pair<uint32_t, uint32_t>> requested; // family, count
};

struct App
{
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND owner = nullptr;
    HWND windows[2] {};
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surfaces[2] {};
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::vector<VkQueueFamilyProperties> families;
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX; // non-graphics, compute, presents to window 1; UINT32_MAX when none
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace {};
    bool lateLoad = false;
    bool oneWindow = false; // --one-window: window 1 and device 1 only

    std::vector<Present> presents;
    std::vector<SwapchainFacts> swapchains;
    std::vector<DeviceFacts> devices;
    bool thirdReusedSecond = false;

    struct Target
    {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<bool> initialized;
        int window = 0;
        std::string deviceName;
        std::string swapchainName;
    };

    HWND MakeWindow(int index, int x, int y, int w, int h)
    {
        // An owned dialog, as in tests/vulkan-overlay: a tiling window manager floats it instead of re-tiling the
        // first window when the second appears, which would put an out-of-date swapchain under its next present.
        HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OptiScalerVkPresentHostHarness",
                                      index == 0 ? L"OptiScaler present host test 1" : L"OptiScaler present host test 2",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, w, h, owner, nullptr, module, nullptr);
        Require(window != nullptr, "no Win32 window under Wine");
        ShowWindow(window, SW_SHOW);
        Pump();
        return window;
    }

    VkSurfaceKHR MakeSurface(HWND window)
    {
        VkWin32SurfaceCreateInfoKHR info { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        info.hinstance = module;
        info.hwnd = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        Check(vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        return surface;
    }

    void Init()
    {
        WNDCLASSW cls {};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = module;
        cls.lpszClassName = L"OptiScalerVkPresentHostHarness";
        Require(RegisterClassW(&cls) != 0, "RegisterClass failed");
        owner = CreateWindowW(cls.lpszClassName, L"OptiScaler present host owner", 0, 0, 0, 1, 1, nullptr, nullptr,
                              module, nullptr);
        Require(owner != nullptr, "could not create the dialog owner");
        windows[0] = MakeWindow(0, 60, 60, 640, 480);

        // The exports before OptiScaler exists in the process.
        SnapshotPrologues(false);

        auto dll = LoadLibraryW(L".\\dxgi.dll");
        Require(dll != nullptr, "could not load OptiScaler as dxgi.dll");

        // vulkan-1.dll loaded again after OptiScaler: its loader hook hands the module to VulkanHooks::Hook a second
        // time, the late-load path.
        lateLoad = LoadLibraryW(L"vulkan-1.dll") != nullptr;

        VkDebugUtilsMessengerCreateInfoEXT debug { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = DebugMessage;
        const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                     VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler Vulkan present host harness";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pNext = &debug;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 3;
        info.ppEnabledExtensionNames = extensions;
        Check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        Require(createDebug != nullptr, "no debug messenger entry point");
        Check(createDebug(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");

        surfaces[0] = MakeSurface(windows[0]);

        uint32_t n = 0;
        Check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "vkEnumeratePhysicalDevices");
        Require(n > 0, "no Vulkan physical device under Wine");
        std::vector<VkPhysicalDevice> all(n);
        Check(vkEnumeratePhysicalDevices(instance, &n, all.data()), "vkEnumeratePhysicalDevices");

        for (VkPhysicalDevice candidate : all)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
            std::vector<VkQueueFamilyProperties> props(count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, props.data());

            uint32_t graphics = UINT32_MAX, compute = UINT32_MAX;
            for (uint32_t i = 0; i < count; ++i)
            {
                VkBool32 present = VK_FALSE;
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surfaces[0], &present), "surface support");
                if (!present || props[i].queueCount == 0)
                    continue;
                if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics == UINT32_MAX)
                    graphics = i;
                if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    compute == UINT32_MAX)
                    compute = i;
            }

            if (graphics != UINT32_MAX)
            {
                physical = candidate;
                families = props;
                graphicsFamily = graphics;
                computeFamily = compute;
                break;
            }
        }

        Require(physical != VK_NULL_HANDLE, "no device with a graphics queue that presents to the window");
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("GPU: %s; graphics family %u, compute-only present family %d\n", properties.deviceName,
                    graphicsFamily, computeFamily == UINT32_MAX ? -1 : static_cast<int>(computeFamily));

        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surfaces[0], &n, nullptr), "surface formats");
        std::vector<VkSurfaceFormatKHR> formats(n);
        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surfaces[0], &n, formats.data()), "surface formats");
        auto chosen = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
                chosen = f;
        format = chosen.format;
        colorSpace = chosen.colorSpace;
    }

    VkDevice MakeDevice(const char* name, bool withCompute)
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queues[2] {};
        uint32_t count = 0;
        for (uint32_t family : { graphicsFamily, withCompute ? computeFamily : UINT32_MAX })
        {
            if (family == UINT32_MAX)
                continue;
            queues[count].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queues[count].queueFamilyIndex = family;
            queues[count].queueCount = 1;
            queues[count].pQueuePriorities = &priority;
            ++count;
        }
        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.queueCreateInfoCount = count;
        info.pQueueCreateInfos = queues;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        VkDevice device = VK_NULL_HANDLE;
        Check(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");

        DeviceFacts facts;
        facts.name = name;
        facts.handle = H(device);
        for (uint32_t i = 0; i < count; ++i)
            facts.requested.emplace_back(queues[i].queueFamilyIndex, queues[i].queueCount);
        devices.push_back(facts);
        return device;
    }

    void ProbeValidation(VkDevice device)
    {
        VkFenceCreateInfo invalid { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        invalid.flags = 0x80000000u;
        VkFence fence = VK_NULL_HANDLE;
        validationProbe = true;
        const VkResult result = vkCreateFence(device, &invalid, nullptr, &fence);
        validationProbe = false;
        if (fence)
            vkDestroyFence(device, fence, nullptr);
        Require(validationActive && result == VK_ERROR_VALIDATION_FAILED_EXT,
                "validation negative control failed: the invalid fence flag was not intercepted");
    }

    void MakeSwapchain(Target& target, const char* name, VkSurfaceKHR surface, uint32_t usage)
    {
        VkSurfaceCapabilitiesKHR caps {};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
        Require((caps.supportedUsageFlags & usage) == usage, "the surface does not offer the usage asked for");
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            RECT rect;
            GetClientRect(windows[target.window - 1], &rect);
            extent.width = std::clamp(uint32_t(rect.right), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(rect.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        VkSwapchainCreateInfoKHR info { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = surface;
        info.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
        info.imageFormat = format;
        info.imageColorSpace = colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        for (auto alpha : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR })
        {
            if (caps.supportedCompositeAlpha & alpha)
            {
                info.compositeAlpha = alpha;
                break;
            }
        }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        const VkSwapchainKHR old = target.swapchain;
        info.oldSwapchain = old;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        Check(vkCreateSwapchainKHR(target.device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
        if (old != VK_NULL_HANDLE)
        {
            Check(vkDeviceWaitIdle(target.device), "idle before destroying the old swapchain");
            vkDestroySwapchainKHR(target.device, old, nullptr);
        }
        target.swapchain = swapchain;
        target.swapchainName = name;
        uint32_t count = 0;
        Check(vkGetSwapchainImagesKHR(target.device, swapchain, &count, nullptr), "swapchain image count");
        target.images.resize(count);
        Check(vkGetSwapchainImagesKHR(target.device, swapchain, &count, target.images.data()), "swapchain images");
        target.initialized.assign(count, false);

        SwapchainFacts facts;
        facts.name = name;
        facts.handle = H(swapchain);
        facts.device = H(target.device);
        facts.window = target.window;
        facts.usage = usage;
        facts.format = format;
        facts.colorSpace = colorSpace;
        facts.width = extent.width;
        facts.height = extent.height;
        facts.images = count;
        facts.sharing = info.imageSharingMode;
        facts.surfaceUsage = caps.supportedUsageFlags;
        swapchains.push_back(facts);
        std::printf("SWAPCHAIN %s %ux%u images=%u usage=0x%X\n", name, extent.width, extent.height, count, usage);
    }

    // One frame: acquire, clear on the present queue, present with `waits` semaphores (0, 1 or 2).
    void Frame(Target& target, uint32_t waits)
    {
        Pump();
        VkDevice device = target.device;
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE, extra = VK_NULL_HANDLE;
        Check(vkCreateSemaphore(device, &si, nullptr, &acquired), "acquire semaphore");
        Check(vkCreateSemaphore(device, &si, nullptr, &rendered), "render semaphore");
        Check(vkCreateSemaphore(device, &si, nullptr, &extra), "extra semaphore");
        uint32_t index = 0;
        const VkResult acquire = vkAcquireNextImageKHR(device, target.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        Require(acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR, "acquire failed");

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = target.family;
        VkCommandPool pool = VK_NULL_HANDLE;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command pool");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &ai, &cmd), "command buffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(cmd, &begin), "begin");
        VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = target.initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = target.images[index];
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // TRANSFER as the source stage, matching the acquire semaphore's wait stage (see tests/vulkan-overlay).
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkClearColorValue color { { target.window == 1 ? 0.05f : 0.2f, 0.12f, 0.2f, 1.0f } };
        vkCmdClearColorImage(cmd, target.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                             &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        Check(vkEndCommandBuffer(cmd), "end");

        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = waits > 0 ? 1 : 0;
        submit.pSignalSemaphores = &rendered;
        Check(vkQueueSubmit(target.queue, 1, &submit, VK_NULL_HANDLE), "submit clear");

        VkSemaphore waitList[2] = { rendered, extra };
        if (waits == 2)
        {
            // A second, empty batch on the same queue, after the clear: its signal is the present's second wait.
            VkSubmitInfo empty { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            empty.signalSemaphoreCount = 1;
            empty.pSignalSemaphores = &extra;
            Check(vkQueueSubmit(target.queue, 1, &empty, VK_NULL_HANDLE), "submit extra signal");
        }
        else if (waits == 0)
        {
            // No semaphore at all: the host has seen the clear complete before presenting.
            Check(vkQueueWaitIdle(target.queue), "wait for the clear before a present with no waits");
        }

        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = waits;
        present.pWaitSemaphores = waits > 0 ? waitList : nullptr;
        present.swapchainCount = 1;
        present.pSwapchains = &target.swapchain;
        present.pImageIndices = &index;
        const VkResult result = vkQueuePresentKHR(target.queue, &present);
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "present failed");
        presents.push_back(Present { target.window, H(target.swapchain), H(target.queue), H(device), waits, index,
                                     target.swapchainName, target.deviceName });

        Check(vkQueueWaitIdle(target.queue), "wait for presentation");
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroySemaphore(device, acquired, nullptr);
        vkDestroySemaphore(device, rendered, nullptr);
        vkDestroySemaphore(device, extra, nullptr);
        target.initialized[index] = true;
    }

    void Run()
    {
        Init();

        // Window 1, device 1: a graphics queue through vkGetDeviceQueue, and a second queue -- the compute-only
        // family when the surface takes presents from one, else the graphics queue again -- through vkGetDeviceQueue2.
        const bool compute = computeFamily != UINT32_MAX;
        Target one;
        one.window = 1;
        one.device = MakeDevice("d1", compute);
        one.deviceName = "d1";
        ProbeValidation(one.device);
        one.family = graphicsFamily;
        vkGetDeviceQueue(one.device, graphicsFamily, 0, &one.queue);
        VkDeviceQueueInfo2 second { VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2 };
        second.queueFamilyIndex = compute ? computeFamily : graphicsFamily;
        second.queueIndex = 0;
        VkQueue secondQueue = VK_NULL_HANDLE;
        vkGetDeviceQueue2(one.device, &second, &secondQueue);
        Require(secondQueue != VK_NULL_HANDLE, "vkGetDeviceQueue2 returned no queue");

        for (size_t i = 0; i < kWatchedCount; ++i)
        {
            auto byDevice = reinterpret_cast<void*>(vkGetDeviceProcAddr(one.device, kWatched[i]));
            if (byDevice != nullptr)
            {
                prologues[i].deviceProcChecked = true;
                prologues[i].sameAsDeviceProc = byDevice == prologues[i].address;
            }
        }

        // Swapchain 1: no TRANSFER_SRC, so the line has to say the round trip needs it.
        MakeSwapchain(one, "sc1", surfaces[0], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        SnapshotPrologues(true);
        for (int frame = 0; frame < 12; ++frame)
            Frame(one, 1);

        // Swapchain 2 replaces it through oldSwapchain, with both transfer usages, presented from the second queue:
        // two presents with no waits, then two waits each, to window 1's 100th present.
        VkSurfaceCapabilitiesKHR caps {};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surfaces[0], &caps), "surface capabilities");
        Require(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "the surface offers no TRANSFER_SRC");
        one.queue = secondQueue;
        one.family = second.queueFamilyIndex;
        MakeSwapchain(one, "sc2", surfaces[0],
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        for (int frame = 0; frame < 88; ++frame)
            Frame(one, frame < 2 ? 0 : 2);

        if (oneWindow)
        {
            // The legacy Vulkan overlay (OverlayMenu=true, no marker) dies when a second window's swapchain is
            // created on a second device -- a null read inside MenuOverlayVk::CreateSwapchain, with this host off
            // too -- so the cases that run it stop after window 1. That overlay also keeps objects on the device
            // and has no hook on its destruction (VUID-vkDestroyDevice-device-05137; tests/vulkan-overlay calls a
            // test-only teardown export the production DLL does not have), so the device and the instance are left
            // to process exit here. See README.md.
            Check(vkDeviceWaitIdle(one.device), "final idle");
            vkDestroySwapchainKHR(one.device, one.swapchain, nullptr);
            Write(compute, second.queueFamilyIndex, H(secondQueue));
            Require(validationErrors == 0, "Vulkan validation reported errors; see the run log");
            return;
        }

        // Window 2, device 2, alive beside device 1.
        windows[1] = MakeWindow(1, 760, 80, 480, 360);
        surfaces[1] = MakeSurface(windows[1]);
        VkBool32 supported = VK_FALSE;
        Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, graphicsFamily, surfaces[1], &supported), "surface support");
        Require(supported, "window 2's surface takes no presents from the graphics family");
        Target two;
        two.window = 2;
        two.device = MakeDevice("d2", false);
        two.deviceName = "d2";
        two.family = graphicsFamily;
        vkGetDeviceQueue(two.device, graphicsFamily, 0, &two.queue);
        MakeSwapchain(two, "sc3", surfaces[1], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (int frame = 0; frame < 3; ++frame)
            Frame(two, 1);

        // Window 1 once more, between window 2's devices: same identity, no line.
        Frame(one, 1);

        // Device 2 goes, device 3 comes, possibly with device 2's handle value.
        Check(vkDeviceWaitIdle(two.device), "idle device 2");
        vkDestroySwapchainKHR(two.device, two.swapchain, nullptr);
        const uint64_t secondHandle = H(two.device);
        vkDestroyDevice(two.device, nullptr);
        Target three;
        three.window = 2;
        three.device = MakeDevice("d3", false);
        three.deviceName = "d3";
        thirdReusedSecond = H(three.device) == secondHandle;
        three.family = graphicsFamily;
        vkGetDeviceQueue(three.device, graphicsFamily, 0, &three.queue);
        MakeSwapchain(three, "sc4", surfaces[1], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (int frame = 0; frame < 3; ++frame)
            Frame(three, 1);

        for (Target* target : { &one, &three })
        {
            Check(vkDeviceWaitIdle(target->device), "final idle");
            vkDestroySwapchainKHR(target->device, target->swapchain, nullptr);
            vkDestroyDevice(target->device, nullptr);
        }

        Write(compute, second.queueFamilyIndex, H(secondQueue));
        Finish();
    }

    void Finish()
    {
        for (VkSurfaceKHR surface : surfaces)
        {
            if (surface != VK_NULL_HANDLE)
                vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyDebug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
        Require(validationErrors == 0, "Vulkan validation reported errors; see the run log");
        for (HWND window : windows)
        {
            if (window != nullptr)
                DestroyWindow(window);
        }
        DestroyWindow(owner);
    }

    void Write(bool compute, uint32_t secondFamily, uint64_t secondQueue)
    {
        std::ostringstream out;
        out << "{\n  \"status\": \"PASS\",\n  \"validation_active\": " << (validationActive ? "true" : "false")
            << ",\n  \"validation_errors\": " << validationErrors << ",\n  \"wine_injected_extension_messages\": " << wineInjected << ",\n  \"late_load\": " << (lateLoad ? "true" : "false")
            << ",\n  \"windows\": [\"" << Hex(H(windows[0])) << "\", \"" << Hex(H(windows[1])) << "\"]"
            << ",\n  \"instance\": \"" << Hex(H(instance)) << "\",\n  \"physical\": \"" << Hex(H(physical)) << "\""
            << ",\n  \"graphics_family\": " << graphicsFamily << ",\n  \"compute_family\": "
            << (compute ? static_cast<int>(computeFamily) : -1) << ",\n  \"second_queue_family\": " << secondFamily
            << ",\n  \"second_queue\": \"" << Hex(secondQueue) << "\",\n  \"third_reused_second\": "
            << (thirdReusedSecond ? "true" : "false") << ",\n  \"families\": [";
        for (size_t i = 0; i < families.size(); ++i)
            out << (i ? ", " : "") << "{\"flags\": " << families[i].queueFlags << ", \"count\": " << families[i].queueCount << "}";
        out << "],\n  \"devices\": [";
        for (size_t i = 0; i < devices.size(); ++i)
        {
            out << (i ? ", " : "") << "{\"name\": \"" << devices[i].name << "\", \"handle\": \"" << Hex(devices[i].handle)
                << "\", \"requested\": [";
            for (size_t r = 0; r < devices[i].requested.size(); ++r)
                out << (r ? ", " : "") << "[" << devices[i].requested[r].first << ", " << devices[i].requested[r].second << "]";
            out << "]}";
        }
        out << "],\n  \"swapchains\": [";
        for (size_t i = 0; i < swapchains.size(); ++i)
        {
            const auto& s = swapchains[i];
            out << (i ? ",\n    " : "\n    ") << "{\"name\": \"" << s.name << "\", \"handle\": \"" << Hex(s.handle)
                << "\", \"device\": \"" << Hex(s.device) << "\", \"window\": " << s.window << ", \"usage\": " << s.usage
                << ", \"format\": " << s.format << ", \"color_space\": " << s.colorSpace << ", \"width\": " << s.width
                << ", \"height\": " << s.height << ", \"images\": " << s.images << ", \"sharing\": " << s.sharing
                << ", \"surface_usage\": " << s.surfaceUsage << "}";
        }
        out << "],\n  \"presents\": [";
        for (size_t i = 0; i < presents.size(); ++i)
        {
            const auto& p = presents[i];
            out << (i ? ",\n    " : "\n    ") << "{\"window\": " << p.window << ", \"swapchain\": \"" << Hex(p.swapchain)
                << "\", \"queue\": \"" << Hex(p.queue) << "\", \"device\": \"" << Hex(p.device) << "\", \"waits\": " << p.waits
                << ", \"image\": " << p.image << ", \"swapchain_name\": \"" << p.swapchainName << "\", \"device_name\": \""
                << p.deviceName << "\"}";
        }
        out << "],\n  \"prologues\": {";
        for (size_t i = 0; i < kWatchedCount; ++i)
        {
            out << (i ? ",\n    " : "\n    ") << "\"" << kWatched[i] << "\": {\"patched\": "
                << (prologues[i].before != prologues[i].after ? "true" : "false") << ", \"device_proc_checked\": "
                << (prologues[i].deviceProcChecked ? "true" : "false") << ", \"same_as_device_proc\": "
                << (prologues[i].sameAsDeviceProc ? "true" : "false") << "}";
        }
        out << "}\n}\n";
        // Written at the end of Run only after validation is known clean; main() rewrites FAIL on any exception.
        pending = out.str();
    }

    std::string pending;
};

int main(int argc, char** argv)
{
    std::ofstream("result.json") << "{\"status\":\"RUNNING\"}\n";
    try
    {
        App app;
        Require(argc == 1 || (argc == 2 && !strcmp(argv[1], "--one-window")), "unknown harness argument");
        app.oneWindow = argc == 2;
        app.Run();
        std::ofstream("result.json") << app.pending;
        std::printf("PASS: %zu presents over %zu device(s) and %zu swapchain(s); validation clean\n", app.presents.size(),
                    app.devices.size(), app.swapchains.size());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::ofstream("result.json") << "{\"status\":\"FAIL\",\"reason\":\"" << error.what() << "\"}\n";
        return 1;
    }
}
