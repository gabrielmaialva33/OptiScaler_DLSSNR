// The Vulkan present host's registry and transport decisions (retoque/dlssnr/DlssNr_VkPresentRegistry.h,
// DlssNr_VkPresentTransport.h and DlssNr_VkPresentNr.h), and the model's one-owner arbitration (DlssNr_VkModelOwner.h),
// as production compiles them, driven through the lifetimes the host has to keep apart, the choices stage 2's round
// trip makes and stage 3's cold NR adds. Plain integers stand in for handles, as the headers themselves use.

#include <vulkan/vulkan.h>

#include "DlssNr_VkModelOwner.h"
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
    HAS(first, "DLSS-NR Vulkan present host (cold NR): window 0xAAA (first present); presents 1, swapchains "
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
                   "taken over 0, submits 0, completions 0, ring 0, max CPU wait 0.000 ms; NR: evaluated 0, refused "
                   "0, history resets 0, pass GPU not measured");
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
              "drain of 0.010 ms; NR: evaluated 0, refused 0, history resets 0, pass GPU not measured");
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

// --- Stage 3: cold NR -------------------------------------------------------------------------------------------

static void NrConstants()
{
    CHECK(kFormatR8G8B8A8Unorm == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(kFormatR8G8B8A8Srgb == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(kFormatB8G8R8A8Unorm == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(kFormatB8G8R8A8Srgb == VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(kFormatA2R10G10B10UnormPack32 == VK_FORMAT_A2R10G10B10_UNORM_PACK32);
    CHECK(kFormatA2B10G10R10UnormPack32 == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    CHECK(kFormatR16G16B16A16Sfloat == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(kFormatR32Sfloat == VK_FORMAT_R32_SFLOAT);
    CHECK(kFormatR16G16Sfloat == VK_FORMAT_R16G16_SFLOAT);
    CHECK(kColourSpaceSrgbNonlinear == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    CHECK(kColourSpaceExtendedSrgbLinear == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT);
    CHECK(kColourSpaceHdr10St2084 == VK_COLOR_SPACE_HDR10_ST2084_EXT);
    CHECK(kColourSpaceHdr10Hlg == VK_COLOR_SPACE_HDR10_HLG_EXT);
    CHECK(kFeatureSampledImage == VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    CHECK(kFeatureStorageImage == VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    CHECK(kFeatureBlitSrc == VK_FORMAT_FEATURE_BLIT_SRC_BIT);
    CHECK(kFeatureBlitDst == VK_FORMAT_FEATURE_BLIT_DST_BIT);
    CHECK(kFeatureTransferSrc == VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
    CHECK(kFeatureTransferDst == VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    CHECK(kWorkingFormat == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK(std::string(kNrExtensions[0]) == "VK_NVX_binary_import");
    CHECK(std::string(kNrExtensions[2]) == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    CHECK(std::string(kNrExtensions[3]) == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    CHECK(std::string(kBdaExt) == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    CHECK(std::string(kFormatFeatureFlags2) == VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
    CHECK(kStructBdaFeatures == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    CHECK(kStructBdaFeaturesExt == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT);
    CHECK(kStructVulkan12Features == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
}

// SDR only: RGBA8/BGRA8, UNORM or sRGB, in sRGB non-linear. sRGB goes through its UNORM twin, so nothing is decoded.
static void ColourPlans()
{
    for (const auto [format, raw] : { std::pair { kFormatB8G8R8A8Unorm, kFormatB8G8R8A8Unorm },
                                      std::pair { kFormatR8G8B8A8Unorm, kFormatR8G8B8A8Unorm },
                                      std::pair { kFormatB8G8R8A8Srgb, kFormatB8G8R8A8Unorm },
                                      std::pair { kFormatR8G8B8A8Srgb, kFormatR8G8B8A8Unorm } })
    {
        const ColourPlan plan = PlanColour(format, kColourSpaceSrgbNonlinear);
        CHECK(plan.sdr && plan.raw == raw && plan.refusal.empty());
    }

    // Every refusal names what it refused, and leaves the raw format the swapchain's own (stage 2's round trip).
    ColourPlan hdr10 = PlanColour(kFormatA2B10G10R10UnormPack32, kColourSpaceHdr10St2084);
    CHECK(!hdr10.sdr && hdr10.raw == kFormatA2B10G10R10UnormPack32);
    CHECK(hdr10.refusal == "an HDR10 swapchain (PQ, Rec.2020) (A2B10G10R10_UNORM_PACK32, colour space 1000104008): the "
                           "present host's NR takes SDR only; the frame is shown as the game rendered it");
    HAS(PlanColour(kFormatR16G16B16A16Sfloat, kColourSpaceExtendedSrgbLinear).refusal, "an scRGB swapchain (linear "
                                                                                       "light) (R16G16B16A16_SFLOAT");
    HAS(PlanColour(kFormatR16G16B16A16Sfloat, kColourSpaceSrgbNonlinear).refusal, "a floating-point swapchain");
    HAS(PlanColour(kFormatA2R10G10B10UnormPack32, kColourSpaceSrgbNonlinear).refusal, "a ten-bit swapchain");
    HAS(PlanColour(kFormatB8G8R8A8Unorm, kColourSpaceHdr10Hlg).refusal, "an HLG swapchain");
    HAS(PlanColour(kFormatB8G8R8A8Unorm, 1000104001).refusal, "a colour space other than sRGB non-linear "
                                                              "(B8G8R8A8_UNORM, colour space 1000104001)");
    HAS(PlanColour(VK_FORMAT_R5G6B5_UNORM_PACK16, 0).refusal, "a format other than RGBA8/BGRA8 (format 4, colour "
                                                              "space 0)");
}

// The model's device extensions at vkCreateDevice: what the device offers and the application did not ask for.
static void ExtensionPlans()
{
    const std::vector<std::string> all = {
        "VK_KHR_swapchain",       "VK_NVX_binary_import",         "VK_NVX_image_view_handle",
        "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address", "VK_EXT_buffer_device_address"
    };

    ExtensionPlan plan = PlanDeviceExtensions({ "VK_KHR_swapchain" }, all, true);
    CHECK(plan.ready && plan.missing.empty() && plan.already.empty());
    CHECK((plan.add == std::vector<std::string> { "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                                                  "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address" }));
    CHECK(ExtensionText(plan) == "NR extensions: added VK_NVX_binary_import, VK_NVX_image_view_handle, "
                                 "VK_KHR_push_descriptor, VK_KHR_buffer_device_address; already enabled none; not "
                                 "possible none");

    // DXVK asks for the NVX pair itself; the bundle has added everything before the host sees the list.
    plan = PlanDeviceExtensions({ "VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor",
                                  "VK_KHR_buffer_device_address" },
                                all, true);
    CHECK(plan.ready && plan.add.empty() && plan.already.size() == 4);

    // The application's EXT buffer device address stands for the KHR one: never both (VUID-...-03328).
    plan = PlanDeviceExtensions({ "VK_EXT_buffer_device_address" }, all, true);
    CHECK(plan.ready && !Listed(plan.add, "VK_KHR_buffer_device_address") &&
          Listed(plan.already, "VK_EXT_buffer_device_address"));

    // Not offered: not asked for (asking makes vkCreateDevice fail), and the device is not ready for the model.
    plan = PlanDeviceExtensions({}, { "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address" }, true);
    CHECK(!plan.ready && plan.add.size() == 2 && plan.missing.size() == 2);
    HAS(ExtensionText(plan), "not possible VK_NVX_binary_import (the device does not offer it), "
                             "VK_NVX_image_view_handle (the device does not offer it)");

    // Format-less storage writes not allowed yet (no feature, below Vulkan 1.3): format_feature_flags2 too.
    std::vector<std::string> withFlags2 = all;
    withFlags2.push_back("VK_KHR_format_feature_flags2");
    plan = PlanDeviceExtensions({ "VK_KHR_swapchain" }, withFlags2, true, false);
    CHECK(plan.ready && plan.add.size() == 5 && plan.add.back() == "VK_KHR_format_feature_flags2");
    plan = PlanDeviceExtensions({}, all, true, false); // not offered: the device is not ready for the model
    CHECK(!plan.ready && plan.missing.size() == 1);
    HAS(ExtensionText(plan), "not possible VK_KHR_format_feature_flags2 (the device does not offer it)");
    CHECK(PlanDeviceExtensions({}, all, true, true).ready); // allowed already: not asked for

    // A Vulkan 1.0 instance without properties2: the two that depend on it are not asked for.
    plan = PlanDeviceExtensions({}, all, false);
    CHECK(!plan.ready && plan.add.size() == 2);
    HAS(ExtensionText(plan), "VK_KHR_push_descriptor (needs Vulkan 1.1 or VK_KHR_get_physical_device_properties2 on "
                             "the instance)");
}

// bufferDeviceAddress: prepended only where nothing in the application's chain could conflict.
static void BdaPlans()
{
    BdaChain chain;
    CHECK(PlanBufferDeviceAddress(chain, true, false) == BdaAction::AddKhr);
    CHECK(PlanBufferDeviceAddress(chain, true, true) == BdaAction::AddExt);
    CHECK(PlanBufferDeviceAddress(chain, false, false) == BdaAction::NotSupported);

    chain.v12Node = true; // DXVK and vkd3d-proton: a Vulkan 1.2 features struct, here with the feature off
    CHECK(PlanBufferDeviceAddress(chain, true, false) == BdaAction::ChainHasItOff);
    chain.enabled = true; // ... or on
    CHECK(PlanBufferDeviceAddress(chain, true, false) == BdaAction::Already);
    CHECK(PlanBufferDeviceAddress(chain, false, false) == BdaAction::Already);

    BdaChain ext;
    ext.extNode = true;
    CHECK(PlanBufferDeviceAddress(ext, true, true) == BdaAction::ChainHasItOff);

    CHECK(BdaReady(BdaAction::Already) && BdaReady(BdaAction::AddKhr) && BdaReady(BdaAction::AddExt));
    CHECK(!BdaReady(BdaAction::NotSupported) && !BdaReady(BdaAction::ChainHasItOff));
    CHECK(std::string(BdaText(BdaAction::AddKhr)) == "bufferDeviceAddress enabled by the host");
    HAS(BdaText(BdaAction::ChainHasItOff), "has it off; not edited");
}

static Outcome WithNr(Outcome o, bool evaluated, NrSkip skip = NrSkip::None, std::string note = {}, bool reset = false)
{
    o.nrEvaluated = evaluated;
    o.nrSkip = skip;
    o.nrNote = std::move(note);
    o.nrReset = reset;
    return o;
}

// The model's counters: evaluated and refused only on presents the round trip carried; GPU times from completions.
static void NrCounters()
{
    TransportStats s;
    CHECK(NrText(s.nr) == "NR: evaluated 0, refused 0, history resets 0, pass GPU not measured");

    s.Add(WithNr(Transported(Route::SameQueue, 1, 0, 1), true, NrSkip::None, {}, true));
    s.Add(WithNr(Transported(Route::SameQueue, 1, 1, 1), true));
    Outcome timed = WithNr(Transported(Route::SameQueue, 1, 1, 1), true);
    timed.nrTimed = 2;
    timed.nrLastGpuMs = 3.0;
    timed.nrMaxGpuMs = 4.0;
    timed.nrSumGpuMs = 7.0;
    s.Add(timed);
    s.Add(WithNr(Transported(Route::SameQueue, 1, 1, 1), false, NrSkip::Colour, "an HDR10 swapchain ..."));
    s.Add(WithNr(Transported(Route::SameQueue, 1, 1, 1), false, NrSkip::Busy, "busy"));
    // Not transported: whatever the model would have said is not counted (the transport's skip is).
    s.Add(WithNr(Skipped(Skip::RingBusy, 4), false, NrSkip::Off, "off"));
    s.AddGpuTimes(1, 2.0, 2.0, 2.0); // at a swapchain's retirement

    CHECK(s.nr.evaluated == 3 && s.nr.refused == 2 && s.nr.resets == 1 && s.nr.timed == 3);
    CHECK(NrText(s.nr) == "NR: evaluated 3, refused 2 (colour 1, model busy 1), history resets 1, pass GPU last 2.000 "
                          "ms, mean 3.000 ms, max 4.000 ms; first refusal: an HDR10 swapchain ...");

    for (size_t i = 1; i < static_cast<size_t>(NrSkip::Count); ++i)
        CHECK(std::string(NrSkipName(static_cast<NrSkip>(i))) != "none");

    CHECK(NrReset(true, false, false) && NrReset(false, true, false) && NrReset(false, false, true));
    CHECK(!NrReset(false, false, false));
    CHECK(kNrInFlightMax * 3 <= 18); // three dispatches a frame in DlssNr_Vk's ring of eighteen slots
}

// One owner of the model in the process: the after-upscale route wins, the present host stands down and gives it up.
static void OwnerArbitration()
{
    using namespace DlssNr::VkModelOwner;
    Arbiter a;
    constexpr uint64_t d1 = 0xD1, d2 = 0xD2;

    // Nobody holds it: the present host may take it on d1, not on d2 while it holds d1.
    CHECK(a.PresentHost(d1, 1000).granted);
    a.Take(d1);
    CHECK(a.Owner() == Who::PresentHost && a.Device() == d1);
    CHECK(a.PresentHost(d1, 1001).granted);
    Verdict v = a.PresentHost(d2, 1001);
    CHECK(!v.granted && v.reason == kOtherDevice);
    CHECK(!a.PresentHostMustYield(d1, 1001));

    // The game's upscaler starts on Vulkan: refused while the present host holds it, which must now yield.
    v = a.AfterUpscale(d1, 2000);
    CHECK(!v.granted && v.reason == kHeldByPresentHost);
    CHECK(a.PresentHostMustYield(d1, 2001) && !a.PresentHostMustYield(d2, 2001));
    v = a.PresentHost(d1, 2001);
    CHECK(!v.granted && v.reason == kAfterUpscaleWants);

    // Released once its frames are seen complete; the after-upscale route takes it and keeps it.
    a.Release(Who::AfterUpscale, d1); // not the holder: nothing
    CHECK(a.Owner() == Who::PresentHost);
    a.Release(Who::PresentHost, d1);
    CHECK(a.Owner() == Who::None);
    CHECK(a.AfterUpscale(d1, 2100).granted && a.Owner() == Who::AfterUpscale);
    v = a.PresentHost(d1, 9000);
    CHECK(!v.granted && v.reason == kAfterUpscaleOwns);
    v = a.PresentHost(d2, 9000);
    CHECK(!v.granted && v.reason == kAfterUpscaleOwns);
    CHECK(!a.PresentHostMustYield(d1, 9000));

    // Its own device change is its own: granted on the new one.
    CHECK(a.AfterUpscale(d2, 9100).granted && a.Device() == d2);

    // A route that asked once, half a second ago or more, no longer keeps the present host down.
    Arbiter b;
    CHECK(b.AfterUpscale(d1, 100).granted);
    b.Release(Who::AfterUpscale, d1);
    v = b.PresentHost(d1, 100 + kAfterUpscaleLiveMs - 1);
    CHECK(!v.granted && v.reason == kAfterUpscaleWants);
    CHECK(b.PresentHost(d1, 100 + kAfterUpscaleLiveMs).granted);
}

// The model's results in the lines: per window, and in a swapchain's last line.
static void NrLines()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);

    PresentView view;
    CHECK(w.r.ViewPresent(0x51, 0x5C1, 0, view) && view.instance == World::instance && !view.retired);
    CHECK(w.r.InstanceOf(World::physical) == World::instance && w.r.InstanceOf(0x9999) == 0);
    CHECK(!w.r.Properties2Of(World::physical)); // the world's instance was recorded without it
    w.r.OnInstanceCreated(0x1001, false, true);
    const Handle physicals[] = { 0x2001 };
    w.r.OnPhysicalDevices(0x1001, physicals, 1);
    CHECK(w.r.Properties2Of(0x2001));

    std::vector<std::string> lines;
    for (int i = 0; i < 10; ++i)
    {
        Outcome o =
            WithNr(Transported(Route::SameQueue, 1, i == 0 ? 0 : 1, 1), i != 3, i == 3 ? NrSkip::Busy : NrSkip::None,
                   i == 3 ? "the model already has 4 frames in flight" : "", i == 0 || i == 4);
        if (i == 9)
        {
            o.nrTimed = 1;
            o.nrLastGpuMs = o.nrMaxGpuMs = o.nrSumGpuMs = 3.25;
        }
        lines = w.Present(0x51, 0x5C1, i % 3, 1, true, 0xD1, Caller::Native, 0x140000000, o);
    }

    CHECK(lines.size() == 1);
    if (!lines.empty())
        HAS(lines[0], "max CPU wait 0.000 ms, routes same queue 10; NR: evaluated 9, refused 1 (model busy 1), history "
                      "resets 2, pass GPU last 3.250 ms, mean 3.250 ms, max 3.250 ms; first refusal: the model already "
                      "has 4 frames in flight");

    // A swapchain created with this one as its oldSwapchain retires it: the model may move on.
    w.Swapchain(0xD1, 0x5C2, 0x5F1, 0x13, 0x5C1);
    CHECK(w.r.ViewPresent(0x51, 0x5C1, 0, view) && view.retired);

    w.r.OnCompletions(0xD1, 0x5C1, 1, 1, 3.75, 3.75, 3.75);
    HAS(w.r.SwapchainSummary(0xD1, 0x5C1, "freed after a drain of 0.010 ms"),
        "completions 10, ring 1, max CPU wait 0.000 ms, routes same queue 10; freed after a drain of 0.010 ms; NR: "
        "evaluated 9, refused 1 (model busy 1), history resets 2, pass GPU last 3.750 ms, mean 3.500 ms, max 3.750 ms");
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
    NrConstants();
    ColourPlans();
    ExtensionPlans();
    BdaPlans();
    NrCounters();
    OwnerArbitration();
    NrLines();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("PASS: registry state machine (records, reuse, many devices, unknowns, internal, lines), the round "
              "trip's decisions (usage plan, route, ring, counters, transport lines) and cold NR's (colour plan, "
              "device extensions, counters, one owner, NR lines)");
    return 0;
}
