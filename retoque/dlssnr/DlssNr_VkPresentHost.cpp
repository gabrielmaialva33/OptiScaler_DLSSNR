#include "pch.h"

#include "DlssNr_VkPresentHost.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_VkModelOwner.h"
#include "DlssNr_VkPresentModel.h"
#include "DlssNr_VkPresentRegistry.h"
#include "DlssNr_VkPresentTransport.h"
#include "DlssNr_Session.h"

#include <Config.h>
#include <State.h>
#include <hooks/Vulkan_Hooks.h>
#include <proxies/KernelBase_Proxy.h>

#include <vulkan/vulkan.h>

#include <detours/detours.h>

#include <intrin.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

// Stages 1 to 3 of docs/dlssnr/design/nr-vulkan-present-host.md: see DlssNr_VkPresentHost.h. Every hook below calls
// down to the trampoline of the target it was attached to, once. Stage 1's hooks hand their own arguments down; stage
// 2 changes exactly two things on the way: the image usage of an eligible swapchain (TRANSFER_SRC/TRANSFER_DST added,
// decided before the call), and the wait semaphores of a transported present (the application's are waited on by the
// round trip, and the present waits on the host's semaphore instead; everything else of VkPresentInfoKHR, its pNext
// chain included, goes down as it came). Stage 3 changes one more: the extension list of a device the host will carry
// presents on, which gets the model's device extensions the device offers (decided before the call, never retried);
// and it runs the model (DlssNr_VkPresentModel.h) inside the round trip's own command buffer.
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
// go through the host's trampoline, below the host's hook on vkQueueSubmit (the bundle may still take its own queue
// lock), and its other device functions are resolved below OptiScaler's hooks (VulkanHooks::GetDeviceProcAddr).

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
PFN_vkGetPhysicalDeviceFormatProperties g_getFormatProperties = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_enumerateDeviceExtensions = nullptr;
PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;

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

// The stall watchdog's activities (DlssNr_VkPresentBuild.h, "Off the present path" in the design note). Every entry
// point that can hold or wait for a host lock records one; the watchdog thread reads them with atomics only.
void TagThread()
{
    if (ThreadTag() == 0)
        SetThreadTag(static_cast<uint32_t>(GetCurrentThreadId()));
}

// One entry point's activity, made the thread's current one so helpers deeper down can name their stage.
const char* Tagged(const char* entry)
{
    TagThread();
    return entry;
}

struct HostActivity
{
    explicit HostActivity(const char* entry, uint64_t object = 0) : scope(Tagged(entry), object), current(scope) {}

    ActivityScope scope;
    CurrentActivity current;
};

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

struct QueueLockEntry
{
    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<QueueLocks>
        locks; // shared: the model's builder may hold them while vkDestroyDevice erases the entry
};

std::shared_mutex g_queueLocksMutex;
std::unordered_map<VkQueue, QueueLockEntry> g_queueLocks;

QueueLocks* QueueLocksFor(VkQueue queue)
{
    std::shared_lock lock(g_queueLocksMutex);
    const auto found = g_queueLocks.find(queue);
    return found != g_queueLocks.end() ? found->second.locks.get() : nullptr;
}

void AddQueueLock(VkDevice device, VkQueue queue, uint32_t family, uint32_t index)
{
    std::unique_lock lock(g_queueLocksMutex);
    auto& entry = g_queueLocks[queue];
    if (entry.locks == nullptr || entry.device != device)
    {
        entry.device = device;
        entry.locks = std::make_shared<QueueLocks>();
        entry.locks->family = family;
        entry.locks->index = index;
    }
}

void EraseQueueLocks(VkDevice device)
{
    std::unique_lock lock(g_queueLocksMutex);
    for (auto it = g_queueLocks.begin(); it != g_queueLocks.end();)
        it = it->second.device == device ? g_queueLocks.erase(it) : std::next(it);
}

template <typename Pfn, typename... Args>
VkResult LockedQueueCall(const char* entry, Pfn next, VkQueue queue, Args... args)
{
    if (QueueLocks* locks = QueueLocksFor(queue))
    {
        HostActivity activity(entry, H(queue));
        activity.scope.Stage("waiting for the queue's submit lock", kLockSubmit, locks->submit.Owner());
        std::lock_guard lock(locks->submit);
        activity.scope.Hold(kLockSubmit, true);
        activity.scope.Stage("calling down");
        const VkResult result = next(queue, args...);
        activity.scope.Hold(kLockSubmit, false);
        return result;
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
    PFN_vkCmdBlitImage cmdBlitImage = nullptr;
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
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
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
    uint64_t nrTicket = 0;                  // the model's frame in this slot's work buffer, until seen complete
};

struct SwapchainTransport
{
    bool on = false;
    std::string off; // why the round trip stopped after creation, when it did
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent {};
    RecordId id = 0;      // the registry's record of it
    bool retired = false; // a newer swapchain named it as its oldSwapchain

    // Stage 3: whether the model may be shown it, and the raw format its bytes are copied through (the sRGB formats'
    // UNORM twins, so nothing is decoded on the way; the swapchain's own format otherwise).
    ColourPlan colour;
    uint32_t colourSpace = 0;
    std::string nrOff; // the model is off for this swapchain (its working colour could not be made)
    bool nrCut = true; // its next frame of the model starts the history over: the first, or one after a gap

    // One work image per swapchain: every slot's round trip runs on one queue (a change of queue waits for the ring),
    // and each recording orders its first write after the previous recording's reads on that queue. In the raw format.
    VkImage work = VK_NULL_HANDLE;
    VkDeviceMemory workMemory = VK_NULL_HANDLE;
    VkQueue lastWorkQueue = VK_NULL_HANDLE;

    // The working colour the model's pass reads and writes (kWorkingFormat), blitted from the work image and back.
    VkImage working = VK_NULL_HANDLE;
    VkDeviceMemory workingMemory = VK_NULL_HANDLE;
    VkImageView workingView = VK_NULL_HANDLE;

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

// The way back from the after-upscale route (DlssNr_VkModelOwner.h): the drain of the route's device, under way. One
// fence per queue of the device, each on an empty submit made after the hand-back began, so each signals once
// everything submitted to its queue before it has completed -- the route's last recordings among it.
struct HandBackDrain
{
    bool active = false;
    bool calledOff = false; // a drain submit failed: the arbiter's hand-back is off, the fences only waited for
    std::vector<VkFence> fences;
    std::chrono::steady_clock::time_point started;
    bool saidSlow = false;
};

// Objects whose GPU completion was not proven when their swapchain went: kept, and tried again at device destruction.
struct Leftover
{
    std::vector<Slot> slots;
    VkImage work = VK_NULL_HANDLE;
    VkDeviceMemory workMemory = VK_NULL_HANDLE;
    VkImage working = VK_NULL_HANDLE;
    VkDeviceMemory workingMemory = VK_NULL_HANDLE;
    VkImageView workingView = VK_NULL_HANDLE;
};

struct DeviceTransport
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t apiVersion = 0; // the application's instance asked for it (0: not on record)
    RecordId id = 0;
    DeviceFns fn;
    VkPhysicalDeviceMemoryProperties memory {};
    bool lost = false;
    std::string off;     // the transport stopped on this device: why (new presents skip with DeviceOff)
    bool leaked = false; // a command buffer whose completion is unknown was left in a pool: never destroy the pools
    std::unordered_map<uint32_t, VkCommandPool> pools; // by family
    std::unordered_map<VkSwapchainKHR, SwapchainTransport> swapchains;
    std::vector<VkSemaphore> retained; // present semaphores nothing proved free, destroyed with the device
    std::vector<Leftover> leftovers;

    // Stage 3. The model's extensions at creation (ExtensionPlan::ready), and why not.
    bool nrReady = false;
    std::string nrMissing;
    std::vector<uint32_t> timestampBits;             // per queue family
    std::unordered_map<uint32_t, uint32_t> features; // optimal-tiling format features, by format, as queried
    std::unique_ptr<DlssNr::VkPresentModel> model;   // made at the first frame it may run on
    RecordId modelSwapchain = 0;                     // the swapchain the model serves (one at a time)
    uint64_t nextTicket = 1;
    std::vector<std::string> lines; // the model's lines, logged once the transport's lock is released
    HandBackDrain handBack;         // the model taken back from the after-upscale route, while its queues drain
    std::chrono::steady_clock::time_point handBackNotBefore; // after a drain submit failed: not tried again until then
    HandBackDiagnostics handBackDiagnostics;                 // once per distinct blocker in a due episode
};

TrackedMutex<std::mutex> g_transportMutex;
std::unordered_map<VkDevice, std::unique_ptr<DeviceTransport>> g_transport;

// The transport lock, its wait and its holding recorded in the thread's current activity.
class TransportLock
{
  public:
    TransportLock()
    {
        ActivityStage("waiting for the transport lock", kLockTransport, g_transportMutex.Owner());
        g_transportMutex.lock();
        ActivityHold(kLockTransport, true);
        ActivityStage("holding the transport lock");
    }
    ~TransportLock()
    {
        ActivityHold(kLockTransport, false);
        g_transportMutex.unlock();
    }
    TransportLock(const TransportLock&) = delete;
    TransportLock& operator=(const TransportLock&) = delete;
};

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
    ok = ok && Resolve(device, "vkCmdBlitImage", f.cmdBlitImage);
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
    ok = ok && Resolve(device, "vkCreateImageView", f.createImageView);
    ok = ok && Resolve(device, "vkDestroyImageView", f.destroyImageView);

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
        DlssNr::SessionGuardDeviceRemoved(); // evidence for the next launch, whatever the game does now
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

void DestroyWorkImage(DeviceTransport& dev, VkImage& image, VkDeviceMemory& memory, VkImageView* view = nullptr)
{
    if (view != nullptr && *view != VK_NULL_HANDLE)
        dev.fn.destroyImageView(dev.device, *view, nullptr);
    if (image != VK_NULL_HANDLE)
        dev.fn.destroyImage(dev.device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        dev.fn.freeMemory(dev.device, memory, nullptr);

    if (view != nullptr)
        *view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

// The model's pass GPU times read back while observing completions, for the lines.
struct GpuTimes
{
    uint32_t timed = 0;
    double last = 0.0;
    double max = 0.0;
    double sum = 0.0;

    void Add(double ms)
    {
        ++timed;
        last = ms;
        max = ms > max ? ms : max;
        sum += ms;
    }
};

// A slot was seen complete: the model's frame in it, if any, is too.
void Observed(DeviceTransport& dev, Slot& slot, GpuTimes& times)
{
    if (slot.nrTicket == 0)
        return;

    if (dev.model != nullptr)
    {
        if (const auto ms = dev.model->Completed(slot.nrTicket))
            times.Add(*ms);
    }

    slot.nrTicket = 0;
}

uint64_t Since(std::chrono::steady_clock::time_point start)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

// Every pending slot whose fence is signalled becomes idle. Returns the completions observed.
uint32_t Poll(DeviceTransport& dev, SwapchainTransport& sc, GpuTimes& times)
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
            Observed(dev, slot, times);
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
bool WaitSlot(DeviceTransport& dev, Slot& slot, uint64_t timeoutNs, uint64_t& waitNs, uint32_t& completions,
              GpuTimes& times)
{
    if (slot.state != SlotState::Pending || dev.lost)
        return slot.state == SlotState::Idle;

    const auto start = std::chrono::steady_clock::now();
    const VkResult result = dev.fn.waitForFences(dev.device, 1, &slot.fence, VK_TRUE, timeoutNs);
    waitNs += Since(start);

    if (result == VK_SUCCESS)
    {
        slot.state = SlotState::Idle;
        Observed(dev, slot, times);
        ++completions;
        return true;
    }

    if (result == VK_ERROR_DEVICE_LOST)
        MarkLost(dev);

    return false;
}

// A device-local image of the swapchain's extent the host owns: the work image (raw format, transfer only) or the
// working colour (kWorkingFormat, also sampled and stored by the model's pass).
bool MakeImage(DeviceTransport& dev, const SwapchainTransport& sc, VkFormat format, VkImageUsageFlags usage,
               VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { sc.extent.width, sc.extent.height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (dev.fn.createImage(dev.device, &info, nullptr, &image) != VK_SUCCESS)
    {
        image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements {};
    dev.fn.getImageMemoryRequirements(dev.device, image, &requirements);

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

    if (type == UINT32_MAX || dev.fn.allocateMemory(dev.device, &allocate, nullptr, &memory) != VK_SUCCESS ||
        dev.fn.bindImageMemory(dev.device, image, memory, 0) != VK_SUCCESS)
    {
        DestroyWorkImage(dev, image, memory);
        return false;
    }

    return true;
}

bool EnsureWorkImage(DeviceTransport& dev, SwapchainTransport& sc)
{
    return sc.work != VK_NULL_HANDLE ||
           MakeImage(dev, sc, static_cast<VkFormat>(sc.colour.raw),
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, sc.work, sc.workMemory);
}

bool EnsureWorkingImage(DeviceTransport& dev, SwapchainTransport& sc)
{
    if (sc.workingView != VK_NULL_HANDLE)
        return true;

    if (sc.working == VK_NULL_HANDLE && !MakeImage(dev, sc, static_cast<VkFormat>(kWorkingFormat),
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                                   sc.working, sc.workingMemory))
        return false;

    VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = sc.working;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = static_cast<VkFormat>(kWorkingFormat);
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (dev.fn.createImageView(dev.device, &view, nullptr, &sc.workingView) != VK_SUCCESS)
    {
        sc.workingView = VK_NULL_HANDLE;
        DestroyWorkImage(dev, sc.working, sc.workingMemory);
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

enum class Recorded : uint8_t
{
    Ok,
    Failed,   // the buffer could not be recorded
    NrFailed, // the model's pass failed after recording began: record the plain round trip instead
};

VkImageBlit WholeImage(VkExtent2D extent)
{
    VkImageBlit blit {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1] = { static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1 };
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1] = blit.srcOffsets[1];
    return blit;
}

// The round trip, on the work queue: the presented image into the work image and back, the image left in PRESENT_SRC.
// On the cross-family route it starts with the acquire of the presenting family's release and ends with the release
// back. With `nr`, the work image is blitted into the working colour, the model's pass runs on that, and it is blitted
// back before the copy out (stage 3); without, the image goes back unchanged. Everything is recorded before any
// application semaphore is consumed.
Recorded RecordWork(DeviceTransport& dev, VkCommandBuffer buffer, VkImage image, VkImage work, VkExtent2D extent,
                    Route route, uint32_t presentFamily, uint32_t workFamily,
                    const DlssNr::VkPresentNrFrame* nr = nullptr)
{
    if (!Begin(dev, buffer))
        return Recorded::Failed;

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

    if (nr != nullptr && dev.model != nullptr && nr->hdrProfile != 0)
    {
        if (!dev.model->Record(*nr))
            return Recorded::NrFailed;
    }
    else if (nr != nullptr && dev.model != nullptr)
    {
        // Into the working colour: a blit converts number by number (and BGRA by channel), from the raw bytes, which
        // are the sRGB formats' UNORM twins, so nothing is decoded. Its earlier frames' reads and writes of the working
        // colour (the pass, the blit back) are in the source scope.
        const VkImageMemoryBarrier toBlit[] = {
            Barrier(work, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(nr->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, toBlit);

        const VkImageBlit blit = WholeImage(extent);
        dev.fn.cmdBlitImage(buffer, work, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, nr->image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);

        const VkImageMemoryBarrier toPass =
            Barrier(nr->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &toPass);

        // The model: encode, the model itself, resolve back into the working colour (DlssNrFeature_Vk.cpp).
        if (!dev.model->Record(*nr))
            return Recorded::NrFailed;

        // And back into the work image, which the copy out below takes to the presented image.
        const VkImageMemoryBarrier back[] = {
            Barrier(nr->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            Barrier(work, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        dev.fn.cmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, back);

        dev.fn.cmdBlitImage(buffer, nr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, work,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    }

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

    return dev.fn.endCommandBuffer(buffer) == VK_SUCCESS ? Recorded::Ok : Recorded::Failed;
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

// --- The model (stage 3) -----------------------------------------------------------------------------------------

// The model's lines, for the host to log after its lock is released.
void TakeModelLines(DeviceTransport& dev)
{
    if (dev.model == nullptr)
        return;

    for (std::string& line : dev.model->TakeLines())
        dev.lines.push_back(std::move(line));
}

// What a format allows on this device in optimal tiling, asked once.
uint32_t FormatFeatures(DeviceTransport& dev, uint32_t format)
{
    if (const auto found = dev.features.find(format); found != dev.features.end())
        return found->second;

    VkFormatProperties properties {};
    if (g_getFormatProperties != nullptr)
        g_getFormatProperties(dev.physical, static_cast<VkFormat>(format), &properties);

    return dev.features[format] = properties.optimalTilingFeatures;
}

// Why the device cannot convert the presented image or hold the model's images, or empty when it can: the raw image
// is blitted from and to, the working colour also sampled and stored, the guides cleared and sampled.
std::string MissingFormatFeatures(DeviceTransport& dev, uint32_t raw, bool hdr = false)
{
    const uint32_t transfer = kFeatureTransferSrc | kFeatureTransferDst;
    const uint32_t images = kFeatureSampledImage | kFeatureStorageImage | transfer;
    const std::pair<uint32_t, uint32_t> needs[] = {
        { raw, hdr ? kFeatureSampledImage | transfer | (raw == kFormatA2R10G10B10UnormPack32 ? kFeatureBlitDst : 0)
                   : kFeatureBlitSrc | kFeatureBlitDst | transfer },
        { kWorkingFormat, kFeatureBlitSrc | kFeatureBlitDst | images },
        { kFormatR32Sfloat, images },
        { kFormatR16G16Sfloat, images },
    };

    for (const auto& [format, wanted] : needs)
    {
        const uint32_t has = FormatFeatures(dev, format);
        if ((has & wanted) != wanted)
            return FormatName(format) + " lacks format features " + Hex(wanted & ~has) +
                   " the conversion or the model needs";
    }

    return {};
}

// Stop using the model on this device: nothing new is recorded, and once nothing of it is in flight it is freed and
// the token given back. Called on every frame that refuses for it until that happens.
void StandDown(DeviceTransport& dev)
{
    // On a lost device nothing is freed: the device's teardown abandons it.
    if (dev.model == nullptr || dev.model->InFlight() != 0 || dev.lost)
        return;

    dev.model->Release();
    if (dev.model->DeviceLost())
    {
        MarkLost(dev);
        return;
    }
    TakeModelLines(dev);
    dev.model.reset();
    dev.modelSwapchain = 0;
    DlssNr::ReleaseModelFromPresentHost(dev.device);
    dev.lines.push_back("DLSS-NR Vulkan present host: device " + Hex(H(dev.device)) + " #" + std::to_string(dev.id) +
                        ": the model stood down and was freed");
}

// --- The way back from the after-upscale route -------------------------------------------------------------------

// Every queue of the device the application retrieved, with its locks. The locks outlive the call: they go only at
// the device's destruction, which no present on it overlaps.
std::vector<std::pair<VkQueue, std::shared_ptr<QueueLocks>>> DeviceQueues(VkDevice device)
{
    std::vector<std::pair<VkQueue, std::shared_ptr<QueueLocks>>> queues;
    std::shared_lock lock(g_queueLocksMutex);
    for (const auto& [queue, entry] : g_queueLocks)
    {
        if (entry.device == device && entry.locks != nullptr)
            queues.emplace_back(queue, entry.locks);
    }
    return queues;
}

// Why no frame of this device can be shown to the model (empty means eligible). The caller checks NR on; the model's
// extensions there, no other route wanting it (with no model here, PresentHostMustYield refuses only for the D3D12
// route), a carried swapchain whose colour the model may see and whose formats the device can convert. A hand-back the
// host could not use would only free the after-upscale route's objects for it to build again.
std::string ModelUnavailable(DeviceTransport& dev)
{
    if (dev.lost)
        return "device lost";
    if (!dev.nrReady)
        return "model device extensions unavailable: " + dev.nrMissing;
    if (!dev.off.empty())
        return "device transport off: " + dev.off;
    if (dev.instance == VK_NULL_HANDLE)
        return "the device's instance is not on record";
    const auto yield = DlssNr::PresentHostMustYield(dev.device);
    if (yield.skip != NrSkip::None)
        return "must yield: " + yield.text;

    std::vector<std::string> refusals;
    for (const auto& [handle, sc] : dev.swapchains)
    {
        const auto colour =
            PlanColour(sc.colour.raw, sc.colourSpace, Config::Instance()->DlssNrHdrPresent.value_or_default());
        std::string reason;
        if (sc.retired)
            reason = "retired";
        else if (!sc.on)
            reason = "transport off: " + sc.off;
        else if (!colour.Eligible())
            reason = "colour: " + colour.refusal;
        else if (!sc.nrOff.empty())
            reason = "model unavailable: " + sc.nrOff;
        else
            reason = MissingFormatFeatures(dev, sc.colour.raw, colour.profile != 0);
        if (reason.empty())
            return {};
        refusals.push_back("swapchain " + Hex(H(handle)) + ": " + reason);
    }
    // Stable across unordered-map iteration and queue changes: the same set of blockers is one diagnostic reason.
    std::sort(refusals.begin(), refusals.end());
    std::string reason = "no eligible swapchain";
    if (refusals.empty())
        reason += " (none on record)";
    for (const auto& refusal : refusals)
        reason += "; " + refusal;
    return reason;
}

std::string DeviceName(const DeviceTransport& dev)
{
    return "DLSS-NR Vulkan present host: device " + Hex(H(dev.device)) + " #" + std::to_string(dev.id);
}

uint64_t PresentNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void HandBackBlocked(DeviceTransport& dev, const std::string& reason, uint64_t delayMs = 0)
{
    if (dev.handBackDiagnostics.Blocked(reason, PresentNowMs(), delayMs))
        dev.lines.push_back(DeviceName(dev) + ": hand-back due but not started: " + reason);
}

std::string QueueName(VkQueue queue, const QueueLocks& locks)
{
    return "queue " + Hex(H(queue)) + " family " + std::to_string(locks.family) + " index " +
           std::to_string(locks.index);
}

// The after-upscale route went quiet on this device: begin the hand-back and drain every queue of the device behind a
// fence. Never waits: a queue another thread holds, or a fence that cannot be made, is tried again at the next present.
void StartHandBack(DeviceTransport& dev)
{
    const auto queues = DeviceQueues(dev.device);
    if (queues.empty())
    {
        // Every queue the application uses is retrieved through the host's hook, the presenting one included; none on
        // record means the host came in late, and nothing proves the route's work done.
        dev.handBackNotBefore =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(DlssNr::VkModelOwner::kAfterUpscaleIdleMs);
        HandBackBlocked(dev, "no queue of the device is on record to drain");
        return;
    }

    // Each queue's present lock, then its submit lock, as a borrower takes them, so no other thread submits to it or
    // presents on it between the hand-back beginning and its fence. The presenting queue's present lock is this
    // thread's already (they are recursive).
    std::vector<std::unique_lock<QueueMutex>> held;
    held.reserve(queues.size() * 2);
    for (const auto& [queue, locks] : queues)
    {
        if (const char* busy = TryHandBackQueueLocks(*locks, held))
        {
            HandBackBlocked(dev, QueueName(queue, *locks) + " " + busy + " lock busy (retrying for at least 1 s)",
                            1000);
            return;
        }
    }

    std::vector<VkFence> fences;
    const auto destroyFrom = [&dev, &fences](size_t first)
    {
        for (size_t i = first; i < fences.size(); ++i)
            dev.fn.destroyFence(dev.device, fences[i], nullptr);
        fences.resize(first);
    };

    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (size_t i = 0; i < queues.size(); ++i)
    {
        VkFence fence = VK_NULL_HANDLE;
        const VkResult result = dev.fn.createFence(dev.device, &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS)
        {
            HandBackBlocked(dev, "fence creation returned " + std::to_string(result) + " for " +
                                     QueueName(queues[i].first, *queues[i].second));
            destroyFrom(0);
            return;
        }
        fences.push_back(fence);
    }

    // Begun before the first fence is submitted: from here the route asking again calls the hand-back off, and
    // everything it recorded before (it last asked kAfterUpscaleIdleMs ago) is ahead of these fences on its queue.
    if (!DlssNr::BeginAfterUpscaleHandBack(dev.device))
    {
        HandBackBlocked(dev, "BeginAfterUpscaleHandBack refused (owner or quiet state changed)");
        destroyFrom(0);
        return;
    }

    const VkSubmitInfo submitInfo { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    size_t submitted = 0;
    VkResult failure = VK_SUCCESS;
    for (; submitted < queues.size(); ++submitted)
    {
        // An empty batch signals the fence after prior queue work. A zero-count/null-array submit is valid
        // Vulkan, but the next layer can be Vulkan_wDx12, which rejects a null submit array.
        failure = dev.fn.queueSubmit(queues[submitted].first, 1, &submitInfo, fences[submitted]);
        if (failure != VK_SUCCESS)
            break;
    }
    held.clear();

    HandBackDrain& hb = dev.handBack;
    hb = {};
    hb.started = std::chrono::steady_clock::now();

    if (failure != VK_SUCCESS)
    {
        DlssNr::CancelAfterUpscaleHandBack(dev.device);
        dev.handBackNotBefore =
            hb.started + std::chrono::milliseconds(DlssNr::VkModelOwner::kAfterUpscaleIdleMs); // not every present
        dev.lines.push_back(DeviceName(dev) + ": a hand-back drain submit returned " + std::to_string(failure) +
                            "; the hand-back is called off, the after-upscale route keeps the model");

        if (failure == VK_ERROR_DEVICE_LOST)
        {
            // Nothing destroyed on a lost device, the fences included.
            MarkLost(dev);
            return;
        }

        // The fences never submitted go now; those submitted are waited for before they do.
        destroyFrom(submitted);
        if (fences.empty())
            return;
        hb.calledOff = true;
    }
    else
    {
        dev.lines.push_back(DeviceName(dev) + ": the after-upscale route has not asked for the model for " +
                            std::to_string(DlssNr::VkModelOwner::kAfterUpscaleIdleMs / 1000) +
                            " s: taking it back, once the device's " + std::to_string(queues.size()) +
                            " queue(s) drained");
    }

    dev.handBackDiagnostics.Reset();
    hb.active = true;
    hb.fences = std::move(fences);
}

// The hand-back's drain, at every present until it is over. Its fences signalled: the route's objects are freed and
// the token released (Done), or the route asked again meanwhile and keeps everything (Cancelled).
void PollHandBack(DeviceTransport& dev)
{
    HandBackDrain& hb = dev.handBack;

    const auto forget = [&dev, &hb](const std::string& why)
    {
        // Nothing destroyed: the device is lost, or the fences' state is unknown. Not tried again at once.
        if (!hb.calledOff)
            DlssNr::CancelAfterUpscaleHandBack(dev.device);
        dev.handBackNotBefore =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(DlssNr::VkModelOwner::kAfterUpscaleIdleMs);
        dev.lines.push_back(DeviceName(dev) + ": " + why +
                            "; the hand-back is called off, the after-upscale route keeps the model");
        hb = {};
    };

    if (dev.lost)
        return forget("the device was lost while it drained");

    while (!hb.fences.empty())
    {
        const VkResult status = dev.fn.getFenceStatus(dev.device, hb.fences.back());

        if (status == VK_NOT_READY)
        {
            if (!hb.saidSlow && Since(hb.started) >= kDrainNs)
            {
                hb.saidSlow = true;
                dev.lines.push_back(DeviceName(dev) + ": the hand-back's drain is still waiting for the GPU after " +
                                    std::to_string(kDrainNs / 1000000000) +
                                    " s; nothing of the after-upscale route's is freed until it is over");
            }
            return;
        }

        if (status != VK_SUCCESS)
        {
            if (status == VK_ERROR_DEVICE_LOST)
                MarkLost(dev);
            return forget("a drain fence returned " + std::to_string(status));
        }

        dev.fn.destroyFence(dev.device, hb.fences.back(), nullptr);
        hb.fences.pop_back();
    }

    if (hb.calledOff)
    {
        hb = {};
        return;
    }

    const uint64_t drainMs = Since(hb.started) / 1000000;
    switch (DlssNr::FinishAfterUpscaleHandBack(dev.device, dev.lines))
    {
    case DlssNr::AfterUpscaleHandBack::Busy:
        return; // the route is evaluating: it called the hand-back off, or finds it over at the next present
    case DlssNr::AfterUpscaleHandBack::Done:
        dev.lines.push_back(DeviceName(dev) + ": the model is the present host's again (the drain took " +
                            std::to_string(drainMs) + " ms)");
        break;
    case DlssNr::AfterUpscaleHandBack::Cancelled:
        dev.lines.push_back(DeviceName(dev) + ": the hand-back is called off: the after-upscale route asked for the "
                                              "model again while the device drained; it keeps it");
        break;
    case DlssNr::AfterUpscaleHandBack::NotRetiring:
        break;
    }

    hb = {};
}

// At the device's destruction, or its loss: the drain's fences waited for within what is left of `budgetNs` and
// destroyed, or forgotten (lost, never signalled); the hand-back off. The route's objects go with the device
// (AfterUpscaleDeviceGone). False when a fence was never seen signalled: the GPU may not be done.
bool EndHandBack(DeviceTransport& dev, uint64_t budgetNs)
{
    HandBackDrain& hb = dev.handBack;
    if (!hb.active)
        return true;

    if (!hb.calledOff)
        DlssNr::CancelAfterUpscaleHandBack(dev.device);

    bool drained = hb.fences.empty();
    if (!dev.lost && !hb.fences.empty() &&
        dev.fn.waitForFences(dev.device, static_cast<uint32_t>(hb.fences.size()), hb.fences.data(), VK_TRUE,
                             budgetNs) == VK_SUCCESS)
    {
        for (const VkFence fence : hb.fences)
            dev.fn.destroyFence(dev.device, fence, nullptr);
        drained = true;
    }

    hb = {};
    return drained;
}

// At the device's destruction, with nothing of the host's on it unproven: every queue of it the application retrieved
// drained behind a fence of the host's, waited for within `budgetNs`, so what is freed next (the after-upscale route's
// objects, NGX on the device) is proven idle without an unbounded device-wide wait. The application destroying the
// device uses none of its queues now. False when a fence could not be made or submitted, or was not seen signalled in
// time (its fences are then kept, never destroyed), or no queue is on record.
bool DrainForDestroy(DeviceTransport& dev, uint64_t budgetNs)
{
    const auto queues = DeviceQueues(dev.device);
    if (dev.lost || queues.empty())
        return false;

    std::vector<VkFence> fences;
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    const VkSubmitInfo submitInfo { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    bool submitted = true;
    for (const auto& [queue, locks] : queues)
    {
        VkFence fence = VK_NULL_HANDLE;
        if (dev.fn.createFence(dev.device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        {
            submitted = false;
            break;
        }

        // As in StartHandBack, keep the submit array non-null for the downstream Vulkan_wDx12 hook.
        const VkResult result = dev.fn.queueSubmit(queue, 1, &submitInfo, fence);
        if (result != VK_SUCCESS)
        {
            dev.fn.destroyFence(dev.device, fence, nullptr); // never submitted
            if (result == VK_ERROR_DEVICE_LOST)
                MarkLost(dev);
            submitted = false;
            break;
        }
        fences.push_back(fence);
    }

    bool drained = submitted;
    if (!fences.empty() && !dev.lost)
    {
        const VkResult result =
            dev.fn.waitForFences(dev.device, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, budgetNs);
        if (result == VK_ERROR_DEVICE_LOST)
            MarkLost(dev);
        if (result != VK_SUCCESS)
            return false; // the fences stay: one may still be pending

        for (const VkFence fence : fences)
            dev.fn.destroyFence(dev.device, fence, nullptr);
    }

    return drained && !dev.lost;
}

// On every present of the device, whatever becomes of it (a frame the transport refuses, a swapchain the model may not
// see): with NR turned off live, or another route asking for the model, the model stands down. Otherwise a model that
// once ran here would hold the token for as long as the device's presents never reached the model's decision -- an
// HDR swapchain after an SDR menu, a swapchain gone observe-only -- and keep the game's own upscaler from it.
//
// And the other way: with no model here, the after-upscale route holding it on this device and quiet for
// kAfterUpscaleIdleMs (the game turned its upscaler off), the hand-back begins, and its drain is followed here.
void Housekeep(DeviceTransport& dev)
{
    if (dev.model != nullptr && dev.model->DeviceLost())
        MarkLost(dev);
    if (dev.lost)
        return;

    if (dev.handBack.active)
        PollHandBack(dev);
    else if (DlssNr::AfterUpscaleIdleOn(dev.device))
    {
        const bool enabled = Config::Instance()->DlssNrEnabled.value_or_default();
        const std::string reason = HandBackBlockReason(
            { enabled, dev.model != nullptr, std::chrono::steady_clock::now() < dev.handBackNotBefore,
              enabled && dev.model == nullptr ? ModelUnavailable(dev) : std::string() });
        if (reason.empty())
            StartHandBack(dev);
        else
            HandBackBlocked(dev, reason);
    }
    else
        dev.handBackDiagnostics.Reset();

    if (dev.model == nullptr)
        return;

    if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
        DlssNr::PresentHostMustYield(dev.device).skip != NrSkip::None)
        StandDown(dev);
}

// The model's builder's submit (VkPresentModelDevice::submit), on the builder thread, which holds no lock of the
// host's: `cmd` on the lowest-index queue of `family` the application retrieved. That queue's present lock, then its
// submit lock, as a borrower takes them -- but in short tries: the present lock is never held while the submit lock is
// waited for (the queue may be the presenting one, whose next present takes its present lock without a bound), and
// within kBuildQueueLockWaitMs in all (VK_NOT_READY: nothing submitted). The host's trampoline below its own submit
// hook, one non-empty batch, the locks given back, then the fence waited for in slices of kBuildSliceMs, up to
// `timeoutNs` (VK_TIMEOUT), or until `cancelled` (VK_INCOMPLETE: the device is going); in both, the fence is left
// alone.
VkResult BuildSubmit(const DeviceFns& fn, VkDevice device, VkCommandBuffer cmd, uint32_t family, uint64_t timeoutNs,
                     const std::atomic<bool>& cancelled)
{
    VkQueue queue = VK_NULL_HANDLE;
    std::shared_ptr<QueueLocks> locks;
    for (const auto& [candidate, candidateLocks] : DeviceQueues(device))
    {
        if (candidateLocks->family == family && (locks == nullptr || candidateLocks->index < locks->index))
        {
            queue = candidate;
            locks = candidateLocks;
        }
    }
    if (queue == VK_NULL_HANDLE || fn.queueSubmit == nullptr)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (fn.createFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    VkResult result = VK_NOT_READY;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kBuildQueueLockWaitMs);
    while (result == VK_NOT_READY && !cancelled.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::unique_lock<QueueMutex> present(locks->present, std::defer_lock);
        std::unique_lock<QueueMutex> submit(locks->submit, std::defer_lock);
        ActivityStage("taking the work queue's present lock for the build (short tries)", kLockBorrowedPresent,
                      locks->present.Owner());
        if (!present.try_lock_for(std::chrono::milliseconds(kBuildSliceMs)))
            continue;
        ActivityHold(kLockBorrowedPresent, true);
        if (submit.try_lock())
        {
            ActivityHold(kLockBorrowedSubmit, true);
            ActivityStage("submitting the build's GPU work");
            VkSubmitInfo info { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            info.commandBufferCount = 1;
            info.pCommandBuffers = &cmd;
            result = fn.queueSubmit(queue, 1, &info, fence);
            submit.unlock();
            ActivityHold(kLockBorrowedSubmit, false);
        }
        present.unlock();
        ActivityHold(kLockBorrowedPresent, false);
        if (result == VK_NOT_READY)
        {
            ActivityStage("the work queue's submit lock is busy: retrying for the build", kLockBorrowedSubmit,
                          locks->submit.Owner());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (result != VK_SUCCESS)
    {
        if (result != VK_ERROR_DEVICE_LOST)
            fn.destroyFence(device, fence, nullptr); // never submitted; nothing is destroyed on a lost device
        return result == VK_NOT_READY && cancelled.load(std::memory_order_acquire) ? VK_INCOMPLETE : result;
    }

    ActivityStage("waiting for the build's GPU work (sliced, bounded)");
    const uint64_t slice = kBuildSliceMs * 1000000ull;
    for (uint64_t waited = 0;; waited += slice)
    {
        if (cancelled.load(std::memory_order_acquire))
            return VK_INCOMPLETE;
        result = fn.waitForFences(device, 1, &fence, VK_TRUE, slice);
        if (result != VK_TIMEOUT)
            break;
        if (waited + slice >= timeoutNs)
            return VK_TIMEOUT;
    }
    if (result == VK_SUCCESS)
        fn.destroyFence(device, fence, nullptr);
    return result;
}

struct NrDecision
{
    NrSkip skip = NrSkip::None;
    std::string note;
    uint32_t hdrProfile = 0;
};

// Whether this present's frame may be shown to the model, decided before anything of it is recorded. With `None`, the
// model exists, is admitted for this size, and the swapchain's working colour exists.
NrDecision DecideNr(DeviceTransport& dev, SwapchainTransport& sc, uint32_t workFamily)
{
    const auto refuse = [](NrSkip skip, std::string note) { return NrDecision { skip, std::move(note) }; };

    // Turned off live: nothing new is recorded, and the model is freed once nothing of it is in flight. It comes back
    // cold when NR is turned on again.
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        StandDown(dev);
        return refuse(NrSkip::Off, "[DlssNr] Enabled is off");
    }

    const auto colour =
        PlanColour(sc.colour.raw, sc.colourSpace, Config::Instance()->DlssNrHdrPresent.value_or_default());
    const auto observed = PlanColour(sc.colour.raw, sc.colourSpace, true);
    if (observed.profile != 0)
        DlssNr::NotePresentHdr(observed.profile == 1 ? DlssNr::PresentColour::Hdr10 : DlssNr::PresentColour::ScRgb,
                               nullptr, false);
    if (!colour.Eligible())
        return refuse(NrSkip::Colour, colour.refusal);
    if (!dev.nrReady)
        return refuse(NrSkip::Extensions, dev.nrMissing);
    if (std::string missing = MissingFormatFeatures(dev, sc.colour.raw, colour.profile != 0); !missing.empty())
        return refuse(NrSkip::FormatFeatures, std::move(missing));
    if (!sc.nrOff.empty())
        return refuse(NrSkip::Unavailable, sc.nrOff);
    if (dev.instance == VK_NULL_HANDLE)
        return refuse(NrSkip::Unavailable, "the device's instance is not on record");

    // Another route wants the model (the game's own upscaler on Vulkan, or the D3D12 route): stand down.
    if (dev.model != nullptr)
    {
        if (const auto yield = DlssNr::PresentHostMustYield(dev.device); yield.skip != NrSkip::None)
        {
            StandDown(dev);
            return refuse(yield.skip, yield.text);
        }
    }

    if (const auto claim = DlssNr::ClaimModelForPresentHost(dev.device); claim.skip != NrSkip::None)
        return refuse(claim.skip, claim.text);

    // One swapchain at a time: the model's size and history are one picture's. A live one keeps it; a retired or
    // destroyed one hands it over (a resize, a recreation), and the frame that takes it starts the history over. Taken
    // only once the model is admitted for this one, so a swapchain too small for it never holds it.
    if (dev.modelSwapchain != 0 && dev.modelSwapchain != sc.id)
    {
        for (const auto& [handle, other] : dev.swapchains)
        {
            if (other.id == dev.modelSwapchain && !other.retired)
                return refuse(NrSkip::OtherSwapchain,
                              "the model serves swapchain #" + std::to_string(dev.modelSwapchain) + " of this device");
        }
    }

    if (dev.model == nullptr)
    {
        DlssNr::VkPresentModelDevice device;
        device.instance = dev.instance;
        device.physical = dev.physical;
        device.device = dev.device;
        device.timestampValidBits = workFamily < dev.timestampBits.size() ? dev.timestampBits[workFamily] : 0;
        device.apiVersion = dev.apiVersion;
        // The builder's submits: the device's functions by value (they outlive no device: vkDestroyDevice waits for a
        // build under way before the device goes).
        device.submit = [fn = dev.fn, d = dev.device](VkCommandBuffer cmd, uint32_t family, uint64_t timeoutNs,
                                                      const std::atomic<bool>& cancelled)
        { return BuildSubmit(fn, d, cmd, family, timeoutNs, cancelled); };
        dev.model = std::make_unique<DlssNr::VkPresentModel>(device);
    }

    // A model that cannot run here holds nothing once nothing of it is in flight: its objects are freed (its reason
    // stays, so it is not brought up again every frame) and the token goes back, for another device or the
    // after-upscale route.
    const auto letGo = [&dev]()
    {
        // A build that lost the device says so: nothing of the device's is freed or called then.
        if (dev.model->DeviceLost())
            MarkLost(dev);
        if (dev.model->InFlight() == 0 && !dev.lost)
        {
            dev.model->Release();
            DlssNr::ReleaseModelFromPresentHost(dev.device);
        }
    };

    const DlssNr::VkPresentAdmission admission =
        dev.model->Admit(sc.extent.width, sc.extent.height, workFamily, colour.profile, colour.raw);
    TakeModelLines(dev);
    if (admission.skip != NrSkip::None)
    {
        if (admission.skip == NrSkip::Unavailable)
            letGo();
        // The swapchain a build was asked for is the one the model serves from now: another live one of the device
        // is refused (another swapchain) rather than asking for a build of its own size in turn.
        if (admission.skip == NrSkip::Building && dev.modelSwapchain != sc.id)
        {
            dev.modelSwapchain = sc.id;
            sc.nrCut = true;
        }
        return refuse(admission.skip, admission.text);
    }

    if (colour.profile == 0 && !EnsureWorkingImage(dev, sc))
    {
        sc.nrOff = "the working colour (" + FormatName(kWorkingFormat) + ") could not be made";
        letGo();
        return refuse(NrSkip::Unavailable, sc.nrOff);
    }

    if (dev.modelSwapchain != sc.id)
    {
        dev.modelSwapchain = sc.id;
        sc.nrCut = true;
    }

    return { NrSkip::None, {}, colour.profile };
}

// A recording of the model's pass, until it is known to have been submitted: anything else -- a refusal after the
// recording, a failed submit -- discards it, so the model never counts a frame that will not run.
struct NrRecording
{
    DeviceTransport* dev = nullptr;
    uint64_t ticket = 0;

    void Submitted(Slot& slot)
    {
        if (dev != nullptr && dev->model != nullptr && ticket != 0)
        {
            dev->model->Submitted(ticket);
            slot.nrTicket = ticket;
        }
        ticket = 0;
    }

    void Discard()
    {
        if (dev != nullptr && dev->model != nullptr && ticket != 0)
        {
            // A submit that lost the device may have run in part: nothing of the model's is released on it (a feature
            // built in this recording included), it is abandoned whole.
            if (dev->lost)
                dev->model->Abandon();
            else
                dev->model->Discarded(ticket);
            TakeModelLines(*dev);
        }
        ticket = 0;
    }

    ~NrRecording() { Discard(); }
};

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

    // What every return hands back besides its reason: the model's GPU times observed on the way, and its lines (any
    // its recording's discard adds after that are logged at the next present).
    GpuTimes times;
    DeviceTransport* devFound = nullptr;
    SwapchainTransport* scFound = nullptr;
    const auto flush = [&prep, &times, &devFound, &scFound]()
    {
        // A present the model did not see leaves its history behind the picture: the next one it sees starts over.
        if (scFound != nullptr && !prep.outcome.nrEvaluated)
            scFound->nrCut = true;

        prep.outcome.nrTimed = times.timed;
        prep.outcome.nrLastGpuMs = times.last;
        prep.outcome.nrMaxGpuMs = times.max;
        prep.outcome.nrSumGpuMs = times.sum;
        if (devFound != nullptr)
        {
            for (std::string& line : devFound->lines)
                prep.warnings.push_back(std::move(line));
            devFound->lines.clear();
        }
    };

    const auto skip = [&prep, &flush](Skip reason) -> Prepared&
    {
        prep.outcome.skip = reason;
        flush();
        return prep;
    };

    // Before any of the refusals below, which return before the transport is looked at: a model on this device stands
    // down when NR is off or another route wants it, whatever becomes of this present.
    {
        TransportLock lock;
        if (DeviceTransport* dev = FindDevice(prep.device))
        {
            ActivityStage("housekeeping (stand-down, hand-back)");
            Housekeep(*dev);
            // Early refusals below never assign devFound. Flush now, under the transport lock, so a due hand-back's
            // diagnostics are visible even when no swapchain can be carried. Log only after leaving Prepare.
            for (std::string& line : dev->lines)
                prep.warnings.push_back(std::move(line));
            dev->lines.clear();
        }
    }

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
    TransportLock lock;

    DeviceTransport* dev = FindDevice(prep.device);
    devFound = dev;
    if (dev == nullptr || !dev->off.empty())
        return skip(Skip::DeviceOff);

    const auto found = dev->swapchains.find(prep.swapchain);
    if (found == dev->swapchains.end() || !found->second.on)
        return skip(Skip::ObserveOnly);

    SwapchainTransport& sc = found->second;
    scFound = &sc;
    Outcome& out = prep.outcome;

    ActivityStage("polling the ring's fences");
    out.completions += Poll(*dev, sc, times);
    out.ring = static_cast<uint32_t>(sc.ring.size());
    if (!dev->off.empty())
        return skip(Skip::DeviceOff);

    // One work image: a round trip on another queue waits until the ring is drained.
    if (sc.lastWorkQueue != VK_NULL_HANDLE && sc.lastWorkQueue != workQueue)
    {
        ActivityStage("waiting for the ring's fences before a queue change (bounded)");
        for (Slot& slot : sc.ring)
        {
            if (slot.state == SlotState::Pending &&
                !WaitSlot(*dev, slot, kSlotWaitNs, out.waitNs, out.completions, times))
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
        ActivityStage("waiting for the oldest ring slot's fence (bounded)");
        if (!WaitSlot(*dev, sc.ring[index], kSlotWaitNs, out.waitNs, out.completions, times))
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

    // Stage 3: the model, when this frame may be shown to it, decided before anything is recorded. The swapchain's next
    // frame of the model starts the history over unless this one reaches it.
    ActivityStage("deciding the model's part (claim, admission)");
    const NrDecision nr = DecideNr(*dev, sc, route.workFamily);
    const bool cut = sc.nrCut;
    sc.nrCut = true;

    DlssNr::VkPresentNrFrame frame;
    NrRecording recording;
    if (nr.skip == NrSkip::None)
    {
        frame.cmd = slot.work;
        frame.hdrProfile = nr.hdrProfile;
        frame.image = frame.hdrProfile != 0 ? sc.work : sc.working;
        frame.view = sc.workingView;
        frame.width = sc.extent.width;
        frame.height = sc.extent.height;
        frame.ticket = dev->nextTicket++;
        frame.cut = cut;
        recording.dev = dev;
        recording.ticket = frame.ticket;
    }
    else
    {
        out.nrSkip = nr.skip;
        out.nrNote = nr.note;
    }

    const bool cross = route.route == Route::CrossFamily;
    ActivityStage("recording the round trip");
    Recorded work = RecordWork(*dev, slot.work, image, sc.work, sc.extent, route.route, presentFamily, route.workFamily,
                               recording.ticket != 0 ? &frame : nullptr);
    if (work == Recorded::NrFailed)
    {
        // The model failed after its recording began: thrown away, and the frame goes back unchanged. The model is
        // off on this device from now on (its reason is in its lines).
        recording.Discard();
        out.nrSkip = NrSkip::RecordFailed;
        out.nrNote = dev->model != nullptr ? dev->model->Failure() : std::string("the model's pass failed");
        work = RecordWork(*dev, slot.work, image, sc.work, sc.extent, route.route, presentFamily, route.workFamily);
    }

    if (work != Recorded::Ok ||
        (cross && (!RecordHandOver(*dev, slot.release, image, true, presentFamily, route.workFamily) ||
                   !RecordHandOver(*dev, slot.acquire, image, false, presentFamily, route.workFamily))))
        return skip(Skip::RecordFailed);

    if (cross && !RecordRecovery(*dev, slot.recover, image))
        return skip(Skip::RecordFailed);

    // A borrowed queue (cross-family and concurrent routes) is not inside the application's present: another of its
    // threads may be submitting to it or presenting on it. Its present lock, then its submit lock, each with a bound,
    // and only now, so a ring wait above never holds them. The presenting queue is the application's own present, whose
    // present lock the hook already holds.
    std::unique_lock<QueueMutex> borrowedPresent;
    std::unique_lock<QueueMutex> borrowedSubmit;
    if (workQueue != queue)
    {
        if (QueueLocks* locks = QueueLocksFor(workQueue))
        {
            borrowedPresent = std::unique_lock(locks->present, std::defer_lock);
            borrowedSubmit = std::unique_lock(locks->submit, std::defer_lock);
            ActivityStage("borrowing the work queue's present lock (bounded)", kLockBorrowedPresent,
                          locks->present.Owner());
            if (!borrowedPresent.try_lock_for(std::chrono::milliseconds(kQueueLockWaitMs)))
                return skip(Skip::QueueBusy);
            ActivityHold(kLockBorrowedPresent, true);
            ActivityStage("borrowing the work queue's submit lock (bounded)", kLockBorrowedSubmit,
                          locks->submit.Owner());
            if (!borrowedSubmit.try_lock_for(std::chrono::milliseconds(kQueueLockWaitMs)))
                return skip(Skip::QueueBusy);
            ActivityHold(kLockBorrowedSubmit, true);
        }
    }

    ActivityStage("submitting the round trip");
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
        recording.Submitted(slot);
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
        {
            chain = slot.toPresent;
            recording.Submitted(slot);
        }

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

    // The model's frame reached the GPU with the round trip: its history continues from it.
    if (slot.nrTicket != 0 && slot.nrTicket == frame.ticket)
    {
        out.nrEvaluated = true;
        out.nrReset = dev->model != nullptr && dev->model->LastFrameReset();
        out.nrSynthesized = dev->model != nullptr && dev->model->LastFrameSynthesized();
        out.nrSkip = NrSkip::None;
        out.nrNote.clear();
        sc.nrCut = false;
    }
    else if (frame.ticket != 0 && out.nrSkip == NrSkip::None)
    {
        out.nrSkip = NrSkip::RecordFailed;
        out.nrNote = "the work queue's submit failed; the model's frame was not run";
    }

    out.transported = true;
    out.skip = Skip::None;
    out.route = route.route;
    out.waitsTaken = info.waitSemaphoreCount;
    prep.wait = presentWait;
    flush();
    return prep;
}

// After the call down: a present that failed in a way that may have left its wait unexecuted makes the semaphore
// uncertain, and a lost device stops everything.
void AfterPresent(const Prepared& prep, VkResult result)
{
    if (!prep.outcome.transported)
        return;

    TransportLock lock;

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
    GpuTimes times;   // the model's pass GPU times read back from its last frames
    std::string text; // for the swapchain's last line

    // Present semaphores whose waits the swapchain's destruction proves done: freed once the call down returned.
    std::vector<VkSemaphore> afterDestroy;
};

// The swapchain is going: drain its round trips (bounded), free what is proven done, keep the rest. Its present
// semaphores: a valid vkDestroySwapchainKHR comes after every use of its images has completed
// (VUID-vkDestroySwapchainKHR-swapchain-01282), the presentation engine's included, and a present's wait on its
// semaphore precedes that use, so every one whose present was enqueued (not `uncertain`) is free once the destroy
// returned. Stage 2 kept them until the device went, unless a reacquire proved them free, and a game that recreates
// its swapchain hundreds of times reached the bound of kRetainedMax (nr-vulkan-present-host.md, "Stage 3 as built").
// `destroyed`: the swapchain itself is being destroyed (false: a handle on record whose destroy was not seen).
Retired RetireSwapchain(DeviceTransport& dev, SwapchainTransport& sc, bool destroyed)
{
    Retired retired;

    if (dev.modelSwapchain == sc.id)
        dev.modelSwapchain = 0;

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
            (spent >= kDrainNs || !WaitSlot(dev, slot, kDrainNs - spent, waitNs, completions, retired.times)))
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
        DestroyWorkImage(dev, sc.working, sc.workingMemory, &sc.workingView);
        retired.text = "freed after a drain of " + Milliseconds(Since(start)) + " ms";
    }
    else
    {
        Leftover left;
        left.slots = std::move(sc.ring);
        left.work = sc.work;
        left.workMemory = sc.workMemory;
        left.working = sc.working;
        left.workingMemory = sc.workingMemory;
        left.workingView = sc.workingView;
        dev.leftovers.push_back(std::move(left));
        retired.text = "the GPU did not finish within " + Milliseconds(kDrainNs) +
                       " ms; its objects are kept until the device goes";
    }

    sc.ring.clear();
    sc.work = VK_NULL_HANDLE;
    sc.workMemory = VK_NULL_HANDLE;
    sc.working = VK_NULL_HANDLE;
    sc.workingMemory = VK_NULL_HANDLE;
    sc.workingView = VK_NULL_HANDLE;

    size_t freed = 0;
    size_t kept = 0;
    for (size_t i = 0; i < sc.presentSemaphores.size(); ++i)
    {
        const VkSemaphore semaphore = sc.presentSemaphores[i];
        if (semaphore == VK_NULL_HANDLE)
            continue;

        if (sc.used[i] == 0 || (destroyed && sc.uncertain[i] == 0))
        {
            retired.afterDestroy.push_back(semaphore);
            ++freed;
        }
        else
        {
            dev.retained.push_back(semaphore);
            ++kept;
        }
    }

    sc.presentSemaphores.clear();
    retired.text += "; present semaphores " + std::to_string(freed) + " freed with it, " + std::to_string(kept) +
                    " kept until the device goes (a present that failed may not have waited on them)";

    if (dev.retained.size() > kRetainedMax && dev.off.empty())
        dev.off = "more than " + std::to_string(kRetainedMax) + " present semaphores retained";

    return retired;
}

// A lost device's model: every handle forgotten, nothing destroyed, the token given back, NGX on the device only
// forgotten (nothing can be called on it).
void AbandonModel(DeviceTransport& dev)
{
    if (dev.model != nullptr)
    {
        dev.model->Abandon();
        TakeModelLines(dev);
        dev.model.reset();
    }
    EndHandBack(dev, 0);
    DlssNr::ReleaseModelFromPresentHost(dev.device);
    DlssNr::AfterUpscaleDeviceGone(dev.device, true, dev.lines);
    DlssNr::ShutdownPresentNgx(dev.device, true, dev.lines);
}

// The device is going: whatever the application left, then everything the host kept. Returns the line.
// At vkDestroyDevice, before anything of the model's: a build under way is waited for (kBuildDestroyWaitMs, longer than
// every bound of the builder's own -- a lost device ends it at once, its calls failing), and if it does not end it is
// orphaned and waited for again (kBuildOrphanWaitMs: every wait of the builder's is sliced and checks the flag). Only a
// builder stuck inside an opaque NGX call outlasts that, and the device then goes under it, said in the log.
void EndModelBuild(DeviceTransport& dev)
{
    if (dev.model == nullptr || !dev.model->Building())
        return;

    ActivityStage("waiting for the model's build under way (bounded)");
    const auto start = std::chrono::steady_clock::now();
    if (dev.model->WaitBuild(kBuildDestroyWaitMs))
    {
        dev.lines.push_back(DeviceName(dev) + ": waited " + std::to_string(Since(start) / 1000000) +
                            " ms for the model's build under way before the device goes");
        return;
    }

    dev.model->Orphan();
    ActivityStage("waiting for the orphaned model build to step out of the device (bounded)");
    const bool out = dev.model->WaitBuild(kBuildOrphanWaitMs);
    dev.lines.push_back(
        DeviceName(dev) + ": the model's build did not end within " + std::to_string(kBuildDestroyWaitMs / 1000) +
        " s of the device's destruction; orphaned" +
        (out ? ", and it stepped out" : ", and it is still inside an NGX call: the device goes anyway"));
    ActivityStage("tearing the device's objects down");
}

std::string TeardownDevice(DeviceTransport& dev)
{
    EndModelBuild(dev);
    if (dev.model != nullptr && dev.model->DeviceLost())
        MarkLost(dev); // a build saw it lost: nothing below is waited for or destroyed

    size_t swapchains = 0;
    std::vector<VkSemaphore> semaphores;
    for (auto& [handle, sc] : dev.swapchains)
    {
        // At device destruction every present semaphore goes: the application has finished with the device.
        Retired retired = RetireSwapchain(dev, sc, true);
        semaphores.insert(semaphores.end(), retired.afterDestroy.begin(), retired.afterDestroy.end());
        ++swapchains;
    }
    dev.swapchains.clear();
    dev.retained.insert(dev.retained.end(), semaphores.begin(), semaphores.end());

    if (dev.lost)
    {
        AbandonModel(dev);
        return "the device was lost: every object of the host's on it is abandoned, none destroyed";
    }

    size_t leftoverSlots = 0;
    size_t abandoned = 0;
    const auto start = std::chrono::steady_clock::now();
    uint64_t waitNs = 0;
    uint32_t completions = 0;
    GpuTimes times;

    for (Leftover& left : dev.leftovers)
    {
        bool done = true;
        for (Slot& slot : left.slots)
        {
            const uint64_t spent = Since(start);
            if (slot.state == SlotState::Abandoned ||
                (slot.state == SlotState::Pending &&
                 (spent >= kDrainNs || !WaitSlot(dev, slot, kDrainNs - spent, waitNs, completions, times))))
                done = false;
        }

        if (done && !dev.lost)
        {
            for (Slot& slot : left.slots)
                DestroySlot(dev, slot);
            DestroyWorkImage(dev, left.work, left.workMemory);
            DestroyWorkImage(dev, left.working, left.workingMemory, &left.workingView);
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
    {
        AbandonModel(dev);
        return "the device was lost during the last drain: every object of the host's on it is abandoned";
    }

    // The model: freed when every frame it recorded was seen complete, abandoned (nothing destroyed) when one never
    // was. Then NGX on the device: shut down after a model freed (or one that stood down earlier, or never existed here
    // -- the forwarder answers only for the device it initialised), only forgotten after one abandoned. A build under
    // way is waited for first, bounded (the builder takes no lock of the host's, so this cannot close a cycle); one
    // that does not end in time leaves the model in flight, so it is abandoned and its job orphaned.
    std::string model;
    bool forgetOnly = false;
    if (dev.model != nullptr)
    {
        const size_t inFlight = dev.model->InFlight();
        if (inFlight == 0)
        {
            dev.model->Release();
            model = "; the model freed";
        }
        else
        {
            dev.model->Abandon();
            forgetOnly = true;
            model = "; the model abandoned (" + std::to_string(inFlight) + " of its frames never seen complete)";
        }
        TakeModelLines(dev);
        dev.model.reset();
    }
    // A hand-back under way: its fences within what is left of the drain's bound, then the hand-back off. The
    // after-upscale route's objects on the device go with it, before NGX on it is shut down.
    const uint64_t spent = Since(start);
    const bool drained = EndHandBack(dev, spent < kDrainNs ? kDrainNs - spent : 0);
    DlssNr::ReleaseModelFromPresentHost(dev.device);
    // The route's objects freed, and NGX shut down on the device, only where the GPU is proven done: not a lost device,
    // none with work of the host's never seen complete (its model's frames, a retired swapchain's slots, the
    // hand-back's fences), and every queue drained now within its own bound. Otherwise both only forget the device.
    const bool unproven = dev.lost || forgetOnly || abandoned != 0 || !drained || !DrainForDestroy(dev, kDrainNs);
    DlssNr::AfterUpscaleDeviceGone(dev.device, unproven, dev.lines);
    DlssNr::ShutdownPresentNgx(dev.device, unproven, dev.lines);

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
           (dev.leaked ? "; command pools kept: a command buffer in them never finished" : "") + model;
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
    HostActivity activity("vkDestroyDevice", H(device));
    std::string line;
    std::vector<std::string> modelLines;
    {
        TransportLock lock;
        if (DeviceTransport* dev = FindDevice(device))
        {
            line = "DLSS-NR Vulkan present host: device " + Hex(H(device)) + " #" + std::to_string(dev->id) +
                   " destroyed; " + TeardownDevice(*dev);
            modelLines.swap(dev->lines);
            g_transport.erase(device);
        }
    }

    Log(modelLines);

    {
        std::lock_guard lock(g_registryMutex);
        g_registry.OnDeviceDestroyed(H(device));
    }

    EraseQueueLocks(device);

    if (!line.empty())
        LOG_INFO("{}", line);

    activity.scope.Stage("calling down vkDestroyDevice");
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

        AddQueueLock(device, *queue, family, index);
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

        AddQueueLock(device, *queue, info->queueFamilyIndex, info->queueIndex);
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

    HostActivity activity("vkCreateSwapchainKHR", H(device));

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
        TransportLock lock;
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

    // The work image is in the raw format: the swapchain's own, or its UNORM twin when it is sRGB (stage 3).
    const ColourPlan colour =
        PlanColour(static_cast<uint32_t>(info->imageFormat), static_cast<uint32_t>(info->imageColorSpace),
                   Config::Instance()->DlssNrHdrPresent.value_or_default());

    if (physical != 0)
    {
        query.formatAllows = FormatAllows(pd, info->imageFormat, Wanted(info->imageUsage), ImageFlagsOf(info->flags));
        query.workAllows = FormatAllows(pd, static_cast<VkFormat>(colour.raw),
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0);
    }

    const UsagePlan plan = PlanUsage(query);

    // Only the usage changes, and only when the plan says the round trip can run: everything else of the create
    // info, its pNext chain and oldSwapchain included, goes down as the application made it.
    VkSwapchainCreateInfoKHR edited = *info;
    edited.imageUsage = plan.usage;
    const VkSwapchainCreateInfoKHR* down = plan.transport && plan.added != 0 ? &edited : info;

    activity.scope.Stage("calling down vkCreateSwapchainKHR");
    const VkResult result = next(device, down, allocator, swapchain);
    activity.scope.Stage("recording the swapchain");

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
    RecordId swapchainId = 0;
    {
        std::lock_guard lock(g_registryMutex);
        swapchainId = g_registry.OnSwapchainCreated(H(device), H(*swapchain), std::move(desc));
        if (!internal)
            line = g_registry.SwapchainLine(H(device), H(*swapchain));
    }

    // What the model may be shown: SDR, or a supported HDR profile with HdrPresent. The round trip runs either way.
    if (!internal && plan.transport)
        line += colour.Eligible() ? "; the model may run on it (" + colour.formatName +
                                        (colour.profile == 1   ? ", HDR10 PQ, Rec.2020)"
                                         : colour.profile == 2 ? ", scRGB linear)"
                                                               : ", sRGB non-linear)")
                                  : "; the model will not run on it: " + colour.refusal;

    {
        TransportLock lock;
        if (DeviceTransport* dev = FindDevice(device))
        {
            SwapchainTransport& sc = dev->swapchains[*swapchain];
            if (!sc.ring.empty() || !sc.presentSemaphores.empty() || sc.work != VK_NULL_HANDLE ||
                sc.working != VK_NULL_HANDLE)
            {
                // A handle on record whose destroy was never seen: only semaphores never handed to a present go now.
                const Retired stale = RetireSwapchain(*dev, sc, false);
                if (!dev->lost)
                {
                    for (const VkSemaphore semaphore : stale.afterDestroy)
                        dev->fn.destroySemaphore(dev->device, semaphore, nullptr);
                }
            }
            sc = SwapchainTransport {};
            sc.on = plan.transport;
            sc.format = info->imageFormat;
            sc.extent = info->imageExtent;
            sc.id = swapchainId;
            sc.colour = colour;
            sc.colourSpace = static_cast<uint32_t>(info->imageColorSpace);

            // The swapchain it replaces is retired: the model may move on from it.
            if (info->oldSwapchain != VK_NULL_HANDLE)
            {
                if (const auto old = dev->swapchains.find(info->oldSwapchain); old != dev->swapchains.end())
                    old->second.retired = true;
            }
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
    HostActivity activity("vkDestroySwapchainKHR", H(swapchain));
    if (swapchain != VK_NULL_HANDLE)
    {
        // Its images are about to go: every round trip on them must be over first (bounded; kept if not).
        Retired retired;
        bool known = false;
        std::vector<std::string> modelLines;
        {
            TransportLock lock;
            if (DeviceTransport* dev = FindDevice(device))
            {
                if (const auto found = dev->swapchains.find(swapchain); found != dev->swapchains.end())
                {
                    known = true;
                    activity.scope.Stage("retiring the swapchain's slots (bounded drain)");
                    retired = RetireSwapchain(*dev, found->second, true);
                    dev->swapchains.erase(found);
                }
                modelLines.swap(dev->lines);
            }
        }

        std::string line;
        {
            std::lock_guard lock(g_registryMutex);
            g_registry.OnCompletions(H(device), H(swapchain), retired.completions, retired.times.timed,
                                     retired.times.last, retired.times.max, retired.times.sum);
            if (known)
                line = g_registry.SwapchainSummary(H(device), H(swapchain), retired.text);
            g_registry.OnSwapchainDestroyed(H(device), H(swapchain));
        }

        Log(modelLines);
        if (!line.empty())
            LOG_INFO("{}", line);

        activity.scope.Stage("calling down vkDestroySwapchainKHR");
        next(device, swapchain, allocator);

        // After the destroy returned, so an implementation that finishes its pending presents inside it has.
        if (!retired.afterDestroy.empty())
        {
            TransportLock lock;
            if (DeviceTransport* dev = FindDevice(device); dev != nullptr && !dev->lost)
            {
                for (const VkSemaphore semaphore : retired.afterDestroy)
                    dev->fn.destroySemaphore(dev->device, semaphore, nullptr);
            }
        }

        return;
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

    HostActivity activity("vkQueuePresentKHR", H(queue));

    // For the whole present, the call down included: a thread borrowing this queue for a round trip waits for it.
    std::unique_lock<QueueMutex> presentLock;
    if (QueueLocks* locks = QueueLocksFor(queue))
    {
        activity.scope.Stage("waiting for the presenting queue's present lock", kLockPresent, locks->present.Owner());
        presentLock = std::unique_lock(locks->present);
        activity.scope.Hold(kLockPresent, true);
    }

    const CallerInfo& who = WhoCalled(returnAddress);

    PresentCall call;
    call.nowMs = PresentNowMs();
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

    activity.scope.Stage("preparing the round trip");
    const Prepared prep = Prepare(queue, *info, viewed ? &view : nullptr);
    activity.scope.Hold(kLockBorrowedPresent | kLockBorrowedSubmit, false); // Prepare's own scope gave them back
    activity.scope.Stage("recording the present");
    call.outcome = prep.outcome;

    std::vector<std::string> lines;
    {
        std::lock_guard lock(g_registryMutex);
        lines = g_registry.OnPresent(call);
    }

    Log(prep.warnings);
    Log(lines);

    activity.scope.Stage("calling down vkQueuePresentKHR");
    if (prep.wait == VK_NULL_HANDLE)
        return next(queue, info);

    // A copy of the caller's present info, its pNext chain (present fences, present modes, present ids) by pointer,
    // with one change: the wait.
    VkPresentInfoKHR local = *info;
    local.waitSemaphoreCount = 1;
    local.pWaitSemaphores = &prep.wait;

    const VkResult result = next(queue, &local);
    activity.scope.Stage("after the present");
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
    return LockedQueueCall("vkQueueSubmit", g_submit.next[S], queue, count, submits, fence);
}

template <size_t S>
VkResult VKAPI_CALL hkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence)
{
    return LockedQueueCall("vkQueueSubmit2", g_submit2.next[S], queue, count, submits, fence);
}

template <size_t S>
VkResult VKAPI_CALL hkQueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* binds, VkFence fence)
{
    return LockedQueueCall("vkQueueBindSparse", g_bindSparse.next[S], queue, count, binds, fence);
}

template <size_t S> VkResult VKAPI_CALL hkQueueWaitIdle(VkQueue queue)
{
    return LockedQueueCall("vkQueueWaitIdle", g_waitIdle.next[S], queue);
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

// Whether an instance can take the model's device extensions' dependencies: Vulkan 1.1, or
// VK_KHR_get_physical_device_properties2 enabled on it.
bool HasProperties2(const VkInstanceCreateInfo* info)
{
    if (info == nullptr)
        return false;

    if (info->pApplicationInfo != nullptr && VK_API_VERSION_MAJOR(info->pApplicationInfo->apiVersion) == 1 &&
        VK_API_VERSION_MINOR(info->pApplicationInfo->apiVersion) >= 1)
        return true;

    for (uint32_t i = 0; info->ppEnabledExtensionNames != nullptr && i < info->enabledExtensionCount; ++i)
    {
        if (std::string_view(info->ppEnabledExtensionNames[i]) ==
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)
            return true;
    }

    return false;
}

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
            id = g_registry.OnInstanceCreated(
                H(*instance), internal, HasProperties2(info),
                info != nullptr && info->pApplicationInfo != nullptr ? info->pApplicationInfo->apiVersion : 0);
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

// Whether the application enabled shaderStorageImageWriteWithoutFormat, in pEnabledFeatures or a
// VkPhysicalDeviceFeatures2 in its pNext chain.
bool StorageWithoutFormatEnabled(const VkDeviceCreateInfo* info)
{
    if (info->pEnabledFeatures != nullptr && info->pEnabledFeatures->shaderStorageImageWriteWithoutFormat)
        return true;

    for (auto chain = static_cast<const VkBaseInStructure*>(info->pNext); chain != nullptr; chain = chain->pNext)
    {
        if (chain->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 &&
            reinterpret_cast<const VkPhysicalDeviceFeatures2*>(chain)->features.shaderStorageImageWriteWithoutFormat)
            return true;
    }

    return false;
}

// What the application's create info says about bufferDeviceAddress.
BdaChain ReadBdaChain(const VkDeviceCreateInfo* info)
{
    BdaChain chain;

    for (auto node = static_cast<const VkBaseInStructure*>(info->pNext); node != nullptr; node = node->pNext)
    {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)
        {
            chain.khrNode = true;
            chain.enabled =
                chain.enabled ||
                reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(node)->bufferDeviceAddress;
        }
        else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT)
        {
            chain.extNode = true;
            chain.enabled =
                chain.enabled ||
                reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeaturesEXT*>(node)->bufferDeviceAddress;
        }
        else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            chain.v12Node = true;
            chain.enabled =
                chain.enabled || reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->bufferDeviceAddress;
        }
    }

    return chain;
}

// Whether the physical device offers bufferDeviceAddress: vkGetPhysicalDeviceFeatures2, or its KHR name on a Vulkan
// 1.0 instance with VK_KHR_get_physical_device_properties2.
bool BdaSupported(VkInstance instance, VkPhysicalDevice physical, uint32_t apiVersion)
{
    if (g_getInstanceProcAddr == nullptr || instance == VK_NULL_HANDLE)
        return false;

    const bool vulkan11 = VK_API_VERSION_MAJOR(apiVersion) > 1 || VK_API_VERSION_MINOR(apiVersion) >= 1;
    const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        g_getInstanceProcAddr(instance, vulkan11 ? "vkGetPhysicalDeviceFeatures2" : "vkGetPhysicalDeviceFeatures2KHR"));
    if (features2 == nullptr)
        return false;

    VkPhysicalDeviceBufferDeviceAddressFeatures bda {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features.pNext = &bda;
    features2(physical, &features);
    return bda.bufferDeviceAddress == VK_TRUE;
}

// What the physical device offers, by name.
std::vector<std::string> SupportedDeviceExtensions(VkPhysicalDevice physical)
{
    std::vector<std::string> names;
    uint32_t count = 0;

    if (g_enumerateDeviceExtensions == nullptr ||
        g_enumerateDeviceExtensions(physical, nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        return names;

    std::vector<VkExtensionProperties> properties(count);
    if (g_enumerateDeviceExtensions(physical, nullptr, &count, properties.data()) != VK_SUCCESS)
        return names;

    for (uint32_t i = 0; i < count; ++i)
        names.emplace_back(properties[i].extensionName);
    return names;
}

// Per queue family, how many bits of a timestamp are valid: 0 where timestamps are not supported.
std::vector<uint32_t> TimestampBits(VkPhysicalDevice physical)
{
    std::vector<uint32_t> bits;
    uint32_t count = 0;

    if (g_getQueueFamilies == nullptr || physical == VK_NULL_HANDLE)
        return bits;

    g_getQueueFamilies(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    g_getQueueFamilies(physical, &count, properties.data());

    for (uint32_t i = 0; i < count; ++i)
        bits.push_back(properties[i].timestampValidBits);
    return bits;
}

VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                   const VkAllocationCallbacks* allocator, VkDevice* device)
{
    void* const returnAddress = _ReturnAddress();
    const bool internal = Internal();
    HostActivity activity("vkCreateDevice");

    // Stage 3: on a device the host will carry presents on, the model's device extensions the device offers are
    // appended to a copy of the create info -- exactly the after-upscale route's list, decided before the call and
    // never by retrying one: a device's extensions are fixed at creation. Everything else goes down as it came; no
    // feature or queue is added. OptiScaler's own devices get nothing.
    ExtensionPlan extensions;
    BdaAction bda = BdaAction::Already;
    VkDeviceCreateInfo withExtensions {};
    std::vector<const char*> names;
    VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    VkPhysicalDeviceBufferDeviceAddressFeaturesEXT bdaFeaturesExt {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT
    };
    bool edited = false;
    if (!internal && info != nullptr)
    {
        std::vector<std::string> asked;
        for (uint32_t i = 0; info->ppEnabledExtensionNames != nullptr && i < info->enabledExtensionCount; ++i)
            asked.emplace_back(info->ppEnabledExtensionNames[i]);

        bool properties2 = false;
        uint32_t apiVersion = 0;
        Handle instanceOfPhysical = 0;
        {
            std::lock_guard lock(g_registryMutex);
            properties2 = g_registry.Properties2Of(H(physical));
            apiVersion = g_registry.ApiVersionOf(H(physical));
            instanceOfPhysical = g_registry.InstanceOf(H(physical));
        }

        const auto atLeast = [apiVersion](uint32_t minor)
        {
            return VK_API_VERSION_MAJOR(apiVersion) > 1 ||
                   (VK_API_VERSION_MAJOR(apiVersion) == 1 && VK_API_VERSION_MINOR(apiVersion) >= minor);
        };

        DeviceCreateFacts facts;
        facts.properties2 = properties2;
        facts.vulkan11 = atLeast(1);
        facts.storageWithoutFormat = atLeast(3) || StorageWithoutFormatEnabled(info);
        for (auto node = static_cast<const VkBaseInStructure*>(info->pNext); node != nullptr; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES)
            {
                facts.vulkan14Features = true;
                facts.pushDescriptorFeature =
                    reinterpret_cast<const VkPhysicalDeviceVulkan14Features*>(node)->pushDescriptor == VK_TRUE;
            }
        }

        extensions = PlanDeviceExtensions(asked, SupportedDeviceExtensions(physical), facts);

        // The feature the model's allocations need, only where its extension will be there: on a device the model
        // cannot use anyway, nothing is added for it.
        if (extensions.ready)
            bda = PlanBufferDeviceAddress(ReadBdaChain(info),
                                          BdaSupported(As<VkInstance>(instanceOfPhysical), physical, apiVersion),
                                          Listed(asked, kBdaExt));

        if (!extensions.add.empty() || bda == BdaAction::AddKhr || bda == BdaAction::AddExt)
        {
            withExtensions = *info;
            edited = true;
        }

        if (!extensions.add.empty())
        {
            for (uint32_t i = 0; info->ppEnabledExtensionNames != nullptr && i < info->enabledExtensionCount; ++i)
                names.push_back(info->ppEnabledExtensionNames[i]);
            for (const std::string& name : extensions.add)
                names.push_back(name.c_str());
            withExtensions.enabledExtensionCount = static_cast<uint32_t>(names.size());
            withExtensions.ppEnabledExtensionNames = names.data();
        }

        // Prepended to the application's chain, which goes down as it came behind it.
        if (bda == BdaAction::AddKhr)
        {
            bdaFeatures.bufferDeviceAddress = VK_TRUE;
            bdaFeatures.pNext = const_cast<void*>(info->pNext);
            withExtensions.pNext = &bdaFeatures;
        }
        else if (bda == BdaAction::AddExt)
        {
            bdaFeaturesExt.bufferDeviceAddress = VK_TRUE;
            bdaFeaturesExt.pNext = const_cast<void*>(info->pNext);
            withExtensions.pNext = &bdaFeaturesExt;
        }
    }

    const VkResult result = o_CreateDevice(physical, edited ? &withExtensions : info, allocator, device);

    if (edited && result != VK_SUCCESS)
        LOG_WARN("DLSS-NR Vulkan present host: vkCreateDevice returned {} with the model's extensions ({}) and {}",
                 static_cast<int>(result), JoinNames(extensions.add), BdaText(bda));

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
    VkInstance instance = VK_NULL_HANDLE;
    uint32_t instanceApiVersion = 0;
    {
        std::lock_guard lock(g_registryMutex);
        id = g_registry.OnDeviceCreated(H(*device), std::move(desc));
        line = g_registry.DeviceLine(H(*device));
        instance = As<VkInstance>(g_registry.InstanceOf(H(physical)));
        instanceApiVersion = g_registry.ApiVersionOf(H(physical));
    }

    // OptiScaler's own devices get no transport: nothing of theirs is ever transported.
    if (!internal)
    {
        auto dev = MakeDeviceTransport(*device, physical, id);
        dev->instance = instance;
        dev->apiVersion = instanceApiVersion;
        dev->nrReady = extensions.ready && BdaReady(bda);
        dev->nrMissing = !extensions.ready
                             ? "the device was created without the model's extensions: " + JoinNames(extensions.missing)
                             : std::string("the device was created without the model's features: ") + BdaText(bda);
        dev->timestampBits = TimestampBits(physical);
        line += "; " + ExtensionText(extensions);
        if (extensions.ready)
            line += std::string("; ") + BdaText(bda);
        if (!dev->off.empty())
            line += "; round trip off on it: " + dev->off;

        TransportLock lock;
        g_transport[*device] = std::move(dev);
    }

    LOG_INFO("{}", line);

    HookDevice(*device);

    // The round trip's own submits go through the host's trampoline of the device's vkQueueSubmit. The next layer
    // can still be Vulkan_wDx12 (as in DOOM), so even fence-only submits need a non-null submit array. The borrowed
    // queue's host locks are taken explicitly.
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

        TransportLock lock;
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
    g_getFormatProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(get("vkGetPhysicalDeviceFormatProperties"));
    g_enumerateDeviceExtensions =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get("vkEnumerateDeviceExtensionProperties"));
    g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));

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
        g_getFormatProperties = nullptr;
        g_enumerateDeviceExtensions = nullptr;
        g_getInstanceProcAddr = nullptr;
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

    // The stall watchdog: every half second, an activity inside the host for longer than its allowance is logged with
    // its stage, the locks it holds and, for a lock it waits on, the holder. It reads the table with atomics only and
    // takes no lock of the host's, so it cannot be caught in the stall it reports. Only with the host installed.
    std::thread(
        []()
        {
            TagThread();
            for (;;)
            {
                Sleep(500);
                for (const std::string& line : Activities().Check(ActivityNowMs()))
                    LOG_WARN("DLSS-NR Vulkan present host: STALL: {}", line);
            }
        })
        .detach();

    LOG_INFO("DLSS-NR Vulkan present host (cold NR on zero guides; stage 3 of nr-vulkan-present-host.md): installed "
             "on vulkan-1.dll, {}",
             State::Instance().vulkanHooksSkipped
                 ? "while the rest of OptiScaler's Vulkan hooks stay off ([Vulkan] SkipHooks or the marker); this "
                   "host is the one exception"
                 : "beside OptiScaler's own Vulkan hooks");
}
