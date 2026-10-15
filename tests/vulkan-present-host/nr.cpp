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
//
// With --route it is instead a game that turns its upscaler on and off (DOOM Eternal's shape, 2026-10-14): one
// swapchain presented throughout; NGX initialised through OptiScaler's own NVSDK_NGX_VULKAN_* exports, as a game
// calls them, and a super-sampling feature created and evaluated every frame for a few seconds (OptiScaler runs its
// upscaler, then the after-upscale route runs the model on its output, inside the evaluate's command buffer); then the
// feature released and nothing evaluated for longer than the present host waits before taking the model back; twice.
// Each phase's read-backs are counted apart, so the runner can tell the frames the present host's model edited from
// those it did not.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include "nvsdk_ngx_vk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

// With --pan, the whole picture pans this many pixels a frame (content moving left): a camera turning, for synthesized
// motion (stage 5) to follow. Zero otherwise.
static uint32_t g_pan = 0;

// The picture: a horizontal and a vertical gradient, a fine texture, flat patches, thin stripes, and a square that
// moves a few pixels a frame. Opaque. R, G, B in that order; the caller swizzles for BGRA. With --pan everything but the
// square is sampled g_pan pixels further right every frame, the gradient repeating every two widths.
static void Picture(uint32_t xIn, uint32_t y, uint32_t w, uint32_t h, uint32_t frame, uint8_t rgb[3])
{
    const uint32_t x = g_pan != 0 ? (xIn + frame * g_pan) % std::max(2 * w, 1u) : xIn;
    int r = 30 + static_cast<int>(170 * (x % std::max(w, 1u)) / std::max(w, 1u));
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
    if (xIn >= sx && xIn < sx + side && y >= sy && y < sy + side) // the moving square
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
    double maxPresentMs = 0.0; // the longest vkQueuePresentKHR call, the host's hook and everything below included
    // With --pan, from the swapchain's 16th frame (past the model's build and the estimator's warm-up), on frames the
    // model edited: how much of the previous frame, unmoved, the model's edit carries -- the projection of (read back - presented) on (previous -
    // presented) at the same pixel, per frame. A model blending a stale history it was told did not move ghosts toward
    // 1; one whose history follows the camera stays near 0.
    double trailSum = 0.0;
    uint32_t trailFrames = 0;
};

// One phase of --route, with the read-backs of the frames presented in it counted apart.
struct PhaseFacts
{
    std::string name;
    uint32_t presents = 0;
    uint32_t evaluates = 0;
    uint32_t readbacks = 0;
    uint32_t exact = 0;
    double maeSum = 0.0;
};

// OptiScaler's NGX exports, as a game reaches them.
using PFN_NgxInit = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                                         PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                         const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_NgxParams = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using PFN_NgxDestroyParams = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
using PFN_NgxCreate = NVSDK_NGX_Result (*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                           NVSDK_NGX_Handle**);
using PFN_NgxEvaluate = NVSDK_NGX_Result (*)(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*,
                                             PFN_NVSDK_NGX_ProgressCallback);
using PFN_NgxRelease = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);
using PFN_NgxShutdown1 = NVSDK_NGX_Result (*)(VkDevice);

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
    bool route = false;
    bool wdx12 = false;
    bool nrToggle = false;
    bool slowBuild = false;
    bool wdx12Probed = false;
    HMODULE optiscaler = nullptr;
    std::vector<PhaseFacts> phases;
    PhaseFacts* phase = nullptr; // the phase read-backs are counted in, with --route
    int64_t phaseFrom = 0;       // ... the first frame number counted in it

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
        uint32_t firstFrame = 0; // the frame number its first present carried
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
        optiscaler = dll;

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
        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME };
        VkDeviceCreateInfo info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        info.queueCreateInfoCount = count;
        info.pQueueCreateInfos = queues;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;

        // With --route, what a game running FSR 2.2 enables: its shaders declare Int16 and Float16.
        VkPhysicalDeviceFeatures features {};
        VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16 {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR
        };
        if (route)
        {
            VkPhysicalDeviceFeatures supported {};
            vkGetPhysicalDeviceFeatures(physical, &supported);
            Require(supported.shaderInt16 == VK_TRUE, "the device has no shaderInt16, which FSR 2.2 needs");
            features.shaderInt16 = VK_TRUE;
            float16.shaderFloat16 = VK_TRUE;
            info.pEnabledFeatures = &features;
            info.pNext = &float16;
            info.enabledExtensionCount = 2;
        }
        Check(vkCreateDevice(physical, &info, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
        if (wdx12)
        {
            // Characterise the real wDx12 hook, not a test double: Vulkan accepts this zero-batch call, but
            // hk_vkQueueSubmit rejects its null array. Exactly this one rejection must appear in the log;
            // the host's hand-back and destroy drains must then get through it with empty, non-null batches.
            auto submit = reinterpret_cast<PFN_vkQueueSubmit>(vkGetDeviceProcAddr(device, "vkQueueSubmit"));
            Require(submit != nullptr, "no vkQueueSubmit for the wDx12 chain probe");
            Require(submit(graphicsQueue, 0, nullptr, VK_NULL_HANDLE) == VK_ERROR_INITIALIZATION_FAILED,
                    "the wDx12 null-submit rejection is absent: the required hook chain is not active");
            wdx12Probed = true;
        }
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
        target.firstFrame = nextFrame;
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
        const int64_t frame = target.expected[index];
        if (g_pan != 0 && eightBit && !exact && frame >= int64_t(target.firstFrame) + 16)
        {
            std::vector<uint8_t> previous(expected.size());
            Fill(previous.data(), target.format, w, h, static_cast<uint32_t>(frame - 1));
            double along = 0.0, norm = 0.0;
            for (size_t i = 0; i < expected.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const double edit = double(got[i + c]) - double(expected[i + c]);
                    const double stale = double(previous[i + c]) - double(expected[i + c]);
                    along += edit * stale;
                    norm += stale * stale;
                }
            }
            if (norm > 0.0)
            {
                facts.trailSum += along / norm;
                ++facts.trailFrames;
            }
        }
        if (phase != nullptr && target.expected[index] >= phaseFrom)
        {
            ++phase->readbacks;
            phase->exact += exact ? 1 : 0;
            phase->maeSum += mae;
        }
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
        const auto presentStart = std::chrono::steady_clock::now();
        const VkResult result = vkQueuePresentKHR(target.queue, &present);
        const double presentMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();
        Require(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR,
                "present of " + swapchains[target.facts].name + " returned " + std::to_string(result));
        ++swapchains[target.facts].presents;
        swapchains[target.facts].maxPresentMs = std::max(swapchains[target.facts].maxPresentMs, presentMs);

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

    // --- --route: the game's upscaler on, off, on, off ---------------------------------------------------------

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        NVSDK_NGX_Resource_VK ngx {};
    };

    Image MakeImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, bool readWrite)
    {
        Image img;
        img.format = format;
        img.width = width;
        img.height = height;
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(vkCreateImage(device, &info, nullptr, &img.image), "vkCreateImage");
        VkMemoryRequirements requirements {};
        vkGetImageMemoryRequirements(device, img.image, &requirements);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = i;
        Require(type != UINT32_MAX, "no device-local memory type for an image");
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &allocate, nullptr, &img.memory), "vkAllocateMemory (image)");
        Check(vkBindImageMemory(device, img.image, img.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = img.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(device, &view, nullptr, &img.view), "vkCreateImageView");
        img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
        img.ngx.Resource.ImageViewInfo.ImageView = img.view;
        img.ngx.Resource.ImageViewInfo.Image = img.image;
        img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
        img.ngx.Resource.ImageViewInfo.Format = format;
        img.ngx.Resource.ImageViewInfo.Width = width;
        img.ngx.Resource.ImageViewInfo.Height = height;
        img.ngx.ReadWrite = readWrite;
        return img;
    }

    void FreeImage(Image& img)
    {
        vkDestroyImageView(device, img.view, nullptr);
        vkDestroyImage(device, img.image, nullptr);
        vkFreeMemory(device, img.memory, nullptr);
        img = Image {};
    }

    template <typename Pfn> Pfn Ngx(const char* name)
    {
        auto address = reinterpret_cast<Pfn>(GetProcAddress(optiscaler, name));
        Require(address != nullptr, std::string("OptiScaler does not export ") + name);
        return address;
    }

    // One command buffer on the graphics queue, recorded by `record`, submitted and waited for.
    template <typename Record> void OneShot(Record record)
    {
        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = graphicsFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command pool (route)");
        VkCommandBufferAllocateInfo ai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &ai, &cmd), "command buffer (route)");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "begin (route)");
        record(cmd);
        Check(vkEndCommandBuffer(cmd), "end (route)");
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE), "submit (route)");
        Check(vkQueueWaitIdle(graphicsQueue), "wait (route)");
        vkDestroyCommandPool(device, pool, nullptr);
    }

    void Begin(const char* name)
    {
        phases.push_back(PhaseFacts {});
        phases.back().name = name;
        phase = &phases.back();
        phaseFrom = nextFrame;
    }

    void RunRoute()
    {
        Init();
        MakeDevice();
        ProbeValidation();
        phases.reserve(16); // Begin keeps a pointer into it

        VkSurfaceFormatKHR format {};
        for (const auto& f : offered)
            if (format.format == VK_FORMAT_UNDEFINED && Sdr(f.format, f.colorSpace))
                format = f;
        Require(format.format != VK_FORMAT_UNDEFINED, "the surface offers no SDR RGBA8/BGRA8 format");
        Target target;
        MakeSwapchain(target, "route", format, graphicsQueue, graphicsFamily);

        using Clock = std::chrono::steady_clock;
        // Frames are begun until `end`; under the validation layer one takes up to a few hundred milliseconds, which
        // the phases' bounds leave room for.
        // With `atLeast`, frames go on past `end` until that many were presented: a phase whose read-backs are judged
        // must have some, however slowly a loaded machine presents (a phase counts only the read-backs of its own
        // frames it makes itself, so a few fewer than its presents: the swapchain's images).
        const auto presentUntil = [&](const char* name, Clock::time_point end, uint32_t atLeast = 0)
        {
            Begin(name);
            while (Clock::now() < end || phase->presents < atLeast)
            {
                Frame(target);
                ++phase->presents;
            }
        };
        using Ms = std::chrono::milliseconds;

        // The game's start window: nothing upscaled, the present host runs the model at present.
        // The input-enabled overlay has a seven-second startup splash. Let it finish before judging the route's
        // unedited frames; the cold phase itself already expects the present host to change the picture.
        // First the model's build, off the present path: its frames are carried unchanged until the product is adopted.
        presentUntil("cold-build", Clock::now() + Ms(2000));
        presentUntil("cold", Clock::now() + Ms(nrToggle ? 8000 : 1500), 12);

        const auto init = Ngx<PFN_NgxInit>("NVSDK_NGX_VULKAN_Init");
        const auto capabilities = Ngx<PFN_NgxParams>("NVSDK_NGX_VULKAN_GetCapabilityParameters");
        const auto destroyParams = Ngx<PFN_NgxDestroyParams>("NVSDK_NGX_VULKAN_DestroyParameters");
        const auto create = Ngx<PFN_NgxCreate>("NVSDK_NGX_VULKAN_CreateFeature1");
        const auto evaluate = Ngx<PFN_NgxEvaluate>("NVSDK_NGX_VULKAN_EvaluateFeature");
        const auto release = Ngx<PFN_NgxRelease>("NVSDK_NGX_VULKAN_ReleaseFeature");
        const auto shutdown = Ngx<PFN_NgxShutdown1>("NVSDK_NGX_VULKAN_Shutdown1");

        Require(init(0x5245544F51ull, L".", instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
                     nullptr, NVSDK_NGX_Version_API) == NVSDK_NGX_Result_Success,
                "NVSDK_NGX_VULKAN_Init through OptiScaler failed");
        NVSDK_NGX_Parameter* params = nullptr;
        Require(capabilities(&params) == NVSDK_NGX_Result_Success && params != nullptr,
                "NVSDK_NGX_VULKAN_GetCapabilityParameters through OptiScaler failed");

        // A game's buffers: half-resolution colour, depth and motion, and the upscaler's output, which the route
        // runs the model over.
        constexpr uint32_t renderW = 640, renderH = 360, outW = 1280, outH = 720;
        const VkImageUsageFlags input = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Image colour = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, renderW, renderH, input, false);
        Image depth = MakeImage(VK_FORMAT_R32_SFLOAT, renderW, renderH, input, false);
        Image motion = MakeImage(VK_FORMAT_R16G16_SFLOAT, renderW, renderH, input, false);
        Image output = MakeImage(VK_FORMAT_R16G16B16A16_SFLOAT, outW, outH,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 true);
        OneShot(
            [&](VkCommandBuffer cmd)
            {
                for (Image* img : { &colour, &depth, &motion, &output })
                {
                    VkImageMemoryBarrier b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img->image;
                    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                         nullptr, 0, nullptr, 1, &b);
                    // A flat colour, a constant depth, no motion, and an output the upscaler overwrites.
                    VkClearColorValue value {};
                    if (img == &colour)
                        value = { { 0.4f, 0.3f, 0.2f, 1.0f } };
                    else if (img == &depth)
                        value = { { 0.5f, 0.0f, 0.0f, 0.0f } };
                    vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                         &b.subresourceRange);
                    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    // As NGX takes them: inputs in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL.
                    b.newLayout = img == &output ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                         0, nullptr, 0, nullptr, 1, &b);
                }
            });

        // The game turns its upscaler on: a feature made, evaluated with every frame `evaluates` times, released. When
        // the last evaluate began: the route asks inside it, so the present host may not take the model back before
        // that + kAfterUpscaleIdleMs.
        const auto upscalerOn = [&](const char* name, int evaluates)
        {
            params->Set(NVSDK_NGX_Parameter_Width, renderW);
            params->Set(NVSDK_NGX_Parameter_Height, renderH);
            params->Set(NVSDK_NGX_Parameter_OutWidth, outW);
            params->Set(NVSDK_NGX_Parameter_OutHeight, outH);
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));
            params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                        static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes));
            params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            NVSDK_NGX_Handle* handle = nullptr;
            NVSDK_NGX_Result created = NVSDK_NGX_Result_Fail;
            OneShot([&](VkCommandBuffer cmd)
                    { created = create(device, cmd, NVSDK_NGX_Feature_SuperSampling, params, &handle); });
            Require(created == NVSDK_NGX_Result_Success && handle != nullptr,
                    "NVSDK_NGX_VULKAN_CreateFeature1 through OptiScaler returned " +
                        std::to_string(static_cast<unsigned>(created)));

            Begin(name);
            Clock::time_point lastAsk;
            for (int i = 0; i < evaluates; ++i)
            {
                lastAsk = Clock::now();
                NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
                OneShot(
                    [&](VkCommandBuffer cmd)
                    {
                        params->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&colour.ngx));
                        params->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(&output.ngx));
                        params->Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(&depth.ngx));
                        params->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(&motion.ngx));
                        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
                        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
                        params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
                        params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
                        params->Set(NVSDK_NGX_Parameter_Reset, i == 0 ? 1 : 0);
                        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, renderW);
                        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, renderH);
                        params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.6f);
                        result = evaluate(cmd, handle, params, nullptr);
                    });
                Require(result == NVSDK_NGX_Result_Success, "NVSDK_NGX_VULKAN_EvaluateFeature through OptiScaler "
                                                            "returned " +
                                                                std::to_string(static_cast<unsigned>(result)));
                ++phase->evaluates;
                Frame(target);
                ++phase->presents;
            }

            Require(release(handle) == NVSDK_NGX_Result_Success, "NVSDK_NGX_VULKAN_ReleaseFeature failed");
            return lastAsk;
        };

        for (int cycle = 0; cycle < 2; ++cycle)
        {
            const Clock::time_point lastAsk = upscalerOn(cycle == 0 ? "on" : "on-again", 30);

            // Only this harness window receives these messages. Exercise the production F7 handler, without
            // global SendInput or a test-only DLL export. The runner requires the actual toggle log and NR-off
            // blocker, so a backend that ignores the messages fails rather than pretending the toggle happened.
            const auto toggle = [&]
            {
                const LPARAM scan = static_cast<LPARAM>(MapVirtualKeyW(VK_F7, MAPVK_VK_TO_VSC)) << 16;
                Require(PostMessageW(window, WM_KEYDOWN, VK_F7, 1 | scan) != 0, "post F7 down");
                for (int i = 0; i < 3; ++i)
                {
                    Frame(target);
                    ++phase->presents;
                }
                Require(PostMessageW(window, WM_KEYUP, VK_F7, 1 | scan | (3ll << 30)) != 0, "post F7 up");
                // F7 draws a two-second toast, plus its fade. Keep those frames in the toggle phase, outside the
                // byte-exact disabled/route checks; ownership timing is still checked against the toggle log.
                const auto toastGone = Clock::now() + Ms(3000);
                while (Clock::now() < toastGone)
                {
                    Frame(target);
                    ++phase->presents;
                }
            };
            if (nrToggle)
            {
                Begin(cycle == 0 ? "toggle-off" : "toggle-off-again");
                toggle();
            }

            // ... and off: nothing evaluated for longer than the present host waits (10 s) before taking the model
            // back: the frames until 9 s after the last ask must be the route's (unedited), those from 12 s on the
            // present host's again.
            presentUntil(cycle == 0 ? "off-waiting" : "off-again-waiting", lastAsk + Ms(9000));
            if (nrToggle)
            {
                // Hold NR off beyond the quiet deadline: exactly one named blocker per cycle, no hand-back.
                presentUntil(cycle == 0 ? "nr-disabled" : "nr-disabled-again", lastAsk + Ms(13000), 12);
                Begin(cycle == 0 ? "toggle-on" : "toggle-on-again");
                toggle();
                // The model must return without waiting for another ten-second idle window.
                presentUntil(cycle == 0 ? "off-handing-back" : "off-again-handing-back", Clock::now() + Ms(3000));
                presentUntil(cycle == 0 ? "off-back" : "off-again-back", Clock::now() + Ms(1500), 12);
            }
            else
            {
                presentUntil(cycle == 0 ? "off-handing-back" : "off-again-handing-back", lastAsk + Ms(12000));
                presentUntil(cycle == 0 ? "off-back" : "off-again-back", lastAsk + Ms(14000), 12);
            }
        }

        // On once more, and the game exits with the route holding the model: its objects go with the device.
        upscalerOn("on-at-exit", 10);
        phase = nullptr;

        Check(vkDeviceWaitIdle(device), "idle before NGX's shutdown");
        destroyParams(params);
        shutdown(device);
        for (Image* img : { &colour, &depth, &motion, &output })
            FreeImage(*img);

        if (nrToggle)
        {
            // The legacy overlay frees its device objects on a new Win32 surface, not on DestroySwapchain or
            // DestroyDevice. Exercise that public cleanup path before teardown; do not suppress its leak VUID.
            // No swapchain or present is made on this temporary surface, and both hand-back cycles are over.
            VkWin32SurfaceCreateInfoKHR info { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
            info.hinstance = module;
            info.hwnd = window;
            VkSurfaceKHR cleanupSurface = VK_NULL_HANDLE;
            Check(vkCreateWin32SurfaceKHR(instance, &info, nullptr, &cleanupSurface), "overlay cleanup surface");
            vkDestroySurfaceKHR(instance, cleanupSurface, nullptr);
        }
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

    // --- --slow-build: the model's build slowed (by the test layer) while presents and swapchain churn go on --------

    void Resize(uint32_t width, uint32_t height)
    {
        RECT outer {}, client {};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        SetWindowPos(window, nullptr, 0, 0, width + (outer.right - outer.left) - client.right,
                     height + (outer.bottom - outer.top) - client.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        for (int i = 0; i < 10; ++i)
        {
            Pump();
            Sleep(20);
        }
    }

    // DOOM Eternal's startup churn, with the layer slowing every allocation, pipeline and NVX module made off the
    // device's creating thread -- the model's builder -- while this thread presents: a swapchain presented once and
    // destroyed (#9), its replacement at the same size, a resize to a second size while a build runs, and back.
    // Every present is timed; none may wait on the model.
    void RunSlowBuild()
    {
        Init();
        MakeDevice();
        ProbeValidation();

        VkSurfaceFormatKHR format {};
        for (const auto& f : offered)
            if (format.format == VK_FORMAT_UNDEFINED && Sdr(f.format, f.colorSpace))
                format = f;
        Require(format.format != VK_FORMAT_UNDEFINED, "the surface offers no SDR RGBA8/BGRA8 format");
        RECT client {};
        GetClientRect(window, &client);
        const uint32_t width = static_cast<uint32_t>(client.right), height = static_cast<uint32_t>(client.bottom);

        using Clock = std::chrono::steady_clock;
        const auto presentFor = [&](Target& target, int milliseconds)
        {
            const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
            while (Clock::now() < end)
                Frame(target);
        };

        Target target;
        MakeSwapchain(target, "once", format, graphicsQueue, graphicsFamily);
        Frame(target); // the build is asked for here, and runs on
        Check(vkDeviceWaitIdle(device), "idle before destroying the one-present swapchain");
        DestroySwapchain(target); // destroyed one present after the model was asked for, as #9

        MakeSwapchain(target, "same-size", format, graphicsQueue, graphicsFamily);
        presentFor(target, 1500);

        Resize(517, 389);
        MakeSwapchain(target, "resized", format, graphicsQueue, graphicsFamily);
        presentFor(target, 1500);

        Resize(width, height);
        MakeSwapchain(target, "back", format, graphicsQueue, graphicsFamily);
        presentFor(target, 12000);

        // A last size, one present -- a build is asked for and runs, slowed -- and the device destroyed at once: its
        // destruction waits for the build under way before anything of the model's goes.
        Resize(600, 400);
        MakeSwapchain(target, "last", format, graphicsQueue, graphicsFamily);
        Frame(target);

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

    void Run()
    {
        if (route)
            return RunRoute();
        if (slowBuild)
            return RunSlowBuild();

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
                << ", \"black\": " << s.black << ", \"destroyed\": " << (s.destroyed ? "true" : "false")
                << ", \"max_present_ms\": " << s.maxPresentMs
                << ", \"trail_mean\": " << (s.trailFrames ? s.trailSum / s.trailFrames : 0.0)
                << ", \"trail_frames\": " << s.trailFrames << "}";
        }
        out << "],\n  \"wdx12_probed\": " << (wdx12Probed ? "true" : "false");
        out << ",\n  \"phases\": [";
        for (size_t i = 0; i < phases.size(); ++i)
        {
            const auto& f = phases[i];
            out << (i ? ",\n    " : "\n    ") << "{\"name\": \"" << f.name << "\", \"presents\": " << f.presents
                << ", \"evaluates\": " << f.evaluates << ", \"readbacks\": " << f.readbacks
                << ", \"exact\": " << f.exact << ", \"mae_mean\": " << (f.readbacks ? f.maeSum / f.readbacks : 0.0)
                << "}";
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
            else if (!strcmp(argv[i], "--route"))
                app.route = true;
            else if (!strcmp(argv[i], "--wdx12"))
                app.wdx12 = true;
            else if (!strcmp(argv[i], "--nr-toggle"))
                app.nrToggle = true;
            else if (!strcmp(argv[i], "--slow-build"))
                app.slowBuild = true;
            else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
                app.frames = static_cast<uint32_t>(std::atoi(argv[++i]));
            else if (!strcmp(argv[i], "--pan") && i + 1 < argc)
                g_pan = static_cast<uint32_t>(std::atoi(argv[++i]));
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
