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
// Everything else passes through. Built by run.py with the host's g++ against external/vulkan's headers.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
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
    if (data.destroy != nullptr)
        data.destroy(device, allocator);
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
        return data.next != nullptr ? data.next(device, name) : nullptr;
    }
}
