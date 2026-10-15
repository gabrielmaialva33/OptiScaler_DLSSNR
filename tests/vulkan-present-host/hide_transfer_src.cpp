// A native (Linux) Vulkan layer for tests/vulkan-present-host only: it clears VK_IMAGE_USAGE_TRANSFER_SRC_BIT from
// every surface's supportedUsageFlags, so a swapchain is made on a surface that does not offer the usage the present
// host's round trip needs. winevulkan answers the Windows application's and OptiScaler's surface queries from the host
// driver through the host loader, so this layer is what both of them see. Listed after the validation layer in
// VK_INSTANCE_LAYERS (nearer the driver), so the validation layer sees the hidden usage too and would report a
// swapchain that asked for it anyway.
//
// With RETOQUE_HIDE_TRANSFER_SRC=0 it leaves the usage alone; with RETOQUE_HIDE_DEVICE_EXTENSIONS set to a
// comma-separated list it also takes those names out of vkEnumerateDeviceExtensionProperties (stage 3's "the device
// does not offer the model's extensions" case).
//
// Two timing knobs for "Off the present path" in the design note:
// - RETOQUE_SLOW_OFF_DEVICE_THREAD_MS: every vkAllocateMemory, vkCreateComputePipelines, vkCreateCuModuleNVX and
//   vkCreateCuFunctionNVX made on a thread other than the one that created the device sleeps this long first, up to
//   RETOQUE_SLOW_OFF_DEVICE_THREAD_TOTAL_MS (default 3000) per burst -- a burst ends after a second with no such call,
//   so each of the model's builds is slowed about as much: the present host's model builder (and NGX's own threads)
//   slowed, the application's own thread -- which presents -- untouched.
// - RETOQUE_STALL_PRESENT_AT=N:MS: the N-th vkQueuePresentKHR sleeps MS before calling down, inside the host's present
//   hook, for the stall watchdog to name.
// - RETOQUE_COUNT_OFF_THREAD_QUEUE=1: vkQueueSubmit, vkQueueSubmit2 and vkQueueWaitIdle made on a thread other than the
//   device's creating one are counted, and the counts written to layer-offthread.txt at the device's destruction: what
//   the model's builder (and NGX inside it) does to the queues behind the host's back.
//
// Everything else passes through. Built by run.py with the host's g++ against external/vulkan's headers.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct InstanceData
{
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr next = nullptr;
    PFN_vkDestroyInstance destroy = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR caps = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR caps2 = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties extensions = nullptr;
};

struct DeviceData
{
    PFN_vkGetDeviceProcAddr next = nullptr;
    PFN_vkDestroyDevice destroy = nullptr;
    PFN_vkAllocateMemory allocate = nullptr;
    PFN_vkCreateComputePipelines pipelines = nullptr;
    PFN_vkCreateCuModuleNVX module = nullptr;
    PFN_vkCreateCuFunctionNVX function = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueSubmit2 submit2 = nullptr;
    PFN_vkQueueWaitIdle waitIdle = nullptr;
    std::thread::id creator;
};

std::mutex g_mutex;
std::unordered_map<void*, InstanceData> g_instances; // by dispatch key: an instance and its physical devices share it
std::unordered_map<void*, DeviceData> g_devices;

template <typename T> void* Key(T handle) { return *reinterpret_cast<void**>(handle); }

bool HideTransferSrc()
{
    const char* value = std::getenv("RETOQUE_HIDE_TRANSFER_SRC");
    return value == nullptr || std::strcmp(value, "0") != 0;
}

bool Hidden(const char* name)
{
    const char* list = std::getenv("RETOQUE_HIDE_DEVICE_EXTENSIONS");
    if (list == nullptr)
        return false;
    const std::string all = std::string(",") + list + ",";
    return all.find(std::string(",") + name + ",") != std::string::npos;
}

DeviceData DeviceOf(void* key)
{
    std::lock_guard lock(g_mutex);
    const auto found = g_devices.find(key);
    return found != g_devices.end() ? found->second : DeviceData {};
}

uint64_t EnvMs(const char* name, uint64_t fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::strtoull(value, nullptr, 10) : fallback;
}

std::mutex g_slowMutex;
uint64_t g_slowedMs = 0;     // in the current burst
uint64_t g_lastSlowedAt = 0; // monotonic ms the last such call ended (or will, when slowed)

uint64_t NowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Off the device's creating thread: sleep the knob's time, within the burst's total.
void SlowOffDeviceThread(const DeviceData& data)
{
    const uint64_t ms = EnvMs("RETOQUE_SLOW_OFF_DEVICE_THREAD_MS", 0);
    if (ms == 0 || std::this_thread::get_id() == data.creator)
        return;
    {
        std::lock_guard lock(g_slowMutex);
        // A burst is a run of these calls with no second's gap between them: one build's, slowed or not.
        const uint64_t now = NowMs();
        if (now > g_lastSlowedAt + 1000)
            g_slowedMs = 0; // a new burst: another build
        g_lastSlowedAt = now;
        if (g_slowedMs + ms > EnvMs("RETOQUE_SLOW_OFF_DEVICE_THREAD_TOTAL_MS", 3000))
            return;
        g_slowedMs += ms;
        g_lastSlowedAt = now + ms;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::atomic<uint64_t> g_presents { 0 };
std::atomic<uint64_t> g_offThreadSubmits { 0 };
std::atomic<uint64_t> g_offThreadWaitIdles { 0 };

bool CountOffThread(const DeviceData& data)
{
    const char* value = std::getenv("RETOQUE_COUNT_OFF_THREAD_QUEUE");
    return value != nullptr && std::strcmp(value, "1") == 0 && std::this_thread::get_id() != data.creator;
}

InstanceData InstanceOf(void* key)
{
    std::lock_guard lock(g_mutex);
    const auto found = g_instances.find(key);
    return found != g_instances.end() ? found->second : InstanceData {};
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                              VkInstance* instance)
{
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(info->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    if (chain == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    const PFN_vkGetInstanceProcAddr next = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    auto create = reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE, "vkCreateInstance"));
    const VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS)
        return result;

    InstanceData data;
    data.instance = *instance;
    data.next = next;
    data.destroy = reinterpret_cast<PFN_vkDestroyInstance>(next(*instance, "vkDestroyInstance"));
    data.caps = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        next(*instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
    data.caps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        next(*instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    data.extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        next(*instance, "vkEnumerateDeviceExtensionProperties"));

    std::lock_guard lock(g_mutex);
    g_instances[Key(*instance)] = data;
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
    const InstanceData data = InstanceOf(Key(instance));
    {
        std::lock_guard lock(g_mutex);
        g_instances.erase(Key(instance));
    }
    if (data.destroy != nullptr)
        data.destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* device)
{
    auto* chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(info->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO))
        chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(chain->pNext));
    if (chain == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    const PFN_vkGetInstanceProcAddr nextInstance = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr nextDevice = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

    auto create =
        reinterpret_cast<PFN_vkCreateDevice>(nextInstance(InstanceOf(Key(physical)).instance, "vkCreateDevice"));
    const VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS)
        return result;

    DeviceData data;
    data.next = nextDevice;
    data.destroy = reinterpret_cast<PFN_vkDestroyDevice>(nextDevice(*device, "vkDestroyDevice"));
    data.allocate = reinterpret_cast<PFN_vkAllocateMemory>(nextDevice(*device, "vkAllocateMemory"));
    data.pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(nextDevice(*device, "vkCreateComputePipelines"));
    data.module = reinterpret_cast<PFN_vkCreateCuModuleNVX>(nextDevice(*device, "vkCreateCuModuleNVX"));
    data.function = reinterpret_cast<PFN_vkCreateCuFunctionNVX>(nextDevice(*device, "vkCreateCuFunctionNVX"));
    data.present = reinterpret_cast<PFN_vkQueuePresentKHR>(nextDevice(*device, "vkQueuePresentKHR"));
    data.submit = reinterpret_cast<PFN_vkQueueSubmit>(nextDevice(*device, "vkQueueSubmit"));
    data.submit2 = reinterpret_cast<PFN_vkQueueSubmit2>(nextDevice(*device, "vkQueueSubmit2"));
    data.waitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(nextDevice(*device, "vkQueueWaitIdle"));
    data.creator = std::this_thread::get_id();
    std::lock_guard lock(g_mutex);
    g_devices[Key(*device)] = data;
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    DeviceData data;
    {
        std::lock_guard lock(g_mutex);
        data = g_devices[Key(device)];
        g_devices.erase(Key(device));
    }
    if (std::getenv("RETOQUE_COUNT_OFF_THREAD_QUEUE") != nullptr)
    {
        if (FILE* out = std::fopen("layer-offthread.txt", "a"))
        {
            std::fprintf(out, "submits %llu wait-idles %llu\n",
                         static_cast<unsigned long long>(g_offThreadSubmits.load()),
                         static_cast<unsigned long long>(g_offThreadWaitIdles.load()));
            std::fclose(out);
        }
    }
    if (data.destroy != nullptr)
        data.destroy(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkDeviceMemory* memory)
{
    const DeviceData data = DeviceOf(Key(device));
    SlowOffDeviceThread(data);
    return data.allocate(device, info, allocator, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo* infos,
                                                      const VkAllocationCallbacks* allocator, VkPipeline* pipelines)
{
    const DeviceData data = DeviceOf(Key(device));
    SlowOffDeviceThread(data);
    return data.pipelines(device, cache, count, infos, allocator, pipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCuModule(VkDevice device, const VkCuModuleCreateInfoNVX* info,
                                              const VkAllocationCallbacks* allocator, VkCuModuleNVX* module)
{
    const DeviceData data = DeviceOf(Key(device));
    SlowOffDeviceThread(data);
    return data.module(device, info, allocator, module);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateCuFunction(VkDevice device, const VkCuFunctionCreateInfoNVX* info,
                                                const VkAllocationCallbacks* allocator, VkCuFunctionNVX* function)
{
    const DeviceData data = DeviceOf(Key(device));
    SlowOffDeviceThread(data);
    return data.function(device, info, allocator, function);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    const DeviceData data = DeviceOf(Key(queue));
    if (CountOffThread(data))
        ++g_offThreadSubmits;
    return data.submit(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
    const DeviceData data = DeviceOf(Key(queue));
    if (CountOffThread(data))
        ++g_offThreadSubmits;
    return data.submit2(queue, count, submits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL QueueWaitIdle(VkQueue queue)
{
    const DeviceData data = DeviceOf(Key(queue));
    if (CountOffThread(data))
        ++g_offThreadWaitIdles;
    return data.waitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* info)
{
    const DeviceData data = DeviceOf(Key(queue));
    const uint64_t n = ++g_presents;
    if (const char* at = std::getenv("RETOQUE_STALL_PRESENT_AT"))
    {
        const uint64_t which = std::strtoull(at, nullptr, 10);
        const char* colon = std::strchr(at, ':');
        if (n == which && colon != nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::strtoull(colon + 1, nullptr, 10)));
    }
    return data.present(queue, info);
}

VKAPI_ATTR VkResult VKAPI_CALL SurfaceCapabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                   VkSurfaceCapabilitiesKHR* capabilities)
{
    const VkResult result = InstanceOf(Key(physical)).caps(physical, surface, capabilities);
    if (result == VK_SUCCESS && capabilities != nullptr && HideTransferSrc())
        capabilities->supportedUsageFlags &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL SurfaceCapabilities2(VkPhysicalDevice physical,
                                                    const VkPhysicalDeviceSurfaceInfo2KHR* info,
                                                    VkSurfaceCapabilities2KHR* capabilities)
{
    const VkResult result = InstanceOf(Key(physical)).caps2(physical, info, capabilities);
    if (result == VK_SUCCESS && capabilities != nullptr && HideTransferSrc())
        capabilities->surfaceCapabilities.supportedUsageFlags &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return result;
}

// The device's own extensions (no layer name), less the hidden ones; the layer's own (a layer name) pass through.
VKAPI_ATTR VkResult VKAPI_CALL DeviceExtensions(VkPhysicalDevice physical, const char* layer, uint32_t* count,
                                                VkExtensionProperties* properties)
{
    const auto next = InstanceOf(Key(physical)).extensions;
    if (layer != nullptr || std::getenv("RETOQUE_HIDE_DEVICE_EXTENSIONS") == nullptr)
        return next(physical, layer, count, properties);

    uint32_t all = 0;
    VkResult result = next(physical, nullptr, &all, nullptr);
    if (result != VK_SUCCESS)
        return result;
    std::vector<VkExtensionProperties> list(all);
    result = next(physical, nullptr, &all, list.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;
    list.resize(all);
    std::vector<VkExtensionProperties> kept;
    for (const auto& extension : list)
        if (!Hidden(extension.extensionName))
            kept.push_back(extension);

    if (properties == nullptr)
    {
        *count = static_cast<uint32_t>(kept.size());
        return VK_SUCCESS;
    }
    const uint32_t n = *count < kept.size() ? *count : static_cast<uint32_t>(kept.size());
    for (uint32_t i = 0; i < n; ++i)
        properties[i] = kept[i];
    *count = n;
    return n < kept.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
} // namespace

extern "C"
{
    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name);

    __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
    vkGetInstanceProcAddr(VkInstance instance, const char* name)
    {
        if (!strcmp(name, "vkGetInstanceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
        if (!strcmp(name, "vkGetDeviceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
        if (!strcmp(name, "vkCreateInstance"))
            return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
        if (!strcmp(name, "vkDestroyInstance"))
            return reinterpret_cast<PFN_vkVoidFunction>(DestroyInstance);
        if (!strcmp(name, "vkCreateDevice"))
            return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
        if (!strcmp(name, "vkDestroyDevice"))
            return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(SurfaceCapabilities);
        if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"))
            return reinterpret_cast<PFN_vkVoidFunction>(SurfaceCapabilities2);
        if (!strcmp(name, "vkEnumerateDeviceExtensionProperties"))
            return reinterpret_cast<PFN_vkVoidFunction>(DeviceExtensions);
        if (instance == VK_NULL_HANDLE)
            return nullptr;
        const InstanceData data = InstanceOf(Key(instance));
        return data.next != nullptr ? data.next(instance, name) : nullptr;
    }

    __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
    vkGetDeviceProcAddr(VkDevice device, const char* name)
    {
        if (!strcmp(name, "vkGetDeviceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
        if (!strcmp(name, "vkDestroyDevice"))
            return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
        DeviceData data;
        {
            std::lock_guard lock(g_mutex);
            const auto found = g_devices.find(Key(device));
            if (found != g_devices.end())
                data = found->second;
        }
        // Only what the device offers: a function the next layer has none of stays absent.
        if (!strcmp(name, "vkAllocateMemory") && data.allocate != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(AllocateMemory);
        if (!strcmp(name, "vkCreateComputePipelines") && data.pipelines != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(CreateComputePipelines);
        if (!strcmp(name, "vkCreateCuModuleNVX") && data.module != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(CreateCuModule);
        if (!strcmp(name, "vkCreateCuFunctionNVX") && data.function != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(CreateCuFunction);
        if (!strcmp(name, "vkQueuePresentKHR") && data.present != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(QueuePresent);
        if (!strcmp(name, "vkQueueSubmit") && data.submit != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit);
        if ((!strcmp(name, "vkQueueSubmit2") || !strcmp(name, "vkQueueSubmit2KHR")) && data.submit2 != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2);
        if (!strcmp(name, "vkQueueWaitIdle") && data.waitIdle != nullptr)
            return reinterpret_cast<PFN_vkVoidFunction>(QueueWaitIdle);
        return data.next != nullptr ? data.next(device, name) : nullptr;
    }
}
