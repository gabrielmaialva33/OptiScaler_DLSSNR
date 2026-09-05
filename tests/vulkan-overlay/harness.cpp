#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "probe_api.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::atomic<unsigned> validationErrors{0};
static std::atomic<bool> validationActive{false};
static std::atomic<bool> validationProbe{false};

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
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
    // A startup info message is version-dependent and can be filtered by Wine. Prove the error
    // callback using one deliberate, rejected fence flag before running the actual test.
    if (validationProbe && data->pMessageIdName &&
        !strcmp(data->pMessageIdName, "VUID-VkFenceCreateInfo-flags-parameter"))
    {
        validationActive = true;
        std::fprintf(stderr, "VALIDATION PROBE (expected): %s\n", text);
        return VK_TRUE; // Ask validation to skip the invalid call, never submit it to the driver.
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ||
        strstr(text, "Khronos Validation Layer Active"))
        std::fprintf(stderr, "VALIDATION: %s\n", text);
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

struct Test
{
    HWND owner = nullptr;
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<bool> initialized;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace{};
    VkExtent2D extent{};
    GetStats get = nullptr;
    ArmFailure arm = nullptr;
    Control teardown = nullptr;
    Control openMenu = nullptr;
    unsigned recreations = 0, countChanges = 0, extentChanges = 0, frames = 0;
    uint32_t previousImageCount = 0;
    VkExtent2D previousExtent{};
    VkLifetimeStats Stats()
    {
        VkLifetimeStats s;
        get(&s);
        Require(s.abi == 1, "wrong instrumentation ABI");
        return s;
    }
    void Init()
    {
        WNDCLASSW cls{};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"OptiScalerVkLifetimeHarness";
        Require(RegisterClassW(&cls) != 0, "RegisterClass failed");
        // An owned dialog can resize even under a tiling window manager. Assert the resulting
        // surface extents below: requesting a resize is not evidence that the compositor applied it.
        owner = CreateWindowW(cls.lpszClassName, L"OptiScaler harness owner", 0,
                              0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
        Require(owner != nullptr, "could not create dialog owner");
        window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"OptiScaler Vulkan lifetime test",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 100, 100, 660, 520, owner, nullptr, cls.hInstance, nullptr);
        Require(window != nullptr, "no Win32 window under Wine");
        ShowWindow(window, SW_SHOW);
        Pump();
        auto dll = LoadLibraryW(L".\\dxgi.dll");
        Require(dll != nullptr, "could not load isolated OptiScaler copy");
        get = reinterpret_cast<GetStats>(GetProcAddress(dll, "VkLifetimeGetStats"));
        arm = reinterpret_cast<ArmFailure>(GetProcAddress(dll, "VkLifetimeArmFailure"));
        teardown = reinterpret_cast<Control>(GetProcAddress(dll, "VkLifetimeDestroy"));
        openMenu = reinterpret_cast<Control>(GetProcAddress(dll, "VkLifetimeOpenMenu"));
        Require(get && arm && teardown && openMenu, "missing instrumentation exports; coverage cannot be proven");

        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = DebugMessage;
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                    VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "OptiScaler Vulkan lifetime harness";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pNext = &debug;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 3;
        info.ppEnabledExtensionNames = extensions;
        // Wine's host loader enables the native layer through VK_INSTANCE_LAYERS. A deliberate
        // validation error below must reach our callback; environment variables alone prove nothing.
        Check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        Require(createDebug != nullptr, "no debug messenger entry point");
        Check(createDebug(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = cls.hInstance;
        surfaceInfo.hwnd = window;
        Check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");
        uint32_t n = 0;
        Check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "vkEnumeratePhysicalDevices");
        Require(n > 0, "no Vulkan physical device under Wine");
        std::vector<VkPhysicalDevice> devices(n);
        Check(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "vkEnumeratePhysicalDevices");
        physical = devices[0];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("GPU: %s\n", properties.deviceName);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
        bool found = false;
        for (uint32_t i = 0; i < n; ++i)
        {
            VkBool32 supportsPresent = false;
            Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supportsPresent), "surface support");
            if (supportsPresent && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                family = i;
                found = true;
                break;
            }
        }
        Require(found, "no combined graphics/present queue");
        float priority = 1.0f;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo d{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        d.queueCreateInfoCount = 1;
        d.pQueueCreateInfos = &q;
        d.enabledExtensionCount = 1;
        d.ppEnabledExtensionNames = deviceExtensions;
        Check(vkCreateDevice(physical, &d, nullptr, &device), "vkCreateDevice");
        VkFenceCreateInfo invalidFence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        invalidFence.flags = 0x80000000u;
        VkFence probeFence = VK_NULL_HANDLE;
        validationProbe = true;
        auto probeResult = vkCreateFence(device, &invalidFence, nullptr, &probeFence);
        validationProbe = false;
        if (probeFence) vkDestroyFence(device, probeFence, nullptr);
        Require(validationActive && probeResult == VK_ERROR_VALIDATION_FAILED_EXT,
                "validation negative control failed: expected fence error was not intercepted");
        vkGetDeviceQueue(device, family, 0, &queue);
        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr), "surface formats");
        Require(n > 0, "no surface formats");
        std::vector<VkSurfaceFormatKHR> formats(n);
        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data()), "surface formats");
        auto chosen = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) chosen = f;
        format = chosen.format;
        colorSpace = chosen.colorSpace;
    }
    void Recreate(unsigned generation, unsigned fault = 0)
    {
        Check(vkDeviceWaitIdle(device), "idle before application recreation");
        SetWindowPos(window, nullptr, 0, 0, generation % 2 ? 820 : 660, generation % 2 ? 620 : 520,
                     SWP_NOMOVE | SWP_NOZORDER);
        // X11/Wayland window configuration is asynchronous under Wine.
        for (unsigned i = 0; i < 20; ++i) { Pump(); Sleep(10); }
        Pump();
        VkSurfaceCapabilitiesKHR caps{};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
        const uint32_t low = std::max(2u, caps.minImageCount);
        const uint32_t limit = caps.maxImageCount ? std::min(8u, caps.maxImageCount) : 8u;
        Require(low < limit, "device cannot supply two legal image counts within overlay's limit of 8");
        const uint32_t requested = generation % 2 ? low + 1 : low;
        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            RECT rect;
            GetClientRect(window, &rect);
            extent.width = std::clamp(uint32_t(rect.right), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(rect.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        Require((caps.supportedUsageFlags & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
                (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), "required image usage unsupported");
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        info.minImageCount = requested;
        info.imageFormat = format;
        info.imageColorSpace = colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & alpha) { info.compositeAlpha = alpha; break; }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain;
        auto before = Stats();
        arm(fault);
        VkSwapchainKHR next = VK_NULL_HANDLE;
        Check(vkCreateSwapchainKHR(device, &info, nullptr, &next), "vkCreateSwapchainKHR");
        auto after = Stats();
        Require(after.createCalls > before.createCalls, "ZERO COVERAGE: overlay CreateSwapchain not called");
        if (fault == 1)
        {
            Require(after.drainFailures == before.drainFailures + 1, "drain failure injection was not reached");
            Require(after.ready == 0 && after.liveBytes == before.liveBytes && after.liveBytes > 0,
                    "failed drain did not retain backing storage and disable overlay");
            Require(after.allocations == before.allocations && after.releases == before.releases &&
                    after.objectsDestroyed == before.objectsDestroyed, "failed drain freed or overwrote live ownership");
            teardown(); // retry the real wait on the real device
            after = Stats();
            Require(after.liveBytes == 0 && after.allocations == after.releases, "drain retry did not release arrays");
        }
        else if (fault == 2)
        {
            Require(after.partialFailures == before.partialFailures + 1, "partial initialization injection not reached");
            Require(after.ready == 0 && after.liveBytes == 0 && after.allocations == after.releases,
                    "partial initialization leaked arrays or published a ready overlay");
            Require(after.objectsCreated == after.objectsDestroyed, "partial initialization leaked Vulkan objects");
        }
        else
        {
            Require(after.ready != 0 && after.liveBytes > 0, "overlay creation did not complete");
        }
        if (swapchain)
        {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            ++recreations;
        }
        swapchain = next;
        uint32_t count = 0;
        Check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "swapchain image count");
        Require(count <= 8, "driver supplied more images than the overlay can track");
        images.resize(count);
        Check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "swapchain images");
        initialized.assign(count, false);
        if (!fault)
        {
            Require(after.imageCount == count, "overlay did not track actual swapchain image count");
            if (previousImageCount && previousImageCount != count) ++countChanges;
            if (previousExtent.width && (previousExtent.width != extent.width || previousExtent.height != extent.height))
                ++extentChanges;
            previousImageCount = count;
            previousExtent = extent;
        }
        std::printf("SWAPCHAIN extent=%ux%u requested=%u actual=%u fault=%u live_bytes=%llu\n",
                    extent.width, extent.height, requested, count, fault, after.liveBytes);
        std::fflush(stdout);
    }
    void Frame()
    {
        Pump();
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
        Check(vkCreateSemaphore(device, &si, nullptr, &acquired), "create acquire semaphore");
        Check(vkCreateSemaphore(device, &si, nullptr, &rendered), "create render semaphore");
        uint32_t index = 0;
        auto acquire = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        Require(acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR, "acquire failed");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "create frame command pool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &ai, &cmd), "allocate frame command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(cmd, &begin), "begin frame");
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue color{{0.04f, 0.12f, 0.2f, 1.0f}};
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        Check(vkEndCommandBuffer(cmd), "end frame");
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired;
        submit.pWaitDstStageMask = &stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit clear frame");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        auto before = Stats();
        auto result = vkQueuePresentKHR(queue, &present);
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "present failed");
        Require(Stats().presentCalls > before.presentCalls, "ZERO COVERAGE: overlay QueuePresent not called");
        Require(Stats().overlaySubmits > before.overlaySubmits, "ZERO COVERAGE: frame had no overlay submission");
        Check(vkQueueWaitIdle(queue), "wait for real presentation");
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroySemaphore(device, acquired, nullptr);
        vkDestroySemaphore(device, rendered, nullptr);
        initialized[index] = true;
        ++frames;
    }
    VkLifetimeStats Finish()
    {
        Check(vkDeviceWaitIdle(device), "final device drain");
        teardown();
        auto final = Stats();
        Require(final.createCalls > 0 && final.destroyCalls > 0 && final.teardownCalls > 0 && final.presentCalls > 0,
                "ZERO COVERAGE: required overlay lifecycle function was not reached");
        Require(final.overlaySubmits > 0, "ZERO COVERAGE: overlay never submitted a rendered frame");
        Require(countChanges > 0, "ZERO COVERAGE: actual image count never changed");
        Require(extentChanges > 0, "ZERO COVERAGE: actual surface extent never changed");
        Require(final.allocations > 0 && final.allocations == final.releases && final.liveBytes == 0,
                "LEAK: overlay backing allocations do not balance");
        Require(final.objectsCreated == final.objectsDestroyed, "LEAK: overlay Vulkan objects do not balance");
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyDebug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
        Require(validationErrors == 0, "Vulkan validation reported errors; see run.log");
        DestroyWindow(window);
        DestroyWindow(owner);
        return final;
    }
};

int main()
{
    // Never let a previous PASS survive a crash, an environment failure or a coverage failure.
    std::ofstream("result.json") << "{\"status\":\"RUNNING\"}\n";
    try
    {
        Test test;
        test.Init();
        for (unsigned generation = 0; generation < 13; ++generation)
        {
            test.Recreate(generation);
            test.openMenu();
            auto before = test.Stats().overlaySubmits;
            for (unsigned frame = 0; frame < 4; ++frame) test.Frame();
            Require(test.Stats().overlaySubmits > before, "ZERO COVERAGE: no overlay submit in this generation");
        }
        test.Recreate(13, 1); // keep ownership on failed drain; retry before destroying old swapchain
        test.Recreate(14);   // recover and render again
        test.openMenu();
        test.Frame();
        test.Recreate(15, 2); // fail the second framebuffer after partial initialization
        test.Recreate(16);   // recover and render again
        test.openMenu();
        test.Frame();
        auto s = test.Finish();
        std::ofstream out("result.json");
        out << "{\n  \"status\": \"PASS\",\n  \"validation_active\": true,\n  \"validation_errors\": " << validationErrors
            << ",\n  \"recreations\": " << test.recreations << ",\n  \"actual_image_count_changes\": " << test.countChanges
            << ",\n  \"actual_extent_changes\": " << test.extentChanges
            << ",\n  \"frames\": " << test.frames << ",\n  \"create_calls\": " << s.createCalls
            << ",\n  \"destroy_calls\": " << s.destroyCalls << ",\n  \"teardown_calls\": " << s.teardownCalls
            << ",\n  \"present_calls\": " << s.presentCalls << ",\n  \"overlay_submits\": " << s.overlaySubmits
            << ",\n  \"allocations\": " << s.allocations << ",\n  \"releases\": " << s.releases
            << ",\n  \"live_bytes\": " << s.liveBytes << ",\n  \"peak_bytes\": " << s.peakBytes
            << ",\n  \"objects_created\": " << s.objectsCreated << ",\n  \"objects_destroyed\": " << s.objectsDestroyed
            << ",\n  \"injected_drain_failures\": " << s.drainFailures
            << ",\n  \"injected_partial_failures\": " << s.partialFailures << "\n}\n";
        std::puts("PASS: measured Vulkan lifecycle coverage, changing actual image counts, balanced allocations and zero validation errors");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::ofstream("result.json") << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
