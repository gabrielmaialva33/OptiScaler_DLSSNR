#pragma once

// What the Vulkan present host has seen, and the lines that say so (docs/dlssnr/design/nr-vulkan-present-host.md,
// stages 1 to 3).
//
// hooks/Vulkan_Hooks.cpp remembers one of each thing -- _instance, _PD, _device, _hwnd, and o_CreateSwapchainKHR as a
// first-device sentinel -- the last one seen, for the overlay. A present host has to know, for any present, which
// device and which queue family it came from and what the swapchain's images allow, for every device at once, and a
// handle value that comes round again after a destroy must not inherit the old object's facts. So every object here
// is a record with an id of its own, taken from one counter that only goes up: a reused handle is a new record with a
// new id, and the log prints the id beside the handle.
//
// Instance -> physical device (vkEnumeratePhysicalDevices) -> device (vkCreateDevice, with the physical device's
// queue families and the queues the create info asked for) -> queues (what vkGetDeviceQueue/vkGetDeviceQueue2
// returned, never inferred from a handle). Surface -> HWND. Swapchain -> device, surface, images, format, extent,
// usage (as asked and as created), sharing, and whether the round trip may run on it. Destroys retire records before
// the downstream call, while the handle cannot yet be handed out again; acquires are counted per image.
//
// The records decide nothing about the round trip (DlssNr_VkPresentTransport.h does); they answer what the transport
// asks (ViewPresent), add up what each present's outcome was, and write the lines. Standard library only, and
// Vulkan's values are plain integers, so tests/nr-vk-present-host drives all of it on the host.

#include "DlssNr_VkPresentTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DlssNr::VkPresentHost
{
using RecordId = uint64_t; // 0: not known to the host

inline constexpr size_t kPresentSwapchains = 4; // swapchains of one present attributed to their windows
inline constexpr size_t kPresentChain = 8;      // pNext structures of one present whose sType is noted
inline constexpr size_t kChainTypesKept = 8;    // distinct present pNext sTypes a window keeps between lines

// Who called vkQueuePresentKHR (or vkCreateDevice), from the module the return address lies in -- the same test
// hooks/Vulkan_Hooks.cpp's IsDxgiBackedCaller makes for the overlay.
enum class Caller : uint8_t
{
    Unknown,     // no module at the return address
    Native,      // any other module: a Vulkan game or engine
    Dxvk,        // dxgi.dll, d3d11.dll, d3d10core.dll, d3d9.dll, d3d8.dll
    Vkd3dProton, // d3d12.dll, d3d12core.dll
    Loader,      // vulkan-1.dll or winevulkan.dll: a trampoline; who called it is not visible from here
    OptiScaler,  // our own module: one of OptiScaler's own Vulkan hooks ran first and called down to this one
};

// `lowerName` is the module's file name in lower case; `ownModule` says it is OptiScaler's own.
inline Caller ClassifyCaller(std::string_view lowerName, bool ownModule)
{
    if (ownModule)
        return Caller::OptiScaler;

    if (lowerName.empty())
        return Caller::Unknown;

    if (lowerName == "dxgi.dll" || lowerName == "d3d11.dll" || lowerName == "d3d10core.dll" ||
        lowerName == "d3d9.dll" || lowerName == "d3d8.dll")
        return Caller::Dxvk;

    if (lowerName == "d3d12.dll" || lowerName == "d3d12core.dll")
        return Caller::Vkd3dProton;

    if (lowerName == "vulkan-1.dll" || lowerName == "winevulkan.dll")
        return Caller::Loader;

    return Caller::Native;
}

inline const char* CallerName(Caller caller)
{
    switch (caller)
    {
    case Caller::Native:
        return "native Vulkan";
    case Caller::Dxvk:
        return "DXVK";
    case Caller::Vkd3dProton:
        return "vkd3d-proton";
    case Caller::Loader:
        return "the Vulkan loader, caller beyond it not visible";
    case Caller::OptiScaler:
        return "OptiScaler's own Vulkan hook, called down to this one";
    default:
        return "unknown";
    }
}

inline std::string Hex(uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

inline const char* YesNo(bool value) { return value ? "yes" : "no"; }

struct QueueRequest // one VkDeviceQueueCreateInfo
{
    uint32_t family = 0;
    uint32_t count = 0;
    uint32_t flags = 0; // VkDeviceQueueCreateFlags
};

struct DeviceDesc
{
    Handle physical = 0;
    std::vector<QueueFamily> families;   // the physical device's, by family index; empty when the query failed
    std::vector<QueueRequest> requested; // what VkDeviceCreateInfo asked for
    bool internal = false;               // created by OptiScaler itself: its presents are not reported
    Caller creator = Caller::Unknown;
    std::string creatorModule;
};

struct SwapchainDesc // the VkSwapchainCreateInfoKHR the driver was given
{
    Handle surface = 0;
    Handle oldSwapchain = 0;
    uint32_t format = 0;
    uint32_t colorSpace = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t arrayLayers = 0;
    uint32_t usage = 0;      // as created: what the driver was given
    uint32_t askedUsage = 0; // as the application asked
    uint32_t sharingMode = 0;
    std::vector<uint32_t> sharingFamilies; // CONCURRENT only
    uint32_t minImageCount = 0;
    uint32_t presentMode = 0;
    uint32_t flags = 0;
    bool surfaceUsageKnown = false;
    uint32_t surfaceUsage = 0; // VkSurfaceCapabilitiesKHR::supportedUsageFlags, when the query answered
    bool transport = false;    // UsagePlan::transport: the round trip may run on it
    std::string transportNote; // UsagePlan::note
};

// One vkQueuePresentKHR, as the hook saw it before calling down.
struct PresentCall
{
    uint64_t nowMs = 0; // monotonic time, supplied by the caller (tests use a fake clock)
    Handle queue = 0;
    uint32_t waitSemaphores = 0;
    uint32_t swapchainCount = 0;
    std::array<Handle, kPresentSwapchains> swapchains {};
    std::array<uint32_t, kPresentSwapchains> imageIndices {};
    uint32_t chainLength = 0; // pNext structures, all of them
    std::array<uint32_t, kPresentChain> chainTypes {};
    Caller caller = Caller::Unknown;
    Handle callerModule = 0;
    std::string_view callerName;
    Outcome outcome; // the transport's result, the same for every swapchain of the call
};

// The round trip's state for a swapchain, in the words the lines use: "on, TRANSFER_SRC added at creation", "on as
// created", or "off, <why>" (observe only).
inline std::string RoundTrip(const SwapchainDesc& desc)
{
    if (desc.transport)
        return desc.transportNote == "as created" ? std::string("on as created") : "on, " + desc.transportNote;

    return "off, " + (desc.transportNote.empty() ? std::string("not planned") : desc.transportNote);
}

// What the transport needs to know about one present, read from the records in one go.
struct PresentView
{
    bool internal = false; // OptiScaler's own device: never transported, never counted
    Handle device = 0;
    Handle physical = 0;
    Handle instance = 0; // the physical device's instance, 0 when not on record
    RecordId deviceId = 0;
    RecordId swapchainId = 0;
    uint32_t presentFamily = 0;
    std::vector<QueueFamily> families;
    std::vector<QueueCandidate> queues; // the device's retrieved queues
    SwapchainDesc desc;
    uint32_t imageIndex = 0;
    bool imageKnown = false; // the index is within the images the application fetched
    Handle image = 0;
    uint32_t imageCount = 0;
    bool acquired = false; // an acquire of the image was observed since its last present
    bool retired = false;  // a newer swapchain named this one as its oldSwapchain
};

class Registry
{
  public:
    struct Instance
    {
        RecordId id = 0;
        bool internal = false;
        bool properties2 = false; // Vulkan 1.1 or VK_KHR_get_physical_device_properties2: what the NR extensions need
        uint32_t apiVersion = 0;  // VkApplicationInfo::apiVersion (0 when none was given: Vulkan 1.0)
    };

    struct Physical
    {
        Handle instance = 0;
        RecordId instanceId = 0;
    };

    struct Device
    {
        RecordId id = 0;
        RecordId instanceId = 0;
        DeviceDesc desc;
    };

    struct Queue
    {
        Handle device = 0;
        RecordId deviceId = 0;
        uint32_t family = 0;
        uint32_t index = 0;
        uint32_t flags = 0; // VkDeviceQueueCreateFlags it was created with
    };

    struct Surface
    {
        RecordId id = 0;
        uint64_t hwnd = 0;
    };

    struct Swapchain
    {
        RecordId id = 0;
        RecordId deviceId = 0;
        SwapchainDesc desc;
        bool hwndKnown = false;
        uint64_t hwnd = 0;
        uint32_t imageCount = 0; // from vkGetSwapchainImagesKHR; 0 until the application asked
        std::vector<Handle> images;
        std::vector<uint8_t> acquired; // per image: acquired through the host and not yet presented
        uint64_t acquires = 0;
        uint64_t presents = 0;
        bool retired = false; // a newer swapchain named this one as oldSwapchain
        bool hasWindow = false;
        TransportStats transport; // every present of this swapchain, and completions observed at its retirement
    };

    // What a window's line reports; a change of any of it writes a line.
    struct Identity
    {
        RecordId swapchainId = 0;
        RecordId deviceId = 0;
        Handle queue = 0;
        Caller caller = Caller::Unknown;
        Handle callerModule = 0;

        bool operator==(const Identity&) const = default;
    };

    struct Window
    {
        bool hwndKnown = false;
        uint64_t hwnd = 0;
        uint64_t presents = 0;
        std::vector<RecordId> swapchainIds; // distinct, ever presented here
        std::vector<RecordId> deviceIds;    // distinct, ever presented here
        uint64_t nextDecade = 10;
        uint64_t lastReportMs = 0;
        bool reported = false;
        Identity last;

        // Since the last line, this present included.
        uint32_t waitsMin = UINT32_MAX;
        uint32_t waitsMax = 0;
        uint64_t multiSwapchain = 0;
        uint64_t unacquired = 0;
        std::vector<uint32_t> chainTypes;

        TransportStats transport; // since the window's first present
        std::string firstNoRoute; // the first NoRoute reason, kept for the line
    };

    struct Stats
    {
        uint64_t presents = 0;
        uint64_t internalPresents = 0; // on a device OptiScaler created itself
        uint64_t unknownSwapchain = 0; // presents of a swapchain the host never saw created
        uint64_t unknownQueue = 0;     // presents on a queue the host never saw retrieved
        uint64_t beyondArray = 0;      // swapchains past kPresentSwapchains in one present, counted only
        uint64_t reusedLiveHandle = 0; // a create returned a handle still on record: its destroy was not seen
        uint64_t acquiresUnknown = 0;  // acquires on a swapchain the host never saw created
        uint64_t imagesOutOfRange = 0; // acquire or present of an index past the images on record
        bool unknownSwapchainReported = false;
    };

    // --- Creation and destruction --------------------------------------------------------------------------------

    RecordId OnInstanceCreated(Handle instance, bool internal, bool properties2 = false, uint32_t apiVersion = 0)
    {
        if (instance == 0)
            return 0;

        if (_instances.count(instance) != 0)
        {
            ++_stats.reusedLiveHandle;
            OnInstanceDestroyed(instance);
        }

        const RecordId id = Next();
        _instances[instance] = Instance { id, internal, properties2, apiVersion };
        return id;
    }

    void OnInstanceDestroyed(Handle instance)
    {
        if (_instances.erase(instance) == 0)
            return;

        // Its physical devices and surfaces go with it. Its devices should be gone already; any left keep their
        // record (and their instance id) until their own destroy.
        for (auto it = _physicals.begin(); it != _physicals.end();)
            it = it->second.instance == instance ? _physicals.erase(it) : std::next(it);

        for (auto it = _surfaces.begin(); it != _surfaces.end();)
            it = it->first.first == instance ? _surfaces.erase(it) : std::next(it);
    }

    void OnPhysicalDevices(Handle instance, const Handle* physical, uint32_t count)
    {
        const auto found = _instances.find(instance);
        const RecordId instanceId = found != _instances.end() ? found->second.id : 0;

        for (uint32_t i = 0; physical != nullptr && i < count; ++i)
        {
            if (physical[i] != 0)
                _physicals[physical[i]] = Physical { instance, instanceId };
        }
    }

    RecordId OnDeviceCreated(Handle device, DeviceDesc desc)
    {
        if (device == 0)
            return 0;

        if (_devices.count(device) != 0)
        {
            ++_stats.reusedLiveHandle;
            OnDeviceDestroyed(device);
        }

        const auto physical = _physicals.find(desc.physical);
        const RecordId id = Next();
        _devices[device] =
            Device { id, physical != _physicals.end() ? physical->second.instanceId : 0, std::move(desc) };
        return id;
    }

    void OnDeviceDestroyed(Handle device)
    {
        const auto found = _devices.find(device);
        if (found == _devices.end())
            return;

        const RecordId id = found->second.id;
        _devices.erase(found);

        // Its queues and swapchains cannot outlive it. Swapchains should be destroyed first; any left are retired here.
        for (auto it = _queues.begin(); it != _queues.end();)
            it = it->second.deviceId == id ? _queues.erase(it) : std::next(it);

        for (auto it = _swapchains.begin(); it != _swapchains.end();)
            it = it->second.deviceId == id ? _swapchains.erase(it) : std::next(it);
    }

    // vkGetDeviceQueue (flags 0) or vkGetDeviceQueue2 (its VkDeviceQueueInfo2::flags). The same queue asked for again
    // is the same record; a queue handle on record for another device means that device's destroy was not seen.
    void OnQueue(Handle device, uint32_t family, uint32_t index, uint32_t flags, Handle queue)
    {
        if (queue == 0)
            return;

        const auto owner = _devices.find(device);
        const RecordId deviceId = owner != _devices.end() ? owner->second.id : 0;

        if (const auto known = _queues.find(queue); known != _queues.end() && known->second.deviceId != deviceId)
            ++_stats.reusedLiveHandle;

        _queues[queue] = Queue { device, deviceId, family, index, flags };
    }

    RecordId OnSurfaceCreated(Handle instance, Handle surface, uint64_t hwnd)
    {
        if (surface == 0)
            return 0;

        const SurfaceKey key { instance, surface };
        if (_surfaces.count(key) != 0)
            ++_stats.reusedLiveHandle;

        const RecordId id = Next();
        _surfaces[key] = Surface { id, hwnd };
        return id;
    }

    void OnSurfaceDestroyed(Handle instance, Handle surface) { _surfaces.erase(SurfaceKey { instance, surface }); }

    RecordId OnSwapchainCreated(Handle device, Handle swapchain, SwapchainDesc desc)
    {
        if (swapchain == 0)
            return 0;

        const SwapchainKey key { device, swapchain };
        if (_swapchains.count(key) != 0)
        {
            ++_stats.reusedLiveHandle;
            _swapchains.erase(key);
        }

        // The old swapchain is retired, not destroyed: images it already handed out can still be presented.
        if (desc.oldSwapchain != 0)
        {
            if (const auto old = _swapchains.find(SwapchainKey { device, desc.oldSwapchain }); old != _swapchains.end())
                old->second.retired = true;
        }

        Swapchain record;
        record.id = Next();

        if (const auto owner = _devices.find(device); owner != _devices.end())
        {
            record.deviceId = owner->second.id;

            const auto physical = _physicals.find(owner->second.desc.physical);
            const Handle instance = physical != _physicals.end() ? physical->second.instance : 0;

            if (const auto surface = _surfaces.find(SurfaceKey { instance, desc.surface }); surface != _surfaces.end())
            {
                record.hwndKnown = true;
                record.hwnd = surface->second.hwnd;
            }
        }

        // A surface whose instance the host could not tie to this device: take it by handle when exactly one
        // instance has a surface with that value.
        if (!record.hwndKnown)
        {
            const Surface* only = nullptr;
            size_t matches = 0;

            for (const auto& [surfaceKey, surface] : _surfaces)
            {
                if (surfaceKey.second == desc.surface)
                {
                    only = &surface;
                    ++matches;
                }
            }

            if (matches == 1)
            {
                record.hwndKnown = true;
                record.hwnd = only->hwnd;
            }
        }

        record.desc = std::move(desc);
        const RecordId id = record.id;
        _swapchains[key] = std::move(record);
        return id;
    }

    // vkGetSwapchainImagesKHR after it returned SUCCESS or INCOMPLETE: `images` null when only the count was asked.
    void OnSwapchainImages(Handle device, Handle swapchain, uint32_t count, const Handle* images)
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        if (found == _swapchains.end())
            return;

        Swapchain& record = found->second;

        if (images == nullptr)
        {
            record.imageCount = count > record.imageCount ? count : record.imageCount;
        }
        else
        {
            record.images.assign(images, images + count);
            record.imageCount = count > record.imageCount ? count : record.imageCount;
        }

        if (record.acquired.size() < record.imageCount)
            record.acquired.resize(record.imageCount, 0);
    }

    void OnSwapchainDestroyed(Handle device, Handle swapchain)
    {
        _swapchains.erase(SwapchainKey { device, swapchain });
    }

    // vkAcquireNextImageKHR or vkAcquireNextImage2KHR after it returned SUCCESS or SUBOPTIMAL.
    void OnAcquire(Handle device, Handle swapchain, uint32_t index)
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        if (found == _swapchains.end())
        {
            ++_stats.acquiresUnknown;
            return;
        }

        Swapchain& record = found->second;
        ++record.acquires;

        if (index >= record.acquired.size())
        {
            if (index >= 64)
            {
                ++_stats.imagesOutOfRange;
                return;
            }

            record.acquired.resize(index + 1, 0);
        }

        record.acquired[index] = 1;
    }

    // --- Presentation ------------------------------------------------------------------------------------------

    // Before the downstream vkQueuePresentKHR. Returns the lines due (most presents: none).
    std::vector<std::string> OnPresent(const PresentCall& call)
    {
        std::vector<std::string> lines;
        ++_stats.presents;

        const auto queueFound = _queues.find(call.queue);
        const Queue* queue = queueFound != _queues.end() ? &queueFound->second : nullptr;
        const Device* device = nullptr;
        Handle deviceHandle = 0;

        if (queue != nullptr)
        {
            if (const auto owner = _devices.find(queue->device); owner != _devices.end())
            {
                device = &owner->second;
                deviceHandle = queue->device;
            }
        }
        else
        {
            ++_stats.unknownQueue;
        }

        if (device != nullptr && device->desc.internal)
        {
            ++_stats.internalPresents;
            return lines;
        }

        const uint32_t attributed =
            call.swapchainCount < kPresentSwapchains ? call.swapchainCount : static_cast<uint32_t>(kPresentSwapchains);
        _stats.beyondArray += call.swapchainCount - attributed;

        for (uint32_t i = 0; i < attributed; ++i)
        {
            Swapchain* swapchain = FindSwapchainFor(deviceHandle, call.swapchains[i]);

            if (swapchain == nullptr)
            {
                ++_stats.unknownSwapchain;

                if (!_stats.unknownSwapchainReported)
                {
                    _stats.unknownSwapchainReported = true;
                    lines.push_back("DLSS-NR Vulkan present host: a present of swapchain " + Hex(call.swapchains[i]) +
                                    ", which the host never saw created (made before the host was installed, by a "
                                    "device it did not see, or not by the driver); such presents are counted, "
                                    "not reported, and go down unchanged");
                }

                continue;
            }

            // A queue the host never saw retrieved: the swapchain still names its device.
            const Device* owner = device;
            Handle ownerHandle = deviceHandle;
            if (owner == nullptr)
            {
                for (const auto& [handle, record] : _devices)
                {
                    if (record.id == swapchain->deviceId)
                    {
                        owner = &record;
                        ownerHandle = handle;
                    }
                }

                if (owner != nullptr && owner->desc.internal)
                {
                    ++_stats.internalPresents;
                    continue;
                }
            }

            Window& window = Find(*swapchain);
            swapchain->hasWindow = true;
            ++swapchain->presents;
            swapchain->transport.Add(call.outcome);
            window.transport.Add(call.outcome);
            if (call.outcome.counted && call.outcome.skip == Skip::NoRoute && window.firstNoRoute.empty())
                window.firstNoRoute = call.outcome.note;

            const uint32_t index = call.imageIndices[i];
            if (index < swapchain->acquired.size() && swapchain->acquired[index] != 0)
                swapchain->acquired[index] = 0;
            else
                ++window.unacquired;

            ++window.presents;
            AddOnce(window.swapchainIds, swapchain->id);
            if (owner != nullptr)
                AddOnce(window.deviceIds, owner->id);

            window.waitsMin = call.waitSemaphores < window.waitsMin ? call.waitSemaphores : window.waitsMin;
            window.waitsMax = call.waitSemaphores > window.waitsMax ? call.waitSemaphores : window.waitsMax;
            if (call.swapchainCount > 1)
                ++window.multiSwapchain;
            for (uint32_t c = 0; c < call.chainLength && c < kPresentChain; ++c)
            {
                if (window.chainTypes.size() < kChainTypesKept)
                    AddOnce(window.chainTypes, call.chainTypes[c]);
            }

            const Identity now { swapchain->id, owner != nullptr ? owner->id : 0, call.queue, call.caller,
                                 call.callerModule };

            std::string why;
            if (!window.reported)
            {
                why = "first present";
            }
            else
            {
                const auto add = [&why](const char* reason)
                { why += why.empty() ? reason : std::string(", ") + reason; };

                if (now.swapchainId != window.last.swapchainId)
                    add("new swapchain");
                if (now.deviceId != window.last.deviceId)
                    add("new device");
                if (now.queue != window.last.queue)
                    add("new queue");
                if (now.caller != window.last.caller || now.callerModule != window.last.callerModule)
                    add("new caller");
            }

            if (window.presents == window.nextDecade)
            {
                window.nextDecade = window.nextDecade <= UINT64_MAX / 10 ? window.nextDecade * 10 : UINT64_MAX;
                if (why.empty())
                    why = std::to_string(window.presents) + " presents";
            }

            if (why.empty() && call.nowMs >= window.lastReportMs && call.nowMs - window.lastReportMs >= 60000)
                why = "60 s heartbeat";

            if (why.empty())
                continue;

            lines.push_back(Line(window, why, call.swapchains[i], *swapchain, owner, ownerHandle, call));

            window.reported = true;
            window.lastReportMs = call.nowMs;
            window.last = now;
            window.waitsMin = UINT32_MAX;
            window.waitsMax = 0;
            window.multiSwapchain = 0;
            window.unacquired = 0;
            window.chainTypes.clear();
        }

        return lines;
    }

    // --- Lookups -------------------------------------------------------------------------------------------------

    const Device* FindDevice(Handle device) const
    {
        const auto found = _devices.find(device);
        return found != _devices.end() ? &found->second : nullptr;
    }

    const Queue* FindQueue(Handle queue) const
    {
        const auto found = _queues.find(queue);
        return found != _queues.end() ? &found->second : nullptr;
    }

    const Swapchain* FindSwapchain(Handle device, Handle swapchain) const
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        return found != _swapchains.end() ? &found->second : nullptr;
    }

    const Surface* FindSurface(Handle instance, Handle surface) const
    {
        const auto found = _surfaces.find(SurfaceKey { instance, surface });
        return found != _surfaces.end() ? &found->second : nullptr;
    }

    const Window* FindWindow(bool hwndKnown, uint64_t key) const
    {
        const auto found = _windows.find(WindowKey { hwndKnown, key });
        return found != _windows.end() ? &found->second : nullptr;
    }

    const Instance* FindInstance(Handle instance) const
    {
        const auto found = _instances.find(instance);
        return found != _instances.end() ? &found->second : nullptr;
    }

    // The physical device a device was created on, 0 when the device is not on record.
    Handle PhysicalOf(Handle device) const
    {
        const auto found = _devices.find(device);
        return found != _devices.end() ? found->second.desc.physical : 0;
    }

    // The instance a physical device was enumerated from, 0 when not on record.
    Handle InstanceOf(Handle physical) const
    {
        const auto found = _physicals.find(physical);
        return found != _physicals.end() ? found->second.instance : 0;
    }

    // The API version the instance a physical device was enumerated from asked for, 0 when not on record.
    uint32_t ApiVersionOf(Handle physical) const
    {
        const auto physicalFound = _physicals.find(physical);
        if (physicalFound == _physicals.end())
            return 0;
        const auto instance = _instances.find(physicalFound->second.instance);
        return instance != _instances.end() ? instance->second.apiVersion : 0;
    }

    // Whether the instance a physical device was enumerated from can take the NR extensions' dependencies (Vulkan 1.1
    // or VK_KHR_get_physical_device_properties2). False when the instance is not on record.
    bool Properties2Of(Handle physical) const
    {
        const auto physicalFound = _physicals.find(physical);
        if (physicalFound == _physicals.end())
            return false;
        const auto instance = _instances.find(physicalFound->second.instance);
        return instance != _instances.end() && instance->second.properties2;
    }

    const Stats& GetStats() const { return _stats; }
    size_t LiveDevices() const { return _devices.size(); }
    size_t LiveQueues() const { return _queues.size(); }
    size_t LiveSwapchains() const { return _swapchains.size(); }
    size_t LiveSurfaces() const { return _surfaces.size(); }
    size_t LivePhysicals() const { return _physicals.size(); }

    // Everything the transport needs for a present of `swapchain` image `index` on `queue`. False when the queue, its
    // device or the swapchain is not on record (the present goes down unchanged and OnPresent counts it).
    bool ViewPresent(Handle queue, Handle swapchain, uint32_t index, PresentView& view) const
    {
        const auto queueFound = _queues.find(queue);
        if (queueFound == _queues.end())
            return false;

        const auto deviceFound = _devices.find(queueFound->second.device);
        if (deviceFound == _devices.end())
            return false;

        const auto found = _swapchains.find(SwapchainKey { queueFound->second.device, swapchain });
        if (found == _swapchains.end())
            return false;

        const Device& device = deviceFound->second;
        const Swapchain& record = found->second;

        view.internal = device.desc.internal;
        view.device = queueFound->second.device;
        view.physical = device.desc.physical;
        view.instance = InstanceOf(device.desc.physical);
        view.deviceId = device.id;
        view.swapchainId = record.id;
        view.presentFamily = queueFound->second.family;
        view.families = device.desc.families;
        view.queues = QueuesOf(view.device);
        view.desc = record.desc;
        view.imageIndex = index;
        view.imageCount = static_cast<uint32_t>(record.images.size());
        view.imageKnown = index < record.images.size();
        view.image = view.imageKnown ? record.images[index] : 0;
        view.acquired = index < record.acquired.size() && record.acquired[index] != 0;
        view.retired = record.retired;
        return true;
    }

    // The queues of a device the application retrieved, ordered by family and index.
    std::vector<QueueCandidate> QueuesOf(Handle device) const
    {
        std::vector<QueueCandidate> queues;
        for (const auto& [handle, queue] : _queues)
        {
            if (queue.device == device)
                queues.push_back(QueueCandidate { handle, queue.family, queue.index, queue.flags });
        }

        for (size_t i = 1; i < queues.size(); ++i)
        {
            for (size_t j = i;
                 j > 0 && (queues[j].family < queues[j - 1].family ||
                           (queues[j].family == queues[j - 1].family && queues[j].index < queues[j - 1].index));
                 --j)
                std::swap(queues[j], queues[j - 1]);
        }

        return queues;
    }

    // Per image of a swapchain: acquired since its last present. A present semaphore of an image acquired again is
    // free.
    std::vector<uint8_t> AcquiredImages(Handle device, Handle swapchain) const
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        return found != _swapchains.end() ? found->second.acquired : std::vector<uint8_t> {};
    }

    // Completions of round trips observed when a swapchain was retired (its last ones, which no later present polled),
    // with the model's pass GPU times read back from them.
    void OnCompletions(Handle device, Handle swapchain, uint64_t completions, uint32_t timed = 0, double lastMs = 0.0,
                       double maxMs = 0.0, double sumMs = 0.0)
    {
        auto found = _swapchains.find(SwapchainKey { device, swapchain });
        if (found == _swapchains.end() || (completions == 0 && timed == 0))
            return;

        found->second.transport.completions += completions;
        found->second.transport.AddGpuTimes(timed, lastMs, maxMs, sumMs);
        if (found->second.hasWindow)
        {
            Window& window = Find(found->second);
            window.transport.completions += completions;
            window.transport.AddGpuTimes(timed, lastMs, maxMs, sumMs);
        }
    }

    // Written once when a swapchain is created: what was asked, what the driver got, and whether the round trip runs.
    std::string SwapchainLine(Handle device, Handle swapchain) const
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        if (found == _swapchains.end())
            return {};

        const Swapchain& record = found->second;
        const SwapchainDesc& d = record.desc;
        const Device* owner = FindDevice(device);

        std::string line =
            "DLSS-NR Vulkan present host: swapchain " + Hex(swapchain) + " #" + std::to_string(record.id) +
            " on device " + Hex(device) + " #" + std::to_string(record.deviceId) + ", window " +
            (record.hwndKnown ? Hex(record.hwnd) : std::string("unknown")) + ", " + std::to_string(d.width) + "x" +
            std::to_string(d.height) + " format " + std::to_string(d.format) + ": usage " + Hex(d.askedUsage);

        if (d.usage != d.askedUsage)
            line += " -> " + Hex(d.usage);

        if (d.surfaceUsageKnown)
            line += " (surface offers " + Hex(d.surfaceUsage) + ")";

        line += "; round trip " + RoundTrip(d);

        if (owner != nullptr && owner->desc.internal)
            line += "; OptiScaler's own device";

        return line;
    }

    // Written when a swapchain is destroyed: what became of its presents, and of the host's objects for it.
    std::string SwapchainSummary(Handle device, Handle swapchain, const std::string& resources) const
    {
        const auto found = _swapchains.find(SwapchainKey { device, swapchain });
        if (found == _swapchains.end())
            return {};

        const Swapchain& record = found->second;
        if (!record.desc.transport && record.transport.presents == 0)
            return {};

        return "DLSS-NR Vulkan present host: swapchain " + Hex(swapchain) + " #" + std::to_string(record.id) +
               " destroyed after " + std::to_string(record.presents) + " presents; " + TransportText(record.transport) +
               (resources.empty() ? "" : "; " + resources) + "; " + NrText(record.transport.nr);
    }

    // The line written when a device is created, for the log and the tests.
    std::string DeviceLine(Handle device) const
    {
        const Device* record = FindDevice(device);
        if (record == nullptr)
            return {};

        std::string line = "DLSS-NR Vulkan present host: device " + Hex(device) + " #" + std::to_string(record->id) +
                           " created by " + (record->desc.creatorModule.empty() ? "?" : record->desc.creatorModule) +
                           " (" + CallerName(record->desc.creator) + "); physical " + Hex(record->desc.physical) +
                           " instance #" + std::to_string(record->instanceId) + "; queue families";

        if (record->desc.families.empty())
            line += " unknown";

        for (size_t f = 0; f < record->desc.families.size(); ++f)
        {
            line += " " + std::to_string(f) + ":" + Hex(record->desc.families[f].flags) + "x" +
                    std::to_string(record->desc.families[f].count);
        }

        line += "; requested";
        for (const QueueRequest& request : record->desc.requested)
        {
            line += " family " + std::to_string(request.family) + " x" + std::to_string(request.count);
            if (request.flags != 0)
                line += " (flags " + Hex(request.flags) + ")";
        }

        if (record->desc.internal)
            line += "; OptiScaler's own device, its presents are not reported";

        return line;
    }

  private:
    using SurfaceKey = std::pair<Handle, Handle>;   // instance, surface
    using SwapchainKey = std::pair<Handle, Handle>; // device, swapchain

    struct WindowKey
    {
        bool hwndKnown = false;
        uint64_t value = 0; // the HWND, or the swapchain's record id when its window is not known

        bool operator==(const WindowKey&) const = default;
    };

    struct PairHash
    {
        size_t operator()(const std::pair<Handle, Handle>& key) const
        {
            return std::hash<Handle> {}(key.first) ^ (std::hash<Handle> {}(key.second) * 0x9E3779B97F4A7C15ull);
        }
    };

    struct WindowHash
    {
        size_t operator()(const WindowKey& key) const
        {
            return std::hash<uint64_t> {}(key.value) ^ (key.hwndKnown ? 0x5BD1E995u : 0u);
        }
    };

    RecordId Next() { return ++_lastId; }

    template <typename T> static void AddOnce(std::vector<T>& list, T value)
    {
        for (const T& item : list)
        {
            if (item == value)
                return;
        }

        list.push_back(value);
    }

    // The swapchain a present names: by its device when the queue is known, otherwise the only live swapchain with
    // that handle value (non-dispatchable handles are only unique per device).
    Swapchain* FindSwapchainFor(Handle device, Handle swapchain)
    {
        if (device != 0)
        {
            const auto found = _swapchains.find(SwapchainKey { device, swapchain });
            return found != _swapchains.end() ? &found->second : nullptr;
        }

        Swapchain* only = nullptr;
        size_t matches = 0;

        for (auto& [key, record] : _swapchains)
        {
            if (key.second == swapchain)
            {
                only = &record;
                ++matches;
            }
        }

        return matches == 1 ? only : nullptr;
    }

    Window& Find(const Swapchain& swapchain)
    {
        const WindowKey key { swapchain.hwndKnown, swapchain.hwndKnown ? swapchain.hwnd : swapchain.id };
        Window& window = _windows[key];
        window.hwndKnown = swapchain.hwndKnown;
        window.hwnd = swapchain.hwnd;
        return window;
    }

    std::string Line(const Window& window, const std::string& why, Handle swapchainHandle, const Swapchain& swapchain,
                     const Device* device, Handle deviceHandle, const PresentCall& call) const
    {
        const SwapchainDesc& d = swapchain.desc;
        const auto n = [](uint64_t value) { return std::to_string(value); };

        std::string line = "DLSS-NR Vulkan present host (cold NR): window ";
        line += window.hwndKnown ? Hex(window.hwnd) : std::string("unknown");
        line += " (" + why + "); presents " + n(window.presents) + ", swapchains " + n(window.swapchainIds.size()) +
                ", devices " + n(window.deviceIds.size());

        line += "; swapchain " + Hex(swapchainHandle) + " #" + n(swapchain.id) + " " + n(d.width) + "x" + n(d.height) +
                " format " + n(d.format) + " colour-space " + n(d.colorSpace) + " images " + n(swapchain.imageCount) +
                " usage " + Hex(d.usage) + " (transfer-src " + YesNo(d.usage & kUsageTransferSrcBit) +
                ", transfer-dst " + YesNo(d.usage & kUsageTransferDstBit) + ", storage " +
                YesNo(d.usage & kUsageStorageBit) + ")";

        if (d.askedUsage != d.usage)
            line += " asked " + Hex(d.askedUsage);

        if (d.surfaceUsageKnown)
            line += " surface-usage " + Hex(d.surfaceUsage);

        line += " sharing ";
        if (d.sharingMode == kSharingModeConcurrent)
        {
            line += "CONCURRENT[";
            for (size_t i = 0; i < d.sharingFamilies.size(); ++i)
                line += (i == 0 ? "" : ",") + n(d.sharingFamilies[i]);
            line += "]";
        }
        else
        {
            line += "EXCLUSIVE";
        }

        if (swapchain.retired)
            line += " retired";

        line += "; round-trip " + RoundTrip(d);

        if (device != nullptr)
        {
            line += "; device " + Hex(deviceHandle) + " #" + n(device->id) + " physical " + Hex(device->desc.physical) +
                    " instance #" + n(device->instanceId);
        }
        else
        {
            line += "; device unknown";
        }

        const auto queueFound = _queues.find(call.queue);
        if (queueFound != _queues.end())
        {
            const Queue& queue = queueFound->second;
            const bool familyKnown = device != nullptr && queue.family < device->desc.families.size();
            const uint32_t flags = familyKnown ? device->desc.families[queue.family].flags : 0;

            line += "; queue " + Hex(call.queue) + " family " + n(queue.family) + " index " + n(queue.index);
            if (familyKnown)
            {
                line += " flags " + Hex(flags) + " (graphics " + YesNo(flags & kQueueGraphicsBit) + ", compute " +
                        YesNo(flags & kQueueComputeBit) + ", transfer " + YesNo(flags & kQueueTransferBit) + ")";
            }
            else
            {
                line += " flags unknown";
            }

            if (queue.flags != 0)
                line += " create-flags " + Hex(queue.flags);
        }
        else
        {
            line += "; queue " + Hex(call.queue) + " unknown (never seen retrieved)";
        }

        line += "; waits " + n(window.waitsMin == UINT32_MAX ? 0 : window.waitsMin) + "-" + n(window.waitsMax);
        line += "; acquires " + n(swapchain.acquires);

        if (window.unacquired != 0)
            line += ", presented without an observed acquire " + n(window.unacquired);

        if (window.multiSwapchain != 0)
            line += "; multi-swapchain presents " + n(window.multiSwapchain);

        if (!window.chainTypes.empty())
        {
            line += "; present pNext";
            for (const uint32_t type : window.chainTypes)
                line += " " + n(type);
        }

        line += "; caller " + (call.callerName.empty() ? std::string("?") : std::string(call.callerName)) + " (" +
                CallerName(call.caller) + ")";

        line += "; transport: " + TransportText(window.transport);
        if (!window.firstNoRoute.empty())
            line += "; first no-route reason: " + window.firstNoRoute;

        line += "; " + NrText(window.transport.nr);
        return line;
    }

    RecordId _lastId = 0;
    std::unordered_map<Handle, Instance> _instances;
    std::unordered_map<Handle, Physical> _physicals;
    std::unordered_map<Handle, Device> _devices;
    std::unordered_map<Handle, Queue> _queues;
    std::unordered_map<SurfaceKey, Surface, PairHash> _surfaces;
    std::unordered_map<SwapchainKey, Swapchain, PairHash> _swapchains;
    std::unordered_map<WindowKey, Window, WindowHash> _windows;
    Stats _stats;
};
} // namespace DlssNr::VkPresentHost
