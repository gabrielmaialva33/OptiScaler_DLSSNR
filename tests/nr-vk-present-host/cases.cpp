// The Vulkan present host's registry (retoque/dlssnr/DlssNr_VkPresentRegistry.h), as production compiles it, driven
// through the lifetimes the host has to keep apart. Plain integers stand in for handles, as the header itself uses.

#include <vulkan/vulkan.h>

#include "DlssNr_VkPresentRegistry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace DlssNr::VkPresentHost;

static int failures = 0;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool Has(const std::string& line, const std::string& part) { return line.find(part) != std::string::npos; }

static void CheckHas(const std::string& line, const std::string& part, int at)
{
    if (!Has(line, part))
    {
        std::fprintf(stderr, "line %d: expected \"%s\" in:\n  %s\n", at, part.c_str(), line.c_str());
        ++failures;
    }
}
#define HAS(line, part) CheckHas(line, part, __LINE__)

// The constants the header repeats so it compiles without vulkan.h.
static void Constants()
{
    CHECK(kQueueGraphicsBit == VK_QUEUE_GRAPHICS_BIT);
    CHECK(kQueueComputeBit == VK_QUEUE_COMPUTE_BIT);
    CHECK(kQueueTransferBit == VK_QUEUE_TRANSFER_BIT);
    CHECK(kQueueProtectedBit == VK_QUEUE_PROTECTED_BIT);
    CHECK(kUsageTransferSrcBit == VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    CHECK(kUsageTransferDstBit == VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CHECK(kUsageStorageBit == VK_IMAGE_USAGE_STORAGE_BIT);
    CHECK(kSharingModeConcurrent == VK_SHARING_MODE_CONCURRENT);
}

static void Callers()
{
    CHECK(ClassifyCaller("game.exe", false) == Caller::Native);
    CHECK(ClassifyCaller("", false) == Caller::Unknown);
    for (const char* dxvk : { "dxgi.dll", "d3d11.dll", "d3d10core.dll", "d3d9.dll", "d3d8.dll" })
        CHECK(ClassifyCaller(dxvk, false) == Caller::Dxvk);
    for (const char* vkd3d : { "d3d12.dll", "d3d12core.dll" })
        CHECK(ClassifyCaller(vkd3d, false) == Caller::Vkd3dProton);
    for (const char* loader : { "vulkan-1.dll", "winevulkan.dll" })
        CHECK(ClassifyCaller(loader, false) == Caller::Loader);
    // OptiScaler is often dxgi.dll itself: its own module wins over the name.
    CHECK(ClassifyCaller("dxgi.dll", true) == Caller::OptiScaler);
    CHECK(std::string(CallerName(Caller::Vkd3dProton)) == "vkd3d-proton");
}

static void RoundTrips()
{
    SwapchainDesc d;
    d.usage = kUsageTransferSrcBit | kUsageTransferDstBit | 0x10;
    CHECK(RoundTrip(d) == "legal as created");
    d.usage = kUsageTransferDstBit | 0x10;
    CHECK(RoundTrip(d) == "needs TRANSFER_SRC, surface support unknown");
    d.surfaceUsageKnown = true;
    d.surfaceUsage = 0x9F;
    CHECK(RoundTrip(d) == "needs TRANSFER_SRC at creation, the surface offers it");
    d.usage = 0x10;
    CHECK(RoundTrip(d) == "needs TRANSFER_SRC+TRANSFER_DST at creation, the surface offers it");
    d.surfaceUsage = 0x12;
    CHECK(RoundTrip(d) == "not possible, the surface does not offer TRANSFER_SRC+TRANSFER_DST");
}

// A small world: one instance, one physical device with a graphics family and a compute-only one.
struct World
{
    Registry r;
    static constexpr Handle instance = 0x1000, physical = 0x2000;

    World()
    {
        r.OnInstanceCreated(instance, false);
        const Handle physicals[] = { physical };
        r.OnPhysicalDevices(instance, physicals, 1);
    }

    RecordId Device(Handle device, bool internal = false)
    {
        DeviceDesc desc;
        desc.physical = physical;
        desc.families = { { 0xF, 16 }, { 0xC, 2 }, { 0xE, 8 } };
        desc.requested = { { 0, 1, 0 }, { 2, 1, 0 } };
        desc.internal = internal;
        desc.creator = Caller::Native;
        desc.creatorModule = "game.exe";
        return r.OnDeviceCreated(device, desc);
    }

    RecordId Swapchain(Handle device, Handle swapchain, Handle surface, uint32_t usage, Handle old = 0)
    {
        SwapchainDesc d;
        d.surface = surface;
        d.oldSwapchain = old;
        d.format = 44;
        d.width = 640;
        d.height = 480;
        d.arrayLayers = 1;
        d.usage = usage;
        d.surfaceUsageKnown = true;
        d.surfaceUsage = 0x9F;
        const RecordId id = r.OnSwapchainCreated(device, swapchain, d);
        const Handle images[] = { swapchain + 1, swapchain + 2, swapchain + 3 };
        r.OnSwapchainImages(device, swapchain, 3, nullptr);
        r.OnSwapchainImages(device, swapchain, 3, images);
        return id;
    }

    std::vector<std::string> Present(Handle queue, Handle swapchain, uint32_t index, uint32_t waits, bool acquire,
                                     Handle device, Caller caller = Caller::Native, Handle module = 0x140000000)
    {
        if (acquire)
            r.OnAcquire(device, swapchain, index);
        PresentCall call;
        call.queue = queue;
        call.waitSemaphores = waits;
        call.swapchainCount = 1;
        call.swapchains[0] = swapchain;
        call.imageIndices[0] = index;
        call.caller = caller;
        call.callerModule = module;
        call.callerName = caller == Caller::Native ? "game.exe" : "d3d11.dll";
        return r.OnPresent(call);
    }
};

static void FirstPresentDecadesAndWaits()
{
    World w;
    const RecordId device = w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnQueue(0xD1, 2, 0, 0, 0x52); // vkGetDeviceQueue2, flags 0
    const RecordId surface = w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    CHECK(surface != 0);
    const RecordId swapchain = w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);
    CHECK(device != 0 && swapchain > device);

    std::vector<std::string> all;
    for (uint32_t i = 0; i < 100; ++i)
    {
        const auto lines = w.Present(0x51, 0x5C1, i % 3, i == 5 ? 0 : (i == 7 ? 2 : 1), true, 0xD1);
        CHECK(lines.size() <= 1);
        if (i == 0 || i == 9 || i == 99)
            CHECK(lines.size() == 1);
        else
            CHECK(lines.empty());
        all.insert(all.end(), lines.begin(), lines.end());
    }

    CHECK(all.size() == 3);
    if (all.size() != 3)
        return;

    const std::string& first = all[0];
    HAS(first, "DLSS-NR Vulkan present host (observe only): window 0xAAA (first present); presents 1, swapchains 1, "
               "devices 1; swapchain 0x5C1 #" +
                   std::to_string(swapchain) + " 640x480 format 44 colour-space 0 images 3 usage 0x12 (transfer-src "
                                               "no, transfer-dst yes, storage no) surface-usage 0x9F sharing EXCLUSIVE; "
                                               "round-trip needs TRANSFER_SRC at creation, the surface offers it; "
                                               "device 0xD1 #" +
                   std::to_string(device) + " physical 0x2000 instance #1; queue 0x51 family 0 index 0 flags 0xF "
                                            "(graphics yes, compute yes, transfer yes); waits 1-1; acquires 1; caller "
                                            "game.exe (native Vulkan)");
    CHECK(!Has(first, "without an observed acquire"));
    HAS(all[1], "(10 presents); presents 10,");
    HAS(all[1], "; waits 0-2; acquires 10;"); // presents 2..10 hold the 0 and the 2
    HAS(all[2], "(100 presents); presents 100,");
    HAS(all[2], "; waits 1-1; acquires 100;"); // the window restarts after each line
}

static void IdentityChanges()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnQueue(0xD1, 2, 0, 0, 0x52);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);

    CHECK(w.Present(0x51, 0x5C1, 0, 1, true, 0xD1).size() == 1);
    CHECK(w.Present(0x51, 0x5C1, 1, 1, true, 0xD1).empty());

    // The same window from the compute family: a new queue.
    auto lines = w.Present(0x52, 0x5C1, 2, 1, true, 0xD1);
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "(new queue); presents 3,");
        HAS(lines[0], "queue 0x52 family 2 index 0 flags 0xE (graphics no, compute yes, transfer yes)");
    }

    // Another module presenting: a new caller.
    lines = w.Present(0x52, 0x5C1, 0, 1, true, 0xD1, Caller::Dxvk, 0x180000000);
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "(new caller)");
        HAS(lines[0], "caller d3d11.dll (DXVK)");
    }

    // A recreation through oldSwapchain: the old one is retired, not forgotten; its images can still be presented.
    const RecordId second = w.Swapchain(0xD1, 0x5C2, 0x5F1, 0x13, 0x5C1);
    CHECK(w.r.FindSwapchain(0xD1, 0x5C1) != nullptr && w.r.FindSwapchain(0xD1, 0x5C1)->retired);
    lines = w.Present(0x52, 0x5C2, 0, 1, true, 0xD1, Caller::Dxvk, 0x180000000);
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "(new swapchain); presents 5, swapchains 2, devices 1; swapchain 0x5C2 #" +
                          std::to_string(second));
        HAS(lines[0], "round-trip legal as created");
    }
    lines = w.Present(0x52, 0x5C1, 1, 1, false, 0xD1, Caller::Dxvk, 0x180000000);
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "(new swapchain)");
        HAS(lines[0], "sharing EXCLUSIVE retired;");
        HAS(lines[0], "presented without an observed acquire 1");
    }

    // Destroyed: its records go before the handle can be handed out again.
    w.r.OnSwapchainDestroyed(0xD1, 0x5C1);
    CHECK(w.r.FindSwapchain(0xD1, 0x5C1) == nullptr);
}

static void ReusedHandlesAndManyDevices()
{
    World w;
    const RecordId d1 = w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.r.OnSurfaceCreated(World::instance, 0x5F2, 0xBBB);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);

    // A second device alive at once whose swapchain has the same handle value: non-dispatchable handles are only
    // unique per device, so the records are keyed by device as well.
    const RecordId d2 = w.Device(0xD2);
    w.r.OnQueue(0xD2, 0, 0, 0, 0x61);
    const RecordId second = w.Swapchain(0xD2, 0x5C1, 0x5F2, 0x12);
    CHECK(d2 != d1);

    auto one = w.Present(0x51, 0x5C1, 0, 1, true, 0xD1);
    auto two = w.Present(0x61, 0x5C1, 0, 1, true, 0xD2);
    CHECK(one.size() == 1 && two.size() == 1);
    if (one.size() == 1 && two.size() == 1)
    {
        HAS(one[0], "window 0xAAA (first present)");
        HAS(one[0], "device 0xD1 #" + std::to_string(d1));
        HAS(two[0], "window 0xBBB (first present)");
        HAS(two[0], "device 0xD2 #" + std::to_string(d2));
        HAS(two[0], "swapchain 0x5C1 #" + std::to_string(second));
    }

    // Device 2 destroyed takes its queue and swapchain with it; device 1's are untouched.
    w.r.OnDeviceDestroyed(0xD2);
    CHECK(w.r.FindDevice(0xD2) == nullptr && w.r.FindQueue(0x61) == nullptr);
    CHECK(w.r.FindSwapchain(0xD2, 0x5C1) == nullptr && w.r.FindSwapchain(0xD1, 0x5C1) != nullptr);
    CHECK(w.r.FindQueue(0x51) != nullptr);

    // A new device with device 2's handle value, and a queue and swapchain with its old values too: new records.
    const RecordId d3 = w.Device(0xD2);
    CHECK(d3 != d2);
    w.r.OnQueue(0xD2, 0, 0, 0, 0x61);
    const RecordId third = w.Swapchain(0xD2, 0x5C1, 0x5F2, 0x12);
    CHECK(third != second);
    two = w.Present(0x61, 0x5C1, 0, 1, true, 0xD2);
    CHECK(two.size() == 1);
    if (!two.empty())
    {
        HAS(two[0], "window 0xBBB (new swapchain, new device); presents 2, swapchains 2, devices 2");
        HAS(two[0], "device 0xD2 #" + std::to_string(d3));
        HAS(two[0], "acquires 1;"); // the new swapchain's own count
    }
    CHECK(w.r.GetStats().reusedLiveHandle == 0);

    // A create returning a handle still on record (its destroy was never seen) replaces the record and says so.
    const RecordId d4 = w.Device(0xD2);
    CHECK(d4 != d3 && w.r.GetStats().reusedLiveHandle == 1);
    CHECK(w.r.FindQueue(0x61) == nullptr && w.r.FindSwapchain(0xD2, 0x5C1) == nullptr);
}

static void UnknownsAndInternal()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);

    // A swapchain the host never saw: one line for the session, then counted.
    auto lines = w.Present(0x51, 0x5C9, 0, 1, false, 0xD1);
    CHECK(lines.size() == 1 && Has(lines[0], "which the host never saw created"));
    CHECK(w.Present(0x51, 0x5C9, 0, 1, false, 0xD1).empty());
    CHECK(w.r.GetStats().unknownSwapchain == 2);

    // A queue the host never saw retrieved: the swapchain's handle, unique here, still names device and window.
    lines = w.Present(0x77, 0x5C1, 0, 1, true, 0xD1);
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "window 0xAAA (first present)");
        HAS(lines[0], "device 0xD1 #");
        HAS(lines[0], "queue 0x77 unknown (never seen retrieved)");
    }
    CHECK(w.r.GetStats().unknownQueue == 1);

    // OptiScaler's own device: never reported.
    w.Device(0xD9, true);
    w.r.OnQueue(0xD9, 0, 0, 0, 0x99);
    w.r.OnSurfaceCreated(World::instance, 0x5F9, 0xCCC);
    w.Swapchain(0xD9, 0x5C7, 0x5F9, 0x12);
    CHECK(w.Present(0x99, 0x5C7, 0, 1, true, 0xD9).empty());
    CHECK(w.r.GetStats().internalPresents == 1);
    CHECK(w.r.FindWindow(true, 0xCCC) == nullptr);
    HAS(w.r.DeviceLine(0xD9), "OptiScaler's own device, its presents are not reported");

    // A surface the host never saw: the window is unknown, and it is not mixed with another one.
    w.Swapchain(0xD1, 0x5C3, 0x5FF, 0x12);
    lines = w.Present(0x51, 0x5C3, 0, 1, true, 0xD1);
    CHECK(lines.size() == 1 && Has(lines[0], "window unknown (first present)"));
}

static void PresentsAndChains()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.r.OnSurfaceCreated(World::instance, 0x5F2, 0xBBB);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);
    w.Swapchain(0xD1, 0x5C2, 0x5F2, 0x12);
    w.r.OnAcquire(0xD1, 0x5C1, 0);
    w.r.OnAcquire(0xD1, 0x5C2, 1);

    // One present of two swapchains, with a present-id structure in its chain: each window counts it once.
    PresentCall call;
    call.queue = 0x51;
    call.waitSemaphores = 3;
    call.swapchainCount = 2;
    call.swapchains = { 0x5C1, 0x5C2 };
    call.imageIndices = { 0, 1 };
    call.chainLength = 1;
    call.chainTypes[0] = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    call.caller = Caller::Native;
    call.callerName = "game.exe";
    const auto lines = w.r.OnPresent(call);
    CHECK(lines.size() == 2);
    for (const std::string& line : lines)
    {
        HAS(line, "; waits 3-3;");
        HAS(line, "; multi-swapchain presents 1;");
        HAS(line, "; present pNext " + std::to_string(VK_STRUCTURE_TYPE_PRESENT_ID_KHR) + ";");
        CHECK(!Has(line, "without an observed acquire"));
    }
    CHECK(Has(lines[0], "window 0xAAA") && Has(lines[1], "window 0xBBB"));

    // An index past what was ever acquired, and an acquire past any sane count.
    w.r.OnAcquire(0xD1, 0x5C1, 1000);
    CHECK(w.r.GetStats().imagesOutOfRange == 1);
    w.r.OnAcquire(0xD1, 0x5CF, 0);
    CHECK(w.r.GetStats().acquiresUnknown == 1);
}

static void Teardown()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);
    CHECK(w.r.LiveDevices() == 1 && w.r.LiveQueues() == 1 && w.r.LiveSwapchains() == 1 && w.r.LiveSurfaces() == 1);

    // A device destroyed with its swapchain still alive (an application error): both go.
    w.r.OnDeviceDestroyed(0xD1);
    CHECK(w.r.LiveDevices() == 0 && w.r.LiveQueues() == 0 && w.r.LiveSwapchains() == 0);

    // The instance takes its physical devices and surfaces.
    w.r.OnInstanceDestroyed(World::instance);
    CHECK(w.r.LiveSurfaces() == 0 && w.r.LivePhysicals() == 0 && w.r.FindInstance(World::instance) == nullptr);

    // Destroys of things never seen are harmless.
    w.r.OnDeviceDestroyed(0xDEAD);
    w.r.OnSwapchainDestroyed(0xDEAD, 0xBEEF);
    w.r.OnSurfaceDestroyed(0xDEAD, 0xBEEF);
    w.r.OnInstanceDestroyed(0xDEAD);

    // A device on a physical device that was never enumerated: instance #0, still recorded.
    DeviceDesc desc;
    desc.physical = 0x9999;
    const RecordId orphan = w.r.OnDeviceCreated(0xD5, desc);
    CHECK(orphan != 0 && w.r.FindDevice(0xD5)->instanceId == 0);
    HAS(w.r.DeviceLine(0xD5), "physical 0x9999 instance #0; queue families unknown; requested");
}

static void DeviceLine()
{
    World w;
    const RecordId id = w.Device(0xD1);
    CHECK(w.r.DeviceLine(0xD1) == "DLSS-NR Vulkan present host: device 0xD1 #" + std::to_string(id) +
                                      " created by game.exe (native Vulkan); physical 0x2000 instance #1; queue "
                                      "families 0:0xFx16 1:0xCx2 2:0xEx8; requested family 0 x1 family 2 x1");
    CHECK(w.r.DeviceLine(0xBAD).empty());
}

int main()
{
    Constants();
    Callers();
    RoundTrips();
    FirstPresentDecadesAndWaits();
    IdentityChanges();
    ReusedHandlesAndManyDevices();
    UnknownsAndInternal();
    PresentsAndChains();
    Teardown();
    DeviceLine();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("PASS: registry state machine (records, reuse, many devices, unknowns, internal, lines)");
    return 0;
}
