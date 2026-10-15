#pragma once

// The decisions of the Vulkan present host's transport (docs/dlssnr/design/nr-vulkan-present-host.md, stage 2: the
// submission-only control). Which swapchains may be read and written at present, on which queue the round trip runs,
// which ring slot a present uses, and the counters the log reports. DlssNr_VkPresentHost.cpp makes the Vulkan calls;
// every choice it acts on is made here, from plain integers, so tests/nr-vk-present-host drives all of it on the host.
//
// The round trip, per transported present: wait on everything the application's present was going to wait on, copy the
// presented image into a work image the host owns and back again, signal a semaphore of the host's, and present
// waiting on that instead. Stage 3 (DlssNr_VkPresentNr.h) runs the model between the two copies, on the same queue;
// a frame it does not run on goes back unchanged.
//
// Standard library only, and Vulkan's values are plain integers; tests/nr-vk-present-host checks each constant
// repeated here against vulkan_core.h.

#include "DlssNr_VkPresentNr.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace DlssNr::VkPresentHost
{
using Handle = uint64_t; // a Vulkan handle's value (dispatchable ones are pointers)

inline constexpr uint32_t kQueueGraphicsBit = 0x1;
inline constexpr uint32_t kQueueComputeBit = 0x2;
inline constexpr uint32_t kQueueTransferBit = 0x4;
inline constexpr uint32_t kQueueProtectedBit = 0x10;
inline constexpr uint32_t kUsageTransferSrcBit = 0x1;
inline constexpr uint32_t kUsageTransferDstBit = 0x2;
inline constexpr uint32_t kUsageStorageBit = 0x8;
inline constexpr uint32_t kSharingModeConcurrent = 1;
inline constexpr uint32_t kPresentModeSharedDemandRefresh = 1000111000;
inline constexpr uint32_t kPresentModeSharedContinuousRefresh = 1000111001;
inline constexpr uint32_t kSwapchainCreateProtectedBit = 0x2;

// What the queue the round trip runs on has to be able to do. Stage 3 evaluates the model on that same queue, and the
// model has only ever run on a graphics queue here (the after-upscale route records into the game's graphics command
// buffer), so a presenting family without graphics hands the image to a graphics family and takes it back, as the
// Vulkan overlay does for id Tech 7. A compute-only presenter is a candidate for the direct route once NGX is proven
// on a compute queue; that is this one constant.
inline constexpr uint32_t kWorkQueueNeeds = kQueueGraphicsBit | kQueueComputeBit;

inline constexpr uint32_t kRingMax = 4;            // ring slots per swapchain: frames of the round trip in flight
inline constexpr uint32_t kMaxWaits = 16;          // application wait semaphores one present may hand over
inline constexpr uint64_t kSlotWaitNs = 100000000; // the longest a present waits for the oldest slot: 100 ms
inline constexpr uint64_t kQueueLockWaitMs = 50;   // the longest a present waits to borrow another queue
inline constexpr uint64_t kDrainNs = 2000000000;   // the longest a retirement waits for the GPU: 2 s
inline constexpr size_t kRetainedMax = 4096;       // present semaphores kept because nothing proved them free

struct QueueFamily
{
    uint32_t flags = 0; // VkQueueFlags
    uint32_t count = 0; // queueCount
};

// --- At swapchain creation ---------------------------------------------------------------------------------------

// What the caller already knows when it asks.
struct UsageQuery
{
    uint32_t asked = 0; // VkSwapchainCreateInfoKHR::imageUsage as the application made it
    uint32_t arrayLayers = 1;
    uint32_t presentMode = 0;
    uint32_t flags = 0;    // VkSwapchainCreateFlagsKHR
    bool deviceOn = false; // the device has a transport (not OptiScaler's own, its functions resolved)
    std::string deviceOff; // why not, when !deviceOn
    bool surfaceKnown = false;
    uint32_t surfaceUsage = 0;
    bool formatAllows = false; // vkGetPhysicalDeviceImageFormatProperties accepted Wanted() for the swapchain format
    bool workAllows = false;   // ... and TRANSFER_SRC|TRANSFER_DST for the work image in that format
};

struct UsagePlan
{
    bool transport = false; // the round trip may run on this swapchain
    uint32_t usage = 0;     // what the driver is given
    uint32_t added = 0;     // usage bits the host added
    std::string note;       // "TRANSFER_SRC added at creation", "as created", or why it stays observe-only
};

// The usage the round trip needs: the application's plus both transfer usages.
inline uint32_t Wanted(uint32_t asked) { return asked | kUsageTransferSrcBit | kUsageTransferDstBit; }

inline std::string UsageNames(uint32_t bits)
{
    std::string names;
    if (bits & kUsageTransferSrcBit)
        names = "TRANSFER_SRC";
    if (bits & kUsageTransferDstBit)
        names += names.empty() ? "TRANSFER_DST" : "+TRANSFER_DST";
    return names;
}

// Decided before the real vkCreateSwapchainKHR, never by retrying one: a swapchain created with an oldSwapchain retires
// it even when the creation fails, and a retired swapchain may not be passed again.
inline UsagePlan PlanUsage(const UsageQuery& q)
{
    UsagePlan plan;
    plan.usage = q.asked;
    const uint32_t missing = Wanted(q.asked) & ~q.asked;

    const auto off = [&plan](std::string why)
    {
        plan.note = std::move(why);
        return plan;
    };

    if (!q.deviceOn)
        return off(q.deviceOff.empty() ? std::string("the device has no transport") : q.deviceOff);
    if (q.flags & kSwapchainCreateProtectedBit)
        return off("a protected swapchain");
    if (q.presentMode == kPresentModeSharedDemandRefresh || q.presentMode == kPresentModeSharedContinuousRefresh)
        return off("a shared present mode, whose image is never in PRESENT_SRC");
    if (q.arrayLayers != 1)
        return off(std::to_string(q.arrayLayers) + " array layers");
    if (missing != 0 && !q.surfaceKnown)
        return off("the surface's usages could not be read, so " + UsageNames(missing) + " cannot be asked for");
    if (missing != 0 && (q.surfaceUsage & missing) != missing)
        return off("the surface does not offer " + UsageNames(missing & ~q.surfaceUsage));
    if (!q.formatAllows)
        return off("the format does not allow " + UsageNames(Wanted(0)) + " on swapchain images");
    if (!q.workAllows)
        return off("the work image cannot be made in the swapchain's format");

    plan.transport = true;
    plan.usage = Wanted(q.asked);
    plan.added = missing;
    plan.note = missing != 0 ? UsageNames(missing) + " added at creation" : std::string("as created");
    return plan;
}

// --- At present: the route ---------------------------------------------------------------------------------------

enum class Route : uint8_t
{
    None,
    SameQueue,   // the presenting queue does the round trip itself
    CrossFamily, // EXCLUSIVE image, presenting family without graphics: release, round trip on a graphics queue, return
    Concurrent,  // CONCURRENT image listing a graphics family: round trip there, the present waits across queues
    Count,
};

inline const char* RouteName(Route route)
{
    switch (route)
    {
    case Route::SameQueue:
        return "same queue";
    case Route::CrossFamily:
        return "cross-family";
    case Route::Concurrent:
        return "concurrent";
    default:
        return "none";
    }
}

struct QueueCandidate // a queue the application retrieved from the device, as the records hold it
{
    Handle queue = 0;
    uint32_t family = 0;
    uint32_t index = 0;
    uint32_t createFlags = 0;
};

struct RouteQuery
{
    Handle presentQueue = 0;
    uint32_t presentFamily = 0;
    std::vector<QueueFamily> families; // the device's, by index
    uint32_t sharingMode = 0;
    std::vector<uint32_t> sharingFamilies; // CONCURRENT only
    std::vector<QueueCandidate> queues;    // every queue of the device the application retrieved
};

struct RoutePlan
{
    Route route = Route::None;
    Handle workQueue = 0;
    uint32_t workFamily = 0;
    std::string why; // when None
};

inline bool Listed(const std::vector<uint32_t>& families, uint32_t family)
{
    for (const uint32_t f : families)
    {
        if (f == family)
            return true;
    }
    return false;
}

// Only queues the application retrieved: one it created but never asked for is never fetched here, and a queue
// created with flags (protected) is never borrowed.
inline RoutePlan ChooseRoute(const RouteQuery& q)
{
    RoutePlan plan;

    if (q.presentFamily >= q.families.size())
    {
        plan.why = "the presenting queue's family is unknown";
        return plan;
    }

    const uint32_t flags = q.families[q.presentFamily].flags;
    const bool concurrent = q.sharingMode == kSharingModeConcurrent;

    if ((flags & kWorkQueueNeeds) == kWorkQueueNeeds)
    {
        if (concurrent && !Listed(q.sharingFamilies, q.presentFamily))
        {
            plan.why = "a CONCURRENT swapchain that does not list the presenting family";
            return plan;
        }

        plan.route = Route::SameQueue;
        plan.workQueue = q.presentQueue;
        plan.workFamily = q.presentFamily;
        return plan;
    }

    const QueueCandidate* best = nullptr;
    for (const QueueCandidate& c : q.queues)
    {
        if (c.createFlags != 0 || c.family >= q.families.size() ||
            (q.families[c.family].flags & kWorkQueueNeeds) != kWorkQueueNeeds)
            continue;
        if (concurrent && !Listed(q.sharingFamilies, c.family))
            continue;
        if (best == nullptr || c.family < best->family || (c.family == best->family && c.index < best->index))
            best = &c;
    }

    if (best == nullptr)
    {
        plan.why = concurrent ? "no retrieved graphics queue of a family the CONCURRENT swapchain lists"
                              : "the presenting family has no graphics and the application retrieved no graphics queue";
        return plan;
    }

    if (concurrent)
    {
        plan.route = Route::Concurrent;
    }
    else
    {
        // The release and the acquire back are pipeline barriers recorded on the presenting family.
        if ((flags & (kQueueGraphicsBit | kQueueComputeBit | kQueueTransferBit)) == 0)
        {
            plan.why = "a present-only family cannot release the image to another family";
            return plan;
        }

        plan.route = Route::CrossFamily;
    }

    plan.workQueue = best->queue;
    plan.workFamily = best->family;
    return plan;
}

inline uint32_t SubmitsFor(Route route) { return route == Route::CrossFamily ? 3 : (route == Route::None ? 0 : 1); }

// --- At present: the ring ----------------------------------------------------------------------------------------

enum class SlotState : uint8_t
{
    Idle,      // never submitted, or its completion was observed
    Pending,   // submitted, completion not yet observed
    Abandoned, // its completion can never be observed (a chain cut short, a lost device): never reused or freed
};

struct SlotChoice
{
    enum Kind : uint8_t
    {
        Use,        // slot `index` is idle
        Grow,       // add a slot (it becomes `index`)
        WaitOldest, // every slot is pending and the ring is full: wait for slot `index`, the oldest
        Full,       // nothing usable at all (every slot abandoned)
    } kind = Full;
    size_t index = 0;
};

// `serials` orders submissions: the smallest pending serial is the oldest. A slot whose fence is known signalled has
// already been moved to Idle by the caller's poll.
inline SlotChoice ChooseSlot(const std::vector<SlotState>& states, const std::vector<uint64_t>& serials, uint32_t max)
{
    SlotChoice choice;

    for (size_t i = 0; i < states.size(); ++i)
    {
        if (states[i] == SlotState::Idle)
        {
            choice.kind = SlotChoice::Use;
            choice.index = i;
            return choice;
        }
    }

    if (states.size() < max)
    {
        choice.kind = SlotChoice::Grow;
        choice.index = states.size();
        return choice;
    }

    bool found = false;
    for (size_t i = 0; i < states.size(); ++i)
    {
        if (states[i] == SlotState::Pending && (!found || serials[i] < serials[choice.index]))
        {
            choice.index = i;
            found = true;
        }
    }

    choice.kind = found ? SlotChoice::WaitOldest : SlotChoice::Full;
    return choice;
}

// --- Outcomes and counters ---------------------------------------------------------------------------------------

// Why a present went down without the round trip. Every one of them is decided before an application wait semaphore
// is consumed, except SubmitFailed, which is only reported when the first submit failed and so consumed nothing.
enum class Skip : uint8_t
{
    None,
    ObserveOnly,    // the swapchain is not eligible (its creation line says why)
    DeviceOff,      // the device's transport stopped (a lost device, a failed drain, too many retained semaphores)
    MultiSwapchain, // one present for several swapchains: the waits belong to the whole call
    ImageUnknown,   // an index past the images the application fetched, or no images fetched
    NoAcquire,      // no acquire of the image observed since its last present
    TooManyWaits,   // more than kMaxWaits semaphores
    NoRoute,        // no legal queue for the image (the window line says the first reason)
    QueueChange,    // a different work queue while the previous one's round trips are still in flight
    RingBusy,       // every slot pending after kSlotWaitNs
    QueueBusy,      // the graphics queue to borrow stayed locked by another thread for kQueueLockWaitMs
    SetupFailed,    // a work image, pool, buffer, fence or semaphore could not be made
    RecordFailed,   // a command buffer could not be recorded
    SubmitFailed,   // the first submit failed without consuming anything
    Count,
};

inline const char* SkipName(Skip skip)
{
    switch (skip)
    {
    case Skip::ObserveOnly:
        return "observe only";
    case Skip::DeviceOff:
        return "device off";
    case Skip::MultiSwapchain:
        return "multi-swapchain present";
    case Skip::ImageUnknown:
        return "image unknown";
    case Skip::NoAcquire:
        return "no acquire seen";
    case Skip::TooManyWaits:
        return "too many waits";
    case Skip::NoRoute:
        return "no route";
    case Skip::QueueChange:
        return "work queue changed";
    case Skip::RingBusy:
        return "ring busy";
    case Skip::QueueBusy:
        return "queue busy";
    case Skip::SetupFailed:
        return "setup failed";
    case Skip::RecordFailed:
        return "record failed";
    case Skip::SubmitFailed:
        return "submit failed";
    default:
        return "none";
    }
}

// One present's result for one swapchain.
struct Outcome
{
    bool counted = false; // false: nothing to count (OptiScaler's own device, objects the host never saw)
    bool transported = false;
    Skip skip = Skip::None;
    Route route = Route::None;
    uint32_t waitsTaken = 0;
    uint32_t submits = 0;
    uint32_t completions = 0; // GPU completions of earlier round trips observed during this call
    uint32_t ring = 0;        // the swapchain's ring depth after this call
    uint64_t waitNs = 0;      // CPU time this call spent waiting on a ring fence
    std::string note;         // NoRoute: RoutePlan::why

    // Stage 3: what became of the model on a transported present.
    bool nrEvaluated = false;     // the pass was recorded and submitted with the round trip
    NrSkip nrSkip = NrSkip::None; // transported and not shown to the model: why
    std::string nrNote;           // the refusal's full text
    bool nrReset = false;         // an evaluated frame that started the model's history over
    uint32_t nrTimed = 0;         // pass GPU times read back during this call, from earlier frames seen complete
    double nrLastGpuMs = 0.0;
    double nrMaxGpuMs = 0.0;
    double nrSumGpuMs = 0.0;
};

struct TransportStats
{
    uint64_t presents = 0;
    uint64_t transported = 0;
    uint64_t skipped = 0;
    uint64_t waitsTaken = 0;
    uint64_t submits = 0;
    uint64_t completions = 0;
    std::array<uint64_t, static_cast<size_t>(Skip::Count)> skips {};
    std::array<uint64_t, static_cast<size_t>(Route::Count)> routes {};
    uint32_t ring = 0;
    uint64_t maxWaitNs = 0;
    NrStats nr;

    // Pass GPU times read back outside a present (a swapchain's last frames, seen complete at its retirement).
    void AddGpuTimes(uint32_t timed, double last, double max, double sum)
    {
        if (timed == 0)
            return;

        nr.timed += timed;
        nr.lastGpuMs = last;
        nr.maxGpuMs = max > nr.maxGpuMs ? max : nr.maxGpuMs;
        nr.sumGpuMs += sum;
    }

    void Add(const Outcome& o)
    {
        if (!o.counted)
            return;

        AddGpuTimes(o.nrTimed, o.nrLastGpuMs, o.nrMaxGpuMs, o.nrSumGpuMs);

        ++presents;
        waitsTaken += o.waitsTaken;
        submits += o.submits;
        completions += o.completions;
        ring = o.ring > ring ? o.ring : ring;
        maxWaitNs = o.waitNs > maxWaitNs ? o.waitNs : maxWaitNs;

        if (o.transported)
        {
            ++transported;
            ++routes[static_cast<size_t>(o.route)];

            if (o.nrEvaluated)
            {
                ++nr.evaluated;
                nr.resets += o.nrReset ? 1 : 0;
            }
            else if (o.nrSkip != NrSkip::None)
            {
                ++nr.refused;
                ++nr.skips[static_cast<size_t>(o.nrSkip)];
                if (nr.firstRefusal.empty())
                    nr.firstRefusal = o.nrNote.empty() ? std::string(NrSkipName(o.nrSkip)) : o.nrNote;
            }
        }
        else
        {
            ++skipped;
            ++skips[static_cast<size_t>(o.skip)];
        }
    }
};

inline std::string Milliseconds(uint64_t ns)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(ns) / 1e6);
    return text;
}

// "transported 12, skipped 2 (ring busy 2), waits taken over 12, submits 12, completions 11, ring 1, max CPU wait
// 0.000 ms, routes same queue 12"
inline std::string TransportText(const TransportStats& s)
{
    const auto n = [](uint64_t value) { return std::to_string(value); };

    std::string text = "transported " + n(s.transported) + ", skipped " + n(s.skipped);

    if (s.skipped != 0)
    {
        std::string reasons;
        for (size_t i = 1; i < s.skips.size(); ++i)
        {
            if (s.skips[i] != 0)
                reasons +=
                    (reasons.empty() ? "" : ", ") + std::string(SkipName(static_cast<Skip>(i))) + " " + n(s.skips[i]);
        }
        text += " (" + reasons + ")";
    }

    text += ", waits taken over " + n(s.waitsTaken) + ", submits " + n(s.submits) + ", completions " +
            n(s.completions) + ", ring " + n(s.ring) + ", max CPU wait " + Milliseconds(s.maxWaitNs) + " ms";

    std::string routes;
    for (size_t i = 1; i < s.routes.size(); ++i)
    {
        if (s.routes[i] != 0)
            routes +=
                (routes.empty() ? "" : ", ") + std::string(RouteName(static_cast<Route>(i))) + " " + n(s.routes[i]);
    }
    if (!routes.empty())
        text += ", routes " + routes;

    return text;
}
} // namespace DlssNr::VkPresentHost
