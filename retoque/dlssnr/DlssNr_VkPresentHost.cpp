#include "pch.h"

#include "DlssNr_VkPresentHost.h"
#include "DlssNr_VkPresentRegistry.h"

#include <Config.h>
#include <State.h>
#include <proxies/KernelBase_Proxy.h>

#include <vulkan/vulkan.h>

#include <detours/detours.h>

#include <intrin.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

// Stage 1 of docs/dlssnr/design/nr-vulkan-present-host.md: see DlssNr_VkPresentHost.h. Every hook below calls down to
// the trampoline of the target it was attached to, with the arguments it was given, and only then (or, for destroys,
// just before) updates the records. The only Vulkan calls the host makes of its own are three read-only queries, none
// of them hooked here: vkGetDeviceProcAddr, vkGetPhysicalDeviceQueueFamilyProperties and
// vkGetPhysicalDeviceSurfaceCapabilitiesKHR.

namespace
{
using namespace DlssNr::VkPresentHost;

// Distinct addresses one device-level function may have: one per ICD (or first layer) in the process.
constexpr size_t kTargets = 4;

std::mutex g_installMutex;  // the detour bookkeeping; held across Detours, never across a call into Vulkan
std::mutex g_registryMutex; // the records; never held across a call into Vulkan or the log
Registry g_registry;
bool g_installed = false; // under g_installMutex
HMODULE g_module = nullptr;

PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_getQueueFamilies = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_getSurfaceCapabilities = nullptr;

// vulkan-1.dll's exports, one address each.
PFN_vkCreateInstance o_CreateInstance = nullptr;
PFN_vkDestroyInstance o_DestroyInstance = nullptr;
PFN_vkEnumeratePhysicalDevices o_EnumeratePhysicalDevices = nullptr;
PFN_vkCreateDevice o_CreateDevice = nullptr;
PFN_vkCreateWin32SurfaceKHR o_CreateWin32Surface = nullptr;
PFN_vkDestroySurfaceKHR o_DestroySurface = nullptr;

// Device-level functions, by address: slot i of every hook calls next[i], the trampoline of target[i].
template <typename Pfn> struct Targets
{
    std::array<void*, kTargets> target {};
    std::array<Pfn, kTargets> next {};
    bool fullReported = false;
};

Targets<PFN_vkDestroyDevice> g_destroyDevice;
Targets<PFN_vkGetDeviceQueue> g_getQueue;
Targets<PFN_vkGetDeviceQueue2> g_getQueue2;
Targets<PFN_vkCreateSwapchainKHR> g_createSwapchain;
Targets<PFN_vkDestroySwapchainKHR> g_destroySwapchain;
Targets<PFN_vkGetSwapchainImagesKHR> g_getImages;
Targets<PFN_vkAcquireNextImageKHR> g_acquire;
Targets<PFN_vkAcquireNextImage2KHR> g_acquire2;
Targets<PFN_vkQueuePresentKHR> g_present;

template <typename T> Handle H(T handle) { return static_cast<Handle>(reinterpret_cast<uintptr_t>(handle)); }

// Objects OptiScaler creates for itself (its own Vulkan work, or a D3D device it is making).
bool Internal() { return ScopedSkipVulkanHooks::Active() || State::Instance().creatingD3DDevice; }

struct CallerInfo
{
    Caller caller = Caller::Unknown;
    Handle module = 0;
    std::string name;
};

// The module a return address lies in, classified. Cached per thread: DXVK and vkd3d-proton present from threads of
// their own, so the cache rarely changes.
const CallerInfo& WhoCalled(void* returnAddress)
{
    thread_local CallerInfo cached;
    thread_local HMODULE cachedModule = nullptr;
    thread_local bool valid = false;

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(returnAddress), &module);

    if (valid && module == cachedModule)
        return cached;

    valid = true;
    cachedModule = module;
    cached.module = H(module);
    cached.name.clear();

    if (module != nullptr)
    {
        char path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        std::string name(path, length);

        if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
            name.erase(0, slash + 1);

        for (char& c : name)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        cached.name = std::move(name);
    }

    cached.caller = ClassifyCaller(cached.name, module != nullptr && module == dllModule);
    return cached;
}

std::vector<QueueFamily> QueueFamilies(VkPhysicalDevice physical)
{
    std::vector<QueueFamily> families;

    if (g_getQueueFamilies == nullptr || physical == VK_NULL_HANDLE)
        return families;

    uint32_t count = 0;
    g_getQueueFamilies(physical, &count, nullptr);

    std::vector<VkQueueFamilyProperties> properties(count);
    g_getQueueFamilies(physical, &count, properties.data());

    for (uint32_t i = 0; i < count; ++i)
        families.push_back(QueueFamily { properties[i].queueFlags, properties[i].queueCount });

    return families;
}

void Log(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines)
        LOG_INFO("{}", line);
}

// --- Attaching ---------------------------------------------------------------------------------------------------

// Detours `address` into slot i of `set` unless it is there already. With g_installMutex held.
template <typename Pfn>
void AttachTarget(Targets<Pfn>& set, const std::array<void*, kTargets>& hooks, PFN_vkVoidFunction address,
                  const char* name)
{
    void* const target = reinterpret_cast<void*>(address);

    if (target == nullptr)
        return;

    for (size_t i = 0; i < kTargets; ++i)
    {
        if (set.target[i] == target)
            return;
    }

    for (size_t i = 0; i < kTargets; ++i)
    {
        if (set.target[i] != nullptr)
            continue;

        set.next[i] = reinterpret_cast<Pfn>(target);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&) set.next[i], hooks[i]);

        if (const LONG result = DetourTransactionCommit(); result != NO_ERROR)
        {
            set.next[i] = nullptr;
            LOG_WARN("DLSS-NR Vulkan present host: could not detour {} at {} ({}); not watched", name, target, result);
            return;
        }

        set.target[i] = target;
        LOG_DEBUG("DLSS-NR Vulkan present host: watching {} at {} (slot {})", name, target, i);
        return;
    }

    if (!set.fullReported)
    {
        set.fullReported = true;
        LOG_WARN("DLSS-NR Vulkan present host: {} has more than {} distinct addresses in this process; the one at {} "
                 "is not watched",
                 name, kTargets, target);
    }
}

// --- Device-level hooks, one instantiation per slot --------------------------------------------------------------

void OnDestroyDevice(PFN_vkDestroyDevice next, VkDevice device, const VkAllocationCallbacks* allocator)
{
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnDeviceDestroyed(H(device));
    }

    next(device, allocator);
}

void OnGetQueue(PFN_vkGetDeviceQueue next, VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    next(device, family, index, queue);

    if (queue != nullptr && *queue != VK_NULL_HANDLE)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnQueue(H(device), family, index, 0, H(*queue));
    }
}

void OnGetQueue2(PFN_vkGetDeviceQueue2 next, VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    next(device, info, queue);

    if (info != nullptr && queue != nullptr && *queue != VK_NULL_HANDLE)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnQueue(H(device), info->queueFamilyIndex, info->queueIndex, info->flags, H(*queue));
    }
}

void HookPresent(VkDevice device);

VkResult OnCreateSwapchain(PFN_vkCreateSwapchainKHR next, VkDevice device, const VkSwapchainCreateInfoKHR* info,
                           const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    // The create info goes down as the caller made it: this stage requests no usage.
    const VkResult result = next(device, info, allocator, swapchain);

    if (result != VK_SUCCESS || info == nullptr || swapchain == nullptr || *swapchain == VK_NULL_HANDLE)
        return result;

    SwapchainDesc desc;
    desc.surface = H(info->surface);
    desc.oldSwapchain = H(info->oldSwapchain);
    desc.format = static_cast<uint32_t>(info->imageFormat);
    desc.colorSpace = static_cast<uint32_t>(info->imageColorSpace);
    desc.width = info->imageExtent.width;
    desc.height = info->imageExtent.height;
    desc.arrayLayers = info->imageArrayLayers;
    desc.usage = info->imageUsage;
    desc.sharingMode = static_cast<uint32_t>(info->imageSharingMode);
    if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT && info->pQueueFamilyIndices != nullptr)
        desc.sharingFamilies.assign(info->pQueueFamilyIndices, info->pQueueFamilyIndices + info->queueFamilyIndexCount);
    desc.minImageCount = info->minImageCount;
    desc.presentMode = static_cast<uint32_t>(info->presentMode);
    desc.flags = info->flags;

    Handle physical = 0;
    {
        std::lock_guard lock(g_registryMutex);
        physical = g_registry.PhysicalOf(H(device));
    }

    // What stage 2 would be allowed to ask for: the surface's usages, a read-only query.
    if (physical != 0 && g_getSurfaceCapabilities != nullptr)
    {
        VkSurfaceCapabilitiesKHR capabilities {};
        if (g_getSurfaceCapabilities(reinterpret_cast<VkPhysicalDevice>(physical), info->surface, &capabilities) ==
            VK_SUCCESS)
        {
            desc.surfaceUsageKnown = true;
            desc.surfaceUsage = capabilities.supportedUsageFlags;
        }
    }

    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnSwapchainCreated(H(device), H(*swapchain), std::move(desc));
    }

    HookPresent(device);
    return result;
}

void OnDestroySwapchain(PFN_vkDestroySwapchainKHR next, VkDevice device, VkSwapchainKHR swapchain,
                        const VkAllocationCallbacks* allocator)
{
    if (swapchain != VK_NULL_HANDLE)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnSwapchainDestroyed(H(device), H(swapchain));
    }

    next(device, swapchain, allocator);
}

VkResult OnGetImages(PFN_vkGetSwapchainImagesKHR next, VkDevice device, VkSwapchainKHR swapchain, uint32_t* count,
                     VkImage* images)
{
    const VkResult result = next(device, swapchain, count, images);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count != nullptr)
    {
        std::vector<Handle> handles;
        if (images != nullptr)
        {
            handles.reserve(*count);
            for (uint32_t i = 0; i < *count; ++i)
                handles.push_back(H(images[i]));
        }

        std::lock_guard lock(g_registryMutex);
        g_registry.OnSwapchainImages(H(device), H(swapchain), *count, images != nullptr ? handles.data() : nullptr);
    }

    return result;
}

VkResult OnAcquire(PFN_vkAcquireNextImageKHR next, VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                   VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
    const VkResult result = next(device, swapchain, timeout, semaphore, fence, index);

    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && index != nullptr)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnAcquire(H(device), H(swapchain), *index);
    }

    return result;
}

VkResult OnAcquire2(PFN_vkAcquireNextImage2KHR next, VkDevice device, const VkAcquireNextImageInfoKHR* info,
                    uint32_t* index)
{
    const VkResult result = next(device, info, index);

    if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) && info != nullptr && index != nullptr)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnAcquire(H(device), H(info->swapchain), *index);
    }

    return result;
}

// The present callback. Records and maybe logs, then calls down with the caller's own present info, untouched. No
// Reflex, frame limiter, frozen-feature tick or upscaler timing: a translated D3D present already ran those at DXGI.
VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* info, void* returnAddress)
{
    if (info != nullptr)
    {
        const CallerInfo& who = WhoCalled(returnAddress);

        PresentCall call;
        call.queue = H(queue);
        call.waitSemaphores = info->waitSemaphoreCount;
        call.swapchainCount = info->swapchainCount;
        for (uint32_t i = 0; i < info->swapchainCount && i < kPresentSwapchains; ++i)
        {
            call.swapchains[i] = info->pSwapchains != nullptr ? H(info->pSwapchains[i]) : 0;
            call.imageIndices[i] = info->pImageIndices != nullptr ? info->pImageIndices[i] : UINT32_MAX;
        }
        for (auto chain = static_cast<const VkBaseInStructure*>(info->pNext); chain != nullptr; chain = chain->pNext)
        {
            if (call.chainLength < kPresentChain)
                call.chainTypes[call.chainLength] = static_cast<uint32_t>(chain->sType);
            ++call.chainLength;
        }
        call.caller = who.caller;
        call.callerModule = who.module;
        call.callerName = who.name;

        std::vector<std::string> lines;
        {
            std::lock_guard lock(g_registryMutex);
            lines = g_registry.OnPresent(call);
        }

        Log(lines);
    }

    return next(queue, info);
}

template <size_t S> void VKAPI_CALL hkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    OnDestroyDevice(g_destroyDevice.next[S], device, allocator);
}

template <size_t S> void VKAPI_CALL hkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    OnGetQueue(g_getQueue.next[S], device, family, index, queue);
}

template <size_t S> void VKAPI_CALL hkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    OnGetQueue2(g_getQueue2.next[S], device, info, queue);
}

template <size_t S>
VkResult VKAPI_CALL hkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                         const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    return OnCreateSwapchain(g_createSwapchain.next[S], device, info, allocator, swapchain);
}

template <size_t S>
void VKAPI_CALL hkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator)
{
    OnDestroySwapchain(g_destroySwapchain.next[S], device, swapchain, allocator);
}

template <size_t S>
VkResult VKAPI_CALL hkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count, VkImage* images)
{
    return OnGetImages(g_getImages.next[S], device, swapchain, count, images);
}

template <size_t S>
VkResult VKAPI_CALL hkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                          VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
    return OnAcquire(g_acquire.next[S], device, swapchain, timeout, semaphore, fence, index);
}

template <size_t S>
VkResult VKAPI_CALL hkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info, uint32_t* index)
{
    return OnAcquire2(g_acquire2.next[S], device, info, index);
}

// _ReturnAddress here, in the function the target jumps to: the caller of vkQueuePresentKHR.
template <size_t S> VkResult VKAPI_CALL hkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
{
    return OnPresent(g_present.next[S], queue, info, _ReturnAddress());
}

static_assert(kTargets == 4, "the slot tables below list four instantiations");
#define DLSSNR_VK_SLOTS(hook)                                                                                          \
    std::array<void*, kTargets> { (void*) &hook<0>, (void*) &hook<1>, (void*) &hook<2>, (void*) &hook<3> }

const std::array<void*, kTargets> kDestroyDeviceHooks = DLSSNR_VK_SLOTS(hkDestroyDevice);
const std::array<void*, kTargets> kGetQueueHooks = DLSSNR_VK_SLOTS(hkGetDeviceQueue);
const std::array<void*, kTargets> kGetQueue2Hooks = DLSSNR_VK_SLOTS(hkGetDeviceQueue2);
const std::array<void*, kTargets> kCreateSwapchainHooks = DLSSNR_VK_SLOTS(hkCreateSwapchainKHR);
const std::array<void*, kTargets> kDestroySwapchainHooks = DLSSNR_VK_SLOTS(hkDestroySwapchainKHR);
const std::array<void*, kTargets> kGetImagesHooks = DLSSNR_VK_SLOTS(hkGetSwapchainImagesKHR);
const std::array<void*, kTargets> kAcquireHooks = DLSSNR_VK_SLOTS(hkAcquireNextImageKHR);
const std::array<void*, kTargets> kAcquire2Hooks = DLSSNR_VK_SLOTS(hkAcquireNextImage2KHR);
const std::array<void*, kTargets> kPresentHooks = DLSSNR_VK_SLOTS(hkQueuePresentKHR);

#undef DLSSNR_VK_SLOTS

// Right after vkCreateDevice returned: the device's own addresses for what the records follow. Resolved through
// vkGetDeviceProcAddr, so a function the application reaches through an export or through a returned pointer is the
// same target.
void HookDevice(VkDevice device)
{
    if (g_getDeviceProcAddr == nullptr)
        return;

    const auto get = [device](const char* name) { return g_getDeviceProcAddr(device, name); };

    const PFN_vkVoidFunction destroyDevice = get("vkDestroyDevice");
    const PFN_vkVoidFunction getQueue = get("vkGetDeviceQueue");
    const PFN_vkVoidFunction getQueue2 = get("vkGetDeviceQueue2");
    const PFN_vkVoidFunction createSwapchain = get("vkCreateSwapchainKHR");
    const PFN_vkVoidFunction destroySwapchain = get("vkDestroySwapchainKHR");
    const PFN_vkVoidFunction getImages = get("vkGetSwapchainImagesKHR");
    const PFN_vkVoidFunction acquire = get("vkAcquireNextImageKHR");
    const PFN_vkVoidFunction acquire2 = get("vkAcquireNextImage2KHR");

    std::lock_guard lock(g_installMutex);
    AttachTarget(g_destroyDevice, kDestroyDeviceHooks, destroyDevice, "vkDestroyDevice");
    AttachTarget(g_getQueue, kGetQueueHooks, getQueue, "vkGetDeviceQueue");
    AttachTarget(g_getQueue2, kGetQueue2Hooks, getQueue2, "vkGetDeviceQueue2");
    AttachTarget(g_createSwapchain, kCreateSwapchainHooks, createSwapchain, "vkCreateSwapchainKHR");
    AttachTarget(g_destroySwapchain, kDestroySwapchainHooks, destroySwapchain, "vkDestroySwapchainKHR");
    AttachTarget(g_getImages, kGetImagesHooks, getImages, "vkGetSwapchainImagesKHR");
    AttachTarget(g_acquire, kAcquireHooks, acquire, "vkAcquireNextImageKHR");
    AttachTarget(g_acquire2, kAcquire2Hooks, acquire2, "vkAcquireNextImage2KHR");
}

// At a device's swapchain creation, not at its creation: by then the bundle's own present hook, which it installs in
// its vkCreateDevice, is in place, so this detour goes on top of it and runs first.
void HookPresent(VkDevice device)
{
    if (g_getDeviceProcAddr == nullptr)
        return;

    const PFN_vkVoidFunction present = g_getDeviceProcAddr(device, "vkQueuePresentKHR");

    std::lock_guard lock(g_installMutex);
    AttachTarget(g_present, kPresentHooks, present, "vkQueuePresentKHR");
}

// --- Instance-level hooks, on vulkan-1.dll's exports ------------------------------------------------------------

VkResult VKAPI_CALL hkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                     VkInstance* instance)
{
    void* const returnAddress = _ReturnAddress();
    const bool internal = Internal();
    const VkResult result = o_CreateInstance(info, allocator, instance);

    if (result == VK_SUCCESS && instance != nullptr && *instance != VK_NULL_HANDLE)
    {
        const CallerInfo& who = WhoCalled(returnAddress);
        RecordId id = 0;
        {
            std::lock_guard lock(g_registryMutex);
            id = g_registry.OnInstanceCreated(H(*instance), internal);
        }

        LOG_INFO("DLSS-NR Vulkan present host: instance {} #{} created by {} ({}){}", Hex(H(*instance)), id,
                 who.name.empty() ? "?" : who.name, CallerName(who.caller), internal ? "; OptiScaler's own" : "");
    }

    return result;
}

void VKAPI_CALL hkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnInstanceDestroyed(H(instance));
    }

    o_DestroyInstance(instance, allocator);
}

VkResult VKAPI_CALL hkEnumeratePhysicalDevices(VkInstance instance, uint32_t* count, VkPhysicalDevice* devices)
{
    const VkResult result = o_EnumeratePhysicalDevices(instance, count, devices);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count != nullptr && devices != nullptr)
    {
        std::vector<Handle> handles;
        handles.reserve(*count);
        for (uint32_t i = 0; i < *count; ++i)
            handles.push_back(H(devices[i]));

        std::lock_guard lock(g_registryMutex);
        g_registry.OnPhysicalDevices(H(instance), handles.data(), static_cast<uint32_t>(handles.size()));
    }

    return result;
}

VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                   const VkAllocationCallbacks* allocator, VkDevice* device)
{
    void* const returnAddress = _ReturnAddress();
    const bool internal = Internal();

    // The create info goes down as the caller made it: this stage adds no extension, feature or queue.
    const VkResult result = o_CreateDevice(physical, info, allocator, device);

    if (result != VK_SUCCESS || device == nullptr || *device == VK_NULL_HANDLE)
        return result;

    DeviceDesc desc;
    desc.physical = H(physical);
    desc.internal = internal;
    desc.families = QueueFamilies(physical);

    if (info != nullptr && info->pQueueCreateInfos != nullptr)
    {
        for (uint32_t i = 0; i < info->queueCreateInfoCount; ++i)
        {
            const VkDeviceQueueCreateInfo& queue = info->pQueueCreateInfos[i];
            desc.requested.push_back(QueueRequest { queue.queueFamilyIndex, queue.queueCount, queue.flags });
        }
    }

    const CallerInfo& who = WhoCalled(returnAddress);
    desc.creator = who.caller;
    desc.creatorModule = who.name;

    std::string line;
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnDeviceCreated(H(*device), std::move(desc));
        line = g_registry.DeviceLine(H(*device));
    }

    LOG_INFO("{}", line);

    HookDevice(*device);
    return result;
}

VkResult VKAPI_CALL hkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
                                            const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface)
{
    const VkResult result = o_CreateWin32Surface(instance, info, allocator, surface);

    if (result == VK_SUCCESS && info != nullptr && surface != nullptr && *surface != VK_NULL_HANDLE)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnSurfaceCreated(H(instance), H(*surface), H(info->hwnd));
    }

    return result;
}

void VKAPI_CALL hkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
{
    if (surface != VK_NULL_HANDLE)
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnSurfaceDestroyed(H(instance), H(surface));
    }

    o_DestroySurface(instance, surface, allocator);
}

template <typename Pfn> void Attach(Pfn& original, void* hook)
{
    if (original != nullptr)
        DetourAttach(&(PVOID&) original, hook);
}
} // namespace

void DlssNr::VkPresentHost::Install(HMODULE vulkan1)
{
    // Off -- the default -- ends here, before any lookup, detour or record (tests/nr-vk-present-host holds this).
    if (vulkan1 == nullptr || !Config::Instance()->DlssNrEnabled.value_or_default() ||
        !Config::Instance()->DlssNrVulkanPresentHost.value_or_default())
        return;

    std::lock_guard lock(g_installMutex);

    if (g_installed)
    {
        // The loader hook hands vulkan-1.dll to VulkanHooks::Hook again on a later load; once is enough to say so.
        static bool reported = false;
        if (!reported)
        {
            reported = true;
            LOG_INFO("DLSS-NR Vulkan present host: vulkan-1.dll reached the hook again ({}); already installed",
                     vulkan1 == g_module ? "the same module" : "a different module, not watched");
        }

        return;
    }

    const auto get = [vulkan1](const char* name) { return KernelBaseProxy::GetProcAddress_()(vulkan1, name); };

    o_CreateInstance = reinterpret_cast<PFN_vkCreateInstance>(get("vkCreateInstance"));
    o_DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(get("vkDestroyInstance"));
    o_EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get("vkEnumeratePhysicalDevices"));
    o_CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(get("vkCreateDevice"));
    o_CreateWin32Surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get("vkCreateWin32SurfaceKHR"));
    o_DestroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(get("vkDestroySurfaceKHR"));
    g_getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get("vkGetDeviceProcAddr"));
    g_getQueueFamilies =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get("vkGetPhysicalDeviceQueueFamilyProperties"));
    g_getSurfaceCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        get("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));

    const auto reset = []()
    {
        o_CreateInstance = nullptr;
        o_DestroyInstance = nullptr;
        o_EnumeratePhysicalDevices = nullptr;
        o_CreateDevice = nullptr;
        o_CreateWin32Surface = nullptr;
        o_DestroySurface = nullptr;
        g_getDeviceProcAddr = nullptr;
        g_getQueueFamilies = nullptr;
        g_getSurfaceCapabilities = nullptr;
    };

    if (o_CreateDevice == nullptr || o_DestroyInstance == nullptr || g_getDeviceProcAddr == nullptr)
    {
        LOG_WARN("DLSS-NR Vulkan present host: vulkan-1.dll does not export vkCreateDevice, vkDestroyInstance or "
                 "vkGetDeviceProcAddr; not installed");
        reset();
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    Attach(o_CreateInstance, (void*) hkCreateInstance);
    Attach(o_DestroyInstance, (void*) hkDestroyInstance);
    Attach(o_EnumeratePhysicalDevices, (void*) hkEnumeratePhysicalDevices);
    Attach(o_CreateDevice, (void*) hkCreateDevice);
    Attach(o_CreateWin32Surface, (void*) hkCreateWin32SurfaceKHR);
    Attach(o_DestroySurface, (void*) hkDestroySurfaceKHR);

    if (const LONG result = DetourTransactionCommit(); result != NO_ERROR)
    {
        LOG_WARN("DLSS-NR Vulkan present host: could not detour vulkan-1.dll's exports ({}); not installed", result);
        reset();
        return;
    }

    g_installed = true;
    g_module = vulkan1;

    LOG_INFO("DLSS-NR Vulkan present host (observe only; stage 1 of nr-vulkan-present-host.md): installed on "
             "vulkan-1.dll, {}",
             State::Instance().vulkanHooksSkipped
                 ? "while the rest of OptiScaler's Vulkan hooks stay off ([Vulkan] SkipHooks or the marker); this "
                   "host is the one exception"
                 : "beside OptiScaler's own Vulkan hooks");
}
