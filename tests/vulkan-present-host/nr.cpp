// A Win32 Vulkan application for the Vulkan present host's stage 3, cold NR
// (docs/dlssnr/design/nr-vulkan-present-host.md, "Stage 3 as built").
//
// It loads the production OptiScaler.dll as dxgi.dll, uninstrumented, and presents a picture -- gradients, flat
// patches, fine stripes and a square that moves a little every frame, opaque -- through swapchains of every SDR
// format the surface offers in the sRGB non-linear colour space (B8G8R8A8 and R8G8B8A8, UNORM and sRGB), one
// four-byte format that is not SDR when the surface offers one, a swapchain presented from the compute-only family
// (the round trip's cross-family route) when there is one, and a swapchain recreated through oldSwapchain at an
// extent that is not a multiple of eight. Its device asks for VK_KHR_swapchain alone: the model's extensions are the
// host's to add. It never calls NGX or an upscaler.
//
// Every presented image is read back when it comes round again, before anything is written to it, and compared with
// what was presented: byte for byte, the mean and largest change of a colour channel, and alpha. What NR is expected
// to have done is the runner's to judge from those facts against OptiScaler.log; this application asserts only that
// the run is valid (validation active and clean, every call succeeding).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static std::atomic<unsigned> validationErrors { 0 };
static std::atomic<unsigned> wineInjected { 0 };
static std::atomic<bool> validationActive { false };
static std::atomic<bool> validationProbe { false };
static std::string firstValidation;
static FILE* validationLog = nullptr; // every report, one per line: Proton does not pass the application's output on
static std::atomic<unsigned> modelMessages { 0 };
static std::string modelVuids; // distinct, comma-separated

static void Require(bool condition, const std::string& message)
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
        return VK_TRUE;
    }
    // Wine 11.19's own VK_EXT_external_memory_dma_buf on the host-side device (see harness.cpp).
    if (data->pMessageIdName && !strcmp(data->pMessageIdName, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387") &&
        strstr(text, "VK_EXT_external_memory_dma_buf") != nullptr &&
        strstr(text, "ppEnabledExtensionNames[0]") == nullptr)
    {
        ++wineInjected;
        return VK_FALSE;
    }
    // An object NVIDIA's model names itself (nv.ngx.*): what the model records into the host's command buffer is the
    // model's. Counted and kept apart, by VUID, so the runner holds exactly which ones are expected.
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && strstr(text, "[nv.ngx.") != nullptr)
    {
        ++modelMessages;
        const std::string id = data->pMessageIdName ? data->pMessageIdName : "?";
        if (("," + modelVuids + ",").find("," + id + ",") == std::string::npos)
            modelVuids += (modelVuids.empty() ? "" : ",") + id;
        if (validationLog == nullptr)
            validationLog = std::fopen("validation.txt", "w");
        if (validationLog != nullptr)
        {
            std::fprintf(validationLog, "MODEL %s | %s\n", id.c_str(), text);
            std::fflush(validationLog);
        }
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        if (validationErrors++ == 0)
            firstValidation = text;
        std::fprintf(stderr, "VALIDATION: %s\n", text);
        if (validationLog == nullptr)
            validationLog = std::fopen("validation.txt", "w");
        if (validationLog != nullptr)
        {
            std::fprintf(validationLog, "%s | %s\n", data->pMessageIdName ? data->pMessageIdName : "?", text);
            std::fflush(validationLog);
        }
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

static std::string Escape(const std::string& text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
    }
    return out;
}

static bool Sdr(VkFormat format, VkColorSpaceKHR space)
{
    return space == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
           (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
            format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB);
}

static bool Bgra(VkFormat format) { return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB; }

static bool FourBytes(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

// The picture: a horizontal and a vertical gradient, a fine texture, flat patches, thin stripes, and a square that
// moves a few pixels a frame. Opaque. R, G, B in that order; the caller swizzles for BGRA.
static void Picture(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t frame, uint8_t rgb[3])
{
    int r = 30 + static_cast<int>(170 * x / std::max(w, 1u));
    int g = 40 + static_cast<int>(160 * y / std::max(h, 1u));
    int b = 90 + static_cast<int>(((x ^ y) & 31));

    if (x > w / 8 && x < w / 3 && y > h / 6 && y < h / 2) // a flat red patch
    {
        r = 200;
        g = 50;
        b = 40;
    }
    if (x > w / 2 && x < (w * 7) / 8 && y > h / 2 && y < (h * 5) / 6 && (y / 3) % 2 == 0) // thin stripes
    {
        r = g = b = 235;
    }
    const uint32_t side = std::max(w, h) / 10;
    const uint32_t sx = (frame * 5) % std::max(w - side, 1u);
    const uint32_t sy = h / 3;
    if (x >= sx && x < sx + side && y >= sy && y < sy + side) // the moving square
    {
        r = 250;
        g = 220;
        b = 30;
    }
    rgb[0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
    rgb[1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
    rgb[2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
}

static uint32_t Hash(uint32_t seed, uint32_t i)
{
    uint32_t x = seed * 0x9E3779B1u ^ (i + 0x7F4A7C15u) * 0x85EBCA77u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return x;
}

static void Fill(uint8_t* out, VkFormat format, uint32_t w, uint32_t h, uint32_t frame)
{
    for (uint32_t y = 0; y < h; ++y)
    {
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* p = out + (size_t(y) * w + x) * 4;
            if (!(format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
                  format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB))
            {
                // Not an eight-bit format: only its exactness is judged, so any bits do.
                const uint32_t v = Hash(frame + 1, y * w + x);
                std::memcpy(p, &v, 4);
                continue;
            }
            uint8_t rgb[3];
            Picture(x, y, w, h, frame, rgb);
            p[0] = Bgra(format) ? rgb[2] : rgb[0];
            p[1] = rgb[1];
            p[2] = Bgra(format) ? rgb[0] : rgb[2];
            p[3] = 255;
        }
    }
}

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
};

struct SwapchainFacts
{
    std::string name;
    uint64_t handle = 0;
    uint32_t format = 0;
    uint32_t colorSpace = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t images = 0;
    uint32_t family = 0;
    bool sdr = false;
    uint32_t presents = 0;
    uint32_t readbacks = 0;
    uint32_t exact = 0;
    double maeSum = 0.0; // mean absolute change of R, G and B, in eight-bit steps, summed over read-backs
    double maeMax = 0.0;
    uint32_t maxDiff = 0;
    uint64_t alphaChanged = 0;
    uint32_t black = 0; // read-backs whose every colour channel came back zero
    bool destroyed = false;
};

struct App
{
    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND owner = nullptr;
    HWND window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::vector<VkQueueFamilyProperties> families;
    VkPhysicalDeviceMemoryProperties memory {};
    uint32_t graphicsFamily = UINT32_MAX;
    uint32_t computeFamily = UINT32_MAX;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    std::vector<VkSurfaceFormatKHR> offered;
    bool nvxOffered = false;
    uint32_t frames = 12;
    bool cross = false;

    std::vector<SwapchainFacts> swapchains;

    struct Target
    {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent {};
        std::vector<VkImage> images;
        std::vector<int64_t> expected; // per image: the frame last presented in it, -1 when none
        Buffer upload;
        Buffer download;
        size_t facts = 0;
    };

    uint32_t nextFrame = 0;

    void Init()
    {
        WNDCLASSW cls {};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = module;
        cls.lpszClassName = L"OptiScalerVkPresentHostNr";
        Require(RegisterClassW(&cls) != 0, "RegisterClass failed");
        // An owned dialog, as in harness.cpp: a tiling window manager floats it instead of tiling it, which would
        // resize it under the swapchain.
        owner = CreateWindowW(cls.lpszClassName, L"OptiScaler present host cold NR owner", 0, 0, 0, 1, 1, nullptr,
                              nullptr, module, nullptr);
        Require(owner != nullptr, "could not create the dialog owner");
        window = CreateWindowExW(WS_EX_DLGMODALFRAME, cls.lpszClassName, L"OptiScaler present host cold NR",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 640, 480, owner, nullptr, module, nullptr);
        Require(window != nullptr, "no Win32 window under Wine");
        ShowWindow(window, SW_SHOW);
        Pump();

        auto dll = LoadLibraryW(L".\\dxgi.dll");
        Require(dll != nullptr, "could not load OptiScaler as dxgi.dll");

        VkDebugUtilsMessengerCreateInfoEXT debug { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = DebugMessage;
        const char* extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                     VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler Vulkan present host cold NR";
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

        VkWin32SurfaceCreateInfoKHR surfaceInfo { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
        surfaceInfo.hinstance = module;
        surfaceInfo.hwnd = window;
        Check(vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");

        uint32_t n = 0;
        Check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "vkEnumeratePhysicalDevices");
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
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present), "surface support");
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
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);

        uint32_t count = 0;
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "device extensions");
        std::vector<VkExtensionProperties> extensionList(count);
        Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensionList.data()),
              "device extensions");
        for (const auto& e : extensionList)
            nvxOffered = nvxOffered || !strcmp(e.extensionName, "VK_NVX_binary_import");

        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "surface formats");
        offered.resize(count);
        Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, offered.data()), "surface formats");
    }

    void MakeDevice()
    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queues[2] {};
        uint32_t count = 0;
        for (uint32_t family : { graphicsFamily, computeFamily })
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
        Check(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
        if (computeFamily != UINT32_MAX)
            vkGetDeviceQueue(device, computeFamily, 0, &computeQueue);
    }

    void ProbeValidation()
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

    Buffer MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
    {
        Buffer b;
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        Check(vkCreateBuffer(device, &info, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device, b.buffer, &requirements);
        const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
        Check(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
        return b;
    }

    void FreeBuffer(Buffer& b)
    {
        if (b.buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, b.memory, nullptr);
        b = Buffer {};
    }

    void DestroySwapchain(Target& target)
    {
        vkDestroySwapchainKHR(device, target.swapchain, nullptr);
        swapchains[target.facts].destroyed = true;
        target.swapchain = VK_NULL_HANDLE;
        FreeBuffer(target.upload);
        FreeBuffer(target.download);
    }

    void MakeSwapchain(Target& target, const char* name, VkSurfaceFormatKHR format, VkQueue queue, uint32_t family)
    {
        VkSurfaceCapabilitiesKHR caps {};
        Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface capabilities");
        const uint32_t usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        Require((caps.supportedUsageFlags & usage) == usage,
                "the surface does not offer TRANSFER_SRC and TRANSFER_DST");
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
        {
            RECT rect;
            GetClientRect(window, &rect);
            extent.width = std::clamp(uint32_t(rect.right), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(rect.bottom), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        VkSwapchainCreateInfoKHR info { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = surface;
        info.minImageCount =
            caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
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
        Check(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain), "vkCreateSwapchainKHR");
        if (old != VK_NULL_HANDLE)
        {
            Check(vkDeviceWaitIdle(device), "idle before destroying the old swapchain");
            DestroySwapchain(target);
        }
        target.swapchain = swapchain;
        target.queue = queue;
        target.family = family;
        target.format = format.format;
        target.extent = extent;
        uint32_t count = 0;
        Check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "swapchain image count");
        target.images.resize(count);
        Check(vkGetSwapchainImagesKHR(device, swapchain, &count, target.images.data()), "swapchain images");
        target.expected.assign(count, -1);
        const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
        target.upload = MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        target.download = MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        SwapchainFacts facts;
        facts.name = name;
        facts.handle = H(swapchain);
        facts.format = format.format;
        facts.colorSpace = format.colorSpace;
        facts.width = extent.width;
        facts.height = extent.height;
        facts.images = count;
        facts.family = family;
        facts.sdr = Sdr(format.format, format.colorSpace);
        swapchains.push_back(facts);
        target.facts = swapchains.size() - 1;
        std::printf("SWAPCHAIN %s format %u colour space %u %ux%u images %u family %u\n", name, format.format,
                    format.colorSpace, extent.width, extent.height, count, family);
    }

    void Compare(Target& target, uint32_t index)
    {
        SwapchainFacts& facts = swapchains[target.facts];
        const uint32_t w = target.extent.width, h = target.extent.height;
        std::vector<uint8_t> expected(size_t(w) * h * 4);
        Fill(expected.data(), target.format, w, h, static_cast<uint32_t>(target.expected[index]));
        const auto* got = static_cast<const uint8_t*>(target.download.mapped);

        const bool eightBit = target.format == VK_FORMAT_B8G8R8A8_UNORM || target.format == VK_FORMAT_B8G8R8A8_SRGB ||
                              target.format == VK_FORMAT_R8G8B8A8_UNORM || target.format == VK_FORMAT_R8G8B8A8_SRGB;
        const bool exact = std::memcmp(expected.data(), got, expected.size()) == 0;
        uint64_t sum = 0;
        uint32_t maxDiff = 0;
        uint64_t alpha = 0;
        bool black = eightBit;
        if (eightBit)
        {
            for (size_t i = 0; i < expected.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const uint32_t d = static_cast<uint32_t>(std::abs(int(expected[i + c]) - int(got[i + c])));
                    sum += d;
                    maxDiff = std::max(maxDiff, d);
                    black = black && got[i + c] == 0;
                }
                alpha += expected[i + 3] != got[i + 3] ? 1 : 0;
            }
        }
        const double mae = eightBit ? double(sum) / (double(w) * h * 3) : (exact ? 0.0 : 255.0);
        ++facts.readbacks;
        facts.exact += exact ? 1 : 0;
        facts.maeSum += mae;
        facts.maeMax = std::max(facts.maeMax, mae);
        facts.maxDiff = std::max(facts.maxDiff, maxDiff);
        facts.alphaChanged += alpha;
        facts.black += black ? 1 : 0;
    }

    // One frame: acquire, read back what this image showed last, upload the next picture, present with one wait,
    // then wait for all of it.
    void Frame(Target& target)
    {
        Pump();
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
        Check(vkCreateSemaphore(device, &si, nullptr, &acquired), "acquire semaphore");
        Check(vkCreateSemaphore(device, &si, nullptr, &rendered), "render semaphore");
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

        const VkImage image = target.images[index];
        const bool readback = target.expected[index] >= 0;
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
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.download.buffer, 1,
                                   &region);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        }
        else
        {
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const uint32_t frame = nextFrame++;
        Fill(static_cast<uint8_t*>(target.upload.mapped), target.format, target.extent.width, target.extent.height,
             frame);
        vkCmdCopyBufferToImage(cmd, target.upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
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
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered;
        Check(vkQueueSubmit(target.queue, 1, &submit, VK_NULL_HANDLE), "submit frame");

        VkPresentInfoKHR present { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &target.swapchain;
        present.pImageIndices = &index;
        const VkResult result = vkQueuePresentKHR(target.queue, &present);
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR,
                "present of " + swapchains[target.facts].name + " returned " + std::to_string(result));
        ++swapchains[target.facts].presents;

        Check(vkQueueWaitIdle(target.queue), "wait for presentation");
        if (readback)
            Compare(target, index);
        vkDestroyCommandPool(device, pool, nullptr);
        vkDestroySemaphore(device, acquired, nullptr);
        vkDestroySemaphore(device, rendered, nullptr);
        target.expected[index] = frame;
    }

    void Frames(Target& target)
    {
        // Enough frames that every image is read back at least once.
        const uint32_t count = std::max<uint32_t>(frames, static_cast<uint32_t>(target.images.size()) * 2 + 2);
        for (uint32_t i = 0; i < count; ++i)
            Frame(target);
    }

    void Run()
    {
        Init();
        MakeDevice();
        ProbeValidation();

        Target target;
        const char* sdrNames[] = { "bgra8-unorm", "bgra8-srgb", "rgba8-unorm", "rgba8-srgb" };
        const VkFormat sdrFormats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_FORMAT_R8G8B8A8_SRGB };
        VkSurfaceFormatKHR first {};
        for (int i = 0; i < 4; ++i)
        {
            for (const auto& f : offered)
            {
                if (f.format == sdrFormats[i] && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    if (first.format == VK_FORMAT_UNDEFINED)
                        first = f;
                    MakeSwapchain(target, sdrNames[i], f, graphicsQueue, graphicsFamily);
                    Frames(target);
                }
            }
        }
        Require(first.format != VK_FORMAT_UNDEFINED, "the surface offers no SDR RGBA8/BGRA8 format");

        // One four-byte format that is not SDR, when the surface offers one: refused, carried unchanged.
        for (const auto& f : offered)
        {
            if (FourBytes(f.format) && !Sdr(f.format, f.colorSpace))
            {
                MakeSwapchain(target, "not-sdr", f, graphicsQueue, graphicsFamily);
                Frames(target);
                break;
            }
        }

        // From the compute-only family: the round trip and the model on the graphics family, handed over and back.
        if (cross && computeQueue != VK_NULL_HANDLE)
        {
            MakeSwapchain(target, "cross", first, computeQueue, computeFamily);
            Frames(target);
        }

        // An extent that is not a multiple of eight, through oldSwapchain: the model rebuilt at the new size.
        RECT outer {}, client {};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        SetWindowPos(window, nullptr, 0, 0, 517 + (outer.right - outer.left) - client.right,
                     389 + (outer.bottom - outer.top) - client.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < 10; ++i)
        {
            Pump();
            Sleep(20);
        }
        MakeSwapchain(target, "odd", first, graphicsQueue, graphicsFamily);
        Frames(target);

        Check(vkDeviceWaitIdle(device), "final idle");
        DestroySwapchain(target);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        auto destroyDebug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        destroyDebug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
        DestroyWindow(window);
        DestroyWindow(owner);
    }

    std::string Facts() const
    {
        std::ostringstream out;
        out << "{\n  \"status\": \"" << (validationErrors == 0 ? "PASS" : "FAIL") << "\",\n  \"first_validation\": \""
            << Escape(firstValidation) << "\",\n  \"validation_active\": " << (validationActive ? "true" : "false")
            << ",\n  \"validation_errors\": " << validationErrors
            << ",\n  \"wine_injected_extension_messages\": " << wineInjected
            << ",\n  \"model_messages\": " << modelMessages << ",\n  \"model_vuids\": \"" << Escape(modelVuids) << "\""
            << ",\n  \"nvx_offered\": " << (nvxOffered ? "true" : "false")
            << ",\n  \"graphics_family\": " << graphicsFamily
            << ",\n  \"compute_family\": " << (computeFamily == UINT32_MAX ? -1 : static_cast<int>(computeFamily))
            << ",\n  \"window\": \"" << Hex(H(window)) << "\",\n  \"device\": \"" << Hex(H(device))
            << "\",\n  \"offered\": [";
        for (size_t i = 0; i < offered.size(); ++i)
            out << (i ? ", " : "") << "[" << offered[i].format << ", " << offered[i].colorSpace << "]";
        out << "],\n  \"swapchains\": [";
        for (size_t i = 0; i < swapchains.size(); ++i)
        {
            const auto& s = swapchains[i];
            out << (i ? ",\n    " : "\n    ") << "{\"name\": \"" << s.name << "\", \"handle\": \"" << Hex(s.handle)
                << "\", \"format\": " << s.format << ", \"color_space\": " << s.colorSpace << ", \"width\": " << s.width
                << ", \"height\": " << s.height << ", \"images\": " << s.images << ", \"family\": " << s.family
                << ", \"sdr\": " << (s.sdr ? "true" : "false") << ", \"presents\": " << s.presents
                << ", \"readbacks\": " << s.readbacks << ", \"exact\": " << s.exact
                << ", \"mae_mean\": " << (s.readbacks ? s.maeSum / s.readbacks : 0.0) << ", \"mae_max\": " << s.maeMax
                << ", \"max_diff\": " << s.maxDiff << ", \"alpha_changed\": " << s.alphaChanged
                << ", \"black\": " << s.black << ", \"destroyed\": " << (s.destroyed ? "true" : "false") << "}";
        }
        out << "]\n}\n";
        return out.str();
    }
};

int main(int argc, char** argv)
{
    std::ofstream("result.json") << "{\"status\":\"RUNNING\"}\n";
    try
    {
        App app;
        for (int i = 1; i < argc; ++i)
        {
            if (!strcmp(argv[i], "--cross"))
                app.cross = true;
            else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
                app.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
            else
                Require(false, std::string("unknown argument ") + argv[i]);
        }
        app.Run();
        std::ofstream("result.json") << app.Facts();
        Require(validationErrors == 0, "Vulkan validation reported errors (first: " + firstValidation + ")");
        uint32_t presents = 0, readbacks = 0;
        for (const auto& s : app.swapchains)
        {
            presents += s.presents;
            readbacks += s.readbacks;
        }
        std::printf("PASS: %u presents over %zu swapchains, %u read-backs; validation clean\n", presents,
                    app.swapchains.size(), readbacks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        std::ifstream existing("result.json");
        const std::string written((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
        existing.close();
        if (written.find("\"swapchains\"") == std::string::npos)
            std::ofstream("result.json") << "{\"status\":\"FAIL\",\"reason\":\"" << Escape(error.what()) << "\"}\n";
        return 1;
    }
}
