#include "pch.h"

#include "DlssNr_VkPresentHost.h"
#include "DlssNr_VkPresentRegistry.h"
#include "DlssNr_VkPresentTransport.h"

#include <Config.h>
#include <State.h>
#include <hooks/Vulkan_Hooks.h>
#include <proxies/KernelBase_Proxy.h>

#include <vulkan/vulkan.h>

#include <detours/detours.h>

#include <intrin.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

// Stages 1 and 2 of docs/dlssnr/design/nr-vulkan-present-host.md: see DlssNr_VkPresentHost.h. Every hook below calls
// down to the trampoline of the target it was attached to, once. Stage 1's hooks hand their own arguments down; stage
// 2 changes exactly two things on the way: the image usage of an eligible swapchain (TRANSFER_SRC/TRANSFER_DST added,
// decided before the call), and the wait semaphores of a transported present (the application's are waited on by the
// round trip, and the present waits on the host's semaphore instead; everything else of VkPresentInfoKHR, its pNext
// chain included, goes down as it came).
//
// Locks. Each queue the application retrieves has two (g_queueLocks): a submit lock, which the queue-call hooks
// (submit, bind sparse, wait idle) take around their call down, and a present lock, which the present hook takes for
// the whole of a present on that queue, the call down included. Taken in this order and never the other way round:
//   1. the presenting queue's present lock (blocking, first thing in the present hook);
//   2. g_transportMutex: the transport's objects, held across the round trip's own Vulkan calls and fence waits;
//   3. a borrowed queue's present lock, then its submit lock, both try-locked with a bound, only around the round
//      trip's submits, after its recording;
//   4. g_registryMutex: the records, never held across a call into Vulkan or the log.
// The two kinds are kept apart because the legacy Vulkan overlay below the present hook submits to a graphics queue
// (through the submit hook) while holding a lock of its own that another thread's present on that queue waits for:
// a single lock per queue, held across that present's call down, would close the cycle. The round trip's own submits
// go through the host's trampoline, below every hook on vkQueueSubmit (the bundle's takes a queue lock of its own),
// and its other device functions are resolved below OptiScaler's hooks (VulkanHooks::GetDeviceProcAddr).

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
std::unordered_set<void*> g_attached; // every address detoured here, whatever function it was taken for

PFN_vkGetDeviceProcAddr g_getDeviceProcAddr = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties g_getQueueFamilies = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_getSurfaceCapabilities = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_getMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceImageFormatProperties g_getImageFormatProperties = nullptr;

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
Targets<PFN_vkQueueSubmit> g_submit;
Targets<PFN_vkQueueSubmit2> g_submit2; // vkQueueSubmit2 and vkQueueSubmit2KHR: one signature
Targets<PFN_vkQueueBindSparse> g_bindSparse;
Targets<PFN_vkQueueWaitIdle> g_waitIdle;

template <typename T> Handle H(T handle) { return static_cast<Handle>(reinterpret_cast<uintptr_t>(handle)); }
template <typename T> T As(Handle handle) { return reinterpret_cast<T>(static_cast<uintptr_t>(handle)); }

// Objects OptiScaler creates for itself: its own Vulkan work runs under ScopedSkipVulkanHooks (the D3D12 bridge, the
// GPU check's DXVK device, Anti-Lag 2, FFX). Not State::creatingD3DDevice, which the D3D hooks also set around the
// game's own D3D12CreateDevice, CreateDXGIFactory and D3D11 device creation: vkd3d-proton and DXVK make their instance
// and device inside those calls, and those are exactly the devices this host is for.
bool Internal() { return ScopedSkipVulkanHooks::Active(); }

bool InOwnModule(const void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    return module != nullptr && module == dllModule;
}

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

// --- Queue locks ------------------------------------------------------------------------------------------------
//
// vkQueueSubmit and its kind need the queue externally synchronised. Inside the application's present, the presenting
// queue is the application's to synchronise and it is in that call; a graphics queue the cross-family route borrows is
// not, and the application may be submitting to it from its render thread right then. So every queue the application
// retrieves gets a lock here, its submits, sparse binds and idle waits take it, and so does the round trip.

struct QueueLocks
{
    std::recursive_timed_mutex submit;  // the application's submits, sparse binds and idle waits; a borrower
    std::recursive_timed_mutex present; // the application's presents through the host; a borrower
};

struct QueueLockEntry
{
    VkDevice device = VK_NULL_HANDLE;
    std::unique_ptr<QueueLocks> locks;
};

std::shared_mutex g_queueLocksMutex;
std::unordered_map<VkQueue, QueueLockEntry> g_queueLocks;

QueueLocks* QueueLocksFor(VkQueue queue)
{
    std::shared_lock lock(g_queueLocksMutex);
    const auto found = g_queueLocks.find(queue);
    return found != g_queueLocks.end() ? found->second.locks.get() : nullptr;
}

void AddQueueLock(VkDevice device, VkQueue queue)
{
    std::unique_lock lock(g_queueLocksMutex);
    auto& entry = g_queueLocks[queue];
    if (entry.locks == nullptr || entry.device != device)
    {
        entry.device = device;
        entry.locks = std::make_unique<QueueLocks>();
    }
}

void EraseQueueLocks(VkDevice device)
{
    std::unique_lock lock(g_queueLocksMutex);
    for (auto it = g_queueLocks.begin(); it != g_queueLocks.end();)
        it = it->second.device == device ? g_queueLocks.erase(it) : std::next(it);
}

template <typename Pfn, typename... Args> VkResult LockedQueueCall(Pfn next, VkQueue queue, Args... args)
{
    if (QueueLocks* locks = QueueLocksFor(queue))
    {
        std::lock_guard lock(locks->submit);
        return next(queue, args...);
    }

    return next(queue, args...);
}

// --- The transport's objects --------------------------------------------------------------------------------------

// The device functions the round trip calls, resolved once per device through vkGetDeviceProcAddr.
struct DeviceFns
{
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
};

// One frame of the round trip in flight. Its command buffers, semaphores and fence are reused only once the fence was
// seen signalled; the fence is on the last submit of the chain, which the chain orders after the others.
struct Slot
{
    SlotState state = SlotState::Idle;
    uint64_t serial = 0;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer work = VK_NULL_HANDLE; // from the work family's pool
    uint32_t workFamily = UINT32_MAX;
    VkCommandBuffer release = VK_NULL_HANDLE; // cross-family: from the presenting family's pool
    VkCommandBuffer acquire = VK_NULL_HANDLE;
    VkCommandBuffer recover = VK_NULL_HANDLE; // cross-family: takes the image back, contents discarded, if the work
                                              // queue's submit fails after the release ran
    uint32_t presentFamily = UINT32_MAX;
    VkSemaphore toWork = VK_NULL_HANDLE;    // cross-family: presenting queue -> work queue
    VkSemaphore toPresent = VK_NULL_HANDLE; // ... and back
};

struct SwapchainTransport
{
    bool on = false;
    std::string off; // why the round trip stopped after creation, when it did
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent {};

    // One work image per swapchain: every slot's round trip runs on one queue (a change of queue waits for the ring),
    // and each recording orders its first write after the previous recording's reads on that queue.
    VkImage work = VK_NULL_HANDLE;
    VkDeviceMemory workMemory = VK_NULL_HANDLE;
    VkQueue lastWorkQueue = VK_NULL_HANDLE;

    // Per swapchain image: the semaphore the present waits on. Reused only for a present of the same image, which needs
    // that image acquired again since, and the validation layer's own advice for exactly this ("a separate semaphore
    // per swapchain image, indexed by the acquired image"). `uncertain`: a present returned an error after which the
    // wait may not have run, so the semaphore is retained and replaced. `used`: handed to a present at least once.
    std::vector<VkSemaphore> presentSemaphores;
    std::vector<uint8_t> uncertain;
    std::vector<uint8_t> used;

    std::vector<Slot> ring;
    uint64_t nextSerial = 1;
};

// Objects whose GPU completion was not proven when their swapchain went: kept, and tried again at device destruction.
struct Leftover
{
    std::vector<Slot> slots;
    VkImage work = VK_NULL_HANDLE;
    VkDeviceMemory workMemory = VK_NULL_HANDLE;
};

struct DeviceTransport
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    RecordId id = 0;
    DeviceFns fn;
    VkPhysicalDeviceMemoryProperties memory {};
    bool lost = false;
    std::string off;     // the transport stopped on this device: why (new presents skip with DeviceOff)
    bool leaked = false; // a command buffer whose completion is unknown was left in a pool: never destroy the pools
    std::unordered_map<uint32_t, VkCommandPool> pools; // by family
    std::unordered_map<VkSwapchainKHR, SwapchainTransport> swapchains;
    std::vector<VkSemaphore> retained; // present semaphores no reacquire proved free, destroyed with the device
    std::vector<Leftover> leftovers;
};

std::mutex g_transportMutex;
std::unordered_map<VkDevice, std::unique_ptr<DeviceTransport>> g_transport;

// Below OptiScaler's own hooks: with the bundle installed, the vkGetDeviceProcAddr export is its detour and hands out
// its own functions (Vulkan_wDx12's vkQueueSubmit takes a queue lock of its own; its command-buffer functions track
// what they see). Without the bundle this is the export itself.
template <typename Pfn> bool Resolve(VkDevice device, const char* name, Pfn& out)
{
    out = reinterpret_cast<Pfn>(VulkanHooks::GetDeviceProcAddr(device, name));
    return out != nullptr;
}

std::unique_ptr<DeviceTransport> MakeDeviceTransport(VkDevice device, VkPhysicalDevice physical, RecordId id)
{
    auto dev = std::make_unique<DeviceTransport>();
    dev->device = device;
    dev->physical = physical;
    dev->id = id;

    DeviceFns& f = dev->fn;
    bool ok = true;
    ok = ok && Resolve(device, "vkCreateCommandPool", f.createCommandPool);
    ok = ok && Resolve(device, "vkDestroyCommandPool", f.destroyCommandPool);
    ok = ok && Resolve(device, "vkAllocateCommandBuffers", f.allocateCommandBuffers);
    ok = ok && Resolve(device, "vkFreeCommandBuffers", f.freeCommandBuffers);
    ok = ok && Resolve(device, "vkResetCommandBuffer", f.resetCommandBuffer);
    ok = ok && Resolve(device, "vkBeginCommandBuffer", f.beginCommandBuffer);
    ok = ok && Resolve(device, "vkEndCommandBuffer", f.endCommandBuffer);
    ok = ok && Resolve(device, "vkCmdPipelineBarrier", f.cmdPipelineBarrier);
    ok = ok && Resolve(device, "vkCmdCopyImage", f.cmdCopyImage);
    ok = ok && Resolve(device, "vkQueueSubmit", f.queueSubmit);
    ok = ok && Resolve(device, "vkCreateFence", f.createFence);
    ok = ok && Resolve(device, "vkDestroyFence", f.destroyFence);
    ok = ok && Resolve(device, "vkResetFences", f.resetFences);
    ok = ok && Resolve(device, "vkGetFenceStatus", f.getFenceStatus);
    ok = ok && Resolve(device, "vkWaitForFences", f.waitForFences);
    ok = ok && Resolve(device, "vkCreateSemaphore", f.createSemaphore);
    ok = ok && Resolve(device, "vkDestroySemaphore", f.destroySemaphore);
    ok = ok && Resolve(device, "vkCreateImage", f.createImage);
    ok = ok && Resolve(device, "vkDestroyImage", f.destroyImage);
    ok = ok && Resolve(device, "vkGetImageMemoryRequirements", f.getImageMemoryRequirements);
    ok = ok && Resolve(device, "vkAllocateMemory", f.allocateMemory);
    ok = ok && Resolve(device, "vkFreeMemory", f.freeMemory);
    ok = ok && Resolve(device, "vkBindImageMemory", f.bindImageMemory);

    if (!ok)
        dev->off = "a device function the round trip needs did not resolve";
    else if (g_getMemoryProperties == nullptr || g_getImageFormatProperties == nullptr)
        dev->off = "vulkan-1.dll lacks vkGetPhysicalDeviceMemoryProperties or vkGetPhysicalDeviceImageFormatProperties";
    else
        g_getMemoryProperties(physical, &dev->memory);

    return dev;
}

DeviceTransport* FindDevice(VkDevice device)
{
    const auto found = g_transport.find(device);
    return found != g_transport.end() ? found->second.get() : nullptr;
}

// After a VK_ERROR_DEVICE_LOST anywhere: nothing of the device's is waited on, reused or destroyed again.
void MarkLost(DeviceTransport& dev)
{
    if (!dev.lost)
    {
        dev.lost = true;
        dev.off = "the device was lost";
    }
}

VkCommandPool PoolFor(DeviceTransport& dev, uint32_t family)
{
    if (const auto found = dev.pools.find(family); found != dev.pools.end())
        return found->second;

    VkCommandPoolCreateInfo info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = family;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (dev.fn.createCommandPool(dev.device, &info, nullptr, &pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    dev.pools[family] = pool;
    return pool;
}

VkCommandBuffer AllocateCommandBuffer(DeviceTransport& dev, uint32_t family)
{
    const VkCommandPool pool = PoolFor(dev, family);
    if (pool == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    VkCommandBufferAllocateInfo info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    VkCommandBuffer buffer = VK_NULL_HANDLE;
    return dev.fn.allocateCommandBuffers(dev.device, &info, &buffer) == VK_SUCCESS ? buffer : VK_NULL_HANDLE;
}

void FreeCommandBuffer(DeviceTransport& dev, uint32_t family, VkCommandBuffer& buffer)
{
    if (buffer != VK_NULL_HANDLE)
    {
        if (const auto found = dev.pools.find(family); found != dev.pools.end())
            dev.fn.freeCommandBuffers(dev.device, found->second, 1, &buffer);
    }

    buffer = VK_NULL_HANDLE;
}

VkSemaphore MakeSemaphore(DeviceTransport& dev)
{
    VkSemaphoreCreateInfo info { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    return dev.fn.createSemaphore(dev.device, &info, nullptr, &semaphore) == VK_SUCCESS ? semaphore : VK_NULL_HANDLE;
}

// Only for a slot whose completion was observed (or that never ran).
void DestroySlot(DeviceTransport& dev, Slot& slot)
{
    FreeCommandBuffer(dev, slot.workFamily, slot.work);
    FreeCommandBuffer(dev, slot.presentFamily, slot.release);
    FreeCommandBuffer(dev, slot.presentFamily, slot.acquire);
    FreeCommandBuffer(dev, slot.presentFamily, slot.recover);

    if (slot.fence != VK_NULL_HANDLE)
        dev.fn.destroyFence(dev.device, slot.fence, nullptr);
    if (slot.toWork != VK_NULL_HANDLE)
        dev.fn.destroySemaphore(dev.device, slot.toWork, nullptr);
    if (slot.toPresent != VK_NULL_HANDLE)
        dev.fn.destroySemaphore(dev.device, slot.toPresent, nullptr);

    slot = Slot {};
}

void DestroyWorkImage(DeviceTransport& dev, VkImage& image, VkDeviceMemory& memory)
{
    if (image != VK_NULL_HANDLE)
        dev.fn.destroyImage(dev.device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        dev.fn.freeMemory(dev.device, memory, nullptr);

    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

uint64_t Since(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

// Every pending slot whose fence is signalled becomes idle. Returns the completions observed.
uint32_t Poll(DeviceTransport& dev, SwapchainTransport& sc)
{
    uint32_t completions = 0;

    for (Slot& slot : sc.ring)
    {
        if (slot.state != SlotState::Pending || dev.lost)
            continue;

        const VkResult status = dev.fn.getFenceStatus(dev.device, slot.fence);
        if (status == VK_SUCCESS)
        {
            slot.state = SlotState::Idle;
            ++completions;
        }
        else if (status == VK_ERROR_DEVICE_LOST)
        {
            MarkLost(dev);
        }
    }

    return completions;
}

// A bounded wait for one pending slot. True when it completed; the time spent is added to `waitNs`.
bool WaitSlot(DeviceTransport& dev, Slot& slot, uint64_t timeoutNs, uint64_t& waitNs, uint32_t& completions)
{
    if (slot.state != SlotState::Pending || dev.lost)
        return slot.state == SlotState::Idle;

    const auto start = std::chrono::steady_clock::now();
    const VkResult result = dev.fn.waitForFences(dev.device, 1, &slot.fence, VK_TRUE, timeoutNs);
    waitNs += Since(start);

    if (result == VK_SUCCESS)
    {
        slot.state = SlotState::Idle;
        ++completions;
        return true;
    }

    if (result == VK_ERROR_DEVICE_LOST)
        MarkLost(dev);

    return false;
}

bool EnsureWorkImage(DeviceTransport& dev, SwapchainTransport& sc)
{
    if (sc.work != VK_NULL_HANDLE)
        return true;

    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = sc.format;
    info.extent = { sc.extent.width, sc.extent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dev.fn.createImage(dev.device, &info, nullptr, &sc.work) != VK_SUCCESS)
    {
        sc.work = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements {};
    dev.fn.getImageMemoryRequirements(dev.device, sc.work, &requirements);

    uint32_t type = UINT32_MAX;
    for (const VkMemoryPropertyFlags wanted : { VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0u })
    {
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (dev.memory.memoryTypes[i].propertyFlags & wanted) == wanted)
                type = i;
        }
    }

    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;

    if (type == UINT32_MAX || dev.fn.allocateMemory(dev.device, &allocate, nullptr, &sc.workMemory) != VK_SUCCESS ||
        dev.fn.bindImageMemory(dev.device, sc.work, sc.workMemory, 0) != VK_SUCCESS)
    {
        DestroyWorkImage(dev, sc.work, sc.workMemory);
        return false;
    }

    return true;
}

// The semaphore image `index`'s present will wait on. A semaphore after an uncertain present is retained, not reused.
VkSemaphore PresentSemaphore(DeviceTransport& dev, SwapchainTransport& sc, uint32_t index, uint32_t imageCount)
{
    if (sc.presentSemaphores.size() < imageCount)
    {
        sc.presentSemaphores.resize(imageCount, VK_NULL_HANDLE);
        sc.uncertain.resize(imageCount, 0);
        sc.used.resize(imageCount, 0);
    }

    if (sc.uncertain[index] != 0 && sc.presentSemaphores[index] != VK_NULL_HANDLE)
    {
        dev.retained.push_back(sc.presentSemaphores[index]);
        sc.presentSemaphores[index] = VK_NULL_HANDLE;

        if (dev.retained.size() > kRetainedMax && dev.off.empty())
            dev.off = "more than " + std::to_string(kRetainedMax) + " present semaphores retained";
    }

    sc.uncertain[index] = 0;

    if (sc.presentSemaphores[index] == VK_NULL_HANDLE)
    {
        sc.presentSemaphores[index] = MakeSemaphore(dev);
        sc.used[index] = 0;
    }

    return sc.presentSemaphores[index];
}

// The slot's command buffers for this route's families, and its chain semaphores when the route needs them.
bool EnsureSlotObjects(DeviceTransport& dev, Slot& slot, Route route, uint32_t workFamily, uint32_t presentFamily)
{
    if (slot.fence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (dev.fn.createFence(dev.device, &info, nullptr, &slot.fence) != VK_SUCCESS)
        {
            slot.fence = VK_NULL_HANDLE;
            return false;
        }
    }

    if (slot.workFamily != workFamily)
    {
        FreeCommandBuffer(dev, slot.workFamily, slot.work);
        slot.work = AllocateCommandBuffer(dev, workFamily);
        slot.workFamily = workFamily;
    }

    if (slot.work == VK_NULL_HANDLE)
        return false;

    if (route != Route::CrossFamily)
        return true;

    if (slot.presentFamily != presentFamily)
    {
        FreeCommandBuffer(dev, slot.presentFamily, slot.release);
        FreeCommandBuffer(dev, slot.presentFamily, slot.acquire);
        FreeCommandBuffer(dev, slot.presentFamily, slot.recover);
        slot.release = AllocateCommandBuffer(dev, presentFamily);
        slot.acquire = AllocateCommandBuffer(dev, presentFamily);
        slot.recover = AllocateCommandBuffer(dev, presentFamily);
        slot.presentFamily = presentFamily;
    }

    if (slot.toWork == VK_NULL_HANDLE)
        slot.toWork = MakeSemaphore(dev);
    if (slot.toPresent == VK_NULL_HANDLE)
        slot.toPresent = MakeSemaphore(dev);

    return slot.release != VK_NULL_HANDLE && slot.acquire != VK_NULL_HANDLE && slot.recover != VK_NULL_HANDLE &&
           slot.toWork != VK_NULL_HANDLE && slot.toPresent != VK_NULL_HANDLE;
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                             VkAccessFlags dstAccess, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                             uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return barrier;
}

bool Begin(DeviceTransport& dev, VkCommandBuffer buffer)
{
    if (dev.fn.resetCommandBuffer(buffer, 0) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return dev.fn.beginCommandBuffer(buffer, &begin) == VK_SUCCESS;
}

// The round trip, on the work queue: the presented image into the work image and back, unchanged, the image left in
// PRESENT_SRC. On the cross-family route it starts with the acquire of the presenting family's release and ends with
// the release back. Everything is recorded before any application semaphore is consumed.
bool RecordWork(DeviceTransport& dev, VkCommandBuffer buffer, VkImage image, VkImage work, VkExtent2D extent,
                Route route, uint32_t presentFamily, uint32_t workFamily)
{
    if (!Begin(dev, buffer))
        return false;

    const bool cross = route == Route::CrossFamily;
    const uint32_t from = cross ? presentFamily : VK_QUEUE_FAMILY_IGNORED;
    const uint32_t to = cross ? workFamily : VK_QUEUE_FAMILY_IGNORED;

    // ALL_COMMANDS as the source: the waits on the application's semaphores (or, cross-family, on the chain's) use it
    // as their destination stage, so the transition is ordered after them. The work image's previous round trip on this
    // queue (its copy out) is in that scope too.
    const VkImageMemoryBarrier in[] = {
        Barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, from, to),
        Barrier(work, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                              0, nullptr, 2, in);

    VkImageCopy region {};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { extent.width, extent.height, 1 };

    dev.fn.cmdCopyImage(buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &region);

    // Stage 3 runs the model here, on the work image.

    const VkImageMemoryBarrier turn[] = {
        Barrier(work, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                              nullptr, 2, turn);

    dev.fn.cmdCopyImage(buffer, work, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &region);

    const VkImageMemoryBarrier out =
        Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, to, from);
    dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &out);

    return dev.fn.endCommandBuffer(buffer) == VK_SUCCESS;
}

// Cross-family, on the presenting queue: the release to the work family (layout PRESENT_SRC -> TRANSFER_SRC, which
// the work queue's acquire repeats), or the acquire back (TRANSFER_DST -> PRESENT_SRC, repeating the work queue's
// release).
bool RecordHandOver(DeviceTransport& dev, VkCommandBuffer buffer, VkImage image, bool release, uint32_t presentFamily,
                    uint32_t workFamily)
{
    if (!Begin(dev, buffer))
        return false;

    const VkImageMemoryBarrier barrier =
        release ? Barrier(image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0,
                          presentFamily, workFamily)
                : Barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
                          workFamily, presentFamily);
    dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &barrier);

    return dev.fn.endCommandBuffer(buffer) == VK_SUCCESS;
}

// Cross-family, on the presenting queue, used only when the work queue's submit failed after the release had run:
// nothing released the image back, so it is taken on this family with its contents discarded (UNDEFINED, no
// ownership transfer, which an EXCLUSIVE image allows when its contents are not needed) and put in PRESENT_SRC. That
// frame shows garbage; the chain stays valid.
bool RecordRecovery(DeviceTransport& dev, VkCommandBuffer buffer, VkImage image)
{
    if (!Begin(dev, buffer))
        return false;

    const VkImageMemoryBarrier barrier =
        Barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0);
    dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &barrier);

    return dev.fn.endCommandBuffer(buffer) == VK_SUCCESS;
}

// --- At present -------------------------------------------------------------------------------------------------

struct Prepared
{
    Outcome outcome;
    VkSemaphore wait = VK_NULL_HANDLE; // the present waits on this, instead of the application's semaphores
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t image = 0;
    std::vector<std::string> warnings; // logged after every lock is released
};

// The round trip for one present, or the reason there is none. Called with no lock held; returns with none held.
Prepared Prepare(VkQueue queue, const VkPresentInfoKHR& info, const PresentView* view)
{
    Prepared prep;

    if (view == nullptr || view->internal)
        return prep;

    prep.outcome.counted = true;
    prep.device = As<VkDevice>(view->device);
    prep.swapchain = info.pSwapchains[0];
    prep.image = view->imageIndex;

    const auto skip = [&prep](Skip reason) -> Prepared&
    {
        prep.outcome.skip = reason;
        return prep;
    };

    if (info.swapchainCount != 1)
        return skip(Skip::MultiSwapchain);
    if (!view->desc.transport)
        return skip(Skip::ObserveOnly);
    if (!view->imageKnown)
        return skip(Skip::ImageUnknown);
    if (!view->acquired)
        return skip(Skip::NoAcquire);
    if (info.waitSemaphoreCount > kMaxWaits)
        return skip(Skip::TooManyWaits);

    RouteQuery query;
    query.presentQueue = H(queue);
    query.presentFamily = view->presentFamily;
    query.families = view->families;
    query.sharingMode = view->desc.sharingMode;
    query.sharingFamilies = view->desc.sharingFamilies;
    query.queues = view->queues;
    const RoutePlan route = ChooseRoute(query);

    if (route.route == Route::None)
    {
        prep.outcome.note = route.why;
        return skip(Skip::NoRoute);
    }

    const VkQueue workQueue = As<VkQueue>(route.workQueue);
    std::lock_guard lock(g_transportMutex);

    DeviceTransport* dev = FindDevice(prep.device);
    if (dev == nullptr || !dev->off.empty())
        return skip(Skip::DeviceOff);

    const auto found = dev->swapchains.find(prep.swapchain);
    if (found == dev->swapchains.end() || !found->second.on)
        return skip(Skip::ObserveOnly);

    SwapchainTransport& sc = found->second;
    Outcome& out = prep.outcome;

    out.completions += Poll(*dev, sc);
    out.ring = static_cast<uint32_t>(sc.ring.size());
    if (!dev->off.empty())
        return skip(Skip::DeviceOff);

    // One work image: a round trip on another queue waits until the ring is drained.
    if (sc.lastWorkQueue != VK_NULL_HANDLE && sc.lastWorkQueue != workQueue)
    {
        for (Slot& slot : sc.ring)
        {
            if (slot.state == SlotState::Pending && !WaitSlot(*dev, slot, kSlotWaitNs, out.waitNs, out.completions))
                return skip(dev->off.empty() ? Skip::QueueChange : Skip::DeviceOff);
        }
    }

    std::vector<SlotState> states;
    std::vector<uint64_t> serials;
    for (const Slot& slot : sc.ring)
    {
        states.push_back(slot.state);
        serials.push_back(slot.serial);
    }

    const SlotChoice choice = ChooseSlot(states, serials, kRingMax);
    size_t index = choice.index;

    switch (choice.kind)
    {
    case SlotChoice::Use:
        break;
    case SlotChoice::Grow:
        sc.ring.emplace_back();
        index = sc.ring.size() - 1;
        break;
    case SlotChoice::WaitOldest:
        if (!WaitSlot(*dev, sc.ring[index], kSlotWaitNs, out.waitNs, out.completions))
            return skip(dev->off.empty() ? Skip::RingBusy : Skip::DeviceOff);
        break;
    default:
        return skip(Skip::RingBusy);
    }

    out.ring = static_cast<uint32_t>(sc.ring.size());
    Slot& slot = sc.ring[index];

    const uint32_t presentFamily = view->presentFamily;
    const VkImage image = As<VkImage>(view->image);
    const VkSemaphore signal = PresentSemaphore(*dev, sc, view->imageIndex, view->imageCount);

    if (!EnsureWorkImage(*dev, sc) || signal == VK_NULL_HANDLE ||
        !EnsureSlotObjects(*dev, slot, route.route, route.workFamily, presentFamily))
    {
        // Not retried every frame: a device short of memory now is short of it next frame too.
        sc.on = false;
        sc.off = "the work image, a command buffer, a fence or a semaphore could not be made";
        prep.warnings.push_back("DLSS-NR Vulkan present host: swapchain " + Hex(H(prep.swapchain)) + " #" +
                                std::to_string(view->swapchainId) + ": " + sc.off + "; round trip off for it");
        return skip(Skip::SetupFailed);
    }

    const bool cross = route.route == Route::CrossFamily;
    if (!RecordWork(*dev, slot.work, image, sc.work, sc.extent, route.route, presentFamily, route.workFamily) ||
        (cross && (!RecordHandOver(*dev, slot.release, image, true, presentFamily, route.workFamily) ||
                   !RecordHandOver(*dev, slot.acquire, image, false, presentFamily, route.workFamily))))
        return skip(Skip::RecordFailed);

    if (cross && !RecordRecovery(*dev, slot.recover, image))
        return skip(Skip::RecordFailed);

    // A borrowed queue (cross-family and concurrent routes) is not inside the application's present: another of its
    // threads may be submitting to it or presenting on it. Its present lock, then its submit lock, each with a bound,
    // and only now, so a ring wait above never holds them. The presenting queue is the application's own present, whose
    // present lock the hook already holds.
    std::unique_lock<std::recursive_timed_mutex> borrowedPresent;
    std::unique_lock<std::recursive_timed_mutex> borrowedSubmit;
    if (workQueue != queue)
    {
        if (QueueLocks* locks = QueueLocksFor(workQueue))
        {
            borrowedPresent = std::unique_lock(locks->present, std::defer_lock);
            borrowedSubmit = std::unique_lock(locks->submit, std::defer_lock);
            if (!borrowedPresent.try_lock_for(std::chrono::milliseconds(kQueueLockWaitMs)) ||
                !borrowedSubmit.try_lock_for(std::chrono::milliseconds(kQueueLockWaitMs)))
                return skip(Skip::QueueBusy);
        }
    }

    if (dev->fn.resetFences(dev->device, 1, &slot.fence) != VK_SUCCESS)
        return skip(Skip::SetupFailed);

    // From the first successful submit on, the application's semaphores are spent: the present must wait on the host's.
    std::array<VkPipelineStageFlags, kMaxWaits> stages;
    stages.fill(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    const VkPipelineStageFlags allCommands = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    const auto submit = [&](VkQueue target, uint32_t waits, const VkSemaphore* wait, const VkPipelineStageFlags* masks,
                            VkCommandBuffer buffer, VkSemaphore signalOut, VkFence fence)
    {
        VkSubmitInfo submitInfo { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.waitSemaphoreCount = waits;
        submitInfo.pWaitSemaphores = wait;
        submitInfo.pWaitDstStageMask = masks;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &buffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalOut;
        const VkResult result = dev->fn.queueSubmit(target, 1, &submitInfo, fence);
        if (result == VK_ERROR_DEVICE_LOST)
            MarkLost(*dev);
        return result;
    };

    VkSemaphore presentWait = VK_NULL_HANDLE;

    if (!cross)
    {
        // Same queue, or a CONCURRENT image's graphics queue: one submit, its fence covering all of it.
        const VkResult result = submit(workQueue, info.waitSemaphoreCount, info.pWaitSemaphores, stages.data(),
                                       slot.work, signal, slot.fence);
        if (result != VK_SUCCESS)
        {
            prep.warnings.push_back("DLSS-NR Vulkan present host: the round trip's submit returned " +
                                    std::to_string(result) + "; the present goes down as the application made it");
            return skip(Skip::SubmitFailed);
        }

        out.submits = 1;
        presentWait = signal;
        slot.state = SlotState::Pending;
    }
    else
    {
        // 1: the presenting queue waits on the application's semaphores and releases the image.
        const VkResult first = submit(queue, info.waitSemaphoreCount, info.pWaitSemaphores, stages.data(), slot.release,
                                      slot.toWork, VK_NULL_HANDLE);
        if (first != VK_SUCCESS)
        {
            prep.warnings.push_back("DLSS-NR Vulkan present host: the release submit returned " +
                                    std::to_string(first) + "; the present goes down as the application made it");
            return skip(Skip::SubmitFailed);
        }

        // 2: the graphics queue acquires it, does the round trip and releases it back.
        VkSemaphore chain = slot.toWork;
        const VkResult second = submit(workQueue, 1, &chain, &allCommands, slot.work, slot.toPresent, VK_NULL_HANDLE);
        if (second == VK_SUCCESS)
            chain = slot.toPresent;

        // 3: the presenting queue acquires it back and signals what the present waits on; the fence is here. Should
        // 2 have failed, nothing released the image back: the recovery takes it with its contents discarded instead.
        const VkCommandBuffer back = second == VK_SUCCESS ? slot.acquire : slot.recover;
        VkResult third = submit(queue, 1, &chain, &allCommands, back, signal, slot.fence);
        if (third != VK_SUCCESS && third != VK_ERROR_DEVICE_LOST)
            third = submit(queue, 1, &chain, &allCommands, back, signal, slot.fence);

        out.submits = 1 + (second == VK_SUCCESS ? 1 : 0) + (third == VK_SUCCESS ? 1 : 0);
        presentWait = third == VK_SUCCESS ? signal : chain;
        slot.state = third == VK_SUCCESS ? SlotState::Pending : SlotState::Abandoned;

        if (second != VK_SUCCESS || third != VK_SUCCESS)
        {
            // The chain still hands the present exactly one signalled semaphore to wait on; with 3 failed twice, the
            // image is left released to the work family (the device is likely going). Nothing more for this
            // swapchain.
            sc.on = false;
            sc.off = "a submit failed in the middle of the cross-family chain";
            if (third != VK_SUCCESS)
                dev->leaked = true;
            prep.warnings.push_back("DLSS-NR Vulkan present host: swapchain " + Hex(H(prep.swapchain)) + " #" +
                                    std::to_string(view->swapchainId) + ": cross-family submits returned " +
                                    std::to_string(second) + " and " + std::to_string(third) +
                                    "; the present waits on the last semaphore signalled; round trip off for it");
        }
    }

    slot.serial = sc.nextSerial++;
    sc.lastWorkQueue = workQueue;
    sc.used[view->imageIndex] = 1;

    out.transported = true;
    out.skip = Skip::None;
    out.route = route.route;
    out.waitsTaken = info.waitSemaphoreCount;
    prep.wait = presentWait;
    return prep;
}

// After the call down: a present that failed in a way that may have left its wait unexecuted makes the semaphore
// uncertain, and a lost device stops everything.
void AfterPresent(const Prepared& prep, VkResult result)
{
    if (!prep.outcome.transported)
        return;

    std::lock_guard lock(g_transportMutex);

    DeviceTransport* dev = FindDevice(prep.device);
    if (dev == nullptr)
        return;

    if (result == VK_ERROR_DEVICE_LOST)
    {
        MarkLost(*dev);
        return;
    }

    // These four leave the present's queue operations enqueued, so the wait runs (vkQueuePresentKHR).
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
        return;

    const auto found = dev->swapchains.find(prep.swapchain);
    if (found != dev->swapchains.end() && prep.image < found->second.uncertain.size())
        found->second.uncertain[prep.image] = 1;
}

// --- Retirement -------------------------------------------------------------------------------------------------

struct Retired
{
    uint64_t completions = 0;
    std::string text; // for the swapchain's last line
};

// The swapchain is going: drain its round trips (bounded), free what is proven done, keep the rest. `acquired`: per
// image, acquired again since its last present, which proves its present semaphore's wait ran.
Retired RetireSwapchain(DeviceTransport& dev, SwapchainTransport& sc, const std::vector<uint8_t>& acquired)
{
    Retired retired;

    if (dev.lost)
    {
        // Abandoned: no destroy can be issued against a lost device's objects.
        retired.text = "objects abandoned, the device was lost";
        return retired;
    }

    const auto start = std::chrono::steady_clock::now();
    bool drained = true;
    uint64_t waitNs = 0;
    uint32_t completions = 0;

    for (Slot& slot : sc.ring)
    {
        if (slot.state == SlotState::Abandoned)
        {
            drained = false;
            continue;
        }

        const uint64_t spent = Since(start);
        if (slot.state == SlotState::Pending &&
            (spent >= kDrainNs || !WaitSlot(dev, slot, kDrainNs - spent, waitNs, completions)))
            drained = false;
    }

    retired.completions = completions;

    if (dev.lost)
    {
        retired.text = "objects abandoned, the device was lost during the drain";
        return retired;
    }

    if (drained)
    {
        for (Slot& slot : sc.ring)
            DestroySlot(dev, slot);
        DestroyWorkImage(dev, sc.work, sc.workMemory);
        retired.text = "freed after a drain of " + Milliseconds(Since(start)) + " ms";
    }
    else
    {
        Leftover left;
        left.slots = std::move(sc.ring);
        left.work = sc.work;
        left.workMemory = sc.workMemory;
        dev.leftovers.push_back(std::move(left));
        retired.text = "the GPU did not finish within " + Milliseconds(kDrainNs) +
                       " ms; its objects are kept until the device goes";
    }

    sc.ring.clear();
    sc.work = VK_NULL_HANDLE;
    sc.workMemory = VK_NULL_HANDLE;

    size_t freed = 0;
    size_t kept = 0;
    for (size_t i = 0; i < sc.presentSemaphores.size(); ++i)
    {
        const VkSemaphore semaphore = sc.presentSemaphores[i];
        if (semaphore == VK_NULL_HANDLE)
            continue;

        const bool proven = sc.used[i] == 0 || (sc.uncertain[i] == 0 && i < acquired.size() && acquired[i] != 0);
        if (proven)
        {
            dev.fn.destroySemaphore(dev.device, semaphore, nullptr);
            ++freed;
        }
        else
        {
            dev.retained.push_back(semaphore);
            ++kept;
        }
    }

    sc.presentSemaphores.clear();
    retired.text += "; present semaphores " + std::to_string(freed) + " freed, " + std::to_string(kept) +
                    " kept until the device goes (no reacquire proved their wait ran)";

    if (dev.retained.size() > kRetainedMax && dev.off.empty())
        dev.off = "more than " + std::to_string(kRetainedMax) + " present semaphores retained";

    return retired;
}

// The device is going: whatever the application left, then everything the host kept. Returns the line.
std::string TeardownDevice(DeviceTransport& dev)
{
    size_t swapchains = 0;
    for (auto& [handle, sc] : dev.swapchains)
    {
        // At device destruction every present semaphore goes: the application has finished with the device.
        RetireSwapchain(dev, sc, std::vector<uint8_t>(sc.presentSemaphores.size(), 1));
        ++swapchains;
    }
    dev.swapchains.clear();

    if (dev.lost)
        return "the device was lost: every object of the host's on it is abandoned, none destroyed";

    size_t leftoverSlots = 0;
    size_t abandoned = 0;
    const auto start = std::chrono::steady_clock::now();
    uint64_t waitNs = 0;
    uint32_t completions = 0;

    for (Leftover& left : dev.leftovers)
    {
        bool done = true;
        for (Slot& slot : left.slots)
        {
            const uint64_t spent = Since(start);
            if (slot.state == SlotState::Abandoned ||
                (slot.state == SlotState::Pending &&
                 (spent >= kDrainNs || !WaitSlot(dev, slot, kDrainNs - spent, waitNs, completions))))
                done = false;
        }

        if (done && !dev.lost)
        {
            for (Slot& slot : left.slots)
                DestroySlot(dev, slot);
            DestroyWorkImage(dev, left.work, left.workMemory);
            leftoverSlots += left.slots.size();
        }
        else
        {
            dev.leaked = true;
            ++abandoned;
        }
    }
    dev.leftovers.clear();

    if (dev.lost)
        return "the device was lost during the last drain: every object of the host's on it is abandoned";

    for (const VkSemaphore semaphore : dev.retained)
        dev.fn.destroySemaphore(dev.device, semaphore, nullptr);
    const size_t retained = dev.retained.size();
    dev.retained.clear();

    if (!dev.leaked)
    {
        for (const auto& [family, pool] : dev.pools)
            dev.fn.destroyCommandPool(dev.device, pool, nullptr);
    }

    return "freed: " + std::to_string(swapchains) + " swapchain(s) the application left, " + std::to_string(retained) +
           " retained present semaphore(s), " + std::to_string(leftoverSlots) +
           " slot(s) kept from earlier swapchains" +
           (abandoned != 0 ? "; " + std::to_string(abandoned) + " set(s) abandoned, the GPU never finished them" : "") +
           (dev.leaked ? "; command pools kept: a command buffer in them never finished" : "");
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

    // vkQueueSubmit2 and vkQueueSubmit2KHR are often one function; once is enough.
    if (g_attached.count(target) != 0)
        return;

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
        g_attached.insert(target);
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
    std::string line;
    {
        std::lock_guard lock(g_transportMutex);
        if (DeviceTransport* dev = FindDevice(device))
        {
            line = "DLSS-NR Vulkan present host: device " + Hex(H(device)) + " #" + std::to_string(dev->id) +
                   " destroyed; " + TeardownDevice(*dev);
            g_transport.erase(device);
        }
    }

    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnDeviceDestroyed(H(device));
    }

    EraseQueueLocks(device);

    if (!line.empty())
        LOG_INFO("{}", line);

    next(device, allocator);
}

void OnGetQueue(PFN_vkGetDeviceQueue next, VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    next(device, family, index, queue);

    if (queue != nullptr && *queue != VK_NULL_HANDLE)
    {
        {
            std::lock_guard lock(g_registryMutex);
            g_registry.OnQueue(H(device), family, index, 0, H(*queue));
        }

        AddQueueLock(device, *queue);
    }
}

void OnGetQueue2(PFN_vkGetDeviceQueue2 next, VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue)
{
    next(device, info, queue);

    if (info != nullptr && queue != nullptr && *queue != VK_NULL_HANDLE)
    {
        {
            std::lock_guard lock(g_registryMutex);
            g_registry.OnQueue(H(device), info->queueFamilyIndex, info->queueIndex, info->flags, H(*queue));
        }

        AddQueueLock(device, *queue);
    }
}

void HookPresent(VkDevice device);

VkImageCreateFlags ImageFlagsOf(VkSwapchainCreateFlagsKHR flags)
{
    VkImageCreateFlags image = 0;
    if (flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR)
        image |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
    if (flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR)
        image |= VK_IMAGE_CREATE_PROTECTED_BIT;
    if (flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
        image |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    return image;
}

bool FormatAllows(VkPhysicalDevice physical, VkFormat format, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    VkImageFormatProperties properties {};
    return g_getImageFormatProperties != nullptr &&
           g_getImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags,
                                      &properties) == VK_SUCCESS;
}

VkResult OnCreateSwapchain(PFN_vkCreateSwapchainKHR next, VkDevice device, const VkSwapchainCreateInfoKHR* info,
                           const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
    if (info == nullptr || swapchain == nullptr)
        return next(device, info, allocator, swapchain);

    Handle physical = 0;
    bool internal = false;
    {
        std::lock_guard lock(g_registryMutex);
        physical = g_registry.PhysicalOf(H(device));
        const auto* record = g_registry.FindDevice(H(device));
        internal = record != nullptr && record->desc.internal;
    }

    UsageQuery query;
    query.asked = info->imageUsage;
    query.arrayLayers = info->imageArrayLayers;
    query.presentMode = static_cast<uint32_t>(info->presentMode);
    query.flags = info->flags;

    {
        std::lock_guard lock(g_transportMutex);
        const DeviceTransport* dev = FindDevice(device);
        query.deviceOn = dev != nullptr && dev->off.empty();
        query.deviceOff = internal         ? "OptiScaler's own device"
                          : dev == nullptr ? "a device the host did not see created"
                                           : dev->off;
    }

    // What the surface allows, and what the format allows: read-only queries, before the real call.
    const VkPhysicalDevice pd = As<VkPhysicalDevice>(physical);
    if (physical != 0 && g_getSurfaceCapabilities != nullptr)
    {
        VkSurfaceCapabilitiesKHR capabilities {};
        if (g_getSurfaceCapabilities(pd, info->surface, &capabilities) == VK_SUCCESS)
        {
            query.surfaceKnown = true;
            query.surfaceUsage = capabilities.supportedUsageFlags;
        }
    }

    if (physical != 0)
    {
        query.formatAllows = FormatAllows(pd, info->imageFormat, Wanted(info->imageUsage), ImageFlagsOf(info->flags));
        query.workAllows =
            FormatAllows(pd, info->imageFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
    }

    const UsagePlan plan = PlanUsage(query);

    // Only the usage changes, and only when the plan says the round trip can run: everything else of the create
    // info, its pNext chain and oldSwapchain included, goes down as the application made it.
    VkSwapchainCreateInfoKHR edited = *info;
    edited.imageUsage = plan.usage;
    const VkSwapchainCreateInfoKHR* down = plan.transport && plan.added != 0 ? &edited : info;

    const VkResult result = next(device, down, allocator, swapchain);

    if (result != VK_SUCCESS || *swapchain == VK_NULL_HANDLE)
        return result;

    SwapchainDesc desc;
    desc.surface = H(info->surface);
    desc.oldSwapchain = H(info->oldSwapchain);
    desc.format = static_cast<uint32_t>(info->imageFormat);
    desc.colorSpace = static_cast<uint32_t>(info->imageColorSpace);
    desc.width = info->imageExtent.width;
    desc.height = info->imageExtent.height;
    desc.arrayLayers = info->imageArrayLayers;
    desc.usage = down->imageUsage;
    desc.askedUsage = info->imageUsage;
    desc.sharingMode = static_cast<uint32_t>(info->imageSharingMode);
    if (info->imageSharingMode == VK_SHARING_MODE_CONCURRENT && info->pQueueFamilyIndices != nullptr)
        desc.sharingFamilies.assign(info->pQueueFamilyIndices, info->pQueueFamilyIndices + info->queueFamilyIndexCount);
    desc.minImageCount = info->minImageCount;
    desc.presentMode = static_cast<uint32_t>(info->presentMode);
    desc.flags = info->flags;
    desc.surfaceUsageKnown = query.surfaceKnown;
    desc.surfaceUsage = query.surfaceUsage;
    desc.transport = plan.transport;
    desc.transportNote = plan.note;

    std::string line;
    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnSwapchainCreated(H(device), H(*swapchain), std::move(desc));
        if (!internal)
            line = g_registry.SwapchainLine(H(device), H(*swapchain));
    }

    {
        std::lock_guard lock(g_transportMutex);
        if (DeviceTransport* dev = FindDevice(device))
        {
            SwapchainTransport& sc = dev->swapchains[*swapchain];
            if (!sc.ring.empty() || !sc.presentSemaphores.empty() || sc.work != VK_NULL_HANDLE)
                RetireSwapchain(*dev, sc, {}); // a handle on record whose destroy was never seen
            sc = SwapchainTransport {};
            sc.on = plan.transport;
            sc.format = info->imageFormat;
            sc.extent = info->imageExtent;
        }
    }

    if (!line.empty())
        LOG_INFO("{}", line);

    HookPresent(device);
    return result;
}

void OnDestroySwapchain(PFN_vkDestroySwapchainKHR next, VkDevice device, VkSwapchainKHR swapchain,
                        const VkAllocationCallbacks* allocator)
{
    if (swapchain != VK_NULL_HANDLE)
    {
        std::vector<uint8_t> acquired;
        {
            std::lock_guard lock(g_registryMutex);
            acquired = g_registry.AcquiredImages(H(device), H(swapchain));
        }

        // Its images are about to go: every round trip on them must be over first (bounded; kept if not).
        Retired retired;
        bool known = false;
        {
            std::lock_guard lock(g_transportMutex);
            if (DeviceTransport* dev = FindDevice(device))
            {
                if (const auto found = dev->swapchains.find(swapchain); found != dev->swapchains.end())
                {
                    known = true;
                    retired = RetireSwapchain(*dev, found->second, acquired);
                    dev->swapchains.erase(found);
                }
            }
        }

        std::string line;
        {
            std::lock_guard lock(g_registryMutex);
            g_registry.OnCompletions(H(device), H(swapchain), retired.completions);
            if (known)
                line = g_registry.SwapchainSummary(H(device), H(swapchain), retired.text);
            g_registry.OnSwapchainDestroyed(H(device), H(swapchain));
        }

        if (!line.empty())
            LOG_INFO("{}", line);
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

// Who called vkQueuePresentKHR, for the bundle's present hook below this one; nested scopes keep the outermost.
thread_local void* t_presentCaller = nullptr;

struct PresentCallerScope
{
    explicit PresentCallerScope(void* caller) : saved(t_presentCaller)
    {
        if (t_presentCaller == nullptr)
            t_presentCaller = caller;
    }
    ~PresentCallerScope() { t_presentCaller = saved; }
    PresentCallerScope(const PresentCallerScope&) = delete;
    PresentCallerScope& operator=(const PresentCallerScope&) = delete;

    void* saved;
};

// The present callback: the round trip when it can run, then the records and maybe a line, then the call down -- with
// the host's semaphore as the only wait when the round trip ran, with the caller's own present info otherwise. No
// Reflex, frame limiter, frozen-feature tick or upscaler timing: a translated D3D present already ran those at DXGI.
VkResult OnPresent(PFN_vkQueuePresentKHR next, VkQueue queue, const VkPresentInfoKHR* info, void* returnAddress)
{
    // The bundle's present hook, below this one, asks who called vkQueuePresentKHR to tell a DXVK or vkd3d-proton
    // present from a native one; with this detour on top its own return address is OptiScaler's. It gets the
    // application's (OriginalPresentCaller).
    const PresentCallerScope callerScope(returnAddress);

    if (info == nullptr)
        return next(queue, info);

    // For the whole present, the call down included: a thread borrowing this queue for a round trip waits for it.
    std::unique_lock<std::recursive_timed_mutex> presentLock;
    if (QueueLocks* locks = QueueLocksFor(queue))
        presentLock = std::unique_lock(locks->present);

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

    PresentView view;
    bool viewed = false;
    if (info->swapchainCount >= 1 && info->pSwapchains != nullptr && info->pImageIndices != nullptr)
    {
        std::lock_guard lock(g_registryMutex);
        viewed = g_registry.ViewPresent(H(queue), H(info->pSwapchains[0]), info->pImageIndices[0], view);
    }

    const Prepared prep = Prepare(queue, *info, viewed ? &view : nullptr);
    call.outcome = prep.outcome;

    std::vector<std::string> lines;
    {
        std::lock_guard lock(g_registryMutex);
        lines = g_registry.OnPresent(call);
    }

    Log(prep.warnings);
    Log(lines);

    if (prep.wait == VK_NULL_HANDLE)
        return next(queue, info);

    // A copy of the caller's present info, its pNext chain (present fences, present modes, present ids) by pointer,
    // with one change: the wait.
    VkPresentInfoKHR local = *info;
    local.waitSemaphoreCount = 1;
    local.pWaitSemaphores = &prep.wait;

    const VkResult result = next(queue, &local);
    AfterPresent(prep, result);
    return result;
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

// The queue calls that need the queue externally synchronised: under that queue's lock, nothing else.
template <size_t S>
VkResult VKAPI_CALL hkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    return LockedQueueCall(g_submit.next[S], queue, count, submits, fence);
}

template <size_t S>
VkResult VKAPI_CALL hkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
    return LockedQueueCall(g_submit2.next[S], queue, count, submits, fence);
}

template <size_t S>
VkResult VKAPI_CALL hkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* binds, VkFence fence)
{
    return LockedQueueCall(g_bindSparse.next[S], queue, count, binds, fence);
}

template <size_t S> VkResult VKAPI_CALL hkQueueWaitIdle(VkQueue queue)
{
    return LockedQueueCall(g_waitIdle.next[S], queue);
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
const std::array<void*, kTargets> kSubmitHooks = DLSSNR_VK_SLOTS(hkQueueSubmit);
const std::array<void*, kTargets> kSubmit2Hooks = DLSSNR_VK_SLOTS(hkQueueSubmit2);
const std::array<void*, kTargets> kBindSparseHooks = DLSSNR_VK_SLOTS(hkQueueBindSparse);
const std::array<void*, kTargets> kWaitIdleHooks = DLSSNR_VK_SLOTS(hkQueueWaitIdle);

#undef DLSSNR_VK_SLOTS

// The real function behind a queue call, for the queue locks: vkGetDeviceProcAddr's answer below OptiScaler's hooks
// (the bundle's own vkGetDeviceProcAddr hands out its own vkQueueSubmit), and vulkan-1.dll's export: one address under
// Wine; on Windows the driver's function and a loader trampoline, both of which a submit can reach. Resolved before
// g_installMutex is taken: without the bundle, VulkanHooks::GetDeviceProcAddr goes through OptiScaler's delay-loaded
// import of vulkan-1.dll, whose first use runs the loader hook and, through VulkanHooks::Hook, Install.
struct QueueCallTargets
{
    PFN_vkVoidFunction byDevice = nullptr;
    PFN_vkVoidFunction byExport = nullptr;
};

QueueCallTargets QueueCall(VkDevice device, const char* name)
{
    QueueCallTargets targets;

    targets.byDevice = VulkanHooks::GetDeviceProcAddr(device, name);
    if (targets.byDevice != nullptr && InOwnModule(reinterpret_cast<const void*>(targets.byDevice)))
        targets.byDevice = nullptr;

    if (g_module != nullptr)
    {
        targets.byExport = reinterpret_cast<PFN_vkVoidFunction>(KernelBaseProxy::GetProcAddress_()(g_module, name));
        if (targets.byExport != nullptr && InOwnModule(reinterpret_cast<const void*>(targets.byExport)))
            targets.byExport = nullptr;
    }

    return targets;
}

// With g_installMutex held.
void AttachQueueCall(const QueueCallTargets& targets, auto& set, const std::array<void*, kTargets>& hooks,
                     const char* name)
{
    AttachTarget(set, hooks, targets.byDevice, name);
    AttachTarget(set, hooks, targets.byExport, name);
}

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
    const QueueCallTargets submit = QueueCall(device, "vkQueueSubmit");
    const QueueCallTargets submit2 = QueueCall(device, "vkQueueSubmit2");
    const QueueCallTargets submit2Khr = QueueCall(device, "vkQueueSubmit2KHR");
    const QueueCallTargets bindSparse = QueueCall(device, "vkQueueBindSparse");
    const QueueCallTargets waitIdle = QueueCall(device, "vkQueueWaitIdle");

    std::lock_guard lock(g_installMutex);
    AttachTarget(g_destroyDevice, kDestroyDeviceHooks, destroyDevice, "vkDestroyDevice");
    AttachTarget(g_getQueue, kGetQueueHooks, getQueue, "vkGetDeviceQueue");
    AttachTarget(g_getQueue2, kGetQueue2Hooks, getQueue2, "vkGetDeviceQueue2");
    AttachTarget(g_createSwapchain, kCreateSwapchainHooks, createSwapchain, "vkCreateSwapchainKHR");
    AttachTarget(g_destroySwapchain, kDestroySwapchainHooks, destroySwapchain, "vkDestroySwapchainKHR");
    AttachTarget(g_getImages, kGetImagesHooks, getImages, "vkGetSwapchainImagesKHR");
    AttachTarget(g_acquire, kAcquireHooks, acquire, "vkAcquireNextImageKHR");
    AttachTarget(g_acquire2, kAcquire2Hooks, acquire2, "vkAcquireNextImage2KHR");
    AttachQueueCall(submit, g_submit, kSubmitHooks, "vkQueueSubmit");
    AttachQueueCall(submit2, g_submit2, kSubmit2Hooks, "vkQueueSubmit2");
    AttachQueueCall(submit2Khr, g_submit2, kSubmit2Hooks, "vkQueueSubmit2KHR");
    AttachQueueCall(bindSparse, g_bindSparse, kBindSparseHooks, "vkQueueBindSparse");
    AttachQueueCall(waitIdle, g_waitIdle, kWaitIdleHooks, "vkQueueWaitIdle");
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

    // The create info goes down as the caller made it: the host adds no extension, feature or queue.
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
    RecordId id = 0;
    {
        std::lock_guard lock(g_registryMutex);
        id = g_registry.OnDeviceCreated(H(*device), std::move(desc));
        line = g_registry.DeviceLine(H(*device));
    }

    // OptiScaler's own devices get no transport: nothing of theirs is ever transported.
    if (!internal)
    {
        auto dev = MakeDeviceTransport(*device, physical, id);
        if (!dev->off.empty())
            line += "; round trip off on it: " + dev->off;

        std::lock_guard lock(g_transportMutex);
        g_transport[*device] = std::move(dev);
    }

    LOG_INFO("{}", line);

    HookDevice(*device);

    // The round trip's own submits go through the host's trampoline of the device's vkQueueSubmit, below every hook
    // on it, its own included: they take no other hook's lock, and the borrowed queue's locks they need are taken
    // explicitly.
    if (!internal)
    {
        const void* real = reinterpret_cast<const void*>(VulkanHooks::GetDeviceProcAddr(*device, "vkQueueSubmit"));
        PFN_vkQueueSubmit below = nullptr;
        {
            std::lock_guard lock(g_installMutex);
            for (size_t i = 0; i < kTargets; ++i)
            {
                if (real != nullptr && g_submit.target[i] == real)
                    below = g_submit.next[i];
            }
        }

        std::lock_guard lock(g_transportMutex);
        if (DeviceTransport* dev = FindDevice(*device); dev != nullptr && below != nullptr)
            dev->fn.queueSubmit = below;
    }

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

void* DlssNr::VkPresentHost::OriginalPresentCaller(void* returnAddress)
{
    return t_presentCaller != nullptr ? t_presentCaller : returnAddress;
}

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
    g_getMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(get("vkGetPhysicalDeviceMemoryProperties"));
    g_getImageFormatProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(get("vkGetPhysicalDeviceImageFormatProperties"));

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
        g_getMemoryProperties = nullptr;
        g_getImageFormatProperties = nullptr;
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

    LOG_INFO("DLSS-NR Vulkan present host (round trip, no NR; stage 2 of nr-vulkan-present-host.md): installed on "
             "vulkan-1.dll, {}",
             State::Instance().vulkanHooksSkipped
                 ? "while the rest of OptiScaler's Vulkan hooks stay off ([Vulkan] SkipHooks or the marker); this "
                   "host is the one exception"
                 : "beside OptiScaler's own Vulkan hooks");
}
