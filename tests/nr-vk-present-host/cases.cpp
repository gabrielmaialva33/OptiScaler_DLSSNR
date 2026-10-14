// The Vulkan present host's registry and transport decisions (retoque/dlssnr/DlssNr_VkPresentRegistry.h and
// DlssNr_VkPresentTransport.h), as production compiles them, driven through the lifetimes the host has to keep apart
// and the choices stage 2's round trip makes. Plain integers stand in for handles, as the headers themselves use.

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
    CHECK(kPresentModeSharedDemandRefresh == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR);
    CHECK(kPresentModeSharedContinuousRefresh == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR);
    CHECK(kSwapchainCreateProtectedBit == VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR);
    CHECK(kWorkQueueNeeds == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
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

// What a swapchain may be given at creation, decided before the call.
static UsageQuery Eligible(uint32_t asked)
{
    UsageQuery q;
    q.asked = asked;
    q.deviceOn = true;
    q.surfaceKnown = true;
    q.surfaceUsage = 0x9F;
    q.formatAllows = true;
    q.workAllows = true;
    return q;
}

static void Plans()
{
    // DXVK's and vkd3d-proton's swapchains: COLOR_ATTACHMENT|TRANSFER_DST on a surface that offers 0x9F.
    UsagePlan plan = PlanUsage(Eligible(0x12));
    CHECK(plan.transport && plan.usage == 0x13 && plan.added == kUsageTransferSrcBit);
    CHECK(plan.note == "TRANSFER_SRC added at creation");

    plan = PlanUsage(Eligible(0x10));
    CHECK(plan.transport && plan.usage == 0x13 && plan.added == 0x3);
    CHECK(plan.note == "TRANSFER_SRC+TRANSFER_DST added at creation");

    plan = PlanUsage(Eligible(0x13));
    CHECK(plan.transport && plan.usage == 0x13 && plan.added == 0 && plan.note == "as created");

    // Every refusal leaves the application's usage alone and names its reason.
    UsageQuery q = Eligible(0x12);
    q.surfaceUsage = 0x9E;
    plan = PlanUsage(q);
    CHECK(!plan.transport && plan.usage == 0x12 && plan.added == 0);
    CHECK(plan.note == "the surface does not offer TRANSFER_SRC");

    q = Eligible(0x10);
    q.surfaceUsage = 0x9C; // offers neither
    CHECK(PlanUsage(q).note == "the surface does not offer TRANSFER_SRC+TRANSFER_DST");
    q.surfaceUsage = 0x9E; // offers DST only: only the one it lacks is named
    CHECK(PlanUsage(q).note == "the surface does not offer TRANSFER_SRC");

    q = Eligible(0x12);
    q.surfaceKnown = false;
    plan = PlanUsage(q);
    CHECK(!plan.transport &&
          plan.note == "the surface's usages could not be read, so TRANSFER_SRC cannot be asked for");
    q = Eligible(0x13); // nothing to ask for: an unknown surface does not matter
    q.surfaceKnown = false;
    CHECK(PlanUsage(q).transport);

    q = Eligible(0x12);
    q.deviceOn = false;
    q.deviceOff = "OptiScaler's own device";
    CHECK(!PlanUsage(q).transport && PlanUsage(q).note == "OptiScaler's own device");
    q.deviceOff.clear();
    CHECK(PlanUsage(q).note == "the device has no transport");

    q = Eligible(0x12);
    q.flags = kSwapchainCreateProtectedBit;
    CHECK(!PlanUsage(q).transport && PlanUsage(q).note == "a protected swapchain");

    q = Eligible(0x12);
    q.presentMode = kPresentModeSharedContinuousRefresh;
    CHECK(!PlanUsage(q).transport);
    q.presentMode = kPresentModeSharedDemandRefresh;
    CHECK(!PlanUsage(q).transport && Has(PlanUsage(q).note, "shared present mode"));

    q = Eligible(0x12);
    q.arrayLayers = 2;
    CHECK(!PlanUsage(q).transport && PlanUsage(q).note == "2 array layers");

    q = Eligible(0x12);
    q.formatAllows = false;
    CHECK(!PlanUsage(q).transport &&
          PlanUsage(q).note == "the format does not allow TRANSFER_SRC+TRANSFER_DST on swapchain images");

    q = Eligible(0x12);
    q.workAllows = false;
    CHECK(!PlanUsage(q).transport && PlanUsage(q).note == "the work image cannot be made in the swapchain's format");

    // The line's words for each.
    SwapchainDesc d;
    d.transport = true;
    d.transportNote = "TRANSFER_SRC added at creation";
    CHECK(RoundTrip(d) == "on, TRANSFER_SRC added at creation");
    d.transportNote = "as created";
    CHECK(RoundTrip(d) == "on as created");
    d.transport = false;
    d.transportNote = "the surface does not offer TRANSFER_SRC";
    CHECK(RoundTrip(d) == "off, the surface does not offer TRANSFER_SRC");
    d.transportNote.clear();
    CHECK(RoundTrip(d) == "off, not planned");
}

// On which queue the round trip runs.
static void Routes()
{
    RouteQuery q;
    q.families = { { 0xF, 16 }, { 0xC, 2 }, { 0xE, 8 }, { 0x0, 1 }, { 0x8, 1 } };
    q.queues = { { 0x52, 2, 0, 0 }, { 0x51, 0, 0, 0 }, { 0x50, 0, 1, 0 } };

    // A graphics presenter does it itself.
    q.presentQueue = 0x51;
    q.presentFamily = 0;
    RoutePlan plan = ChooseRoute(q);
    CHECK(plan.route == Route::SameQueue && plan.workQueue == 0x51 && plan.workFamily == 0);

    // A compute-only presenter (family 2, 0xE) hands an EXCLUSIVE image to the lowest graphics queue retrieved.
    q.presentQueue = 0x52;
    q.presentFamily = 2;
    plan = ChooseRoute(q);
    CHECK(plan.route == Route::CrossFamily && plan.workQueue == 0x51 && plan.workFamily == 0);
    CHECK(SubmitsFor(plan.route) == 3 && SubmitsFor(Route::SameQueue) == 1 && SubmitsFor(Route::Concurrent) == 1);

    // ... never to a queue created with flags (protected), and never one the application did not retrieve.
    RouteQuery onlyProtected = q;
    onlyProtected.queues = { { 0x52, 2, 0, 0 }, { 0x51, 0, 0, 0x1 } };
    plan = ChooseRoute(onlyProtected);
    CHECK(plan.route == Route::None &&
          plan.why == "the presenting family has no graphics and the application retrieved no graphics queue");
    RouteQuery noGraphics = q;
    noGraphics.queues = { { 0x52, 2, 0, 0 } };
    CHECK(ChooseRoute(noGraphics).route == Route::None);

    // A CONCURRENT image listing the graphics family: no ownership transfer, the present waits across queues.
    RouteQuery concurrent = q;
    concurrent.sharingMode = kSharingModeConcurrent;
    concurrent.sharingFamilies = { 2, 0 };
    plan = ChooseRoute(concurrent);
    CHECK(plan.route == Route::Concurrent && plan.workQueue == 0x51);
    concurrent.sharingFamilies = { 2 };
    plan = ChooseRoute(concurrent);
    CHECK(plan.route == Route::None &&
          plan.why == "no retrieved graphics queue of a family the CONCURRENT swapchain lists");

    // A CONCURRENT image that does not list the graphics presenter: the presenter may not touch it.
    concurrent.presentQueue = 0x51;
    concurrent.presentFamily = 0;
    concurrent.sharingFamilies = { 2 };
    plan = ChooseRoute(concurrent);
    CHECK(plan.route == Route::None && plan.why == "a CONCURRENT swapchain that does not list the presenting family");

    // A present-only family cannot release an EXCLUSIVE image; neither can a sparse-only one.
    for (const uint32_t family : { 3u, 4u })
    {
        q.presentQueue = 0x53;
        q.presentFamily = family;
        plan = ChooseRoute(q);
        CHECK(plan.route == Route::None &&
              plan.why == "a present-only family cannot release the image to another family");
    }

    // A family that can create protected queues is still a graphics family; only a queue created protected is not
    // borrowed (createFlags, above).
    RouteQuery protectedCapable = q;
    protectedCapable.families[0].flags = 0x1F;
    protectedCapable.presentQueue = 0x52;
    protectedCapable.presentFamily = 2;
    plan = ChooseRoute(protectedCapable);
    CHECK(plan.route == Route::CrossFamily && plan.workQueue == 0x51 && plan.workFamily == 0);

    // A family past the device's list.
    q.presentFamily = 9;
    CHECK(ChooseRoute(q).route == Route::None && ChooseRoute(q).why == "the presenting queue's family is unknown");
}

// Which slot of the ring a present uses.
static void Slots()
{
    using S = SlotState;

    SlotChoice c = ChooseSlot({}, {}, kRingMax);
    CHECK(c.kind == SlotChoice::Grow && c.index == 0);

    c = ChooseSlot({ S::Pending, S::Idle }, { 1, 2 }, kRingMax);
    CHECK(c.kind == SlotChoice::Use && c.index == 1);

    c = ChooseSlot({ S::Pending, S::Pending }, { 1, 2 }, kRingMax);
    CHECK(c.kind == SlotChoice::Grow && c.index == 2);

    // Full and all in flight: wait for the oldest, never reuse one whose completion is unknown.
    c = ChooseSlot({ S::Pending, S::Pending, S::Pending, S::Pending }, { 7, 5, 8, 6 }, kRingMax);
    CHECK(c.kind == SlotChoice::WaitOldest && c.index == 1);

    // An abandoned slot is never chosen, even as the oldest.
    c = ChooseSlot({ S::Abandoned, S::Pending, S::Pending, S::Pending }, { 1, 5, 8, 6 }, kRingMax);
    CHECK(c.kind == SlotChoice::WaitOldest && c.index == 1);
    c = ChooseSlot({ S::Abandoned, S::Abandoned, S::Abandoned, S::Abandoned }, { 1, 2, 3, 4 }, kRingMax);
    CHECK(c.kind == SlotChoice::Full);
    c = ChooseSlot({ S::Abandoned }, { 1 }, kRingMax);
    CHECK(c.kind == SlotChoice::Grow);
}

static Outcome Transported(Route route, uint32_t waits, uint32_t completions, uint32_t ring, uint64_t waitNs = 0)
{
    Outcome o;
    o.counted = true;
    o.transported = true;
    o.route = route;
    o.waitsTaken = waits;
    o.submits = SubmitsFor(route);
    o.completions = completions;
    o.ring = ring;
    o.waitNs = waitNs;
    return o;
}

static Outcome Skipped(Skip skip, uint32_t ring = 0, uint64_t waitNs = 0)
{
    Outcome o;
    o.counted = true;
    o.skip = skip;
    o.ring = ring;
    o.waitNs = waitNs;
    return o;
}

static void Stats()
{
    TransportStats s;
    CHECK(TransportText(s) == "transported 0, skipped 0, waits taken over 0, submits 0, completions 0, ring 0, max "
                              "CPU wait 0.000 ms");

    s.Add(Transported(Route::SameQueue, 1, 0, 1));
    s.Add(Transported(Route::CrossFamily, 2, 1, 2));
    s.Add(Skipped(Skip::RingBusy, 4, 100000000));
    s.Add(Skipped(Skip::RingBusy, 4, 50000000));
    s.Add(Skipped(Skip::NoAcquire));
    s.Add(Outcome {}); // not counted: OptiScaler's own device

    CHECK(s.presents == 5 && s.transported == 2 && s.skipped == 3 && s.waitsTaken == 3 && s.submits == 4);
    CHECK(TransportText(s) == "transported 2, skipped 3 (no acquire seen 1, ring busy 2), waits taken over 3, submits "
                              "4, completions 1, ring 4, max CPU wait 100.000 ms, routes same queue 1, cross-family 1");

    for (size_t i = 0; i < static_cast<size_t>(Skip::Count); ++i)
        CHECK(std::string(SkipName(static_cast<Skip>(i))) != "none" || i == 0);
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

    RecordId Swapchain(Handle device, Handle swapchain, Handle surface, uint32_t usage, Handle old = 0,
                       uint32_t surfaceUsage = 0x9F)
    {
        UsageQuery q = Eligible(usage);
        q.surfaceUsage = surfaceUsage;
        const UsagePlan plan = PlanUsage(q);

        SwapchainDesc d;
        d.surface = surface;
        d.oldSwapchain = old;
        d.format = 44;
        d.width = 640;
        d.height = 480;
        d.arrayLayers = 1;
        d.usage = plan.usage;
        d.askedUsage = usage;
        d.surfaceUsageKnown = true;
        d.surfaceUsage = surfaceUsage;
        d.transport = plan.transport;
        d.transportNote = plan.note;
        const RecordId id = r.OnSwapchainCreated(device, swapchain, d);
        const Handle images[] = { swapchain + 1, swapchain + 2, swapchain + 3 };
        r.OnSwapchainImages(device, swapchain, 3, nullptr);
        r.OnSwapchainImages(device, swapchain, 3, images);
        return id;
    }

    std::vector<std::string> Present(Handle queue, Handle swapchain, uint32_t index, uint32_t waits, bool acquire,
                                     Handle device, Caller caller = Caller::Native, Handle module = 0x140000000,
                                     const Outcome& outcome = Outcome {})
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
        call.outcome = outcome;
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
    HAS(first, "DLSS-NR Vulkan present host (round trip, no NR): window 0xAAA (first present); presents 1, swapchains "
               "1, devices 1; swapchain 0x5C1 #" +
                   std::to_string(swapchain) +
                   " 640x480 format 44 colour-space 0 images 3 usage 0x13 (transfer-src "
                   "yes, transfer-dst yes, storage no) asked 0x12 surface-usage 0x9F "
                   "sharing EXCLUSIVE; round-trip on, TRANSFER_SRC added at creation; "
                   "device 0xD1 #" +
                   std::to_string(device) +
                   " physical 0x2000 instance #1; queue 0x51 family 0 index 0 flags 0xF "
                   "(graphics yes, compute yes, transfer yes); waits 1-1; acquires 1; caller "
                   "game.exe (native Vulkan); transport: transported 0, skipped 0, waits "
                   "taken over 0, submits 0, completions 0, ring 0, max CPU wait 0.000 ms");
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
        HAS(lines[0],
            "(new swapchain); presents 5, swapchains 2, devices 1; swapchain 0x5C2 #" + std::to_string(second));
        HAS(lines[0], "usage 0x13 (transfer-src yes, transfer-dst yes, storage no) surface-usage");
        HAS(lines[0], "round-trip on as created;");
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

// The round trip's outcomes in the lines: per window since its first present, per swapchain to its last line.
static void TransportLines()
{
    World w;
    const RecordId device = w.Device(0xD1);
    w.r.OnQueue(0xD1, 2, 0, 0, 0x52);
    w.r.OnQueue(0xD1, 0, 1, 0, 0x50);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    const RecordId sc = w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);

    // At creation: asked, given, and whether the round trip runs.
    CHECK(w.r.SwapchainLine(0xD1, 0x5C1) == "DLSS-NR Vulkan present host: swapchain 0x5C1 #" + std::to_string(sc) +
                                                " on device 0xD1 #" + std::to_string(device) +
                                                ", window 0xAAA, 640x480 format 44: usage 0x12 -> 0x13 (surface "
                                                "offers 0x9F); round trip on, TRANSFER_SRC added at creation");

    // The view the transport works from: queues ordered, the image known, the acquire seen and then spent.
    PresentView view;
    CHECK(!w.r.ViewPresent(0x99, 0x5C1, 0, view)); // a queue never retrieved
    CHECK(!w.r.ViewPresent(0x51, 0x5C9, 0, view)); // a swapchain never created
    w.r.OnAcquire(0xD1, 0x5C1, 1);
    CHECK(w.r.ViewPresent(0x52, 0x5C1, 1, view));
    CHECK(view.device == 0xD1 && view.physical == World::physical && view.deviceId == device && view.swapchainId == sc);
    CHECK(view.presentFamily == 2 && view.families.size() == 3 && view.imageKnown && view.image == 0x5C3);
    CHECK(view.imageCount == 3 && view.acquired && view.desc.transport && view.desc.usage == 0x13);
    CHECK(view.queues.size() == 3 && view.queues[0].queue == 0x51 && view.queues[1].queue == 0x50 &&
          view.queues[2].queue == 0x52);
    CHECK(w.r.ViewPresent(0x52, 0x5C1, 7, view) && !view.imageKnown && view.image == 0);

    auto lines = w.Present(0x52, 0x5C1, 1, 1, false, 0xD1, Caller::Native, 0x140000000,
                           Transported(Route::CrossFamily, 1, 0, 1));
    CHECK(lines.size() == 1);
    if (!lines.empty())
        HAS(lines[0],
            "; transport: transported 1, skipped 0, waits taken over 1, submits 3, completions 0, ring 1, max "
            "CPU wait 0.000 ms, routes cross-family 1");
    CHECK(w.r.ViewPresent(0x52, 0x5C1, 1, view) && !view.acquired); // the present spent the acquire

    for (int i = 0; i < 8; ++i)
        w.Present(0x52, 0x5C1, i % 3, 2, true, 0xD1, Caller::Native, 0x140000000,
                  Transported(Route::CrossFamily, 2, 1, 2));
    lines =
        w.Present(0x52, 0x5C1, 0, 0, true, 0xD1, Caller::Native, 0x140000000, Skipped(Skip::RingBusy, 4, 100000000));
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "(10 presents)");
        HAS(lines[0], "; transport: transported 9, skipped 1 (ring busy 1), waits taken over 17, submits 27, "
                      "completions 8, ring 4, max CPU wait 100.000 ms, routes cross-family 9");
    }

    // A first no-route reason is kept for the window's lines.
    Outcome noRoute = Skipped(Skip::NoRoute);
    noRoute.note = "a present-only family cannot release the image to another family";
    for (int i = 0; i < 90; ++i)
        lines = w.Present(0x52, 0x5C1, i % 3, 1, true, 0xD1, Caller::Native, 0x140000000, noRoute);
    CHECK(lines.size() == 1);
    if (!lines.empty())
        HAS(lines[0], "skipped 91 (no route 90, ring busy 1)");
    if (!lines.empty())
        HAS(lines[0], "; first no-route reason: a present-only family cannot release the image to another family");

    // Its last line: the presents, and the completions observed when it was retired.
    w.r.OnCompletions(0xD1, 0x5C1, 1);
    CHECK(w.r.SwapchainSummary(0xD1, 0x5C1, "freed after a drain of 0.010 ms") ==
          "DLSS-NR Vulkan present host: swapchain 0x5C1 #" + std::to_string(sc) +
              " destroyed after 100 presents; transported 9, skipped 91 (no route 90, ring busy 1), waits taken over "
              "17, submits 27, completions 9, ring 4, max CPU wait 100.000 ms, routes cross-family 9; freed after a "
              "drain of 0.010 ms");
    CHECK(w.r.FindWindow(true, 0xAAA)->transport.completions == 9);

    // An observe-only swapchain: its creation line says why, and nothing about it is ever transported.
    w.r.OnSurfaceCreated(World::instance, 0x5F2, 0xBBB);
    w.Swapchain(0xD1, 0x5C2, 0x5F2, 0x12, 0, 0x9E);
    HAS(w.r.SwapchainLine(0xD1, 0x5C2), ": usage 0x12 (surface offers 0x9E); round trip off, the surface does not "
                                        "offer TRANSFER_SRC");
    lines = w.Present(0x51, 0x5C2, 0, 1, true, 0xD1, Caller::Native, 0x140000000, Skipped(Skip::ObserveOnly));
    CHECK(lines.size() == 1);
    if (!lines.empty())
    {
        HAS(lines[0], "usage 0x12 (transfer-src no, transfer-dst yes, storage no) surface-usage 0x9E");
        HAS(lines[0], "round-trip off, the surface does not offer TRANSFER_SRC;");
        HAS(lines[0], "; transport: transported 0, skipped 1 (observe only 1),");
    }

    // Retirement of something never seen is harmless, and no summary is written for it.
    w.r.OnCompletions(0xD1, 0xBAD, 3);
    CHECK(w.r.SwapchainSummary(0xD1, 0xBAD, "").empty());
    CHECK(w.r.SwapchainLine(0xD1, 0xBAD).empty());
}

int main()
{
    Constants();
    Callers();
    Plans();
    Routes();
    Slots();
    Stats();
    FirstPresentDecadesAndWaits();
    IdentityChanges();
    ReusedHandlesAndManyDevices();
    UnknownsAndInternal();
    PresentsAndChains();
    Teardown();
    DeviceLine();
    TransportLines();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("PASS: registry state machine (records, reuse, many devices, unknowns, internal, lines) and the "
              "round trip's decisions (usage plan, route, ring, counters, transport lines)");
    return 0;
}
