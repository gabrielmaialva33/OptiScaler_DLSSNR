// A Win32 Vulkan application for the Vulkan present host (docs/dlssnr/design/nr-vulkan-present-host.md, stages 1 and
// 2).
//
// It loads the production OptiScaler.dll as dxgi.dll, uninstrumented, and drives real Vulkan under Wine through the
// objects the host has to keep apart and the cases its round trip has to survive: two windows, three devices (the
// third created after the second is destroyed, so a reused handle value is possible), a swapchain recreated through
// oldSwapchain, presents from a non-graphics compute family (the round trip's cross-family route), presents with
// zero, one and two wait semaphores, a batch of presents queued behind about a second of GPU work until more of them
// are in flight than the host's ring holds, a resize, vkGetDeviceQueue and vkGetDeviceQueue2. It never calls NGX or
// an upscaler.
//
// Every frame uploads a pattern of its own into the image; when the same image comes round again, its content is read
// back and compared with the pattern last presented in it. The host's round trip copies the image out and back at
// present, so equality is the proof that it is lossless (and an image the host did not touch must compare equal too).
// An image can only be read back with TRANSFER_SRC: on swapchains that ask for it, and with --host-adds-src on every
// swapchain (the host adds the usage; the validation layer sits below it and sees the real usage).
//
// It asserts nothing about the host's log. It writes every fact it controls -- handles, families, usages, image
// counts, the order of its presents and their waits, which of them were held back, the read-backs -- to result.json,
// and whether vulkan-1.dll's exports were patched, by comparing their first bytes before OptiScaler loaded with the
// same bytes after the first swapchain. run.py holds OptiScaler.log against those facts. Its own checks are about the
// run being valid: validation active (proved by a deliberately invalid fence the layer must reject), zero validation
// errors, every call succeeding, every read-back equal.

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
    if (validationProbe && data->pMessageIdName &&
        !strcmp(data->pMessageIdName, "VUID-VkFenceCreateInfo-flags-parameter"))
    {
        validationActive = true;
        std::fprintf(stderr, "VALIDATION PROBE (expected): %s\n", text);
        return VK_TRUE; // the layer skips the invalid call
    }
    // winevulkan (11.19) appends VK_EXT_external_memory_dma_buf to the host-side device it creates for every
    // vkCreateDevice, without VK_KHR_external_memory_fd, and the native layer sees Wine's list rather than ours: the
    // index it names is past the extensions this application asks for. Counted and reported, not an error of the
    // application or of OptiScaler, which passes the create info down unchanged.
    if (data->pMessageIdName && !strcmp(data->pMessageIdName, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387") &&
        strstr(text, "VK_EXT_external_memory_dma_buf") != nullptr &&
        strstr(text, "ppEnabledExtensionNames[0]") == nullptr)
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

// A frame's pattern: every pixel a hash of the frame's seed and the pixel's position, all four bytes.
static uint32_t Pixel(uint32_t seed, uint32_t i)
{
    uint32_t x = seed * 0x9E3779B1u ^ (i + 0x7F4A7C15u) * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    x *= 0x297A2D39u;
    x ^= x >> 15;
    return x;
}

static void FillPattern(void* memory, uint32_t pixels, uint32_t seed)
{
    auto* out = static_cast<uint32_t*>(memory);
    for (uint32_t i = 0; i < pixels; ++i)
        out[i] = Pixel(seed, i);
}

// Pixels that differ from the pattern.
static uint32_t Mismatches(const void* memory, uint32_t pixels, uint32_t seed)
{
    const auto* in = static_cast<const uint32_t*>(memory);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < pixels; ++i)
        wrong += in[i] != Pixel(seed, i) ? 1 : 0;
    return wrong;
}

// --- Export prologues ----------------------------------------------------------------------------------------------

static const char* const kWatched[] = {
    "vkCreateInstance",      "vkDestroyInstance",       "vkEnumeratePhysicalDevices",
    "vkCreateDevice",        "vkCreateWin32SurfaceKHR", "vkDestroySurfaceKHR",
    "vkDestroyDevice",       "vkGetDeviceQueue",        "vkGetDeviceQueue2",
    "vkCreateSwapchainKHR",  "vkDestroySwapchainKHR",   "vkGetSwapchainImagesKHR",
    "vkAcquireNextImageKHR", "vkQueuePresentKHR",       "vkQueueSubmit",
    "vkQueueWaitIdle",       "vkCreateFence",
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

constexpr uint32_t kUploadSlots = 12; // pattern uploads in flight at once: the held-back batch's
constexpr uint32_t kDelayMax = 7;     // presents the held-back batch queues: three more than the host's ring of four

struct Present
{
    int window = 0; // 1 or 2
    uint64_t swapchain = 0;
    uint64_t queue = 0;
    uint64_t device = 0;
    uint32_t family = 0;
    uint32_t waits = 0;
    uint32_t image = 0;
    int delayed = -1;    // its position in the held-back batch, -1 outside it
    bool gpuIdle = true; // every earlier GPU work of the application had completed when it was presented
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
    bool readback = false;
    bool destroyed = false;
};

struct DeviceFacts
{
    std::string name;
    uint64_t handle = 0;
    std::vector<std::pair<uint32_t, uint32_t>> requested; // family, count
    bool destroyed = false;
};

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
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
    VkPhysicalDeviceMemoryProperties memory {};
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX; // non-graphics, compute, presents to window 1; UINT32_MAX when none
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace {};
    bool lateLoad = false;
    bool oneWindow = false;     // --one-window: window 1 and device 1 only
    bool hostAddsSrc = false;   // --host-adds-src: every swapchain can be read back
    bool noTransferSrc = false; // --no-transfer-src: the surface does not offer TRANSFER_SRC (a test layer hides it)
    bool overlayDraws = false;  // --overlay-draws: the legacy Vulkan overlay draws its splash into presented images

    std::vector<Present> presents;
    std::vector<SwapchainFacts> swapchains;
    std::vector<DeviceFacts> devices;
    bool thirdReusedSecond = false;
    uint32_t readbacks = 0;
    uint32_t readbackMismatches = 0;
    uint32_t overlayMismatches = 0; // with --overlay-draws: read-backs the overlay's splash changed, not failures
    uint32_t delayQueued = 0;
    double heldGpuMs = 0;      // the block of GPU work the batch queued behind: submit to completion
    double heldPresentsMs = 0; // from that submit to the return of the batch's last present
    std::string firstMismatch;

    struct Target
    {
        VkDevice device = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        std::vector<bool> initialized;
        std::vector<int64_t> expected; // per image: the seed of the pattern last presented, -1 when none
        int window = 0;
        std::string deviceName;
        std::string swapchainName;
        VkExtent2D extent {};
        bool readback = false;
        Buffer upload;   // kUploadSlots patterns
        Buffer download; // one image
    };

    uint32_t nextSeed = 1;

    HWND MakeWindow(int index, int x, int y, int w, int h)
    {
        // An owned dialog, as in tests/vulkan-overlay: a tiling window manager floats it instead of re-tiling the
        // first window when the second appears, which would put an out-of-date swapchain under its next present.
        HWND window =
            CreateWindowExW(WS_EX_DLGMODALFRAME, L"OptiScalerVkPresentHostHarness",
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
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
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
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
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
        Require(format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM ||
                    format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB,
                "no four-byte surface format to upload patterns into");
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

    void DestroyDevice(VkDevice device, const std::string& name)
    {
        vkDestroyDevice(device, nullptr);
        for (auto& d : devices)
            if (d.name == name)
                d.destroyed = true;
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

    Buffer MakeBuffer(VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible = true)
    {
        Buffer b;
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(vkCreateBuffer(device, &info, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
        const VkMemoryPropertyFlags wanted =
            hostVisible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                type = i;
        Require(type != UINT32_MAX, "no memory type for a buffer");
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &allocate, nullptr, &b.memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        if (hostVisible)
            Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
        return b;
    }

    void FreeBuffer(VkDevice device, Buffer& b)
    {
        if (b.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, b.memory, nullptr);
        b = Buffer {};
    }

    uint32_t Pixels(const Target& t) const { return t.extent.width * t.extent.height; }

    void DestroySwapchain(Target& target)
    {
        vkDestroySwapchainKHR(target.device, target.swapchain, nullptr);
        for (auto& s : swapchains)
            if (s.handle == H(target.swapchain) && s.device == H(target.device) && !s.destroyed)
                s.destroyed = true;
        target.swapchain = VK_NULL_HANDLE;
        FreeBuffer(target.device, target.upload);
        FreeBuffer(target.device, target.download);
    }

    void MakeSwapchain(Target& target, const char* name, VkSurfaceKHR surface, uint32_t usage, uint32_t extraImages = 1)
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
        info.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + extraImages, caps.maxImageCount)
                                                : caps.minImageCount + extraImages;
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
            DestroySwapchain(target);
        }
        target.swapchain = swapchain;
        target.swapchainName = name;
        target.extent = extent;
        uint32_t count = 0;
        Check(vkGetSwapchainImagesKHR(target.device, swapchain, &count, nullptr), "swapchain image count");
        target.images.resize(count);
        Check(vkGetSwapchainImagesKHR(target.device, swapchain, &count, target.images.data()), "swapchain images");
        target.initialized.assign(count, false);
        target.expected.assign(count, -1);
        target.readback = (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 || hostAddsSrc;
        const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
        target.upload = MakeBuffer(target.device, bytes * kUploadSlots, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        target.download = MakeBuffer(target.device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

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
        facts.readback = target.readback;
        swapchains.push_back(facts);
        std::printf("SWAPCHAIN %s %ux%u images=%u usage=0x%X readback=%d\n", name, extent.width, extent.height, count,
                    usage, target.readback ? 1 : 0);
    }

    uint32_t TransferUsage() const
    {
        return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (noTransferSrc ? 0u : uint32_t(VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    }

    // Record into `cmd`: read the image back when its last pattern is known (and it can be read), then upload pattern
    // `seed` from upload slot `slot`, leaving the image in PRESENT_SRC. Returns whether a read-back was recorded.
    bool RecordFrame(Target& target, VkCommandBuffer cmd, uint32_t index, uint32_t seed, uint32_t slot)
    {
        const VkImage image = target.images[index];
        const bool readback = target.readback && target.expected[index] >= 0;
        VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { target.extent.width, target.extent.height, 1 };

        if (readback)
        {
            b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            // TRANSFER as the source stage, matching the acquire semaphore's wait stage (see tests/vulkan-overlay).
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.download.buffer, 1,
                                   &region);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        }
        else
        {
            b.oldLayout = target.initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
        }
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);

        const VkDeviceSize bytes = VkDeviceSize(Pixels(target)) * 4;
        FillPattern(static_cast<char*>(target.upload.mapped) + bytes * slot, Pixels(target), seed);
        region.bufferOffset = bytes * slot;
        vkCmdCopyBufferToImage(cmd, target.upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
        return readback;
    }

    void CompareReadback(Target& target, uint32_t index)
    {
        const uint32_t wrong =
            Mismatches(target.download.mapped, Pixels(target), static_cast<uint32_t>(target.expected[index]));
        ++readbacks;
        if (wrong != 0 && overlayDraws)
        {
            ++overlayMismatches;
        }
        else if (wrong != 0)
        {
            ++readbackMismatches;
            if (firstMismatch.empty())
                firstMismatch = target.swapchainName + " image " + std::to_string(index) + ": " +
                                std::to_string(wrong) + " of " + std::to_string(Pixels(target)) +
                                " pixels differ from pattern " + std::to_string(target.expected[index]);
            std::fprintf(stderr, "READBACK MISMATCH: %s image %u: %u pixels differ\n", target.swapchainName.c_str(),
                         index, wrong);
        }
    }

    void Record(const Target& target, uint32_t waits, uint32_t index, int delayed, bool gpuIdle)
    {
        presents.push_back(Present { target.window, H(target.swapchain), H(target.queue), H(target.device),
                                     target.family, waits, index, delayed, gpuIdle, target.swapchainName,
                                     target.deviceName });
    }

    // One frame: acquire, read back and upload a pattern on the present queue, present with `waits` semaphores (0, 1
    // or 2), then wait for all of it.
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
        const VkResult acquire =
            vkAcquireNextImageKHR(device, target.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
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
        const uint32_t seed = nextSeed++;
        const bool readback = RecordFrame(target, cmd, index, seed, 0);
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
        Check(vkQueueSubmit(target.queue, 1, &submit, VK_NULL_HANDLE), "submit frame");

        VkSemaphore waitList[2] = { rendered, extra };
        if (waits == 2)
        {
            // A second, empty batch on the same queue, after the frame: its signal is the present's second wait.
            VkSubmitInfo empty { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            empty.signalSemaphoreCount = 1;
            empty.pSignalSemaphores = &extra;
            Check(vkQueueSubmit(target.queue, 1, &empty, VK_NULL_HANDLE), "submit extra signal");
        }
        else if (waits == 0)
        {
            // No semaphore at all: the host has seen the frame complete before presenting.
            Check(vkQueueWaitIdle(target.queue), "wait for the frame before a present with no waits");
        }

        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = waits;
        present.pWaitSemaphores = waits > 0 ? waitList : nullptr;
        present.swapchainCount = 1;
        present.pSwapchains = &target.swapchain;
        present.pImageIndices = &index;
        const VkResult result = vkQueuePresentKHR(target.queue, &present);
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "present failed");
        Record(target, waits, index, -1, true);

        Check(vkQueueWaitIdle(target.queue), "wait for presentation");
        if (readback)
            CompareReadback(target, index);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroySemaphore(device, acquired, nullptr);
        vkDestroySemaphore(device, rendered, nullptr);
        vkDestroySemaphore(device, extra, nullptr);
        target.initialized[index] = true;
        target.expected[index] = seed;
    }

    // A command buffer of `copies` copies between two large device-local buffers: GPU work that takes a while.
    VkCommandBuffer RecordBusyWork(VkDevice device, VkCommandPool pool, const Buffer& from, const Buffer& to,
                                   VkDeviceSize bytes, uint32_t copies)
    {
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &ai, &cmd), "command buffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        Check(vkBeginCommandBuffer(cmd, &begin), "begin");
        VkBufferCopy region { 0, 0, bytes };
        VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        for (uint32_t i = 0; i < copies; ++i)
        {
            vkCmdCopyBuffer(cmd, from.buffer, to.buffer, 1, &region);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                                 nullptr, 0, nullptr);
        }
        Check(vkEndCommandBuffer(cmd), "end");
        return cmd;
    }

    static double Ms(LARGE_INTEGER from, LARGE_INTEGER to)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1000.0 * double(to.QuadPart - from.QuadPart) / double(frequency.QuadPart);
    }

    // About 1.2 s of GPU work is submitted on the presenting queue, then presents are queued behind it one after
    // another until no image is left to acquire: each of them reaches the host with every earlier one of the batch
    // still in flight, more of them than the host's ring holds. Then everything drains.
    void HeldBackBatch(Target& target)
    {
        VkDevice device = target.device;
        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = target.family;
        VkCommandPool pool = VK_NULL_HANDLE;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command pool");
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence = VK_NULL_HANDLE;
        Check(vkCreateFence(device, &fi, nullptr, &fence), "fence");

        // How long a copy takes here, to size the block.
        const VkDeviceSize bytes = VkDeviceSize(64) << 20;
        Buffer from = MakeBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
        Buffer to = MakeBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        constexpr uint32_t kProbe = 100;
        VkCommandBuffer probe = RecordBusyWork(device, pool, from, to, bytes, kProbe);
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &probe;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        Check(vkQueueSubmit(target.queue, 1, &submit, fence), "submit the probe");
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for the probe");
        QueryPerformanceCounter(&t1);
        Check(vkResetFences(device, 1, &fence), "reset fence");
        const double perCopy = std::max(Ms(t0, t1) / kProbe, 0.001);
        const uint32_t copies = static_cast<uint32_t>(std::clamp(1200.0 / perCopy, 100.0, 200000.0));

        VkCommandBuffer busy = RecordBusyWork(device, pool, from, to, bytes, copies);
        submit.pCommandBuffers = &busy;
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        Check(vkQueueSubmit(target.queue, 1, &submit, fence), "submit the block of work");

        std::vector<VkSemaphore> semaphores;
        std::vector<std::pair<uint32_t, uint32_t>> queued; // image, seed
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        LARGE_INTEGER lastPresent = start;

        for (uint32_t k = 0; k < kDelayMax && k < kUploadSlots; ++k)
        {
            Pump();
            VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
            Check(vkCreateSemaphore(device, &si, nullptr, &acquired), "acquire semaphore");
            Check(vkCreateSemaphore(device, &si, nullptr, &rendered), "render semaphore");
            semaphores.push_back(acquired);
            semaphores.push_back(rendered);

            uint32_t index = 0;
            const VkResult acquire =
                vkAcquireNextImageKHR(device, target.swapchain, 0, acquired, VK_NULL_HANDLE, &index);
            if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT)
            {
                // Never signalled: destroyable as it is.
                break;
            }
            Require(acquire == VK_SUCCESS || acquire == VK_SUBOPTIMAL_KHR, "acquire failed in the held-back batch");

            VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            Check(vkAllocateCommandBuffers(device, &ai, &cmd), "command buffer");
            VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            Check(vkBeginCommandBuffer(cmd, &begin), "begin");
            const uint32_t seed = nextSeed++;
            const bool savedReadback = target.readback;
            target.readback = false; // nothing is read back while frames are in flight
            RecordFrame(target, cmd, index, seed, k);
            target.readback = savedReadback;
            Check(vkEndCommandBuffer(cmd), "end");

            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo frame { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            frame.waitSemaphoreCount = 1;
            frame.pWaitSemaphores = &acquired;
            frame.pWaitDstStageMask = &stage;
            frame.commandBufferCount = 1;
            frame.pCommandBuffers = &cmd;
            frame.signalSemaphoreCount = 1;
            frame.pSignalSemaphores = &rendered;
            Check(vkQueueSubmit(target.queue, 1, &frame, VK_NULL_HANDLE), "submit held-back frame");

            VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &rendered;
            present.swapchainCount = 1;
            present.pSwapchains = &target.swapchain;
            present.pImageIndices = &index;
            const VkResult result = vkQueuePresentKHR(target.queue, &present);
            Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "present failed in the held-back batch");
            QueryPerformanceCounter(&lastPresent);
            Record(target, 1, index, static_cast<int>(k), false);
            queued.emplace_back(index, seed);
        }

        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "wait for the block of work");
        LARGE_INTEGER done;
        QueryPerformanceCounter(&done);
        delayQueued = static_cast<uint32_t>(queued.size());
        heldGpuMs = Ms(start, done);
        heldPresentsMs = Ms(start, lastPresent);
        std::printf("HELD BACK: %u presents queued behind %u copies (%.1f ms on the GPU; the last present returned at "
                    "%.1f ms)\n",
                    delayQueued, copies, heldGpuMs, heldPresentsMs);
        Check(vkDeviceWaitIdle(device), "drain the held-back batch");

        for (const auto& [index, seed] : queued)
        {
            target.initialized[index] = true;
            target.expected[index] = seed;
        }
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroyFence(device, fence, nullptr);
        FreeBuffer(device, from);
        FreeBuffer(device, to);
        for (VkSemaphore s : semaphores)
            vkDestroySemaphore(device, s, nullptr);
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
        const VkQueue graphicsQueue = one.queue;
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

        // Swapchain 1: no TRANSFER_SRC, as DXVK and vkd3d-proton ask (0x12): the host has to add it.
        MakeSwapchain(one, "sc1", surfaces[0], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        SnapshotPrologues(true);
        for (int frame = 0; frame < 12; ++frame)
            Frame(one, 1);

        // Swapchain 2 replaces it through oldSwapchain, with both transfer usages (unless the surface hides
        // TRANSFER_SRC), presented from the second queue -- the compute-only family, so the round trip crosses to the
        // graphics family and back: two presents with no waits, then two waits each, to window 1's 100th present.
        VkSurfaceCapabilitiesKHR caps {};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surfaces[0], &caps), "surface capabilities");
        Require(noTransferSrc == !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
                noTransferSrc ? "the surface still offers TRANSFER_SRC: the test layer is not active"
                              : "the surface offers no TRANSFER_SRC");
        one.queue = secondQueue;
        one.family = second.queueFamilyIndex;
        MakeSwapchain(one, "sc2", surfaces[0], TransferUsage());
        for (int frame = 0; frame < 88; ++frame)
            Frame(one, frame < 2 ? 0 : 2);

        // Swapchain 3 on window 1, from the graphics queue again, with as many images as the surface allows: a batch
        // of presents queued behind a block of GPU work, more of them than the host's ring holds, then frames that read
        // every image back.
        one.queue = graphicsQueue;
        one.family = graphicsFamily;
        MakeSwapchain(one, "sc-held", surfaces[0], TransferUsage(), 7);
        // Not under the legacy overlay: on a new swapchain its present waits for the GPU (it re-initialises its
        // objects there), so presents never pile up behind the block of work.
        if (!overlayDraws)
            HeldBackBatch(one);
        for (size_t frame = 0; frame < one.images.size() + 2; ++frame)
            Frame(one, 1);

        if (oneWindow)
        {
            // The legacy Vulkan overlay (OverlayMenu=true, no marker) dies when a second window's swapchain is
            // created on a second device -- a null read inside MenuOverlayVk::CreateSwapchain, with this host off
            // too -- so the cases that run it stop after window 1. That overlay also keeps objects on the device
            // and has no hook on its destruction (VUID-vkDestroyDevice-device-05137; tests/vulkan-overlay calls a
            // test-only teardown export the production DLL does not have), so the device and the instance are left
            // to process exit here. See README.md.
            Check(vkDeviceWaitIdle(one.device), "final idle");
            DestroySwapchain(one);
            Write(compute, second.queueFamilyIndex, H(secondQueue));
            Require(validationErrors == 0, "Vulkan validation reported errors; see the run log");
            return;
        }

        // Window 2, device 2, alive beside device 1.
        windows[1] = MakeWindow(1, 760, 80, 480, 360);
        surfaces[1] = MakeSurface(windows[1]);
        VkBool32 supported = VK_FALSE;
        Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, graphicsFamily, surfaces[1], &supported),
              "surface support");
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
        DestroySwapchain(two);
        const uint64_t secondHandle = H(two.device);
        DestroyDevice(two.device, "d2");
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

        // The window is resized and its swapchain recreated through oldSwapchain at the new extent.
        SetWindowPos(windows[1], nullptr, 0, 0, 400, 300, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < 10; ++i)
        {
            Pump();
            Sleep(20);
        }
        MakeSwapchain(three, "sc5", surfaces[1], VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for (int frame = 0; frame < 6; ++frame)
            Frame(three, 1);

        for (Target* target : { &one, &three })
        {
            Check(vkDeviceWaitIdle(target->device), "final idle");
            DestroySwapchain(*target);
            DestroyDevice(target->device, target->deviceName);
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
        Require(readbackMismatches == 0,
                ("a presented image did not read back as it was made: " + firstMismatch).c_str());
        std::ostringstream out;
        out << "{\n  \"status\": \"PASS\",\n  \"validation_active\": " << (validationActive ? "true" : "false")
            << ",\n  \"validation_errors\": " << validationErrors
            << ",\n  \"wine_injected_extension_messages\": " << wineInjected
            << ",\n  \"late_load\": " << (lateLoad ? "true" : "false") << ",\n  \"windows\": [\"" << Hex(H(windows[0]))
            << "\", \"" << Hex(H(windows[1])) << "\"]"
            << ",\n  \"instance\": \"" << Hex(H(instance)) << "\",\n  \"physical\": \"" << Hex(H(physical)) << "\""
            << ",\n  \"graphics_family\": " << graphicsFamily
            << ",\n  \"compute_family\": " << (compute ? static_cast<int>(computeFamily) : -1)
            << ",\n  \"second_queue_family\": " << secondFamily << ",\n  \"second_queue\": \"" << Hex(secondQueue)
            << "\",\n  \"third_reused_second\": " << (thirdReusedSecond ? "true" : "false")
            << ",\n  \"held_back_queued\": " << delayQueued << ",\n  \"held_back_gpu_ms\": " << heldGpuMs
            << ",\n  \"held_back_presents_ms\": " << heldPresentsMs << ",\n  \"readbacks\": " << readbacks
            << ",\n  \"readback_mismatches\": " << readbackMismatches
            << ",\n  \"overlay_mismatches\": " << overlayMismatches
            << ",\n  \"host_adds_src\": " << (hostAddsSrc ? "true" : "false")
            << ",\n  \"no_transfer_src\": " << (noTransferSrc ? "true" : "false") << ",\n  \"families\": [";
        for (size_t i = 0; i < families.size(); ++i)
            out << (i ? ", " : "") << "{\"flags\": " << families[i].queueFlags
                << ", \"count\": " << families[i].queueCount << "}";
        out << "],\n  \"devices\": [";
        for (size_t i = 0; i < devices.size(); ++i)
        {
            out << (i ? ", " : "") << "{\"name\": \"" << devices[i].name << "\", \"handle\": \""
                << Hex(devices[i].handle) << "\", \"destroyed\": " << (devices[i].destroyed ? "true" : "false")
                << ", \"requested\": [";
            for (size_t r = 0; r < devices[i].requested.size(); ++r)
                out << (r ? ", " : "") << "[" << devices[i].requested[r].first << ", " << devices[i].requested[r].second
                    << "]";
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
                << ", \"surface_usage\": " << s.surfaceUsage << ", \"readback\": " << (s.readback ? "true" : "false")
                << ", \"destroyed\": " << (s.destroyed ? "true" : "false") << "}";
        }
        out << "],\n  \"presents\": [";
        for (size_t i = 0; i < presents.size(); ++i)
        {
            const auto& p = presents[i];
            out << (i ? ",\n    " : "\n    ") << "{\"window\": " << p.window << ", \"swapchain\": \""
                << Hex(p.swapchain) << "\", \"queue\": \"" << Hex(p.queue) << "\", \"device\": \"" << Hex(p.device)
                << "\", \"family\": " << p.family << ", \"waits\": " << p.waits << ", \"image\": " << p.image
                << ", \"held_back\": " << p.delayed << ", \"gpu_idle\": " << (p.gpuIdle ? "true" : "false")
                << ", \"swapchain_name\": \"" << p.swapchainName << "\", \"device_name\": \"" << p.deviceName << "\"}";
        }
        out << "],\n  \"prologues\": {";
        for (size_t i = 0; i < kWatchedCount; ++i)
        {
            out << (i ? ",\n    " : "\n    ") << "\"" << kWatched[i]
                << "\": {\"patched\": " << (prologues[i].before != prologues[i].after ? "true" : "false")
                << ", \"device_proc_checked\": " << (prologues[i].deviceProcChecked ? "true" : "false")
                << ", \"same_as_device_proc\": " << (prologues[i].sameAsDeviceProc ? "true" : "false") << "}";
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
        for (int i = 1; i < argc; ++i)
        {
            if (!strcmp(argv[i], "--one-window"))
                app.oneWindow = true;
            else if (!strcmp(argv[i], "--host-adds-src"))
                app.hostAddsSrc = true;
            else if (!strcmp(argv[i], "--no-transfer-src"))
                app.noTransferSrc = true;
            else if (!strcmp(argv[i], "--overlay-draws"))
                app.overlayDraws = true;
            else
                Require(false, "unknown harness argument");
        }
        app.Run();
        std::ofstream("result.json") << app.pending;
        std::printf(
            "PASS: %zu presents over %zu device(s) and %zu swapchain(s); %u read-backs equal; validation clean\n",
            app.presents.size(), app.devices.size(), app.swapchains.size(), app.readbacks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::ofstream("result.json") << "{\"status\":\"FAIL\",\"reason\":\"" << error.what() << "\"}\n";
        return 1;
    }
}
