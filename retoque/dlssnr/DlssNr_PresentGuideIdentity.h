#pragma once

// Whether a capture in the present guide ring belongs to the frame being presented.
//
// The ring (DlssNr_PresentGuideRing.h) decides by freshness: the newest capture whose copy has finished, younger than
// 250 ms. That says a capture is safe to read, not that it is this frame's. This is the identity layer
// docs/dlssnr/design/present-real-guides.md designs (D1, D2): a stamp on every capture, the present's own facts, and
// the eight rules that compare them. It runs after PickForPresent and may only narrow its answer: the slot it
// returns is the ring's pick or none, never another one, so an identity failure can never make an unsafe slot
// readable.
//
// Built in step 0 of that note, which is log only: both consumers ask, nothing acts on the answer, and Consumer
// counts what the answer would have changed. Standard library only, so tests/nr-present-guide-identity drives every
// rule on the host.

#include "DlssNr_PresentGuideRing.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DlssNr::PresentGuideIdentity
{
using PresentGuides::kRingSize;

// How many frame boundaries a correct capture may trail the present that reads it. Assumed, not measured. An engine
// that evaluates and presents on one thread captures in the interval just before the present's own boundary, which
// is 1; one whose render thread runs a frame ahead of its present thread lands on 2. Step 0's log carries the
// distribution that should set it.
constexpr uint64_t kLag = 1;

// A swapchain counts as presenting on its device for this long after its last present. The registry counts
// swapchains that present, not swapchains that exist: a launcher's chain that stops presenting stops counting.
constexpr int64_t kPresenterLiveMs = 1000;
constexpr size_t kPresenters = 4;

// Distinct output extents counted per frame and device. A capture past that is counted as unknown, which rule 4
// reads as ambiguous: fail closed.
constexpr size_t kExtentsPerFrame = 4;
constexpr uint32_t kOrdinalUnknown = UINT32_MAX;

// What a capture says about itself, written when it is committed to the ring.
struct Stamp
{
    const void* device = nullptr;    // the device the game's guides, and so their copies, live on
    uint32_t outputWidth = 0;        // the upscaler's output resource; zero when the evaluate named none
    uint32_t outputHeight = 0;       //
    uint32_t outputBaseX = 0;        // DLSS.Output.Subrect.Base: non-zero means the output is part of a larger texture
    uint32_t outputBaseY = 0;        //
    uint32_t renderWidth = 0;        // how much of the guides is real (RenderExtent)
    uint32_t renderHeight = 0;       //
    uint64_t presentEpoch = 0;       // the frame boundary count when the capture was made
    uint32_t evalsSincePresent = 0;  // this capture's ordinal since that boundary, on its device at its output extent
    const void* swapchain = nullptr; // the device's one presenting swapchain; null for none or several
};

using Stamps = std::array<Stamp, kRingSize>;

// What a present can say about itself. Tracker::AtPresent fills epoch and swapchainsOnDevice.
struct Present
{
    const void* device = nullptr; // the device the present's list reads the copies on
    uint32_t width = 0;           // the image the pass edits: the backbuffer
    uint32_t height = 0;          //
    const void* swapchain = nullptr;
    uint32_t swapchainsOnDevice = 0; // presenting swapchains on that device, this one included
    uint64_t epoch = 0;              // the frame boundary count, this present's own included
    uint64_t lastConsumed = 0;       // ring sequence of the capture this consumer last read; 0 for none
    int64_t nowMs = 0;
};

// In the order the rules are checked; the first that fails is the answer.
enum class Reason : uint8_t
{
    Accepted,
    NoCapture, // PickForPresent offered nothing: not an identity refusal, and the fallback runs today too
    Device,    // rule 1
    Extent,    // rule 2
    OutputBase,
    Swapchain, // rule 3
    Ambiguous, // rule 4
    Consumed,  // rule 5
    Lag,       // rule 6
    Stale,     // rule 7
    Subrect,   // rule 8
};
constexpr size_t kReasons = 11;

inline const char* ReasonName(Reason reason)
{
    switch (reason)
    {
    case Reason::Accepted:
        return "accepted";
    case Reason::NoCapture:
        return "no capture";
    case Reason::Device:
        return "device";
    case Reason::Extent:
        return "extent";
    case Reason::OutputBase:
        return "output-base";
    case Reason::Swapchain:
        return "swapchain";
    case Reason::Ambiguous:
        return "ambiguous";
    case Reason::Consumed:
        return "consumed";
    case Reason::Lag:
        return "lag";
    case Reason::Stale:
        return "stale";
    case Reason::Subrect:
        return "subrect";
    }
    return "?";
}

struct Decision
{
    size_t slot = kRingSize;
    Reason reason = Reason::NoCapture;
};

struct Extent
{
    uint32_t width = 0;
    uint32_t height = 0;
};

// How much of the guides the pass treats as real: the render subrect when the game stated one, else the depth
// texture's own extent, which is what the pass then reads (DlssNrFrameInfo::RenderSubrectWidth: "zero means the
// game did not say"). Half a subrect is kept as stated, so rule 8 refuses it.
inline Extent RenderExtent(uint32_t subrectWidth, uint32_t subrectHeight, uint32_t depthWidth, uint32_t depthHeight)
{
    if (subrectWidth == 0 && subrectHeight == 0)
        return { depthWidth, depthHeight };

    return { subrectWidth, subrectHeight };
}

// Two captures of one frame at one extent: the same device, the same output extent, the same boundary count.
inline bool SameFrame(const Stamp& a, const Stamp& b)
{
    return a.device == b.device && a.outputWidth == b.outputWidth && a.outputHeight == b.outputHeight &&
           a.presentEpoch == b.presentEpoch;
}

// How many captures fed the picked one's frame at its extent: those before it, which its ordinal counts even after
// the ring has overwritten them, and those after it that are still in the ring. Rule 4's quantity.
inline uint32_t FrameCaptures(size_t picked, const PresentGuides::Ring& ring, const Stamps& stamps)
{
    if (picked >= kRingSize)
        return 0;

    const Stamp& pick = stamps[picked];

    if (pick.evalsSincePresent == kOrdinalUnknown)
        return kOrdinalUnknown;

    uint32_t count = pick.evalsSincePresent != 0 ? pick.evalsSincePresent : 1;
    const uint64_t sequence = ring.GetSlot(picked).sequence;

    for (size_t i = 0; i < kRingSize; ++i)
    {
        const auto& slot = ring.GetSlot(i);

        if (i != picked && slot.recorded && slot.sequence > sequence && SameFrame(stamps[i], pick) &&
            count < kOrdinalUnknown - 1)
        {
            ++count;
        }
    }

    return count;
}

// The rules of present-real-guides.md D2, in its order. `picked` is PickForPresent's answer; the result is that slot
// or kRingSize, never a third.
inline Decision Decide(size_t picked, const PresentGuides::Ring& ring, const Stamps& stamps, const Present& present)
{
    if (picked >= kRingSize || !ring.GetSlot(picked).recorded)
        return { kRingSize, Reason::NoCapture };

    const auto refuse = [](Reason reason) { return Decision { kRingSize, reason }; };
    const PresentGuides::Slot& slot = ring.GetSlot(picked);
    const Stamp& stamp = stamps[picked];

    // 1. The copies live on the device that will read them. Unknown on either side is not a match.
    if (stamp.device == nullptr || stamp.device != present.device)
        return refuse(Reason::Device);

    // 2. The upscaler produced the picture being presented: its extent, at its origin.
    if (stamp.outputWidth == 0 || stamp.outputHeight == 0 || stamp.outputWidth != present.width ||
        stamp.outputHeight != present.height)
    {
        return refuse(Reason::Extent);
    }

    if (stamp.outputBaseX != 0 || stamp.outputBaseY != 0)
        return refuse(Reason::OutputBase);

    // 3. The presenting swapchain, or no stamp and nobody else presenting on that device.
    if (stamp.swapchain != nullptr ? stamp.swapchain != present.swapchain : present.swapchainsOnDevice != 1)
        return refuse(Reason::Swapchain);

    // 4. The only capture of its frame at that extent.
    if (FrameCaptures(picked, ring, stamps) != 1)
        return refuse(Reason::Ambiguous);

    // 5. Not already read by an earlier present: a generated frame has no capture of its own.
    if (slot.sequence <= present.lastConsumed)
        return refuse(Reason::Consumed);

    // 6. No more than kLag frame boundaries behind this present.
    const uint64_t lag = present.epoch > stamp.presentEpoch ? present.epoch - stamp.presentEpoch : 0;

    if (lag > kLag)
        return refuse(Reason::Lag);

    // 7. The ring's own age limit, asked again with the present's clock.
    if (present.nowMs > 0 && slot.capturedAtMs > 0 &&
        present.nowMs - slot.capturedAtMs > PresentGuides::kPresentGuideMaxAgeMs)
    {
        return refuse(Reason::Stale);
    }

    // 8. A real render extent, inside the output.
    if (stamp.renderWidth == 0 || stamp.renderHeight == 0 || stamp.renderWidth > stamp.outputWidth ||
        stamp.renderHeight > stamp.outputHeight)
    {
        return refuse(Reason::Subrect);
    }

    return { picked, Reason::Accepted };
}

// The swapchains that have presented on each device lately.
class Presenters
{
    struct Entry
    {
        const void* device = nullptr;
        const void* presenter = nullptr;
        int64_t atMs = 0;
    };

    std::array<Entry, kPresenters> _entries {};

    static bool Live(const Entry& entry, int64_t nowMs)
    {
        return entry.presenter != nullptr && nowMs - entry.atMs <= kPresenterLiveMs;
    }

  public:
    void Note(const void* device, const void* presenter, int64_t nowMs)
    {
        if (presenter == nullptr)
            return;

        Entry* target = nullptr;

        for (auto& entry : _entries)
        {
            if (entry.presenter == presenter && entry.device == device)
            {
                target = &entry;
                break;
            }
        }

        // A free or expired entry, else the one that presented longest ago.
        for (size_t i = 0; target == nullptr && i < kPresenters; ++i)
        {
            if (!Live(_entries[i], nowMs))
                target = &_entries[i];
        }

        if (target == nullptr)
        {
            target = &_entries[0];

            for (auto& entry : _entries)
            {
                if (entry.atMs < target->atMs)
                    target = &entry;
            }
        }

        *target = { device, presenter, nowMs };
    }

    uint32_t Count(const void* device, int64_t nowMs) const
    {
        uint32_t count = 0;

        for (const auto& entry : _entries)
            count += Live(entry, nowMs) && entry.device == device ? 1 : 0;

        return count;
    }

    // The only swapchain presenting on that device, or null for none or several.
    const void* Sole(const void* device, int64_t nowMs) const
    {
        const void* sole = nullptr;

        for (const auto& entry : _entries)
        {
            if (!Live(entry, nowMs) || entry.device != device)
                continue;

            if (sole != nullptr)
                return nullptr;

            sole = entry.presenter;
        }

        return sole;
    }

    void Reset() { _entries = {}; }
};

// The ordinal a capture gets among the captures since the last frame boundary, per device and output extent.
class FrameCounter
{
    struct Entry
    {
        const void* device = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t count = 0;
    };

    std::array<Entry, kExtentsPerFrame> _entries {};
    size_t _used = 0;
    uint64_t _epoch = 0;

  public:
    uint32_t Next(const void* device, uint32_t width, uint32_t height, uint64_t epoch)
    {
        if (epoch != _epoch)
        {
            _entries = {};
            _used = 0;
            _epoch = epoch;
        }

        for (size_t i = 0; i < _used; ++i)
        {
            Entry& entry = _entries[i];

            if (entry.device == device && entry.width == width && entry.height == height)
            {
                if (entry.count < kOrdinalUnknown - 1)
                    ++entry.count;

                return entry.count;
            }
        }

        if (_used == kExtentsPerFrame)
            return kOrdinalUnknown;

        _entries[_used++] = { device, width, height, 1 };
        return 1;
    }

    void Reset() { *this = FrameCounter {}; }
};

// Everything one ring's identity needs besides the ring: the stamps, the presenting swapchains, the frame boundary
// count. Not thread safe; it lives beside the ring and is used under the ring's lock, by the capture and by every
// consumer's present.
class Tracker
{
    Stamps _stamps {};
    Presenters _presenters;
    FrameCounter _frame;
    uint64_t _epoch = 0;

  public:
    // At capture, before the slot is committed. The caller fills device, output and render extent; the frame
    // boundary, the ordinal and the swapchain are filled here.
    void StampCapture(size_t slot, Stamp stamp, int64_t nowMs)
    {
        if (slot >= kRingSize)
            return;

        stamp.presentEpoch = _epoch;
        stamp.evalsSincePresent = _frame.Next(stamp.device, stamp.outputWidth, stamp.outputHeight, _epoch);
        stamp.swapchain = _presenters.Sole(stamp.device, nowMs);
        _stamps[slot] = stamp;
    }

    // At a present, after PickForPresent. Marks a frame boundary, notes the presenter, fills the present's epoch and
    // swapchain count, and decides.
    Decision AtPresent(size_t picked, const PresentGuides::Ring& ring, Present& present)
    {
        ++_epoch;
        _presenters.Note(present.device, present.swapchain, present.nowMs);
        present.epoch = _epoch;
        present.swapchainsOnDevice = _presenters.Count(present.device, present.nowMs);
        return Decide(picked, ring, _stamps, present);
    }

    const Stamps& Stamped() const { return _stamps; }
    uint64_t Epoch() const { return _epoch; }

    void Reset()
    {
        _stamps = {};
        _presenters.Reset();
        _frame.Reset();
        _epoch = 0;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Step 0: what the decision would have changed, counted per consumer and written as one line per window.
// ---------------------------------------------------------------------------------------------------------------

// What the present does today with the slot PickForPresent offered.
enum class Today : uint8_t
{
    Read,    // reads the picked capture
    Skipped, // runs no pass: nothing was captured since its last one (the D3D12 present pass's own skip)
    None,    // nothing was picked; zero or synthesized guides
};

using Clock = std::chrono::steady_clock;

// A line after at least kWindow. While the window looks like the last line written (the same buckets in use), the
// next one waits for kSteadyWindow instead, carrying the counts of the whole interval; a change is written at once.
constexpr Clock::duration kWindow = std::chrono::seconds(2);
constexpr Clock::duration kSteadyWindow = std::chrono::seconds(30);

constexpr size_t kLagBuckets = 5;   // 0, 1, 2, 3, 4 or more
constexpr size_t kFrameBuckets = 3; // 1, 2, 3 or more

struct Counts
{
    uint32_t presents = 0;
    uint32_t read = 0;
    uint32_t skipped = 0;
    uint32_t none = 0;
    std::array<uint32_t, kReasons> readReasons {}; // the decision, over presents that read a capture today
    uint32_t skippedWouldPass = 0;                 // presents that skip today on a capture the rules accept
    std::array<uint32_t, kLagBuckets> lag {};      // epoch lag, over picks no present had read yet
    std::array<uint32_t, kFrameBuckets> frame {};  // FrameCaptures, over the same picks

    // The last extent or output-base refusal of the window, for the line.
    bool haveMismatch = false;
    Stamp mismatch {};
    uint32_t mismatchWidth = 0;
    uint32_t mismatchHeight = 0;
};

// Which buckets are in use: what makes a window look like the last one written.
inline uint32_t Shape(const Counts& counts)
{
    uint32_t shape = 0;
    uint32_t bit = 0;
    const auto add = [&shape, &bit](uint32_t value) { shape |= (value != 0 ? 1u : 0u) << bit++; };

    add(counts.read);
    add(counts.skipped);
    add(counts.none);
    for (const uint32_t n : counts.readReasons)
        add(n);
    add(counts.skippedWouldPass);
    for (const uint32_t n : counts.lag)
        add(n);
    for (const uint32_t n : counts.frame)
        add(n);

    return shape;
}

inline std::string FormatLine(const char* consumer, const Counts& counts, Clock::duration elapsed)
{
    const auto n = [](uint32_t value) { return std::to_string(value); };
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    const uint32_t passes = counts.readReasons[static_cast<size_t>(Reason::Accepted)];
    const uint32_t refused = counts.read - passes;

    std::string line = "DLSS-NR present guides, identity check (log only, nothing refused), ";
    line += consumer;
    line += ": " + n(counts.presents) + " presents in " + std::to_string(ms / 1000) + "." +
            std::to_string(ms % 1000 / 100) + " s";
    line += " | today " + n(counts.read) + " read a capture, " + n(counts.skipped) + " skipped with no new capture, " +
            n(counts.none) + " had none";
    line += " | of those read, " + n(passes) + " would pass, " + n(refused) + " would be refused";

    if (refused != 0)
    {
        std::string reasons;

        for (size_t r = static_cast<size_t>(Reason::NoCapture); r < kReasons; ++r)
        {
            if (counts.readReasons[r] == 0)
                continue;

            reasons += reasons.empty() ? " (" : ", ";
            reasons += std::string(ReasonName(static_cast<Reason>(r))) + " " + n(counts.readReasons[r]);
        }

        line += reasons + ")";
    }

    line += " | skipped but would pass " + n(counts.skippedWouldPass);
    line += " | epoch lag of new captures 0:" + n(counts.lag[0]) + " 1:" + n(counts.lag[1]) + " 2:" + n(counts.lag[2]) +
            " 3:" + n(counts.lag[3]) + " 4+:" + n(counts.lag[4]);
    line += " | captures per frame at their extent 1:" + n(counts.frame[0]) + " 2:" + n(counts.frame[1]) +
            " 3+:" + n(counts.frame[2]);

    if (counts.haveMismatch)
    {
        line += " | last extent refusal: output " + n(counts.mismatch.outputWidth) + "x" +
                n(counts.mismatch.outputHeight) + " at (" + n(counts.mismatch.outputBaseX) + "," +
                n(counts.mismatch.outputBaseY) + "), backbuffer " + n(counts.mismatchWidth) + "x" +
                n(counts.mismatchHeight);
    }

    return line;
}

// One present stream's step-0 record: the counts of the open window, and the last capture it read, which rule 5
// compares against. Used under the ring's lock, by that consumer only.
class Consumer
{
    const char* _name;
    Counts _counts {};
    Clock::time_point _start {};
    bool _started = false;
    uint32_t _writtenShape = 0;
    bool _haveWritten = false;
    uint64_t _lastConsumed = 0;
    uint64_t _lastCounted = 0; // the newest capture the histograms have counted

  public:
    explicit Consumer(const char* name) : _name(name) {}

    uint64_t LastConsumed() const { return _lastConsumed; }
    const Counts& Open() const { return _counts; }

    // Counts one present; `present` is what Tracker::AtPresent filled. True when a line is due, written to *line.
    bool Note(const Decision& decision, size_t picked, const PresentGuides::Ring& ring, const Stamps& stamps,
              const Present& present, Today today, Clock::time_point now, std::string* line)
    {
        if (!_started)
        {
            _start = now;
            _started = true;
        }

        ++_counts.presents;

        const bool havePick = picked < kRingSize && ring.GetSlot(picked).recorded;
        const uint64_t sequence = havePick ? ring.GetSlot(picked).sequence : 0;

        // The lag and frame histograms count a capture once, at the first present it is offered to. Offered again
        // after it was read, it is counted as consumed, and its lag only grows.
        if (havePick && sequence > _lastCounted)
        {
            _lastCounted = sequence;

            const Stamp& stamp = stamps[picked];
            const uint64_t lag = present.epoch > stamp.presentEpoch ? present.epoch - stamp.presentEpoch : 0;
            ++_counts.lag[lag < kLagBuckets - 1 ? lag : kLagBuckets - 1];

            const uint32_t frame = FrameCaptures(picked, ring, stamps);
            ++_counts.frame[frame >= kFrameBuckets ? kFrameBuckets - 1 : (frame == 0 ? 0 : frame - 1)];
        }

        switch (today)
        {
        case Today::Read:
            ++_counts.read;
            ++_counts.readReasons[static_cast<size_t>(decision.reason)];

            if (decision.reason == Reason::Extent || decision.reason == Reason::OutputBase)
            {
                _counts.haveMismatch = true;
                _counts.mismatch = stamps[picked < kRingSize ? picked : 0];
                _counts.mismatchWidth = present.width;
                _counts.mismatchHeight = present.height;
            }

            // Never backwards: an older capture read after a newer one does not make the newer one unread.
            if (havePick && sequence > _lastConsumed)
                _lastConsumed = sequence;
            break;

        case Today::Skipped:
            ++_counts.skipped;
            _counts.skippedWouldPass += decision.reason == Reason::Accepted ? 1 : 0;
            break;

        case Today::None:
            ++_counts.none;
            break;
        }

        const Clock::duration elapsed = now - _start;

        if (elapsed < kWindow)
            return false;

        const uint32_t shape = Shape(_counts);

        if (_haveWritten && shape == _writtenShape && elapsed < kSteadyWindow)
            return false;

        if (line != nullptr)
            *line = FormatLine(_name, _counts, elapsed);

        _writtenShape = shape;
        _haveWritten = true;
        _counts = {};
        _start = now;
        return true;
    }

    void Reset() { *this = Consumer(_name); }
};
} // namespace DlssNr::PresentGuideIdentity
