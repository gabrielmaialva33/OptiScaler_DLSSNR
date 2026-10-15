#include "pch.h"

#include "DlssNrFeature_Vk.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <NVNGX_Parameter.h>
#include <proxies/NVNGX_Proxy.h>
#include <dlssnr/DlssNr_Consumers.h>
#include <dlssnr/DlssNrFeature_Dx12.h>
#include <dlssnr/DlssNr_VkModelOwner.h>
#include <dlssnr/DlssNr_VkPresentModel.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_ModelLog.h>
#include <dlssnr/DlssNr_Session.h>
#include <dlssnr/DlssNr_RenderSize.h>
#include <dlssnr/DlssNr_WorkingScale.h>

#include <shaders/dlssnr/DlssNr_Vk.h>
#include <shaders/dlssnr/DlssNr_GuideMatch.h>
#include <shaders/output_scaling/OS_Vk.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace DlssNr
{

namespace
{

// The forwarder's Vulkan surface. The model checks its caller's module path and requires nvngx.dll in
// it, whichever API is being used, so these calls go through the same shim the D3D12 path does.
using PFN_VkProbe = int(__cdecl*)(const wchar_t*);
using PFN_VkInit = int(__cdecl*)(const wchar_t*, const wchar_t*, void*, void*, void*, int);
using PFN_VkCreate = void*(__cdecl*) (void*, void*, unsigned int, unsigned int, int, float, int, float, float, float,
                                      int, int);
using PFN_VkEvaluate = int(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, unsigned int, unsigned int,
                                     unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
                                     unsigned int, unsigned int, int, int, float, int, float, float, float, int, float,
                                     float);
using PFN_VkRelease = void(__cdecl*)(void*);
using PFN_NrSetFloatSlot = void(__cdecl*)(int);
using PFN_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);

// One image this pass owns: the storage, the view, and the NGX wrapper that describes it. Kept
// together because they are created, resized and destroyed as one thing.
struct OwnedImage
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    NVSDK_NGX_Resource_VK ngx {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    bool Valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }
};

// The forwarder, once per process: one DLL, one snippet inside it, one fault record and one float slot, whichever
// route calls it -- the after-upscale route below, or the Vulkan present host's model (VkPresentModel). Loaded under
// its own lock, because the two are called from different threads under different locks.
struct Forwarder
{
    std::mutex mutex;
    bool tried = false;
    const char* failure = nullptr; // why it is not usable, latched for the process

    HMODULE module = nullptr;
    PFN_VkProbe probe = nullptr;
    PFN_VkInit init = nullptr;
    PFN_VkCreate create = nullptr;
    PFN_VkEvaluate evaluate = nullptr;
    PFN_VkRelease release = nullptr;

    // The forwarder's record of a fault caught inside the model. Null on a forwarder that predates it.
    int (*faultState)(unsigned long*, void**) = nullptr;

    // Where a parameter block keeps floats, and the forwarder calls that find out. Optional: an older
    // forwarder lacks them and the floats go to the header's slot, as they always did here.
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;

    // NGX's shutdown on one device (NVSDK_NGX_VULKAN_Shutdown1), so the forwarder forgets the device it initialised.
    // Absent from a forwarder older than the present host's stage 3.
    int(__cdecl* shutdown)(void*) = nullptr;
};

Forwarder g_fwd;

struct VkState
{
    bool failed = false;
    const char* reason = "";

    // Whether the float slot was found for this owner's parameter block (the forwarder's slot is one, the blocks may
    // differ).
    bool floatSlotKnown = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    bool ngxInitialised = false;
    void* feature = nullptr;
    NVSDK_NGX_Parameter* capabilityParams = nullptr;

    // What the model writes, the proxy it is shown, and the frame as the upscaler left it.
    OwnedImage output;
    OwnedImage proxy;
    OwnedImage keep;

    // The proxy at the model's working size, when that is below the frame. The model -- 98% of the
    // cost -- then runs on this instead of the full proxy, which is the whole point of the working
    // scale slider. Unused (and never created) at scale 1, so the default path is unchanged.
    OwnedImage proxySmall;

    // Supersampling (working scale > 1): the model runs above native, superUp enlarges the proxy to
    // that size and superDown averages the answer (output) back into outputNative at native for a 1:1
    // composite. nrScaler is the filter both were built with, so a changed DlssNrScalingDownscaler
    // rebuilds them. Unused and never created at scale <= 1.
    OwnedImage outputNative;
    std::unique_ptr<OS_Vk> superUp;
    std::unique_ptr<OS_Vk> superDown;
    Scaler nrScaler = Scaler::Count;

    std::unique_ptr<DlssNr_Vk> pass;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;
    unsigned long long frames = 0;

    // Timing. A pair of timestamps per frame across a ring, read back three frames later: a query
    // read the frame it was written stalls the CPU on the GPU, which would cost more than the pass
    // it is measuring. Vulkan reports ticks, and timestampPeriod is how many nanoseconds a tick is.
    VkQueryPool queryPool = VK_NULL_HANDLE;
    float timestampPeriod = 0.0f;
    unsigned long long timedFrames = 0;
    std::optional<double> lastGpuTime;

    // Whether the game hands over an exposure texture, and what it said when it did.
    bool exposureOffered = false;

    // The game's own exposure, read off its 1x1 texture, and the scale it multiplied its buffer by.
    //
    // gameExposure holds its last good value rather than resetting when a frame arrives without a
    // texture: GTA V dropped it three times in one session on the D3D12 path, and falling back to a
    // default on those frames is a flicker, not a fallback.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;
    DlssNr::Exposure::PlausibilityLatch exposureLatch;
    bool exposureSettingWasOn = false;

    // The exposure's courier: an 8x8 R32_FLOAT image the meter writes, and a ring of host-visible
    // buffers it is copied into. Only texel (0,0) is ever read -- the rest of the grid belongs to the
    // frame-statistics meter that was removed from the shared shader, and 8x8 is here only so that a
    // single 8x8 thread group lands entirely inside the image.
    OwnedImage meter;
    VkBuffer meterReadback[4] = {};
    VkDeviceMemory meterReadbackMemory[4] = {};
    void* meterMapped[4] = {};
    unsigned long long meterFrames = 0;
};

// The grid the meter writes, and the size of one readback. 8 * 8 * sizeof(float).
constexpr uint32_t kMeterSide = 8;
constexpr VkDeviceSize kMeterBytes = kMeterSide * kMeterSide * sizeof(float);

// Four, so the slot being read is four frames behind the slot being written and the read never waits
// on the GPU. Same depth as the D3D12 meter's ring, for the same reason.
constexpr unsigned long long kMeterSlots = 4;

// Four frames of pairs. Three would do, four keeps the modulo cheap and the slot being written well
// clear of the slot being read.
constexpr uint32_t kTimingSlots = 4;

VkState g_vk;
std::mutex g_vkMutex;

// Set whenever an evaluate finds the pass switched off, taken under the lock by the next one that
// runs, so switching NR back on starts the model's history over instead of resuming the one from
// before. Same rule as g_resetOnReturn on the D3D12 side.
std::atomic<bool> g_vkResetOnReturn { false };

// Whether the last evaluate's buffer was linear HDR. For the menu, which offers highlight recovery only where it
// can act; the same as g_lastLinearHdr on the D3D12 side.
std::atomic<bool> g_vkLinearHdr { false };

// One owner of the model in the process: the after-upscale route above, or the Vulkan present host's model.
std::mutex g_ownerMutex;
VkModelOwner::Arbiter g_owner;

uint64_t OwnerNowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The after-upscale route, at an evaluate it is about to run on `device`, with g_vkMutex held. False while the present
// host holds the model.
bool ClaimModelForAfterUpscale(VkDevice device)
{
    VkModelOwner::Verdict verdict;
    bool held = false;
    {
        std::lock_guard<std::mutex> lock(g_ownerMutex);
        held = g_owner.Owner() == VkModelOwner::Who::AfterUpscale;
        verdict = g_owner.AfterUpscale(reinterpret_cast<uint64_t>(device), OwnerNowMs());
    }

    // Said at each change, not each frame: the model changes hands at most once per kAfterUpscaleIdleMs each way.
    static bool waiting = false;
    static bool everHeld = false;

    if (!verdict.granted)
    {
        if (!waiting)
            LOG_INFO("DLSS-NR Vulkan: this route waits: {}", verdict.reason);
        waiting = true;
        return false;
    }

    if (!held)
    {
        // The forwarder keeps one float slot for the process and the present host's parameter block may keep floats
        // elsewhere than this route's: found again for this route's block before anything is written to it.
        g_vk.floatSlotKnown = false;

        if (everHeld || waiting)
            LOG_INFO("DLSS-NR Vulkan: this route has the model again{}",
                     waiting ? " (the present host stood down)" : " (after handing it back)");
    }

    everHeld = true;
    waiting = false;
    return true;
}

void Fail(const char* why)
{
    if (g_vk.failed)
        return;

    g_vk.failed = true;
    g_vk.reason = why;
    LOG_ERROR("DLSS-NR Vulkan unavailable: {}", why);
    SessionGuardOff(); // not running any more
}

bool ModelFaulted(std::string* description);

// Whether the forwarder has caught a fault inside the model; if so, says where once and fails the
// session with that reason. Called before the generic "refused" failures, because a faulted call comes
// back as an ordinary failed result and would otherwise be reported as one.
bool NoteModelFault()
{
    std::string description;

    if (!ModelFaulted(&description))
        return false;

    if (!g_vk.failed)
        LOG_ERROR("DLSS-NR Vulkan: {}", description);

    Fail("the model faulted inside its own code; off for this session");
    return true;
}

// ---------------------------------------------------------------------------------------------
// Images this pass owns
// ---------------------------------------------------------------------------------------------

void DestroyImageOn(VkDevice device, OwnedImage& img)
{
    if (device == VK_NULL_HANDLE)
        return;

    if (img.view != VK_NULL_HANDLE)
        vkDestroyImageView(device, img.view, nullptr);

    if (img.image != VK_NULL_HANDLE)
        vkDestroyImage(device, img.image, nullptr);

    if (img.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, img.memory, nullptr);

    img = OwnedImage {};
}

void DestroyImage(OwnedImage& img) { DestroyImageOn(g_vk.device, img); }

uint32_t FindMemoryTypeIndexOn(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps {};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

uint32_t FindMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    return FindMemoryTypeIndexOn(g_vk.physicalDevice, typeBits, properties);
}

// STORAGE and SAMPLED both, because every one of these is written by one dispatch and read by the
// next; TRANSFER_SRC so a capture can copy it out without a second surface.
// Build the OS_Vk resample descriptor for one of our own images. OS_Vk reads Width/Height/Format from
// this (the NR override makes it size from the images, not the current feature).
static VkImageInfo ImageInfoOf(const OwnedImage& img)
{
    VkImageInfo info {};
    info.ImageView = img.view;
    info.Image = img.image;
    info.SubresourceRange = VkImageSubresourceRange { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    info.Format = img.format;
    info.Width = img.width;
    info.Height = img.height;
    return info;
}

bool CreateImageOn(VkDevice device, VkPhysicalDevice physicalDevice, OwnedImage& img, uint32_t width, uint32_t height,
                   VkFormat format, bool readWrite)
{
    DestroyImageOn(device, img);

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { width, height, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &info, nullptr, &img.image) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not create a {}x{} image", width, height);
        return false;
    }

    VkMemoryRequirements req {};
    vkGetImageMemoryRequirements(device, img.image, &req);

    VkMemoryAllocateInfo alloc {};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        FindMemoryTypeIndexOn(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &alloc, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, img.image, img.memory, 0) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not back a {}x{} image", width, height);
        DestroyImageOn(device, img);
        return false;
    }

    VkImageViewCreateInfo view {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(device, &view, nullptr, &img.view) != VK_SUCCESS)
    {
        LOG_ERROR("DLSS-NR Vulkan: could not view a {}x{} image", width, height);
        DestroyImageOn(device, img);
        return false;
    }

    img.width = width;
    img.height = height;
    img.format = format;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The NGX wrapper. Filled once, because none of it changes until the image is recreated.
    img.ngx.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    img.ngx.Resource.ImageViewInfo.ImageView = img.view;
    img.ngx.Resource.ImageViewInfo.Image = img.image;
    img.ngx.Resource.ImageViewInfo.SubresourceRange = view.subresourceRange;
    img.ngx.Resource.ImageViewInfo.Format = format;
    img.ngx.Resource.ImageViewInfo.Width = width;
    img.ngx.Resource.ImageViewInfo.Height = height;
    img.ngx.ReadWrite = readWrite;

    return true;
}

bool CreateImage(OwnedImage& img, uint32_t width, uint32_t height, VkFormat format, bool readWrite)
{
    return CreateImageOn(g_vk.device, g_vk.physicalDevice, img, width, height, format, readWrite);
}

// The ring of host-visible buffers the meter's grid is copied into, created once and mapped for
// good. HOST_COHERENT so the read needs no invalidate; it is universally available for a buffer this
// small and the alternative is a vkInvalidateMappedMemoryRanges on a path that runs every frame.
bool CreateMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            continue;

        VkBufferCreateInfo info {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = kMeterBytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(g_vk.device, &info, nullptr, &g_vk.meterReadback[i]) != VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not create the exposure readback buffer");
            return false;
        }

        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(g_vk.device, g_vk.meterReadback[i], &req);

        VkMemoryAllocateInfo alloc {};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = FindMemoryTypeIndex(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(g_vk.device, &alloc, nullptr, &g_vk.meterReadbackMemory[i]) != VK_SUCCESS ||
            vkBindBufferMemory(g_vk.device, g_vk.meterReadback[i], g_vk.meterReadbackMemory[i], 0) != VK_SUCCESS ||
            vkMapMemory(g_vk.device, g_vk.meterReadbackMemory[i], 0, kMeterBytes, 0, &g_vk.meterMapped[i]) !=
                VK_SUCCESS)
        {
            LOG_WARN("DLSS-NR Vulkan: could not back the exposure readback buffer");
            return false;
        }
    }

    return true;
}

void DestroyMeterReadback()
{
    for (unsigned long long i = 0; i < kMeterSlots; ++i)
    {
        if (g_vk.meterReadbackMemory[i] != VK_NULL_HANDLE)
        {
            if (g_vk.meterMapped[i] != nullptr)
                vkUnmapMemory(g_vk.device, g_vk.meterReadbackMemory[i]);

            vkFreeMemory(g_vk.device, g_vk.meterReadbackMemory[i], nullptr);
        }

        if (g_vk.meterReadback[i] != VK_NULL_HANDLE)
            vkDestroyBuffer(g_vk.device, g_vk.meterReadback[i], nullptr);

        g_vk.meterMapped[i] = nullptr;
        g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
        g_vk.meterReadback[i] = VK_NULL_HANDLE;
    }

    g_vk.meterFrames = 0;
}

// A layout transition with the access masks that go with it. Vulkan has no equivalent of D3D12's
// state promotion, so every read and every write says which layout it needs and this is how it gets
// there. Tracked per image so a no-op transition is not recorded.
void Transition(VkCommandBuffer cmd, OwnedImage& img, VkImageLayout to)
{
    if (img.image == VK_NULL_HANDLE || img.layout == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = img.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = img.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    img.layout = to;
}

// A resource the game owns. Its layout is the game's business, so this records the transition and
// puts it back exactly as it was rather than tracking it.
void TransitionForeign(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange range, VkImageLayout from,
                       VkImageLayout to)
{
    if (image == VK_NULL_HANDLE || from == to)
        return;

    VkImageMemoryBarrier barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = range;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// Bring-up
// ---------------------------------------------------------------------------------------------

// nullptr when the forwarder is loaded and usable, otherwise why not. Tried once per process: a forwarder that is
// missing or from another build stays so.
const char* LoadForwarderShared()
{
    std::lock_guard<std::mutex> lock(g_fwd.mutex);

    if (g_fwd.tried)
        return g_fwd.failure;

    g_fwd.tried = true;

    auto path = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
        path = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!path.has_value())
        return g_fwd.failure = "nvngx.dll_dlssnr.dll was not found beside OptiScaler or the game";

    g_fwd.module = LoadLibraryW(path->wstring().c_str());

    if (g_fwd.module == nullptr)
        return g_fwd.failure = "the forwarder would not load";

    g_fwd.probe = (PFN_VkProbe) GetProcAddress(g_fwd.module, "dlssnr_vk_probe");
    g_fwd.init = (PFN_VkInit) GetProcAddress(g_fwd.module, "dlssnr_vk_init");
    g_fwd.create = (PFN_VkCreate) GetProcAddress(g_fwd.module, "dlssnr_vk_create");
    g_fwd.evaluate = (PFN_VkEvaluate) GetProcAddress(g_fwd.module, "dlssnr_vk_evaluate");
    g_fwd.release = (PFN_VkRelease) GetProcAddress(g_fwd.module, "dlssnr_vk_release");
    g_fwd.faultState = (int (*)(unsigned long*, void**)) GetProcAddress(g_fwd.module, "dlssnr_fault_state");
    // Optional, and shared with the D3D12 path: one global slot inside the forwarder.
    g_fwd.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_fwd.module, "dlssnr_call_set_float_slot");
    g_fwd.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_fwd.module, "dlssnr_call_probe_float");
    g_fwd.shutdown = (int(__cdecl*)(void*)) GetProcAddress(g_fwd.module, "dlssnr_vk_shutdown");

    if (g_fwd.init == nullptr || g_fwd.create == nullptr || g_fwd.evaluate == nullptr)
        return g_fwd.failure = "the forwarder is missing its Vulkan entry points";

    // The same refusal the D3D12 loader makes, for the same reason: the export names have outlived
    // several argument lists, so an old forwarder resolves and then misreads every argument past the
    // fourth. See kDlssNrForwarderAbi.
    const auto* forwarderAbi = (const int*) GetProcAddress(g_fwd.module, "dlssnr_abi_version");

    if (forwarderAbi == nullptr || *forwarderAbi != kDlssNrForwarderAbi)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll is version {} and this build needs {}; copy the forwarder from "
                  "the same build as OptiScaler",
                  forwarderAbi == nullptr ? 1 : *forwarderAbi, kDlssNrForwarderAbi);
        return g_fwd.failure = "nvngx.dll_dlssnr.dll is from a different build";
    }

    // The same announcement the D3D12 loader makes; a host too old to make it is refused by the
    // forwarder rather than mismatching its arguments in silence.
    if (const auto setHostAbi = (void(__cdecl*)(int)) GetProcAddress(g_fwd.module, "dlssnr_set_host_abi"))
        setHostAbi(kDlssNrForwarderAbi);

    // Same check the D3D12 loader makes, once per process either way.
    DlssNr::Consumers::WarnIfCompeting();

    return nullptr;
}

bool LoadForwarder()
{
    if (const char* why = LoadForwarderShared())
    {
        Fail(why);
        return false;
    }

    return true;
}

// Whether the forwarder has caught a fault inside the model, and the description of it. Shared by both owners: the
// forwarder refuses every call after the first fault, whoever made it.
bool ModelFaulted(std::string* description)
{
    unsigned long code = 0;
    void* address = nullptr;

    if (g_fwd.faultState == nullptr || g_fwd.faultState(&code, &address) == 0)
        return false;

    if (description != nullptr)
        *description = DlssNr::ModelLog::DescribeFault(code, address);

    return true;
}

// Where this parameter block keeps floats. The same probe as DlssNr_Dx12's DiscoverFloatSlot, for
// the reason documented there: the header's slot is not where the driver's own block keeps them.
// This path never ran it, so every float the model was given went into a slot that discards it.
void DiscoverFloatSlotFor(bool& known, NVSDK_NGX_Parameter* params)
{
    if (known || params == nullptr || g_fwd.probeFloat == nullptr || g_fwd.setFloatSlot == nullptr)
        return;

    known = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_fwd.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_fwd.setFloatSlot(slot);
            LOG_INFO("DLSS-NR Vulkan: float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR Vulkan could not find the float setter: the motion vector scale, intensity, "
              "local structure, local tone and skin structure will have no effect. The uint "
              "parameters still apply.");
}

void DiscoverFloatSlot(NVSDK_NGX_Parameter* params) { DiscoverFloatSlotFor(g_vk.floatSlotKnown, params); }

// Whether a format can hold linear, open-ended light. A frame the game already tone mapped has white
// at 1 and must not be encoded a second time; an 8-bit or normalised format cannot be scene-referred
// whatever the game says. The D3D12 path asks the same question of DXGI formats.
bool FormatCanHoldLinearHdr(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R16G16B16_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return true;
    default:
        return false;
    }
}

// The create flags the game gave its own upscaler, which is where HDR and inverted depth are stated.
// Read from the parameter block rather than configured, because they describe the game's buffers and
// getting either wrong is silent: an encoded frame encoded twice, or depth read backwards.
unsigned int GameCreateFlags(NVSDK_NGX_Parameter* params)
{
    unsigned int flags = 0;

    if (params != nullptr)
        params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags);

    return flags;
}

// Both Vulkan routes set the encode's Transfer here. Transfer 2's private DLSS is D3D12 only: sent as is, the shader
// would decode this path's plain answer as the carrier that DLSS writes; it runs matched residual instead
// (docs/dlssnr/design/dlss-enlargement.md).
void SetVulkanTransfer(DlssNrConstants& encode, const Config& cfg)
{
    encode.Transfer = Enlarge::VulkanTransfer(cfg.DlssNrTransfer.value_or_default());
}

std::optional<std::filesystem::path> FindSnippet()
{
    auto snippet = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    return snippet;
}

} // namespace

// ---------------------------------------------------------------------------------------------

bool IsRunningVk() { return g_vk.feature != nullptr && !g_vk.failed; }

const char* FailureReasonVk() { return g_vk.failed ? g_vk.reason : ""; }

unsigned long long FramesVk() { return g_vk.frames; }

bool ExposureOfferedVk() { return g_vk.exposureOffered; }

bool ExposureLatchedVk() { return g_vk.exposureLatch.Latched(); }

bool LinearHdrVk() { return g_vkLinearHdr.load(); }

std::optional<double> LastGpuTimeVk() { return g_vk.lastGpuTime; }

void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params, VkInstance instance,
                            VkPhysicalDevice physicalDevice, VkDevice device)
{
    auto& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default())
    {
        g_vkResetOnReturn = true;
        SessionGuardOff();
        return;
    }

    if (cmdBuffer == VK_NULL_HANDLE || params == nullptr || device == VK_NULL_HANDLE ||
        physicalDevice == VK_NULL_HANDLE)
        return;

    std::lock_guard<std::mutex> lock(g_vkMutex);

    if (g_vk.failed)
        return;

    if (g_vkResetOnReturn.exchange(false))
        g_vk.reset = true;

    const uint32_t wpSource = cfg.DlssNrWhitePointSource.value_or_default();
    if (DlssNr::Exposure::InvalidateOnSourceChange(g_vk.exposureSettingWasOn, wpSource))
    {
        g_vk.gameExposure = 0.0f;
        g_vk.meterFrames = 0;
        g_vk.exposureLatch.Reset();
    }
    else if (g_vk.exposureSettingWasOn && wpSource != 1)
    {
        g_vk.gameExposure = 0.0f;
        g_vk.meterFrames = 0;
        g_vk.exposureLatch.Reset();
    }
    g_vk.exposureSettingWasOn = (wpSource == 1);

    // A fault the forwarder caught inside the model ends the session's NR; see dlssnr_fault_state.
    if (NoteModelFault())
        return;

    // The game's own resources, already wrapped: NGX hands Vulkan resources over as
    // NVSDK_NGX_Resource_VK, so only this pass's own images need building.
    NVSDK_NGX_Resource_VK* colour = nullptr;
    NVSDK_NGX_Resource_VK* depth = nullptr;
    NVSDK_NGX_Resource_VK* motion = nullptr;

    params->Get(NVSDK_NGX_Parameter_Output, (void**) &colour);
    params->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &motion);

    // The game's exposure, now read rather than only counted.
    //
    // What blocked this was the layout: a descriptor names the layout its image will be in when the
    // shader runs, NVIDIA's Vulkan header does not use the word "layout" once, and a barrier is no
    // safer because it needs the layout it is coming from. Three things in this tree answer it, and
    // they agree. FSR2Feature_Vk hands this same texture to FidelityFX as COMPUTE_READ, which its
    // Vulkan backend maps to SHADER_READ_ONLY_OPTIMAL, on a path that works in these games. The
    // D3D12-on-Vulkan bridge transitions the game's exposure image out of SHADER_READ_ONLY_OPTIMAL,
    // on a path that works. And the header stating nothing means there is no contract to break --
    // the convention is the contract.
    //
    // So it is bound in SHADER_READ_ONLY_OPTIMAL and no barrier is recorded: this never transitions a
    // resource it does not own. If a game turns out to leave it somewhere else the cost is a wrong
    // number, not a lost device, and the gate on the readback throws a wrong number away.
    NVSDK_NGX_Resource_VK* exposure = nullptr;
    float preExposure = 1.0f;
    const bool havePre = params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

    params->Get(NVSDK_NGX_Parameter_ExposureTexture, (void**) &exposure);

    static bool saidExposure = false;

    if (!saidExposure)
    {
        saidExposure = true;
        LOG_INFO("DLSS-NR Vulkan: exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}",
                 havePre ? std::to_string(preExposure) : std::string("not supplied"),
                 exposure != nullptr ? "supplied" : "not supplied");
    }

    g_vk.exposureOffered = exposure != nullptr;

    if (havePre && std::isfinite(preExposure) && preExposure > 0.0f)
        g_vk.gamePreExposure = preExposure;

    // Take the grid written four frames ago. Retired by now, so this reads mapped memory rather than
    // waiting on the GPU -- which is the whole reason for the ring.
    if (g_vk.meterFrames >= kMeterSlots)
    {
        const void* mapped = g_vk.meterMapped[g_vk.meterFrames % kMeterSlots];

        if (mapped != nullptr)
        {
            float measured = 0.0f;
            std::memcpy(&measured, mapped, sizeof(float));

            // Believed only if it could be an exposure. A texel read through a layout the game did
            // not leave it in, or a slot the game stopped filling, fails here and the last good
            // value stands.
            if (DlssNr::Exposure::ValidSample(measured))
            {
                g_vk.gameExposure = measured;

                // The game's own implied white, without the user's trim (see ConsumeMeterReadback).
                const float implied = DlssNr::Exposure::PreExposure(g_vk.gamePreExposure) / measured;
                if (g_vk.exposureLatch.Observe(implied))
                {
                    LOG_WARN("DLSS-NR Vulkan: the game's exposure is not usable (implied white point as low as {:.4f} "
                             "and as high as {:.1f}; {} readings below 0.02, {} above 512); using paper white for "
                             "the rest of the session",
                             g_vk.exposureLatch.Lowest(), g_vk.exposureLatch.Highest(), g_vk.exposureLatch.LowCount(),
                             g_vk.exposureLatch.HighCount());
                }
            }
        }
    }

    // Said when it moves by more than a fiftieth, not every frame. Enough to see in a log that the
    // number is the game's and that it tracks the scene, without a line per frame.
    static float loggedExposure = -1.0f;

    if (g_vk.gameExposure > 1e-6f &&
        std::abs(loggedExposure - g_vk.gameExposure) > std::max(0.02f * g_vk.gameExposure, 1e-5f))
    {
        loggedExposure = g_vk.gameExposure;
        LOG_INFO("DLSS-NR Vulkan: the game's exposure is {}, pre-exposure {}, so white point {}", g_vk.gameExposure,
                 g_vk.gamePreExposure, g_vk.gamePreExposure / g_vk.gameExposure);
    }

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        static bool said = false;

        if (!said)
        {
            said = true;
            LOG_INFO("DLSS-NR Vulkan: the parameter block carried no {}",
                     colour == nullptr ? "output" : (depth == nullptr ? "depth" : "motion vectors"));
        }

        return;
    }

    const uint32_t width = colour->Resource.ImageViewInfo.Width;
    const uint32_t height = colour->Resource.ImageViewInfo.Height;
    uint32_t guideWidth = depth->Resource.ImageViewInfo.Width;
    uint32_t guideHeight = depth->Resource.ImageViewInfo.Height;
    const uint32_t motionAllocationWidth = motion->Resource.ImageViewInfo.Width;
    const uint32_t motionAllocationHeight = motion->Resource.ImageViewInfo.Height;

    if (width == 0 || height == 0)
        return;

    // The model's working size. The slider is a fraction of the frame; at 1 it is the frame, and the
    // reduced path below never runs, so the default is byte-for-byte what it was.
    // Above 1 the model supersamples (up to 2x): the proxy is enlarged, the model runs above native,
    // and superDown averages the answer back. Vulkan matches the D3D12 cap.
    // WorkingScale=auto depends on the GPU; see DlssNr_WorkingScale.h. Vulkan knows it by PCI id.
    {
        static VkPhysicalDevice noted = VK_NULL_HANDLE;

        if (physicalDevice != noted)
        {
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(physicalDevice, &props);
            DlssNr::NoteAdapter(props.vendorID, props.deviceID);
            noted = physicalDevice;
        }
    }
    const float workScale = std::clamp(DlssNr::WorkingScale(), 0.25f, 2.0f);
    const uint32_t workWidth = (uint32_t) (width * workScale + 0.5f);
    const uint32_t workHeight = (uint32_t) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    // See kDlssNrMinExtent: a probe-sized swapchain must never get a model built for it.
    if (width < kDlssNrMinExtent || height < kDlssNrMinExtent || workWidth < kDlssNrMinExtent ||
        workHeight < kDlssNrMinExtent)
    {
        static bool reported = false;

        if (!reported)
        {
            reported = true;
            LOG_INFO("DLSS-NR Vulkan did not run: {}x{} (working {}x{}) is below {} pixels; the model is not "
                     "built that small",
                     width, height, workWidth, workHeight, kDlssNrMinExtent);
        }

        return;
    }

    // One owner of the model in the process (DlssNr_VkModelOwner.h): this route wins, but not on a frame the Vulkan
    // present host may still have in flight; asking is what makes it stand down at its next present. Never refused
    // when the present host is off, so the route is what it was.
    if (!ClaimModelForAfterUpscale(device))
        return;

    // Whatever this evaluate records, the present host's hand-back counts the route's quiet from its end, not from the
    // ask above: a cold feature build inside it can take a while (DlssNr_VkModelOwner.h, "The way back").
    struct RecordingDone
    {
        ~RecordingDone()
        {
            std::lock_guard<std::mutex> lock(g_ownerMutex);
            g_owner.AfterUpscaleDone(OwnerNowMs());
        }
    } recordingDone;

    g_vk.instance = instance;
    g_vk.physicalDevice = physicalDevice;

    // A device change invalidates everything. The OLD device is presumed dead here -- the game
    // destroyed it, which already freed every resource made on it -- so abandon those handles rather
    // than call vkDestroy*/wait-idle on a dead device (that would be use-after-free). Rebuild fresh.
    if (g_vk.device != device)
    {
        ShutdownVk(false);
        g_vk.device = device;
        g_vk.instance = instance;
        g_vk.physicalDevice = physicalDevice;
    }

    if (!LoadForwarder())
        return;

    // Initialise NGX on this device, once. The snippet path is the model itself; the forwarder loads
    // it so the caller gate sees a module named nvngx.dll.
    if (!g_vk.ngxInitialised)
    {
        auto snippet = FindSnippet();

        if (!snippet.has_value())
        {
            Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");
            return;
        }

        // Before init and the first build, so the model's own account of them is in the log.
        DlssNr::ModelLog::Install(*snippet);

        const int probe = g_fwd.probe != nullptr ? g_fwd.probe(snippet->wstring().c_str()) : 0;

        // Four bits, one per entry point. Anything short of fifteen means the model's Vulkan surface
        // is not entirely reachable and there is no point going further.
        if (probe != 15)
        {
            LOG_ERROR("DLSS-NR Vulkan: the model's Vulkan surface is incomplete (probe {})", probe);
            Fail("the model does not expose a complete Vulkan surface");
            return;
        }

        const int result = g_fwd.init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                                      (void*) instance, (void*) physicalDevice, (void*) device, 0x0000015);

        if (result != 1)
        {
            LOG_ERROR("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned {}", result);
            NoteModelFault();
            Fail("the model would not initialise on this Vulkan device");
            return;
        }

        g_vk.ngxInitialised = true;
        LOG_INFO("DLSS-NR Vulkan: the model initialised on this device");
    }

    if (g_vk.capabilityParams == nullptr)
    {
        if (NVSDK_NGX_VULKAN_AllocateParameters(&g_vk.capabilityParams) != NVSDK_NGX_Result_Success ||
            g_vk.capabilityParams == nullptr)
        {
            Fail("a parameter block could not be allocated");
            return;
        }
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_vk.capabilityParams);

    if (g_vk.queryPool == VK_NULL_HANDLE)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        // A period of zero means the device does not support timestamps on this queue. The pass runs
        // regardless; it simply reports no cost, which is what the D3D12 path does when its heap is
        // unavailable.
        g_vk.timestampPeriod = props.limits.timestampPeriod;

        if (g_vk.timestampPeriod > 0.0f)
        {
            VkQueryPoolCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kTimingSlots * 2;

            if (vkCreateQueryPool(device, &info, nullptr, &g_vk.queryPool) != VK_SUCCESS)
            {
                g_vk.queryPool = VK_NULL_HANDLE;
                LOG_INFO("DLSS-NR Vulkan: no timestamp pool, the pass will not report its cost");
            }
        }
    }

    if (g_vk.pass == nullptr)
    {
        // Before anything is written to it, work out where this block keeps floats.
        DiscoverFloatSlot(g_vk.capabilityParams);

        g_vk.pass = std::make_unique<DlssNr_Vk>("Neural Rendering", device, physicalDevice);

        if (!g_vk.pass->IsInit())
        {
            g_vk.pass.reset();
            Fail("the composition pass could not be created");
            return;
        }
    }

    // Resize. The feature is built for a size and has to be rebuilt when the frame OR the working
    // size changes -- moving the slider is a rebuild, which is why it is compared here.
    if (g_vk.width != width || g_vk.height != height || g_vk.workWidth != workWidth || g_vk.workHeight != workHeight)
    {
        // This block releases the feature and frees the surfaces below IMMEDIATELY. A frame-size
        // change is already fenced by the game -- it recreates the swapchain around it -- but moving
        // the working-scale slider is not: the game is mid-flight and previous frames' command
        // buffers still reference the feature and images about to be destroyed. Freeing a Vulkan
        // resource that in-flight GPU work still touches is device removal (ERR_GFX_STATE, reproduced
        // on RDR2 and Enshrouded by dragging the model-resolution slider). Drain the device first.
        // Only the rare resize path reaches here, so the CPU stall is a one-off hitch, not per-frame.
        if (g_vk.device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(g_vk.device);

        if (g_vk.feature != nullptr && g_fwd.release != nullptr)
        {
            g_fwd.release(g_vk.feature);
            g_vk.feature = nullptr;
        }

        const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;

        // The meter is a fixed 8x8 whatever the frame is, so it is only built the once -- but it is
        // built alongside the rest so that a failure here is caught by the same check.
        const bool meterReady =
            (g_vk.meter.Valid() || CreateImage(g_vk.meter, kMeterSide, kMeterSide, VK_FORMAT_R32_SFLOAT, true)) &&
            CreateMeterReadback();

        if (!meterReady)
            LOG_WARN("DLSS-NR Vulkan: no exposure meter; the white point stays on the slider");

        DestroyImage(g_vk.proxySmall);
        DestroyImage(g_vk.outputNative);

        // output is the model's target, so it is the working size. proxy and keep are full: proxy is
        // the source the downsample reads, keep is the untouched frame the resolve composites onto.
        // outputNative is the native buffer the supersample down-leg averages the answer into.
        const bool ok = CreateImage(g_vk.output, workWidth, workHeight, working, true) &&
                        CreateImage(g_vk.proxy, width, height, working, true) &&
                        CreateImage(g_vk.keep, width, height, working, true) &&
                        (!reduced || CreateImage(g_vk.proxySmall, workWidth, workHeight, working, true)) &&
                        (workScale <= 1.0f || CreateImage(g_vk.outputNative, width, height, working, true));

        if (!ok)
        {
            Fail("the pass could not allocate its own surfaces");
            return;
        }

        g_vk.width = width;
        g_vk.height = height;
        g_vk.workWidth = workWidth;
        g_vk.workHeight = workHeight;
        g_vk.reset = true;
    }

    if (g_vk.feature == nullptr)
    {
        g_vk.feature = g_fwd.create(
            (void*) cmdBuffer, g_vk.capabilityParams, workWidth, workHeight, (int) cfg.DlssNrPreset.value_or_default(),
            cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
            cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
            cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (g_vk.feature == nullptr)
        {
            NoteModelFault();
            Fail("the model would not build a feature on this device");
            return;
        }

        LOG_INFO("DLSS-NR Vulkan: feature up at {}x{} (frame {}x{})", workWidth, workHeight, width, height);
        g_vk.reset = true;
    }

    // -----------------------------------------------------------------------------------------
    // Encode: the frame the upscaler wrote -> a display-referred proxy, plus an untouched copy
    // -----------------------------------------------------------------------------------------

    const unsigned int createFlags = GameCreateFlags(params);
    const bool gameSaysHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const bool depthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    const bool lowResolutionMotion = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;

    // Everything below mirrors the D3D12 path deliberately. The two are one behaviour reached through
    // two APIs, and the last time they were written independently they disagreed about which frame
    // the model was being shown.
    uint32_t guideAllocWidth = guideWidth;
    uint32_t guideAllocHeight = guideHeight;

    if (guideAllocWidth == 0 || guideAllocHeight == 0)
    {
        guideAllocWidth = width;
        guideAllocHeight = height;
    }

    guideWidth = guideAllocWidth;
    guideHeight = guideAllocHeight;

    const auto [renderWidth, renderHeight] = DlssNr::GetRenderSize(params);

    uint32_t depthBaseX = 0, depthBaseY = 0, motionBaseX = 0, motionBaseY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &depthBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &depthBaseY);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &motionBaseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &motionBaseY);

    // Zero is the game declining to answer, not a zero-sized frame: the block leaves the value alone
    // when the key is absent, and a game is free to publish it as zero. Taking it literally would
    // hand the model an empty depth subrect, which is why the D3D12 path has always guarded it.
    if (renderWidth != 0 && renderHeight != 0)
    {
        guideWidth = std::min(renderWidth, guideWidth);
        guideHeight = std::min(renderHeight, guideHeight);
    }

    depthBaseX = std::min(depthBaseX, guideAllocWidth);
    depthBaseY = std::min(depthBaseY, guideAllocHeight);
    guideWidth = std::min(guideWidth, guideAllocWidth - depthBaseX);
    guideHeight = std::min(guideHeight, guideAllocHeight - depthBaseY);

    // MVLowRes decides whether the vectors cover the render or the display frame, so the motion
    // subrect is derived from the flag rather than shared with depth.
    const uint32_t wantedMotionWidth = lowResolutionMotion ? guideWidth : width;
    const uint32_t wantedMotionHeight = lowResolutionMotion ? guideHeight : height;

    uint32_t motionAllocWidth = motionAllocationWidth;
    uint32_t motionAllocHeight = motionAllocationHeight;

    if (motionAllocWidth == 0 || motionAllocHeight == 0)
    {
        motionAllocWidth = wantedMotionWidth;
        motionAllocHeight = wantedMotionHeight;
    }

    motionBaseX = std::min(motionBaseX, motionAllocWidth);
    motionBaseY = std::min(motionBaseY, motionAllocHeight);
    const uint32_t motionWidth = std::min(wantedMotionWidth, motionAllocWidth - motionBaseX);
    const uint32_t motionHeight = std::min(wantedMotionHeight, motionAllocHeight - motionBaseY);

    // The only account of what the model was shown on this path. The Vulkan half of the subrect work
    // has no test that reaches it -- vulkan-overlay instruments the menu, not the evaluate -- and the
    // marker leaves it inert on the D3D12 test target, so this line is the evidence.
    {
        static uint32_t said[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
        const uint32_t now[6] = { guideWidth, guideHeight, depthBaseX, depthBaseY, motionWidth, motionHeight };

        if (!std::equal(std::begin(now), std::end(now), std::begin(said)))
        {
            std::copy(std::begin(now), std::end(now), std::begin(said));
            LOG_INFO("DLSS-NR Vulkan guides: depth {}x{} at ({},{}), motion vectors {}x{} at ({},{}) in a "
                     "{}x{} image, {}",
                     guideWidth, guideHeight, depthBaseX, depthBaseY, motionWidth, motionHeight, motionBaseX,
                     motionBaseY, motionAllocWidth, motionAllocHeight,
                     lowResolutionMotion ? "render resolution" : "display resolution");
        }
    }

    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);

    // Zero or not finite is as good as absent, as on D3D12.
    if (!std::isfinite(mvScaleX) || mvScaleX == 0.0f)
        mvScaleX = 1.0f;

    if (!std::isfinite(mvScaleY) || mvScaleY == 0.0f)
        mvScaleY = 1.0f;

    // The game asking the upscaler to forget its history -- a cut, a teleport, a load. Same omission
    // as the D3D12 path had: the model's history was only ever reset by things that happened to us,
    // never by anything that happened in the game.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success && gameReset != 0)
        {
            g_vk.reset = true;

            static unsigned long long resets = 0;
            ++resets;

            if (resets <= 3 || resets % 100 == 0)
                LOG_INFO("DLSS-NR Vulkan: the game asked for a history reset ({} so far)", resets);
        }
    }

    // Both have to agree. A game can set the HDR flag on a buffer that cannot hold open-ended light,
    // and encoding an already tone-mapped frame a second time looks washed out and banded.
    const bool linearHdr = gameSaysHdr && FormatCanHoldLinearHdr(colour->Resource.ImageViewInfo.Format);
    g_vkLinearHdr.store(linearHdr);

    // The same rule as the D3D12 path, deliberately spelled the same way: the game divides its frame
    // by preExposure and multiplies by exposure, so undoing that is the divisor this pass wants, and
    // the slider becomes a trim on top rather than the answer.
    //
    // The trim is bounded here, at the point of use, rather than at the slider. Someone who found 64
    // by hand on the manual path and then switches the exposure source on keeps that 64 in their ini;
    // bounding it in the menu would leave the picture wrong for a reason the menu no longer showed.
    // Their value stays in the config untouched, so switching back to manual restores it.
    float whitePoint = cfg.DlssNrWhitePointScale.value_or_default();

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && !g_vk.exposureLatch.Latched() &&
        g_vk.gameExposure > 1e-6f)
    {
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.01f, 4.0f);
        whitePoint = std::clamp(g_vk.gamePreExposure / g_vk.gameExposure * trim, 0.01f, 4096.0f);
    }

    static bool saidEncoding = false;

    if (!saidEncoding)
    {
        saidEncoding = true;
        LOG_INFO("DLSS-NR Vulkan: the game's buffer is {} (flag {}, format {}), depth {}",
                 linearHdr ? "linear HDR" : "already tone-mapped", gameSaysHdr ? "set" : "clear",
                 (int) colour->Resource.ImageViewInfo.Format, depthInverted ? "inverted" : "normal");
    }

    DlssNrConstants encode {};
    encode.Mode = DlssNrMode_Encode;
    encode.Width = width;
    encode.Height = height;
    encode.WhitePoint = whitePoint;
    encode.Passthrough = linearHdr ? 0u : 1u;
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encode.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    encode.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    encode.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    encode.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
    SetVulkanTransfer(encode, cfg);
    encode.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
    encode.GuideWidth = guideWidth;
    encode.GuideHeight = guideHeight;
    // Read by the resolve, which takes a copy of these constants. Linear HDR only, as on D3D12
    // (docs/dlssnr/design/hdr-highlight-recovery.md).
    encode.HighlightRecovery = linearHdr && cfg.DlssNrHighlightRecovery.value_or_default() ? 1u : 0u;
    // The residual limit, as on D3D12 (docs/dlssnr/design/residual-limit.md); the shared shader reads it in the
    // resolve.
    encode.ResidualLimit = DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default());

    // This path has no composition summary, so the settings a screenshot cannot show are said when they change. Both
    // start at off, so a session that never turns them on logs nothing about them.
    static uint32_t saidRecovery = 0u;
    static float saidResidualLimit = 0.0f;

    if (encode.HighlightRecovery != saidRecovery)
    {
        saidRecovery = encode.HighlightRecovery;
        LOG_INFO("DLSS-NR Vulkan: highlight recovery {}", saidRecovery != 0 ? "on" : "off");
    }

    if (encode.ResidualLimit != saidResidualLimit)
    {
        saidResidualLimit = encode.ResidualLimit;
        if (saidResidualLimit > 0.0f)
            LOG_INFO("DLSS-NR Vulkan: residual limit {:.2f} stops", saidResidualLimit);
        else
            LOG_INFO("DLSS-NR Vulkan: residual limit off");
    }

    const VkImageSubresourceRange colourRange = colour->Resource.ImageViewInfo.SubresourceRange;

    // Open the measurement. Reset immediately before writing: a query pool slot must be reset before
    // it is written again, and doing it here rather than at the end keeps the two in one place.
    const uint32_t timingSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);

    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdResetQueryPool(cmdBuffer, g_vk.queryPool, timingSlot * 2, 2);
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2);
    }

    // The game's colour is read here and written at the end. Its layout on arrival is GENERAL, which
    // is what NGX requires of a resource it is handed, so it is left alone.
    Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_GENERAL);

    // Read in GENERAL, which is the layout it is actually in.
    //
    // This slot used to take the default and declare SHADER_READ_ONLY_OPTIMAL, which disagreed with
    // the comment four lines up and with the resolve below -- the resolve writes this same image as a
    // storage image, which is only legal in GENERAL, and nothing transitions it in between. It is the
    // upscaler's output, a storage image the upscaler has just written, so GENERAL is what it is.
    // Inert on the only hardware this model runs on, wrong everywhere it is read.
    if (!g_vk.pass->Dispatch(cmdBuffer, encode, width, height, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE,
                             VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxy.view, g_vk.keep.view, VK_IMAGE_LAYOUT_GENERAL))
    {
        Fail("the encode dispatch failed");
        return;
    }

    // The model's input: the full proxy, or a downsampled copy of it when the working scale is below
    // the frame. Mirrors the D3D12 path -- the encode always writes a full proxy, and a separate
    // downsample makes the small one the model actually reads.
    OwnedImage* modelInput = &g_vk.proxy;

    if (reduced && g_vk.proxySmall.Valid())
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: upscale the proxy to the super-native working size with the chosen filter so
            // the model sees a clean input. Rebuild both scalers when the NR downscaler changed (baked
            // at construction). proxy -> SHADER_READ_ONLY (sampled), proxySmall -> GENERAL (storage).
            const Scaler wantScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_vk.nrScaler != wantScaler)
            {
                // Rebuilding frees the old scalers' pipelines/descriptors. The filter dropdown changes
                // no size, so this does NOT go through the resize block's drain -- and prior frames'
                // submitted command buffers still bind these pipelines. Freeing them under in-flight GPU
                // work is device removal (the same hazard the resize path drains for). Drain first. A
                // filter change is rare, so the one-off stall is a hitch, not a per-frame cost.
                if (g_vk.device != VK_NULL_HANDLE)
                    vkDeviceWaitIdle(g_vk.device);
                g_vk.superUp.reset();
                g_vk.superDown.reset();
                g_vk.nrScaler = wantScaler;
            }
            if (!g_vk.superUp)
                g_vk.superUp =
                    std::make_unique<OS_Vk>("DLSS-NR VK supersample up", device, physicalDevice, true, wantScaler);
            if (!g_vk.superDown)
                g_vk.superDown =
                    std::make_unique<OS_Vk>("DLSS-NR VK supersample down", device, physicalDevice, false, wantScaler);

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            VkImageInfo upin = ImageInfoOf(g_vk.proxy);
            VkImageInfo upout = ImageInfoOf(g_vk.proxySmall);

            if (g_vk.superUp && g_vk.superUp->IsInit() && g_vk.superUp->Dispatch(cmdBuffer, upin, upout))
                built = true;
            else
            {
                static bool warnedVkSuper = false;
                if (!warnedVkSuper)
                {
                    warnedVkSuper = true;
                    LOG_WARN("DLSS-NR Vulkan supersample: upscaler unavailable, falling back to box enlarge.");
                }
            }
        }

        if (!built)
        {
            DlssNrConstants down = encode;
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;

            Transition(cmdBuffer, g_vk.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            Transition(cmdBuffer, g_vk.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

            if (!g_vk.pass->Dispatch(cmdBuffer, down, workWidth, workHeight, g_vk.proxy.view, VK_NULL_HANDLE,
                                     VK_NULL_HANDLE, VK_NULL_HANDLE, g_vk.proxySmall.view, VK_NULL_HANDLE,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Fail("the downsample dispatch failed");
                return;
            }
        }

        modelInput = &g_vk.proxySmall;
    }

    // -----------------------------------------------------------------------------------------
    // The meter: the game's 1x1 exposure -> texel (0,0) of the grid -> a buffer the CPU can read
    // -----------------------------------------------------------------------------------------

    // The motion slot carries it, because the meter has no use for motion vectors and the shader is
    // one shader with a fixed set of bindings. The source slot is left empty and gets the dummy.
    //
    // Gated on the setting that consumes the answer, which is not merely tidy. This is the only place
    // the pass binds a resource it does not own on a guess about its layout, and the guess is good
    // but it is still a guess. A user who has not asked for the exposure source never has the game's
    // image touched at all, so if some engine does leave it somewhere unexpected, the blast radius is
    // people who turned the thing on rather than everyone on Vulkan.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && exposure != nullptr &&
        exposure->Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW &&
        exposure->Resource.ImageViewInfo.ImageView != VK_NULL_HANDLE && g_vk.meter.Valid())
    {
        const unsigned long long slot = g_vk.meterFrames % kMeterSlots;

        if (g_vk.meterReadback[slot] != VK_NULL_HANDLE)
        {
            DlssNrConstants meter = encode;
            meter.Mode = DlssNrMode_Meter;
            meter.Width = kMeterSide;
            meter.Height = kMeterSide;

            Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_GENERAL);

            if (g_vk.pass->Dispatch(cmdBuffer, meter, kMeterSide, kMeterSide, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, exposure->Resource.ImageViewInfo.ImageView, g_vk.meter.view,
                                    VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            {
                Transition(cmdBuffer, g_vk.meter, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

                VkBufferImageCopy region {};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { 0, 0, 0 };
                region.imageExtent = { kMeterSide, kMeterSide, 1 };

                vkCmdCopyImageToBuffer(cmdBuffer, g_vk.meter.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       g_vk.meterReadback[slot], 1, &region);

                // The copy has to be visible to a host read, and only the host will read it.
                VkBufferMemoryBarrier toHost {};
                toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toHost.buffer = g_vk.meterReadback[slot];
                toHost.offset = 0;
                toHost.size = kMeterBytes;

                vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                     nullptr, 1, &toHost, 0, nullptr);

                g_vk.meterFrames++;
            }
        }
    }

    // -----------------------------------------------------------------------------------------
    // The model
    // -----------------------------------------------------------------------------------------

    Transition(cmdBuffer, g_vk.output, VK_IMAGE_LAYOUT_GENERAL);

    // The model reads its motion-vector scale in pixels of the motion texture it is handed, and the
    // game's scale is in pixels of the size its vectors are measured in -- the render size for
    // low-resolution vectors (reduced-scale-guides.md, fix 2; jlrouzies-fr's v0.8.92 of wilsjo2's fork).
    // working / frame shrank every such vector below 100%. Fix 1, the guides resampled to the working
    // size, exists only on the D3D12 path so far: this path has no Vulkan resample shader yet, so below
    // native it still hands the model frame-size guides for a smaller colour, and says so once.
    const bool renderMotionScale = cfg.DlssNrRenderMotionScale.value_or_default();
    const float modelMvScaleX = renderMotionScale
                                    ? DlssNr::GuideMatch::ModelMotionScale(mvScaleX, motionWidth, wantedMotionWidth)
                                    : DlssNr::GuideMatch::LegacyMotionScale(mvScaleX, workWidth, width);
    const float modelMvScaleY = renderMotionScale
                                    ? DlssNr::GuideMatch::ModelMotionScale(mvScaleY, motionHeight, wantedMotionHeight)
                                    : DlssNr::GuideMatch::LegacyMotionScale(mvScaleY, workHeight, height);

    {
        static float saidScale[2] = { -1.0f, -1.0f };
        static bool saidUnmatched = false;
        const bool unmatched =
            DlssNr::GuideMatch::Wanted(cfg.DlssNrMatchGuides.value_or_default(), true, workWidth, workHeight,
                                       guideWidth, guideHeight, motionWidth, motionHeight);

        if (saidScale[0] != modelMvScaleX || saidScale[1] != modelMvScaleY)
        {
            saidScale[0] = modelMvScaleX;
            saidScale[1] = modelMvScaleY;
            LOG_INFO("DLSS-NR Vulkan model motion scale {:.1f} x {:.1f}: game scale {:.1f} x {:.1f} measured against "
                     "{}x{} ({}), motion texture {}x{} (the game's region), model {}x{}{}",
                     modelMvScaleX, modelMvScaleY, mvScaleX, mvScaleY, wantedMotionWidth, wantedMotionHeight,
                     lowResolutionMotion ? "render size, low-resolution vectors" : "output size", motionWidth,
                     motionHeight, workWidth, workHeight,
                     renderMotionScale ? "" : " -- legacy working/frame conversion");
        }

        if (unmatched && !saidUnmatched)
        {
            saidUnmatched = true;
            LOG_INFO("DLSS-NR Vulkan guides: larger than the {}x{} model and not resampled (no Vulkan resample yet); "
                     "the model gets the frame-size guides",
                     workWidth, workHeight);
        }
    }
    const int evaluated = g_fwd.evaluate(
        (void*) cmdBuffer, g_vk.feature, g_vk.capabilityParams, &modelInput->ngx, depth, motion, &g_vk.output.ngx,
        workWidth, workHeight, guideWidth, guideHeight, motionWidth, motionHeight, depthBaseX, depthBaseY, motionBaseX,
        motionBaseY, depthInverted ? 1 : 0, g_vk.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
        (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
        cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, modelMvScaleX, modelMvScaleY);

    g_vk.reset = false;
    g_vk.frames++;

    if (evaluated != 1)
    {
        LOG_ERROR("DLSS-NR Vulkan: evaluate returned {}", evaluated);
        NoteModelFault();
        Fail("the model refused to evaluate");
        return;
    }

    SessionGuardEvaluated();

    // -----------------------------------------------------------------------------------------
    // Resolve: proxy + the model's answer + the untouched copy -> the frame
    // -----------------------------------------------------------------------------------------

    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    // Supersampling down-leg (Vulkan). Average the Nx model answer back to native with the chosen
    // filter so the resolve composites a native answer against the native proxy 1:1 -- not the single
    // bilinear tap the Nx answer would otherwise get, which aliases the model's detail into noise. On
    // failure it falls back to the Nx pair (modelInput + output), the old behaviour.
    OwnedImage* resolveProxy = modelInput;
    OwnedImage* resolveAnswer = &g_vk.output;

    if (workScale > 1.0f && g_vk.superDown && g_vk.superDown->IsInit() && g_vk.outputNative.Valid())
    {
        Transition(cmdBuffer, g_vk.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmdBuffer, g_vk.outputNative, VK_IMAGE_LAYOUT_GENERAL);

        VkImageInfo dsin = ImageInfoOf(g_vk.output);
        VkImageInfo dsout = ImageInfoOf(g_vk.outputNative);

        if (g_vk.superDown->Dispatch(cmdBuffer, dsin, dsout))
        {
            resolveProxy = &g_vk.proxy;
            resolveAnswer = &g_vk.outputNative;
        }
    }

    Transition(cmdBuffer, *resolveProxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, *resolveAnswer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmdBuffer, g_vk.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    if (!g_vk.pass->Dispatch(cmdBuffer, resolve, width, height, resolveProxy->view, resolveAnswer->view, g_vk.keep.view,
                             VK_NULL_HANDLE, colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
    {
        Fail("the resolve dispatch failed");
        return;
    }

    // Close it, and read the pair from three frames ago -- retired by now, so the read does not wait.
    if (g_vk.queryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(cmdBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_vk.queryPool, timingSlot * 2 + 1);
        g_vk.timedFrames++;

        if (g_vk.timedFrames > kTimingSlots)
        {
            const uint32_t readSlot = (uint32_t) (g_vk.timedFrames % kTimingSlots);
            uint64_t ticks[2] = {};

            // Without WAIT: a slot this old is retired, and if it somehow is not, NOT_READY is the
            // right answer rather than a stall.
            if (vkGetQueryPoolResults(device, g_vk.queryPool, readSlot * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ticks[1] > ticks[0])
            {
                const double ms = (double) (ticks[1] - ticks[0]) * (double) g_vk.timestampPeriod / 1e6;

                // A pass that appears to have taken over a second did not; the queue was reset under
                // it or the pair straddled a device change.
                if (ms > 0.0 && ms < 1000.0)
                    g_vk.lastGpuTime = ms;
            }
        }
    }

    static bool reported = false;

    if (!reported && g_vk.frames > 2)
    {
        reported = true;
        LOG_INFO("DLSS-NR Vulkan: running natively at {}x{}, guides {}x{}", width, height, guideWidth, guideHeight);
    }
}

namespace
{
// Everything of the after-upscale route's, on a live device, once nothing the GPU runs can still use it: the caller
// proved that (ShutdownVk waits for the device to go idle; a hand-back to the present host drained every queue of the
// device behind fences of its own). NGX stays initialised on the device: it is the device's, and goes with it.
void FreeRouteObjects()
{
    if (g_vk.feature != nullptr && g_fwd.release != nullptr)
        g_fwd.release(g_vk.feature);

    g_vk.feature = nullptr;

    DestroyImage(g_vk.output);
    DestroyImage(g_vk.proxy);
    DestroyImage(g_vk.proxySmall);
    DestroyImage(g_vk.outputNative);
    DestroyImage(g_vk.keep);
    DestroyImage(g_vk.meter);
    DestroyMeterReadback();

    g_vk.pass.reset();
    g_vk.superUp.reset();
    g_vk.superDown.reset();
    g_vk.nrScaler = Scaler::Count;

    if (g_vk.capabilityParams != nullptr)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(g_vk.capabilityParams);
        g_vk.capabilityParams = nullptr;
    }

    if (g_vk.queryPool != VK_NULL_HANDLE && g_vk.device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(g_vk.device, g_vk.queryPool, nullptr);
        g_vk.queryPool = VK_NULL_HANDLE;
    }

    g_vk.timedFrames = 0;
    g_vk.meterFrames = 0;
    g_vk.exposureLatch.Reset();
    g_vk.lastGpuTime.reset();

    g_vk.device = VK_NULL_HANDLE;
    g_vk.width = 0;
    g_vk.height = 0;
    g_vk.ngxInitialised = false;
    g_vk.floatSlotKnown = false;
    g_vk.reset = true;
}
} // namespace

void ShutdownVk(bool deviceAlive)
{
    if (!deviceAlive)
    {
        // The device these handles belong to is gone (a device change was detected). Destroying a
        // VkDevice already frees every resource created on it, so touch NOTHING on the old device --
        // no wait-idle, no vkDestroy*, no NGX release, and crucially no DlssNr_Vk destructor (it would
        // vkDestroy its pipelines on the dead device). Abandon the handles; the driver reclaimed them
        // when the device died. The one-off CPU-side leak of the pass object is the rare cost of a
        // device recreation, and far cheaper than the use-after-free it replaces. Zeroing the
        // OwnedImage/meter handles matters: the resize path gates on `.Valid()`, so a stale non-null
        // handle from the dead device would be reused on the NEW device and crash.
        g_vk.pass.release();
        g_vk.superUp.release();
        g_vk.superDown.release();
        g_vk.nrScaler = Scaler::Count;
        g_vk.feature = nullptr;
        g_vk.capabilityParams = nullptr;
        g_vk.queryPool = VK_NULL_HANDLE;
        g_vk.output = OwnedImage {};
        g_vk.proxy = OwnedImage {};
        g_vk.proxySmall = OwnedImage {};
        g_vk.outputNative = OwnedImage {};
        g_vk.keep = OwnedImage {};
        g_vk.meter = OwnedImage {};

        for (int i = 0; i < 4; ++i)
        {
            g_vk.meterReadback[i] = VK_NULL_HANDLE;
            g_vk.meterReadbackMemory[i] = VK_NULL_HANDLE;
            g_vk.meterMapped[i] = nullptr;
        }

        g_vk.device = VK_NULL_HANDLE;
        g_vk.width = 0;
        g_vk.height = 0;
        g_vk.workWidth = 0;
        g_vk.workHeight = 0;
        g_vk.timedFrames = 0;
        g_vk.meterFrames = 0;
        g_vk.exposureLatch.Reset();
        g_vk.lastGpuTime.reset();
        g_vk.ngxInitialised = false;
        g_vk.reset = true;
        return;
    }

    // The device is alive (real teardown): drain before freeing so nothing the GPU is still using is
    // destroyed under it, the same rule as the resize path.
    if (g_vk.device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(g_vk.device);

    FreeRouteObjects();
}

// ---------------------------------------------------------------------------------------------
// The way back to the Vulkan present host (DlssNr_VkPresentModel.h, DlssNr_VkModelOwner.h): the after-upscale route's
// side of a hand-back, and of its device going.
// ---------------------------------------------------------------------------------------------

bool AfterUpscaleIdleOn(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_ownerMutex);
    return device != VK_NULL_HANDLE &&
           g_owner.IdleAfterUpscaleDevice(OwnerNowMs()) == reinterpret_cast<uint64_t>(device);
}

bool BeginAfterUpscaleHandBack(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_ownerMutex);
    return g_owner.BeginHandBack(reinterpret_cast<uint64_t>(device), OwnerNowMs());
}

AfterUpscaleHandBack FinishAfterUpscaleHandBack(VkDevice device, std::vector<std::string>& lines)
{
    // The route's lock, then the owner's, as the route takes them; never waited for under the host's transport lock.
    // Held by the route evaluating, which asks for the model (calling the hand-back off) or returns without recording.
    std::unique_lock<std::mutex> route(g_vkMutex, std::try_to_lock);
    if (!route.owns_lock())
        return AfterUpscaleHandBack::Busy;

    VkModelOwner::HandBack result;
    {
        std::lock_guard<std::mutex> lock(g_ownerMutex);
        result = g_owner.FinishHandBack(reinterpret_cast<uint64_t>(device));
    }

    if (result == VkModelOwner::HandBack::Cancelled)
        return AfterUpscaleHandBack::Cancelled;
    if (result == VkModelOwner::HandBack::NotRetiring)
        return AfterUpscaleHandBack::NotRetiring;

    // The token is released, and the route cannot take it back before this returns: its lock is held. Nothing of the
    // route's is in flight: it recorded nothing since the hand-back began (it would have called it off), and the
    // command buffers it recorded into before were submitted ahead of the drain's fences, all of which signalled.
    if (g_vk.device == device)
    {
        const bool feature = g_vk.feature != nullptr;
        const std::string size = std::to_string(g_vk.width) + "x" + std::to_string(g_vk.height);
        FreeRouteObjects();

        if (feature)
            SessionGuardOff();

        lines.push_back(std::string("DLSS-NR Vulkan: the after-upscale route's model was freed for the hand-back (") +
                        (feature ? "its feature at " + size + ", " : "no feature, ") +
                        "its images, pass and parameter block); NGX stays initialised on the device");
    }
    else
    {
        lines.push_back("DLSS-NR Vulkan: the after-upscale route had nothing on the device to free for the hand-back");
    }

    return AfterUpscaleHandBack::Done;
}

void CancelAfterUpscaleHandBack(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_ownerMutex);
    g_owner.CancelHandBack(reinterpret_cast<uint64_t>(device));
}

void AfterUpscaleDeviceGone(VkDevice device, bool forgetOnly, std::vector<std::string>& lines)
{
    // The route's lock may be waited for here: the route never takes the host's transport lock or a queue's present
    // lock, and vkDestroyDevice holds no queue's submit lock.
    {
        std::lock_guard<std::mutex> route(g_vkMutex);

        if (device != VK_NULL_HANDLE && g_vk.device == device)
        {
            const bool feature = g_vk.feature != nullptr;

            if (forgetOnly)
            {
                // Nothing called on a lost device, nor on one whose GPU was not proven done: forgotten, its destruction
                // frees them.
                ShutdownVk(false);
                lines.push_back("DLSS-NR Vulkan: the after-upscale route's objects on the device are abandoned with it "
                                "(the device is lost, or work on it was never seen complete)");
            }
            else
            {
                // Freed before the device goes and before NGX is shut down on it (its children must go first, the
                // feature before NGX): the host drained every queue of the device behind fences of its own, all seen
                // signalled, so nothing the GPU runs can still use them -- no device-wide wait, which could block every
                // present unbounded on a hung GPU.
                FreeRouteObjects();
                lines.push_back("DLSS-NR Vulkan: the after-upscale route's objects on the device freed before it "
                                "goes");
            }

            if (feature)
                SessionGuardOff();
        }
    }

    std::lock_guard<std::mutex> lock(g_ownerMutex);
    g_owner.DeviceGone(reinterpret_cast<uint64_t>(device));
}

// ---------------------------------------------------------------------------------------------
// The Vulkan present host's model (DlssNr_VkPresentModel.h)
// ---------------------------------------------------------------------------------------------

namespace
{
using VkPresentHost::NrSkip;

// Timestamp pairs for the pass's GPU time: more than the frames that can be in flight, so a frame finds one free.
constexpr uint32_t kPresentQueryPairs = 8;

// DlssNr_Vk's constant/descriptor slots (kSlotsPerFrame 6 x kFramesInFlight 3), handed out in order.
constexpr uint64_t kPresentPassSlots = 18;

// The device the present host brought the driver's NGX core up on (NVNGXProxy holds one), if it did. Read and written
// only under the host's transport lock, as every present-model call is.
VkDevice g_presentCoreDevice = VK_NULL_HANDLE;

// The device the present host last initialised NGX on through the forwarder, if it did. Under the same lock.
VkDevice g_presentForwarderDevice = VK_NULL_HANDLE;

// A feature and images replaced while frames that used them were in flight: freed once every one of those frames is
// seen complete, never before.
struct RetiredSet
{
    void* feature = nullptr;
    std::vector<OwnedImage> images;
    std::vector<uint64_t> waitFor;
};

// The core's shutdown on one device. Its device-specific entry takes an out count of what is left, as the D3D12 one
// does (NVNGXProxy::ShutdownDx12Device), which NVNGXProxy's Vulkan typedef leaves out: called through that, the core
// wrote the count through whatever was in the register (a null write inside _nvngx.dll, 2026-10-14). Called here with
// it.
void ShutdownPresentCore(VkDevice device)
{
    using PFN_Shutdown1 = NVSDK_NGX_Result (*)(VkDevice, unsigned int*);

    if (const auto shutdown = reinterpret_cast<PFN_Shutdown1>(NVNGXProxy::VULKAN_Shutdown1()))
    {
        unsigned int remaining = 0;
        shutdown(device, &remaining);
    }
}

// The forwarder's record of an NGX call's result (an exported int), as NGX names it.
std::string LastNgxResult(const char* variable)
{
    const auto* value = g_fwd.module != nullptr ? (const int*) GetProcAddress(g_fwd.module, variable) : nullptr;

    if (value == nullptr)
        return "result not recorded";

    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned int>(*value));
    return text;
}

std::wstring PresentModelDataPath()
{
    // NGX wants a writable folder for its own files. Nothing populates State's path when no game initialised NGX,
    // which is exactly this route's case: OptiScaler's own folder, where its log is written, is writable.
    if (!State::Instance().NVNGX_ApplicationDataPath.empty())
        return State::Instance().NVNGX_ApplicationDataPath;

    return Util::DllPath().parent_path().wstring();
}
} // namespace

struct VkPresentModel::Impl
{
    VkPresentModelDevice dev;

    bool failed = false;
    std::string failure;
    std::vector<std::string> lines; // for the host to log once its lock is released

    bool ngxInitialised = false;
    bool coreReady = false;                // the driver's NGX core is up on this device (NVNGXProxy)
    NVSDK_NGX_Parameter* params = nullptr; // the core's capability block
    bool floatSlotKnown = false;
    std::unique_ptr<DlssNr_Vk> pass;
    bool everSubmitted = false; // a recording of this model reached the GPU: the pass's one-time setup is real

    void* feature = nullptr;
    uint64_t featureTicket = 0; // the recording that created it and records its initialisation, until submitted

    OwnedImage output;     // the model's answer, at the working size
    OwnedImage proxy;      // the encode's display-referred proxy, full size
    OwnedImage keep;       // the untouched frame, full size
    OwnedImage proxySmall; // the proxy at the working size, below native only
    OwnedImage depth;      // zero guides at the working size: constant depth ...
    OwnedImage motion;     // ... and no motion
    bool guidesCleared = false;
    uint64_t guidesTicket = 0; // the recording that clears them, until submitted

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    bool reset = true;

    VkQueryPool queryPool = VK_NULL_HANDLE;
    double tickNs = 0.0;
    uint64_t tickMask = ~0ull;
    std::array<uint64_t, kPresentQueryPairs> queryTicket {}; // 0: free

    std::vector<uint64_t> pending; // submitted, not yet seen complete

    // The pass's descriptor/constant ring: DlssNr_Vk hands out its eighteen slots in order, one per dispatch, with no
    // completion test. Every dispatch recorded is counted here, discarded recordings' included (they advanced the
    // ring too), and each submitted frame keeps the count at its first, so a frame is admitted only while the oldest
    // frame in flight is less than a ring behind it.
    uint64_t dispatches = 0;
    uint64_t recordFirst = 0;
    std::vector<std::pair<uint64_t, uint64_t>> firstDispatch; // ticket, the dispatch count at its first
    std::vector<RetiredSet> retired;
    bool adapterNoted = false;
    uint64_t submitted = 0;

    void Say(std::string line) { lines.push_back("DLSS-NR Vulkan present host: " + std::move(line)); }

    VkPresentAdmission Latch(NrSkip skip, std::string why)
    {
        if (!failed)
        {
            failed = true;
            failure = std::move(why);
            Say("the model is off on this device: " + failure);
        }

        return { skip, failure };
    }

    bool FaultLatched()
    {
        std::string description;
        if (!ModelFaulted(&description))
            return false;

        Latch(NrSkip::Unavailable, "the model faulted inside its own code (" + description + "); off for this session");
        return true;
    }

    void Free(RetiredSet& set)
    {
        if (set.feature != nullptr && g_fwd.release != nullptr)
            g_fwd.release(set.feature);

        set.feature = nullptr;

        for (OwnedImage& image : set.images)
            DestroyImageOn(dev.device, image);

        set.images.clear();
    }

    // Free the retired sets nothing in flight uses any more.
    void FreeRetired()
    {
        for (auto it = retired.begin(); it != retired.end();)
        {
            if (it->waitFor.empty())
            {
                Free(*it);
                it = retired.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
};

VkPresentModel::VkPresentModel(const VkPresentModelDevice& device) : _impl(std::make_unique<Impl>())
{
    _impl->dev = device;
}

// Neither Release nor Abandon ran: whatever is left is abandoned (nothing here may call Vulkan).
VkPresentModel::~VkPresentModel() { Abandon(); }

size_t VkPresentModel::InFlight() const { return _impl->pending.size(); }

const std::string& VkPresentModel::Failure() const { return _impl->failure; }

std::vector<std::string> VkPresentModel::TakeLines()
{
    std::vector<std::string> lines;
    lines.swap(_impl->lines);
    return lines;
}

VkPresentAdmission VkPresentModel::Admit(uint32_t width, uint32_t height)
{
    Impl& m = *_impl;
    auto& cfg = *Config::Instance();

    if (m.failed)
        return { NrSkip::Unavailable, m.failure };

    if (m.FaultLatched())
        return { NrSkip::Unavailable, m.failure };

    if (!m.adapterNoted)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(m.dev.physical, &props);
        DlssNr::NoteAdapter(props.vendorID, props.deviceID);
        m.adapterNoted = true;
    }

    // Never above native: on zero guides the model refuses a working size above the frame (FAIL_InvalidParameter),
    // as the D3D12 present hosts found; they clamp it the same way.
    const float workScale = std::clamp(DlssNr::WorkingScale(), 0.25f, 1.0f);
    const uint32_t workWidth = (uint32_t) (width * workScale + 0.5f);
    const uint32_t workHeight = (uint32_t) (height * workScale + 0.5f);

    // See kDlssNrMinExtent: a probe-sized swapchain must never get a model built for it.
    if (width < kDlssNrMinExtent || height < kDlssNrMinExtent || workWidth < kDlssNrMinExtent ||
        workHeight < kDlssNrMinExtent)
    {
        return { NrSkip::TooSmall, std::to_string(width) + "x" + std::to_string(height) + " (working " +
                                       std::to_string(workWidth) + "x" + std::to_string(workHeight) + ") is below " +
                                       std::to_string(kDlssNrMinExtent) +
                                       " pixels; the model is not built that small" };
    }

    if (m.pending.size() >= VkPresentHost::kNrInFlightMax)
        return { NrSkip::Busy, "the model already has " + std::to_string(m.pending.size()) + " frames in flight" };

    // A frame takes at most three slots (encode, downsample, resolve): it may not reach one of the oldest frame's.
    if (!m.firstDispatch.empty())
    {
        uint64_t oldest = m.firstDispatch.front().second;
        for (const auto& [ticket, first] : m.firstDispatch)
            oldest = std::min(oldest, first);
        if (m.dispatches + 3 - oldest > kPresentPassSlots)
            return { NrSkip::Busy, "the pass's descriptor ring would reach a frame still in flight" };
    }

    if (const char* why = LoadForwarderShared())
        return m.Latch(NrSkip::Unavailable, why);

    if (!m.ngxInitialised)
    {
        auto snippet = FindSnippet();

        if (!snippet.has_value())
            return m.Latch(NrSkip::Unavailable, "nvngx_dlssnr.dll was not found beside OptiScaler or the game");

        // Before init and the first build, so the model's own account of them is in the log.
        DlssNr::ModelLog::Install(*snippet);

        // Four bits, one per entry point: anything short of fifteen and the model's Vulkan surface is not all there.
        const int probe = g_fwd.probe != nullptr ? g_fwd.probe(snippet->wstring().c_str()) : 0;

        if (probe != 15)
            return m.Latch(NrSkip::Unavailable,
                           "the model does not expose a complete Vulkan surface (probe " + std::to_string(probe) + ")");

        // Cold: no application NGX initialisation exists to reuse. The same SDK version the after-upscale route
        // initialises with.
        const std::wstring dataPath = PresentModelDataPath();
        const int result = g_fwd.init(snippet->wstring().c_str(), dataPath.c_str(), (void*) m.dev.instance,
                                      (void*) m.dev.physical, (void*) m.dev.device, 0x0000015);

        if (result != 1)
        {
            if (m.FaultLatched())
                return { NrSkip::Unavailable, m.failure };

            return m.Latch(NrSkip::Unavailable, "NVSDK_NGX_VULKAN_Init_Ext returned " + std::to_string(result) +
                                                    " on this device (NGX " + LastNgxResult("dlssnr_vk_last_init") +
                                                    ")");
        }

        m.ngxInitialised = true;
        g_presentForwarderDevice = m.dev.device;
        char device[24];
        std::snprintf(device, sizeof(device), "0x%llX",
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(m.dev.device)));
        m.Say(std::string("the model initialised on device ") + device +
              " (cold: no application NGX initialisation; the driver's NGX core brought up on it)");
    }

    // The driver's NGX core on this device, as the D3D12 route brings it up: the model's feature wants the core's own
    // capability block, which carries the snippet's and the presets' callbacks; a freshly allocated block (OptiScaler's
    // own, when no DLSS is loaded) does not, and the feature is then refused at creation.
    if (!m.coreReady)
    {
        // Brought up by an earlier model of the host's on another device, still alive: shut down there first, since
        // NVNGXProxy holds one. Brought up by anything else (a game's own NGX through OptiScaler): used as it is.
        if (g_presentCoreDevice != VK_NULL_HANDLE && g_presentCoreDevice != m.dev.device)
        {
            ShutdownPresentCore(g_presentCoreDevice);
            NVNGXProxy::SetVulkanInited(false);
            g_presentCoreDevice = VK_NULL_HANDLE;
        }

        if (!NVNGXProxy::IsVulkanInited())
        {
            NVNGXProxy::InitVulkan(m.dev.instance, m.dev.physical, m.dev.device, vkGetInstanceProcAddr,
                                   vkGetDeviceProcAddr);

            if (!NVNGXProxy::IsVulkanInited())
                return m.Latch(NrSkip::Unavailable, "the driver's NGX core would not initialise on this device");

            g_presentCoreDevice = m.dev.device;
        }

        m.coreReady = true;
    }

    if (m.params == nullptr)
    {
        const auto capabilities = NVNGXProxy::VULKAN_GetCapabilityParameters();

        if (capabilities == nullptr || capabilities(&m.params) != NVSDK_NGX_Result_Success || m.params == nullptr)
        {
            m.params = nullptr;
            return m.Latch(NrSkip::Unavailable, "the driver's NGX core gave no capability parameters");
        }
    }

    // Before anything is written to it, where this block keeps floats.
    DiscoverFloatSlotFor(m.floatSlotKnown, m.params);

    if (m.queryPool == VK_NULL_HANDLE && m.dev.timestampValidBits != 0)
    {
        VkPhysicalDeviceProperties props {};
        vkGetPhysicalDeviceProperties(m.dev.physical, &props);

        if (props.limits.timestampPeriod > 0.0f)
        {
            VkQueryPoolCreateInfo info {};
            info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kPresentQueryPairs * 2;

            if (vkCreateQueryPool(m.dev.device, &info, nullptr, &m.queryPool) == VK_SUCCESS)
            {
                m.tickNs = props.limits.timestampPeriod;
                m.tickMask = m.dev.timestampValidBits >= 64 ? ~0ull : ((1ull << m.dev.timestampValidBits) - 1);
            }
            else
            {
                m.queryPool = VK_NULL_HANDLE;
            }
        }
    }

    if (m.pass == nullptr)
    {
        m.pass = std::make_unique<DlssNr_Vk>("Neural Rendering (present host)", m.dev.device, m.dev.physical);

        if (!m.pass->IsInit())
        {
            // Nothing of it was ever recorded: its own destructor frees what it made.
            m.pass.reset();
            return m.Latch(NrSkip::Unavailable, "the composition pass could not be created");
        }
    }

    if (m.width != width || m.height != height || m.workWidth != workWidth || m.workHeight != workHeight)
    {
        // The feature is built for a size and the images are this size. Whatever frames in flight still use the old
        // ones keep them until they are seen complete (DEVELOPMENT.md rule 8), then they are freed.
        RetiredSet old;
        old.feature = m.feature;
        for (OwnedImage* image : { &m.output, &m.proxy, &m.keep, &m.proxySmall, &m.depth, &m.motion })
        {
            if (image->image != VK_NULL_HANDLE || image->memory != VK_NULL_HANDLE)
                old.images.push_back(*image);
            *image = OwnedImage {};
        }
        old.waitFor = m.pending;
        m.feature = nullptr;
        m.featureTicket = 0;

        if (old.feature != nullptr || !old.images.empty())
            m.retired.push_back(std::move(old));
        m.FreeRetired();

        const bool reduced = workWidth != width || workHeight != height;
        const VkFormat working = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkDevice d = m.dev.device;
        VkPhysicalDevice pd = m.dev.physical;

        // The guides are made at the working size: a resample of a constant is the constant, so this is the
        // source-to-working scaling done once, exactly.
        const bool ok = CreateImageOn(d, pd, m.output, workWidth, workHeight, working, true) &&
                        CreateImageOn(d, pd, m.proxy, width, height, working, true) &&
                        CreateImageOn(d, pd, m.keep, width, height, working, true) &&
                        (!reduced || CreateImageOn(d, pd, m.proxySmall, workWidth, workHeight, working, true)) &&
                        CreateImageOn(d, pd, m.depth, workWidth, workHeight, VK_FORMAT_R32_SFLOAT, false) &&
                        CreateImageOn(d, pd, m.motion, workWidth, workHeight, VK_FORMAT_R16G16_SFLOAT, false);

        m.width = width;
        m.height = height;
        m.workWidth = workWidth;
        m.workHeight = workHeight;
        m.guidesCleared = false;
        m.guidesTicket = 0;
        m.reset = true;

        if (!ok)
            return m.Latch(NrSkip::Unavailable, "the model's images could not be allocated at " +
                                                    std::to_string(width) + "x" + std::to_string(height));

        m.Say("the model's images built for " + std::to_string(width) + "x" + std::to_string(height) + " (working " +
              std::to_string(workWidth) + "x" + std::to_string(workHeight) +
              "), zero guides (constant depth, no motion) at the working size");
    }

    return {};
}

bool VkPresentModel::Record(const VkPresentNrFrame& frame)
{
    Impl& m = *_impl;
    auto& cfg = *Config::Instance();
    VkCommandBuffer cmd = frame.cmd;

    if (m.failed || m.pass == nullptr || cmd == VK_NULL_HANDLE || frame.view == VK_NULL_HANDLE ||
        frame.width != m.width || frame.height != m.height)
        return false;

    const bool reduced = m.workWidth != m.width || m.workHeight != m.height;
    m.recordFirst = m.dispatches;

    // Every image the pass rewrites starts the frame discarded, so no layout is carried over from a recording that may
    // have been thrown away. Earlier frames' use of them is ordered before by Transition's ALL_COMMANDS dependency.
    for (OwnedImage* image : { &m.output, &m.proxy, &m.keep, &m.proxySmall })
        image->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // The pass's GPU time: a pair of timestamps, read once this frame's fence is seen signalled, never waited on.
    uint32_t pair = UINT32_MAX;
    if (m.queryPool != VK_NULL_HANDLE)
    {
        for (uint32_t i = 0; i < kPresentQueryPairs && pair == UINT32_MAX; ++i)
        {
            if (m.queryTicket[i] == 0)
                pair = i;
        }

        if (pair != UINT32_MAX)
        {
            m.queryTicket[pair] = frame.ticket;
            vkCmdResetQueryPool(cmd, m.queryPool, pair * 2, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m.queryPool, pair * 2);
        }
    }

    const auto fail = [&m](std::string why)
    {
        if (!m.FaultLatched())
            m.Latch(NrSkip::RecordFailed, std::move(why));
        return false;
    };

    // The zero guides, once per allocation: cleared with vkCmdClearColorImage and left in GENERAL, where the model
    // reads them, with the clear's writes made visible to it.
    if (!m.guidesCleared)
    {
        VkImageMemoryBarrier toClear[2] {};
        VkImageMemoryBarrier toRead[2] {};
        OwnedImage* guides[2] = { &m.depth, &m.motion };

        for (int i = 0; i < 2; ++i)
        {
            toClear[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toClear[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toClear[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear[i].image = guides[i]->image;
            toClear[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            toClear[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            toRead[i] = toClear[i];
            toRead[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toRead[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, toClear);

        const VkClearColorValue zero {};
        const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, m.depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        vkCmdClearColorImage(cmd, m.motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 2, toRead);

        m.depth.layout = VK_IMAGE_LAYOUT_GENERAL;
        m.motion.layout = VK_IMAGE_LAYOUT_GENERAL;
        m.guidesTicket = frame.ticket;
    }

    // Encode. The presented frame is display-referred SDR: the encode is the identity (gPassthrough), and so is every
    // place the shader reproduces it. No game exposure exists on this route; the white point is the slider's.
    DlssNrConstants encode {};
    encode.Mode = DlssNrMode_Encode;
    encode.Width = m.width;
    encode.Height = m.height;
    encode.WhitePoint = cfg.DlssNrWhitePointScale.value_or_default();
    encode.Passthrough = 1u;
    encode.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encode.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
    encode.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
    encode.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
    encode.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
    SetVulkanTransfer(encode, cfg);
    encode.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
    encode.GuideWidth = m.workWidth;
    encode.GuideHeight = m.workHeight;
    encode.HighlightRecovery = 0u; // linear HDR only, and this frame is SDR
    encode.ResidualLimit = DlssNr::ResidualLimitStops(cfg.DlssNrResidualLimit.value_or_default());

    Transition(cmd, m.proxy, VK_IMAGE_LAYOUT_GENERAL);
    Transition(cmd, m.keep, VK_IMAGE_LAYOUT_GENERAL);

    ++m.dispatches;
    if (!m.pass->Dispatch(cmd, encode, m.width, m.height, frame.view, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                          m.proxy.view, m.keep.view, VK_IMAGE_LAYOUT_GENERAL))
        return fail("the encode dispatch failed");

    OwnedImage* modelInput = &m.proxy;

    if (reduced)
    {
        DlssNrConstants down = encode;
        down.Mode = DlssNrMode_Downsample;
        down.Width = m.workWidth;
        down.Height = m.workHeight;

        Transition(cmd, m.proxy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, m.proxySmall, VK_IMAGE_LAYOUT_GENERAL);

        ++m.dispatches;
        if (!m.pass->Dispatch(cmd, down, m.workWidth, m.workHeight, m.proxy.view, VK_NULL_HANDLE, VK_NULL_HANDLE,
                              VK_NULL_HANDLE, m.proxySmall.view, VK_NULL_HANDLE,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            return fail("the downsample dispatch failed");

        modelInput = &m.proxySmall;
    }

    Transition(cmd, m.output, VK_IMAGE_LAYOUT_GENERAL);

    // The feature, built cold: no game parameters, the tuning from the config, UI correction on as the after-upscale
    // route creates it. Its initialisation is recorded into this buffer, which is why a discarded recording releases
    // it again (Discarded).
    if (m.feature == nullptr)
    {
        m.feature =
            g_fwd.create((void*) cmd, m.params, m.workWidth, m.workHeight, (int) cfg.DlssNrPreset.value_or_default(),
                         cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
                         cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                         cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

        if (m.feature == nullptr)
            return fail("the model would not build a feature at " + std::to_string(m.workWidth) + "x" +
                        std::to_string(m.workHeight) + " (NGX " + LastNgxResult("dlssnr_vk_last_create") + ")");

        m.featureTicket = frame.ticket;
        m.reset = true;
        m.Say("the model's feature is up at " + std::to_string(m.workWidth) + "x" + std::to_string(m.workHeight) +
              " (frame " + std::to_string(m.width) + "x" + std::to_string(m.height) + ")");
    }

    // Zero guides: constant depth (read inverted, so zero is the far plane) and no motion, both covering the working
    // size from the origin, at unit scale: nothing moved, so the scale has nothing to scale.
    const bool reset = VkPresentHost::NrReset(m.reset, frame.cut, cfg.DlssNrZeroGuideReset.value_or_default());
    const int evaluated = g_fwd.evaluate(
        (void*) cmd, m.feature, m.params, &modelInput->ngx, &m.depth.ngx, &m.motion.ngx, &m.output.ngx, m.workWidth,
        m.workHeight, m.workWidth, m.workHeight, m.workWidth, m.workHeight, 0, 0, 0, 0, 1, reset ? 1 : 0,
        cfg.DlssNrIntensity.value_or_default(), (int) cfg.DlssNrStyle.value_or_default(),
        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
        cfg.DlssNrSkinStructure.value_or_default(), cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1.0f, 1.0f);

    if (evaluated != 1)
        return fail("the model refused to evaluate (returned " + std::to_string(evaluated) + ")");

    _lastReset = reset;
    m.reset = false;

    // Resolve: the proxy, the model's answer and the untouched frame back into the working colour.
    DlssNrConstants resolve = encode;
    resolve.Mode = DlssNrMode_Resolve;

    Transition(cmd, *modelInput, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m.output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Transition(cmd, m.keep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ++m.dispatches;
    if (!m.pass->Dispatch(cmd, resolve, m.width, m.height, modelInput->view, m.output.view, m.keep.view, VK_NULL_HANDLE,
                          frame.view, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        return fail("the resolve dispatch failed");

    if (pair != UINT32_MAX)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m.queryPool, pair * 2 + 1);

    return true;
}

void VkPresentModel::Submitted(uint64_t ticket)
{
    Impl& m = *_impl;

    m.pending.push_back(ticket);
    m.firstDispatch.emplace_back(ticket, m.recordFirst);
    m.everSubmitted = true;

    if (m.guidesTicket == ticket)
    {
        m.guidesCleared = true;
        m.guidesTicket = 0;
    }

    if (m.featureTicket == ticket)
        m.featureTicket = 0;

    if (++m.submitted == 1)
        m.Say("the model runs at present on " + std::to_string(m.width) + "x" + std::to_string(m.height) +
              " (working " + std::to_string(m.workWidth) + "x" + std::to_string(m.workHeight) + "), on zero guides");

    SessionGuardEvaluated();
}

void VkPresentModel::Discarded(uint64_t ticket)
{
    Impl& m = *_impl;

    // The clear never ran: still owed, from UNDEFINED, which is where the guides still are.
    if (m.guidesTicket == ticket)
    {
        m.guidesTicket = 0;
        m.guidesCleared = false;
    }

    // Built in this recording, so its initialisation never ran and no frame on the GPU uses it: built again next time.
    if (m.featureTicket == ticket && m.feature != nullptr)
    {
        if (g_fwd.release != nullptr)
            g_fwd.release(m.feature);
        m.feature = nullptr;
        m.featureTicket = 0;
    }

    for (uint64_t& owner : m.queryTicket)
    {
        if (owner == ticket)
            owner = 0;
    }

    m.reset = true;

    // The pass records its own one-time setup (its placeholder image's layout) into its first recording; if no
    // recording of it ever reached the GPU, that setup is not real. Nothing submitted ever used it, so it is rebuilt.
    if (!m.everSubmitted)
        m.pass.reset();
}

std::optional<double> VkPresentModel::Completed(uint64_t ticket)
{
    Impl& m = *_impl;

    const auto found = std::find(m.pending.begin(), m.pending.end(), ticket);
    if (found == m.pending.end())
        return std::nullopt;

    m.pending.erase(found);
    std::erase_if(m.firstDispatch, [ticket](const auto& entry) { return entry.first == ticket; });

    std::optional<double> ms;
    for (uint32_t i = 0; i < kPresentQueryPairs; ++i)
    {
        if (m.queryTicket[i] != ticket)
            continue;

        uint64_t ticks[2] = {};
        // Without WAIT: the frame's fence was seen signalled, so the pair is written.
        if (m.queryPool != VK_NULL_HANDLE &&
            vkGetQueryPoolResults(m.dev.device, m.queryPool, i * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const uint64_t begin = ticks[0] & m.tickMask;
            const uint64_t end = ticks[1] & m.tickMask;
            if (end > begin)
            {
                const double value = (double) (end - begin) * m.tickNs / 1e6;
                if (value > 0.0 && value < 1000.0)
                    ms = value;
            }
        }

        m.queryTicket[i] = 0;
    }

    for (RetiredSet& set : m.retired)
        std::erase(set.waitFor, ticket);
    m.FreeRetired();

    return ms;
}

void VkPresentModel::Release()
{
    Impl& m = *_impl;

    // Only with nothing in flight: the caller saw every frame complete.
    if (!m.pending.empty())
    {
        Abandon();
        return;
    }

    for (RetiredSet& set : m.retired)
        set.waitFor.clear();
    m.FreeRetired();

    if (m.feature != nullptr && g_fwd.release != nullptr)
        g_fwd.release(m.feature);
    m.feature = nullptr;

    if (m.params != nullptr)
    {
        if (const auto destroy = NVNGXProxy::VULKAN_DestroyParameters())
            destroy(m.params);
        m.params = nullptr;
    }

    for (OwnedImage* image : { &m.output, &m.proxy, &m.keep, &m.proxySmall, &m.depth, &m.motion })
        DestroyImageOn(m.dev.device, *image);

    m.pass.reset();

    if (m.queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(m.dev.device, m.queryPool, nullptr);
        m.queryPool = VK_NULL_HANDLE;
    }

    // NGX and the driver's core stay up on the device: they are the device's, not this model's, and go with it
    // (ShutdownPresentNgx, at vkDestroyDevice). A model made again on this device finds them up.
    m.coreReady = false;

    if (m.ngxInitialised)
        SessionGuardOff();

    // A latched failure stays: a model that could not run here keeps saying why, without holding anything.
    m.ngxInitialised = false;
    m.width = m.height = m.workWidth = m.workHeight = 0;
    m.guidesCleared = false;
    m.reset = true;
    m.everSubmitted = false;
    m.dispatches = 0;
    m.recordFirst = 0;
    m.firstDispatch.clear();
}

void VkPresentModel::Abandon()
{
    Impl& m = *_impl;

    // The pass's destructor would call vkDestroy* on a device that may be gone: leaked instead, as ShutdownVk(false)
    // leaks the after-upscale route's.
    (void) m.pass.release();

    m.feature = nullptr;
    m.params = nullptr;
    m.queryPool = VK_NULL_HANDLE;
    m.retired.clear();
    m.pending.clear();
    for (OwnedImage* image : { &m.output, &m.proxy, &m.keep, &m.proxySmall, &m.depth, &m.motion })
        *image = OwnedImage {};
    m.width = m.height = m.workWidth = m.workHeight = 0;
    m.ngxInitialised = false;

    m.coreReady = false;
    m.firstDispatch.clear();
}

void ShutdownPresentNgx(VkDevice device, bool forgetOnly, std::vector<std::string>& lines)
{
    if (g_presentCoreDevice == device)
    {
        if (!forgetOnly)
            ShutdownPresentCore(device);
        NVNGXProxy::SetVulkanInited(false);
        g_presentCoreDevice = VK_NULL_HANDLE;
        lines.push_back(forgetOnly
                            ? "DLSS-NR Vulkan present host: the driver's NGX core on the device is forgotten, "
                              "not shut down (it is lost, or frames on it never completed)"
                            : "DLSS-NR Vulkan present host: the driver's NGX core shut down on the device before "
                              "it goes");
    }

    // The forwarder answers only for the device it initialised (-1 otherwise), so this is asked for every device: one
    // the after-upscale route initialised NGX on goes the same way.
    if (g_fwd.module == nullptr)
        return;

    const bool ours = g_presentForwarderDevice == device;
    if (ours)
        g_presentForwarderDevice = VK_NULL_HANDLE;

    if (forgetOnly)
    {
        if (const auto forget = (int(__cdecl*)(void*)) GetProcAddress(g_fwd.module, "dlssnr_vk_forget"))
        {
            if (forget((void*) device) == 1)
                lines.push_back("DLSS-NR Vulkan present host: NGX on the device forgotten by the forwarder, not shut "
                                "down");
        }
        return;
    }

    if (g_fwd.shutdown != nullptr)
    {
        const int result = g_fwd.shutdown((void*) device);
        if (result != -1)
            lines.push_back("DLSS-NR Vulkan present host: NGX shut down on the device before it goes (returned " +
                            std::to_string(result) + ")");
    }
    else if (ours)
    {
        lines.push_back("DLSS-NR Vulkan present host: this forwarder cannot shut NGX down on a device (no "
                        "dlssnr_vk_shutdown); a later device with this handle value would not be initialised again");
    }
}

VkPresentAdmission ClaimModelForPresentHost(VkDevice device)
{
    // The D3D12 route's model in this process (a translated D3D12 title's own NR, a Vulkan title through the D3D12
    // bridge, or the D3D11 bridge's present host): the frame would be processed twice.
    if (DlssNr::IsRunning())
        return { NrSkip::D3d12, "the D3D12 route has the model running in this process; the Vulkan present host "
                                "stands down" };

    std::lock_guard<std::mutex> lock(g_ownerMutex);
    const auto verdict = g_owner.PresentHost(reinterpret_cast<uint64_t>(device), OwnerNowMs());

    if (!verdict.granted)
        return { verdict.reason == VkModelOwner::kOtherDevice ? NrSkip::OtherDevice : NrSkip::AfterUpscale,
                 verdict.reason };

    g_owner.Take(reinterpret_cast<uint64_t>(device));
    return {};
}

VkPresentAdmission PresentHostMustYield(VkDevice device)
{
    if (DlssNr::IsRunning())
        return { NrSkip::D3d12, "the D3D12 route has the model running in this process; the Vulkan present host "
                                "stands down" };

    std::lock_guard<std::mutex> lock(g_ownerMutex);
    if (g_owner.PresentHostMustYield(reinterpret_cast<uint64_t>(device), OwnerNowMs()))
        return { NrSkip::AfterUpscale, VkModelOwner::kAfterUpscaleWants };

    return {};
}

void ReleaseModelFromPresentHost(VkDevice device)
{
    std::lock_guard<std::mutex> lock(g_ownerMutex);
    g_owner.Release(VkModelOwner::Who::PresentHost, reinterpret_cast<uint64_t>(device));
}

} // namespace DlssNr
