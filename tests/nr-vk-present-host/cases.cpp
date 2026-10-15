// The Vulkan present host's registry and transport decisions (retoque/dlssnr/DlssNr_VkPresentRegistry.h,
// DlssNr_VkPresentTransport.h and DlssNr_VkPresentNr.h), and the model's one-owner arbitration (DlssNr_VkModelOwner.h),
// as production compiles them, driven through the lifetimes the host has to keep apart, the choices stage 2's round
// trip makes and stage 3's cold NR adds. Plain integers stand in for handles, as the headers themselves use.

#include <vulkan/vulkan.h>

#include "DlssNr_VkModelOwner.h"
#include "DlssNr_VkPresentRegistry.h"

#include <future>
#include <thread>
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

    uint64_t nowMs = 0;

    std::vector<std::string> Present(Handle queue, Handle swapchain, uint32_t index, uint32_t waits, bool acquire,
                                     Handle device, Caller caller = Caller::Native, Handle module = 0x140000000,
                                     const Outcome& outcome = Outcome {})
    {
        if (acquire)
            r.OnAcquire(device, swapchain, index);
        PresentCall call;
        call.nowMs = nowMs;
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

static void Heartbeat()
{
    World w;
    w.Device(0xD1);
    w.r.OnQueue(0xD1, 0, 0, 0, 0x51);
    w.r.OnSurfaceCreated(World::instance, 0x5F1, 0xAAA);
    w.Swapchain(0xD1, 0x5C1, 0x5F1, 0x12);
    w.nowMs = 1234;
    CHECK(w.Present(0x51, 0x5C1, 0, 1, true, 0xD1).size() == 1);
    w.nowMs += 59999;
    CHECK(w.Present(0x51, 0x5C1, 1, 1, true, 0xD1).empty());
    ++w.nowMs;
    auto lines = w.Present(0x51, 0x5C1, 2, 1, true, 0xD1);
    CHECK(lines.size() == 1);
    if (!lines.empty())
        HAS(lines[0], "60 s heartbeat");
    CHECK(w.Present(0x51, 0x5C1, 0, 1, true, 0xD1).empty());
    // The first present after a long gap emits one current line, not a catch-up burst.
    w.nowMs += 180000;
    lines = w.Present(0x51, 0x5C1, 1, 1, true, 0xD1);
    CHECK(lines.size() == 1);
    if (!lines.empty())
        HAS(lines[0], "presents 5");
    CHECK(w.Present(0x51, 0x5C1, 2, 1, true, 0xD1).empty());
    // Any event line resets the heartbeat deadline.
    w.r.OnQueue(0xD1, 2, 0, 0, 0x52);
    w.nowMs += 50000;
    CHECK(w.Present(0x52, 0x5C1, 0, 1, true, 0xD1).size() == 1);
    w.nowMs += 10000;
    CHECK(w.Present(0x52, 0x5C1, 1, 1, true, 0xD1).empty());
}

static void HandBackDiagnosticsAndLocks()
{
    using namespace DlssNr::VkModelOwner;
    Arbiter owner;
    CHECK(owner.AfterUpscale(1, 0).granted);
    owner.AfterUpscaleDone(0);
    const uint64_t due = kAfterUpscaleIdleMs;
    CHECK(owner.IdleAfterUpscaleDevice(due) == 1);
    HandBackReadiness ready;
    HandBackDiagnostics diagnostics;
    ready.enabled = false; // F7 off: the route's idle clock is not restarted.
    auto reason = HandBackBlockReason(ready);
    CHECK(reason == "NR off ([DlssNr] Enabled=false)");
    CHECK(diagnostics.Blocked(reason, due));
    for (uint64_t i = 1; i < 100; ++i)
        CHECK(!diagnostics.Blocked(reason, due + i));
    ready.enabled = true;
    CHECK(HandBackBlockReason(ready).empty());
    CHECK(owner.BeginHandBack(1, due + 100)); // at once, not another ten seconds
    CHECK(owner.FinishHandBack(1) == HandBack::Done);
    diagnostics.Reset();
    CHECK(diagnostics.Blocked(reason, due + 101)); // a new due episode can report it again
    CHECK(diagnostics.Blocked("device lost", due + 102));
    CHECK(!diagnostics.Blocked(reason, due + 103)); // distinct reasons do not spam when alternating

    for (bool submitBusy : { false, true })
    {
        QueueLocks queue;
        std::promise<void> locked, release;
        auto released = release.get_future();
        std::thread worker(
            [&]
            {
                std::lock_guard lock(submitBusy ? queue.submit : queue.present);
                locked.set_value();
                released.wait();
            });
        locked.get_future().wait();
        diagnostics.Reset();
        std::vector<std::unique_lock<QueueMutex>> held;
        const std::string expected = submitBusy ? "submit" : "present";
        for (uint64_t ms : { 0, 999, 1000, 1001, 5000 })
        {
            const char* busy = TryHandBackQueueLocks(queue, held);
            CHECK(busy != nullptr && expected == busy && held.empty());
            CHECK(diagnostics.Blocked(expected, ms, 1000) == (ms == 1000));
        }
        release.set_value();
        worker.join();
        CHECK(TryHandBackQueueLocks(queue, held) == nullptr && held.size() == 2);
        held.clear();
    }
}

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
    for (const auto& [format, raw] : { std::pair { kFormatB8G8R8A8Unorm, kFormatB8G8R8A8Unorm },
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
static DeviceCreateFacts Facts(bool properties2 = true, bool vulkan11 = true, bool storage = true)
{
    DeviceCreateFacts f;
    f.properties2 = properties2;
    f.vulkan11 = vulkan11;
    f.storageWithoutFormat = storage;
    return f;
}

static void ExtensionPlans()
{
    const std::vector<std::string> all = {
        "VK_KHR_swapchain",       "VK_NVX_binary_import",         "VK_NVX_image_view_handle",
        "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address", "VK_EXT_buffer_device_address"
    };

    ExtensionPlan plan = PlanDeviceExtensions({ "VK_KHR_swapchain" }, all, Facts());
    CHECK(plan.ready && plan.missing.empty() && plan.already.empty());
    CHECK((plan.add == std::vector<std::string> { "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                                                  "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address" }));
    CHECK(ExtensionText(plan) == "NR extensions: added VK_NVX_binary_import, VK_NVX_image_view_handle, "
                                 "VK_KHR_push_descriptor, VK_KHR_buffer_device_address; already enabled none; not "
                                 "possible none");

    // DXVK asks for the NVX pair itself; the bundle has added everything before the host sees the list.
    plan = PlanDeviceExtensions({ "VK_NVX_binary_import", "VK_NVX_image_view_handle", "VK_KHR_push_descriptor",
                                  "VK_KHR_buffer_device_address" },
                                all, Facts());
    CHECK(plan.ready && plan.add.empty() && plan.already.size() == 4);

    // The application's EXT buffer device address stands for the KHR one: never both (VUID-...-03328).
    plan = PlanDeviceExtensions({ "VK_EXT_buffer_device_address" }, all, Facts());
    CHECK(plan.ready && !Listed(plan.add, "VK_KHR_buffer_device_address") &&
          Listed(plan.already, "VK_EXT_buffer_device_address"));

    // Without the NVX pair the model cannot run on the device: nothing at all is added to it (another vendor's device
    // keeps its create info), and it is not ready.
    plan = PlanDeviceExtensions({}, { "VK_KHR_push_descriptor", "VK_KHR_buffer_device_address" }, Facts());
    CHECK(!plan.ready && plan.add.empty() && plan.missing.size() == 2);
    HAS(ExtensionText(plan), "added none; already enabled none; not possible VK_NVX_binary_import (the device does not "
                             "offer it), VK_NVX_image_view_handle (the device does not offer it)");
    plan = PlanDeviceExtensions({ "VK_NVX_binary_import" }, { "VK_KHR_push_descriptor" }, Facts());
    CHECK(!plan.ready && plan.add.empty() && plan.missing.size() == 1 && plan.already.size() == 1);

    // Format-less storage writes not allowed yet (no feature, below Vulkan 1.3): format_feature_flags2 too.
    std::vector<std::string> withFlags2 = all;
    withFlags2.push_back("VK_KHR_format_feature_flags2");
    plan = PlanDeviceExtensions({ "VK_KHR_swapchain" }, withFlags2, Facts(true, true, false));
    CHECK(plan.ready && plan.add.size() == 5 && plan.add.back() == "VK_KHR_format_feature_flags2");
    plan = PlanDeviceExtensions({}, all, Facts(true, true, false)); // not offered: the device is not ready
    CHECK(!plan.ready && plan.missing.size() == 1);
    HAS(ExtensionText(plan), "not possible VK_KHR_format_feature_flags2 (the device does not offer it)");
    CHECK(PlanDeviceExtensions({}, all, Facts(true, true, true)).ready); // allowed already: not asked for

    // A Vulkan 1.0 instance without properties2: the two that depend on it are not asked for.
    plan = PlanDeviceExtensions({}, all, Facts(false, false));
    CHECK(!plan.ready && plan.add.size() == 2);
    HAS(ExtensionText(plan), "VK_KHR_push_descriptor (needs Vulkan 1.1 or VK_KHR_get_physical_device_properties2 on "
                             "the instance)");
    // With properties2 but still 1.0: the KHR buffer device address also needs VK_KHR_device_group, not added.
    plan = PlanDeviceExtensions({}, all, Facts(true, false));
    CHECK(!plan.ready && plan.add.size() == 3);
    HAS(ExtensionText(plan), "VK_KHR_buffer_device_address (needs a Vulkan 1.1 instance: VK_KHR_device_group is not "
                             "added)");

    // A Vulkan 1.4 features struct with pushDescriptor off: VK_KHR_push_descriptor may not be enabled beside it.
    DeviceCreateFacts v14 = Facts();
    v14.vulkan14Features = true;
    plan = PlanDeviceExtensions({}, all, v14);
    CHECK(!plan.ready && !Listed(plan.add, "VK_KHR_push_descriptor"));
    HAS(ExtensionText(plan), "VK_KHR_push_descriptor (the application's VkPhysicalDeviceVulkan14Features has "
                             "pushDescriptor off)");
    v14.pushDescriptorFeature = true;
    CHECK(PlanDeviceExtensions({}, all, v14).ready);
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
    CHECK(!BdaReady(BdaAction::NotSupported));
    // A chain with it off is not edited, and the model runs as the after-upscale route runs it.
    CHECK(BdaReady(BdaAction::ChainHasItOff));
    CHECK(std::string(BdaText(BdaAction::AddKhr)) == "bufferDeviceAddress enabled by the host");
    HAS(BdaText(BdaAction::ChainHasItOff), "off in the application's create info, not edited");
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

// Stage 5, synthesized motion at the Vulkan present (DlssNr_VkPresentNr.h): which products ask for the estimator, when
// its history starts over, what the model is told about motion, when the model's history resets, and the counter.
static void SynthMotionDecisions()
{
    // The key on, the device not refusing it, the model's working size (what the estimator runs at) no smaller than
    // it can: 517x389 at WorkingScale 0.5 is 259x195, enough; 480x270 at 0.5 is not.
    CHECK(SynthWanted(true, false, 3440, 1440) && SynthWanted(true, false, 256, 128) && SynthWanted(true, false, 259, 195));
    CHECK(!SynthWanted(false, false, 3440, 1440) && !SynthWanted(true, true, 3440, 1440));
    CHECK(!SynthWanted(true, false, 255, 720) && !SynthWanted(true, false, 1280, 127) && !SynthWanted(true, false, 240, 135));

    // A product with the estimator is another key: turning the key on or off is a rebuild, frames carried meanwhile.
    const BuildKey plain { 3440, 1440, 3440, 1440, 0 }, synth { 3440, 1440, 3440, 1440, 0, true };
    CHECK(DecideBuild(false, BuildState::None, true, plain, synth) == BuildStep::Request);
    CHECK(DecideBuild(false, BuildState::None, true, synth, plain) == BuildStep::Request);
    CHECK(DecideBuild(false, BuildState::None, true, synth, synth) == BuildStep::Use);

    // The estimator's history: over on its first recording and after a gap over 250 ms (the D3D12 rule), not on the
    // model's resets or the host's cut (the model forgets its own history then).
    CHECK(SynthReset(0, 1000));
    CHECK(!SynthReset(990, 1000));
    CHECK(!SynthReset(1000 - kSynthStaleMs, 1000));
    CHECK(SynthReset(1000 - kSynthStaleMs - 1, 1000));

    // The guide: the zero one until the field is Ready, then the field; either at the model's working size (the field
    // is estimated on the model's own input, in working pixels), at unit scale. Never larger than the model's colour.
    const MotionGuide zero = ChooseMotionGuide(false, 1720, 720);
    CHECK(!zero.field && zero.width == 1720 && zero.height == 720 && zero.scaleX == 1.0f && zero.scaleY == 1.0f);
    const MotionGuide field = ChooseMotionGuide(true, 1720, 720);
    CHECK(field.field && field.width == 1720 && field.height == 720 && field.scaleX == 1.0f && field.scaleY == 1.0f);

    // The model's reset: a scene cut is a cut; ZeroGuideReset applies only to a frame on the zero guide.
    CHECK(NrResetWithMotion(false, false, true, false, true));
    CHECK(!NrResetWithMotion(false, false, false, true, true));
    CHECK(NrResetWithMotion(false, false, false, true, false));
    CHECK(NrResetWithMotion(true, false, false, false, true) && NrResetWithMotion(false, true, false, false, true));
    CHECK(!NrResetWithMotion(false, false, false, false, false));

    // Counted per evaluated frame, and said only once a field has been handed over.
    TransportStats s;
    Outcome withField = WithNr(Transported(Route::SameQueue, 1, 1, 1), true);
    withField.nrSynthesized = true;
    s.Add(WithNr(Transported(Route::SameQueue, 1, 1, 1), true));
    CHECK(NrText(s.nr) == "NR: evaluated 1, refused 0, history resets 0, pass GPU not measured");
    s.Add(withField);
    s.Add(withField);
    CHECK(s.nr.evaluated == 3 && s.nr.synthesized == 2);
    CHECK(NrText(s.nr) == "NR: evaluated 3, refused 0, history resets 0, motion synthesized 2, pass GPU not measured");
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

// The way back (DOOM Eternal, 2026-10-14): the game's DLSS turned off left the after-upscale route holding the model
// for the session. Once it has been out of an evaluate for kAfterUpscaleIdleMs the present host takes it back; asking
// again is the ordinary stand-down, as many times as the game turns its upscaler on and off.
//
// One evaluate of the route: granted (or not) at `at`, its recording over `takes` later.
static DlssNr::VkModelOwner::Verdict Evaluate(DlssNr::VkModelOwner::Arbiter& a, uint64_t device, uint64_t at,
                                              uint64_t takes = 2)
{
    const auto verdict = a.AfterUpscale(device, at);
    if (verdict.granted)
    {
        CHECK(a.Recording() && a.IdleAfterUpscaleDevice(at + 100000) == 0);
        a.AfterUpscaleDone(at + takes);
        CHECK(!a.Recording());
    }
    return verdict;
}

static void OwnerHandBack()
{
    using namespace DlssNr::VkModelOwner;
    constexpr uint64_t d1 = 0xD1, d2 = 0xD2;
    static_assert(kAfterUpscaleIdleMs > kAfterUpscaleLiveMs * 10, "a pause of the game's upscaler must not flap");

    // The present host on the start window, then the game creates its DLSS feature: stand down, the route takes it.
    Arbiter a;
    CHECK(a.PresentHost(d1, 1000).granted);
    a.Take(d1);

    uint64_t now = 2000;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        // DLSS on: the route asks; the present host yields, releases once drained, and the route takes the model.
        Verdict v = Evaluate(a, d1, now);
        CHECK(!v.granted && v.reason == kHeldByPresentHost && !a.Recording());
        CHECK(a.PresentHostMustYield(d1, now + 1));
        CHECK(a.IdleAfterUpscaleDevice(now + 1) == 0);
        a.Release(Who::PresentHost, d1);
        CHECK(Evaluate(a, d1, now + 16).granted && a.Owner() == Who::AfterUpscale && a.Device() == d1);

        // It evaluates for a minute, every frame; never idle, nothing to hand back.
        uint64_t last = now + 16;
        for (uint64_t frame = last; frame < now + 60000; frame += 16)
        {
            CHECK(Evaluate(a, d1, frame).granted);
            CHECK(!a.BeginHandBack(d1, frame + 8));
            last = frame + 2; // its recording's end
        }

        // DLSS off: the route stops asking. Not idle until kAfterUpscaleIdleMs has passed since its last recording.
        v = a.PresentHost(d1, last + kAfterUpscaleIdleMs - 1);
        CHECK(!v.granted && v.reason == kAfterUpscaleOwns);
        CHECK(a.IdleAfterUpscaleDevice(last + kAfterUpscaleIdleMs - 1) == 0);
        CHECK(!a.BeginHandBack(d1, last + kAfterUpscaleIdleMs - 1));

        const uint64_t idle = last + kAfterUpscaleIdleMs;
        CHECK(a.IdleAfterUpscaleDevice(idle) == d1);
        CHECK(!a.BeginHandBack(d2, idle)); // not the route's device: nothing to drain there
        CHECK(a.BeginHandBack(d1, idle) && a.Retiring());

        // While its queues drain: refused for the present host with its own reason, not idle again, not begun twice.
        v = a.PresentHost(d1, idle + 1);
        CHECK(!v.granted && v.reason == kAfterUpscaleRetiring);
        CHECK(a.IdleAfterUpscaleDevice(idle + 1) == 0 && !a.BeginHandBack(d1, idle + 1));
        CHECK(a.Owner() == Who::AfterUpscale);

        // Drained: the token is released, and the present host takes the model back on the route's device. Finishing
        // another device's drain, or one never begun, changes nothing.
        CHECK(a.FinishHandBack(d2) == HandBack::NotRetiring && a.Retiring());
        CHECK(a.FinishHandBack(d1) == HandBack::Done);
        CHECK(a.Owner() == Who::None && a.Device() == 0 && !a.Retiring());
        CHECK(a.FinishHandBack(d1) == HandBack::NotRetiring);
        CHECK(a.PresentHost(d1, idle + 2).granted);
        a.Take(d1);
        CHECK(!a.PresentHostMustYield(d1, idle + 3));

        now = idle + 5000; // NR at present for five seconds, then DLSS on again
    }

    // The quiet counts from the end of the route's last recording, not from its ask: an evaluate that builds a cold
    // feature for longer than the window is never handed back under it, nor right after it.
    a.Release(Who::PresentHost, d1);
    CHECK(a.AfterUpscale(d1, 400000).granted && a.Recording());
    CHECK(a.IdleAfterUpscaleDevice(400000 + 3 * kAfterUpscaleIdleMs) == 0);
    CHECK(!a.BeginHandBack(d1, 400000 + 3 * kAfterUpscaleIdleMs));
    a.AfterUpscaleDone(400000 + 3 * kAfterUpscaleIdleMs);
    CHECK(a.IdleAfterUpscaleDevice(400000 + 4 * kAfterUpscaleIdleMs - 1) == 0);
    CHECK(a.IdleAfterUpscaleDevice(400000 + 4 * kAfterUpscaleIdleMs) == d1);
    a.AfterUpscaleDone(400000 + 5 * kAfterUpscaleIdleMs); // not recording: nothing moves
    CHECK(a.IdleAfterUpscaleDevice(400000 + 4 * kAfterUpscaleIdleMs) == d1);

    // The route asking again while its queues drain calls the hand-back off: it keeps the model and everything of its,
    // and the next idle window starts from that recording.
    CHECK(Evaluate(a, d1, 500000).granted);
    CHECK(a.BeginHandBack(d1, 500002 + kAfterUpscaleIdleMs));
    CHECK(Evaluate(a, d1, 500002 + kAfterUpscaleIdleMs + 3).granted && a.Owner() == Who::AfterUpscale);
    CHECK(a.FinishHandBack(d1) == HandBack::Cancelled);
    CHECK(a.Owner() == Who::AfterUpscale && a.Device() == d1 && !a.Retiring());
    CHECK(a.IdleAfterUpscaleDevice(500002 + 2 * kAfterUpscaleIdleMs + 4) == 0);
    CHECK(a.IdleAfterUpscaleDevice(500002 + 2 * kAfterUpscaleIdleMs + 5) == d1);
    // A route that asks while its queues drain and goes quiet again: still called off, nothing freed.
    const uint64_t again = 500002 + 2 * kAfterUpscaleIdleMs + 5;
    CHECK(a.BeginHandBack(d1, again));
    CHECK(Evaluate(a, d1, again + 1).granted);
    CHECK(a.FinishHandBack(d1) == HandBack::Cancelled && a.Owner() == Who::AfterUpscale);

    // A drain that cannot finish (a submit failed, the device lost): off, the route keeps the model. Calling off
    // another device's drain changes nothing.
    CHECK(a.BeginHandBack(d1, 600000));
    a.CancelHandBack(d2);
    CHECK(a.Retiring());
    a.CancelHandBack(d1);
    CHECK(!a.Retiring() && a.Owner() == Who::AfterUpscale && a.FinishHandBack(d1) == HandBack::NotRetiring);
    CHECK(a.PresentHost(d1, 600001).reason == kAfterUpscaleOwns);

    // Loading screens and menus: a route that pauses for less than the idle window, again and again, never loses it.
    Arbiter b;
    uint64_t t = 1000;
    CHECK(Evaluate(b, d1, t, 0).granted);
    for (int pause = 0; pause < 20; ++pause)
    {
        t += kAfterUpscaleIdleMs - 1;
        CHECK(b.IdleAfterUpscaleDevice(t) == 0 && !b.BeginHandBack(d1, t));
        CHECK(Evaluate(b, d1, t, 0).granted);
    }
    CHECK(b.Owner() == Who::AfterUpscale);

    // A clock read out of order never makes the route idle.
    CHECK(b.IdleAfterUpscaleDevice(t - 1) == 0);

    // The route's own device change moves the token: the present host on the new device sees it idle there.
    CHECK(Evaluate(b, d2, t + 1, 0).granted);
    CHECK(b.IdleAfterUpscaleDevice(t + 1 + kAfterUpscaleIdleMs) == d2);
    CHECK(!b.BeginHandBack(d1, t + 1 + kAfterUpscaleIdleMs));

    // The device it held the model on destroyed: the token goes back, so a route that never asks again does not hold
    // the model for a device that is gone. Another device's destruction changes nothing; nor does one with nobody.
    b.DeviceGone(d1);
    CHECK(b.Owner() == Who::AfterUpscale);
    b.DeviceGone(d2);
    CHECK(b.Owner() == Who::None && b.Device() == 0);
    CHECK(b.PresentHost(d1, t + 1 + kAfterUpscaleIdleMs).granted);
    b.DeviceGone(d2);
    CHECK(b.Owner() == Who::None);

    // ... during a hand-back too, and for the present host's own token.
    Arbiter c;
    CHECK(Evaluate(c, d1, 0, 0).granted);
    CHECK(c.BeginHandBack(d1, kAfterUpscaleIdleMs) && c.Retiring());
    c.DeviceGone(d1);
    CHECK(c.Owner() == Who::None && !c.Retiring() && c.FinishHandBack(d1) == HandBack::NotRetiring);
    CHECK(c.PresentHost(d2, kAfterUpscaleIdleMs + 1).granted);
    c.Take(d2);
    c.DeviceGone(d2);
    CHECK(c.Owner() == Who::None);

    // Frames slower than the live window (a loaded machine, a game at a few frames a second): the route refused at T,
    // the host standing down at a present 800 ms later, must not take the model back before the route's next ask --
    // that flapped in a harness run under load (stood down, took it again, stood down) -- and the route then takes it.
    Arbiter s;
    CHECK(s.PresentHost(d1, 0).granted);
    s.Take(d1);
    CHECK(!Evaluate(s, d1, 1000).granted && s.PresentHostMustYield(d1, 1000));
    CHECK(s.YieldReason(1000) == kAfterUpscaleWants);
    CHECK(s.PresentHostMustYield(d1, 1000 + kAfterUpscaleLiveMs + 300)); // still: it wants the model
    CHECK(s.YieldReason(1000 + kAfterUpscaleLiveMs + 300) == kAfterUpscaleWaiting);
    s.Release(Who::PresentHost, d1);
    Verdict w = s.PresentHost(d1, 1000 + kAfterUpscaleLiveMs + 300);
    CHECK(!w.granted && w.reason == kAfterUpscaleWaiting);
    CHECK(Evaluate(s, d1, 2600).granted && s.Owner() == Who::AfterUpscale);
    w = s.PresentHost(d1, 2600 + kAfterUpscaleLiveMs + 1);
    CHECK(!w.granted && w.reason == kAfterUpscaleOwns);
    // Granted, the route no longer waits: its device gone, the host on another device takes the model once the live
    // window is past, not ten seconds later.
    s.DeviceGone(d1);
    CHECK(s.PresentHost(d2, 2602 + kAfterUpscaleLiveMs).granted);
    // A route refused once that never asks again keeps the host down for the idle window, not for good; the host then
    // takes the model and the reservation is gone.
    Arbiter r;
    CHECK(r.PresentHost(d1, 0).granted);
    r.Take(d1);
    CHECK(!Evaluate(r, d1, 100).granted);
    r.Release(Who::PresentHost, d1);
    CHECK(r.PresentHost(d1, 100 + kAfterUpscaleIdleMs - 1).reason == kAfterUpscaleWaiting);
    CHECK(r.PresentHost(d1, 99).reason == kAfterUpscaleWaiting); // a clock read out of order: still waiting
    CHECK(r.PresentHost(d1, 100 + kAfterUpscaleIdleMs).granted);
    r.Take(d1);
    CHECK(!r.PresentHostMustYield(d1, 100 + kAfterUpscaleIdleMs + 1));

    // A drain under way on d1 when the route moves to d2 (the review, 2026-10-14): the move calls it off, and d1 then
    // destroyed or lost before its drain is seen through ends the hand-back -- it must not stay open for the session,
    // or the present host would be refused (kAfterUpscaleRetiring) and never see the route idle again.
    for (int ending = 0; ending < 3; ++ending)
    {
        Arbiter e;
        CHECK(Evaluate(e, d1, 0, 0).granted);
        CHECK(e.BeginHandBack(d1, kAfterUpscaleIdleMs));
        CHECK(Evaluate(e, d2, kAfterUpscaleIdleMs + 1, 0).granted && e.Device() == d2);
        if (ending == 0)
            e.DeviceGone(d1); // destroyed
        else if (ending == 1)
            e.CancelHandBack(d1); // lost, or its drain failed
        else
            CHECK(e.FinishHandBack(d1) == HandBack::Cancelled); // drained: the route moved, nothing of d1's freed
        CHECK(!e.Retiring() && e.Owner() == Who::AfterUpscale && e.Device() == d2);
        CHECK(e.IdleAfterUpscaleDevice(2 * kAfterUpscaleIdleMs + 1) == d2);
        CHECK(e.BeginHandBack(d2, 2 * kAfterUpscaleIdleMs + 1));
        CHECK(e.FinishHandBack(d2) == HandBack::Done && e.Owner() == Who::None);
    }
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

// Off the present path (DOOM Eternal's startup hang, 2026-10-14): the model's build state machine. Nothing is built on
// a frame; a job is requested, waited for, adopted, and only then used; a failure latches; a resize churn while a job
// runs costs at most one extra build.
static void BuildDecisions()
{
    const BuildKey a { 841, 671, 841, 671, 0 }, b { 3440, 1440, 3440, 1440, 0 }, bOther { 3440, 1440, 3440, 1440, 2 };
    const BuildKey bHalf { 3440, 1440, 1720, 720, 0 };

    // Cold: nothing built, no job -> request; while queued or running -> wait (the frame carried); done -> adopt.
    CHECK(DecideBuild(false, BuildState::None, false, {}, a) == BuildStep::Request);
    CHECK(DecideBuild(false, BuildState::Queued, false, {}, a) == BuildStep::Wait);
    CHECK(DecideBuild(false, BuildState::Running, false, {}, a) == BuildStep::Wait);
    CHECK(DecideBuild(false, BuildState::Done, false, {}, a) == BuildStep::Adopt);
    CHECK(DecideBuild(false, BuildState::Failed, false, {}, a) == BuildStep::Adopt);
    // Adopted: used while the key holds; a new size, working scale or family is a new build.
    CHECK(DecideBuild(false, BuildState::None, true, a, a) == BuildStep::Use);
    CHECK(DecideBuild(false, BuildState::None, true, a, b) == BuildStep::Request);
    CHECK(DecideBuild(false, BuildState::None, true, b, bHalf) == BuildStep::Request);
    CHECK(DecideBuild(false, BuildState::None, true, b, bOther) == BuildStep::Request);
    // A latched failure ends it, whatever else.
    CHECK(DecideBuild(true, BuildState::None, true, a, a) == BuildStep::Off);
    CHECK(DecideBuild(true, BuildState::Running, false, {}, a) == BuildStep::Off);

    // The DOOM churn, driven frame by frame: 841x671, then #9 at 3440x1440 for one present, then #11 at 3440x1440,
    // the builder slow. Requests: one per key the frames ask for while nothing runs.
    struct Model
    {
        bool built = false;
        BuildKey key;
        BuildState job = BuildState::None;
        BuildKey jobKey;
        int requests = 0, used = 0, carried = 0;
    } m;
    const auto frame = [&m](const BuildKey& want)
    {
        for (;;)
        {
            switch (DecideBuild(false, m.job, m.built, m.key, want))
            {
            case BuildStep::Use:
                ++m.used;
                return;
            case BuildStep::Adopt:
                m.built = true;
                m.key = m.jobKey;
                m.job = BuildState::None;
                continue;
            case BuildStep::Request:
                ++m.requests;
                m.built = false;
                m.job = BuildState::Queued;
                m.jobKey = want;
                ++m.carried;
                return;
            default:
                ++m.carried;
                return;
            }
        }
    };
    frame(a);                    // request a
    m.job = BuildState::Running; // the builder takes it
    frame(a);                    // carried
    m.job = BuildState::Done;
    for (int i = 0; i < 183; ++i)
        frame(a); // adopted at the first, then used
    frame(b);     // #9's one present: a new build
    m.job = BuildState::Running;
    frame(b); // #11: carried, the build for b is running
    frame(b);
    m.job = BuildState::Done;
    frame(b); // adopted, used
    CHECK(m.requests == 2 && m.used == 184 && m.carried == 5);

    // A resize while a job runs: the job ends, is adopted for the old key, and the next frame asks again.
    m = {};
    frame(a);
    m.job = BuildState::Running;
    frame(b);
    CHECK(m.requests == 1);
    m.job = BuildState::Done;
    frame(b); // adopts a, then requests b
    CHECK(m.requests == 2 && m.key == a && m.jobKey == b && m.job == BuildState::Queued);

    CHECK(std::string(BuildStateName(BuildState::Running)) == "running");
    CHECK(std::string(NrSkipName(NrSkip::Building)) == "model building");
}

// The stall watchdog's table: lines after 2 s (5 s for the build), once per occupancy then every 10 s, with the stage,
// the object, the locks held and, for a lock waited on, its holder and what the holder is doing.
static void StallWatch()
{
    ActivityTable table;
    SetThreadTag(1652);
    const size_t present = table.Enter("vkQueuePresentKHR", 0x51, 1000);
    table.Hold(present, kLockPresent, true);
    SetThreadTag(1700);
    const size_t destroy = table.Enter("vkDestroySwapchainKHR", 0x5C9, 900);
    table.Hold(destroy, kLockTransport, true);
    table.Stage(destroy, "retiring the swapchain's slots (bounded drain)");
    table.Stage(present, "waiting for the transport lock", kLockTransport, 1700);
    SetThreadTag(1800);
    const size_t build = table.Enter("model build", 0xD1, 1000, kBuildStallMs);
    table.Stage(build, "creating the feature (NGX)");

    CHECK(table.Check(2899).empty()); // nobody has been in for its allowance yet
    std::vector<std::string> lines = table.Check(3000);
    CHECK(lines.size() == 2); // the destroy (2.1 s) and the present (2.0 s); the build has 5 s
    std::string presentLine;
    for (const std::string& line : lines)
    {
        if (line.find("thread 1652") == 0)
            presentLine = line;
    }
    CHECK(presentLine == "thread 1652 in vkQueuePresentKHR (0x51) for 2.0 s: waiting for the transport lock (the "
                         "transport lock is held by thread 1700, in vkDestroySwapchainKHR for 2.1 s: retiring the "
                         "swapchain's slots (bounded drain)); holds the present lock");
    CHECK(table.Check(3500).empty()); // once per occupancy ...
    CHECK(table.Check(5999).empty());
    // ... the build's allowance is 5 s; its line names it holding no host lock ...
    lines = table.Check(6000);
    CHECK(lines.size() == 1 &&
          lines[0] == "thread 1800 in model build (0xD1) for 5.0 s: creating the feature (NGX); holds no host lock");
    CHECK(table.Check(12999).empty());     // ... and each is said again 10 s after its last line
    CHECK(table.Check(13000).size() == 2); // the present and the destroy
    CHECK(table.Check(13001).empty());
    table.Leave(present);
    table.Leave(destroy);
    CHECK(table.Check(15999).empty() && table.Check(16000).size() == 1); // the build

    // A slot freed and taken again is a new occupancy, reported on its own clock.
    table.Leave(build);
    SetThreadTag(1652);
    const size_t again = table.Enter("vkQueuePresentKHR", 0x51, 20000);
    CHECK(again == present || again == destroy || again == build);
    CHECK(table.Check(21999).empty() && table.Check(22000).size() == 1);
    table.Leave(again);
    CHECK(table.Check(40000).empty());

    // A holder outside every entry point, and a full table: nothing waits, the activity goes unrecorded.
    const size_t lonely = table.Enter("vkQueueSubmit", 0x51, 0);
    table.Stage(lonely, "waiting for the queue's submit lock", kLockSubmit, 4242);
    lines = table.Check(2500);
    CHECK(lines.size() == 1 && lines[0].find("(the submit lock is held by thread 4242, outside the host's entry "
                                             "points)") != std::string::npos);
    table.Leave(lonely);
    std::vector<size_t> slots;
    for (size_t i = 0; i < kActivitySlots; ++i)
        slots.push_back(table.Enter("vkQueueSubmit", 0, 0));
    CHECK(table.Enter("vkQueueSubmit", 0, 0) == kActivitySlots);
    table.Stage(kActivitySlots, "ignored");
    table.Hold(kActivitySlots, kLockSubmit, true);
    table.Leave(kActivitySlots);
    for (size_t slot : slots)
        table.Leave(slot);

    CHECK(HostLockNames(kLockPresent | kLockTransport) == "the present lock, the transport lock");
    CHECK(HostLockNames(kLockNone).empty());

    // The tracked lock: its holder while held, through recursion, nobody once given back; a waiter reads it.
    TrackedMutex<std::recursive_timed_mutex> lock;
    SetThreadTag(77);
    CHECK(lock.Owner() == 0);
    lock.lock();
    CHECK(lock.Owner() == 77);
    CHECK(lock.try_lock() && lock.Owner() == 77);
    lock.unlock();
    CHECK(lock.Owner() == 77);
    uint32_t seen = 0;
    std::thread other(
        [&]
        {
            SetThreadTag(78);
            seen = lock.Owner();
            CHECK(!lock.try_lock_for(std::chrono::milliseconds(1)));
        });
    other.join();
    CHECK(seen == 77);
    lock.unlock();
    CHECK(lock.Owner() == 0);
    SetThreadTag(0);
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
    SynthMotionDecisions();
    OwnerArbitration();
    OwnerHandBack();
    Heartbeat();
    HandBackDiagnosticsAndLocks();
    BuildDecisions();
    StallWatch();
    NrLines();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("PASS: registry state machine (records, reuse, many devices, unknowns, internal, lines), the round "
              "trip's decisions (usage plan, route, ring, counters, transport lines) and cold NR's (colour plan, "
              "device extensions, counters, one owner and the way back to the present host, NR lines), the model's build off "
              "the present path, synthesized motion's decisions and the stall watchdog's table");
    return 0;
}
