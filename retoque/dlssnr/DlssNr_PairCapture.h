// Same-frame comparison capture: the pure half (docs/dlssnr/design/comparison-capture.md).
//
// One press saves two image files of one frame -- the frame the pass read and the frame it produced -- with a
// provenance record. Both copies are recorded on the command list that ran the pass, so "same frame" is a property
// of the recording, not of timing; the readbacks are only read once the GPU is proven past them, and never on the
// render thread.
//
// This header holds what can be tested without a GPU (tests/nr-capture): the request/record/ticket/hand-off state
// machine over an abstract backend, the names and the 64-pair cap, the provenance record, the UTC stamp, and what
// the menu button shows. DlssNr_PairCapture.cpp is the D3D12 backend and the writer thread.

#pragma once

#include "DlssNr_ImageFile.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DlssNr::PairCapture
{
// --- Bounds ----------------------------------------------------------------------------------------------------

// Pairs kept in the folder; older ones are deleted, newest kept. A constant, like the burst's kMaxFrames, so the
// feature cannot fill a drive: about 1.9 GB of uncompressed PNG at 3440x1440, 3.2 GB at 3840x2160.
constexpr size_t kMaxPairs = 64;

// Where no recording can be tracked to its completion, the copies are read after this many dispatches -- the burst's
// assumption, and the provenance says it was one -- and our reference is let go after the retirement list's count.
constexpr uint64_t kAssumedPolls = 8;
constexpr uint64_t kUntrackedReleasePolls = 32;

// A tracked recording that has not completed after this many dispatches never will (device removed, a list that
// was dropped, a queue the tracker cannot fence): its readbacks are kept for the rest of the session, never
// released, and the capture is off for the session, so a route that never completes cannot leak a pair per press.
constexpr uint64_t kGiveUpPolls = 1200;

// A request no frame took within this long is withdrawn, so the button does not stay "waiting" forever.
constexpr int64_t kRequestExpiryMs = 5000;

// Pairs whose readbacks this side still holds until the GPU is proven done with them. Past it, a new capture is
// refused rather than holding more memory for recordings that may never be released.
constexpr size_t kMaxRetained = 4;

// --- Reasons (literals; the menu translates them) --------------------------------------------------------------

inline constexpr const char* kReasonCompare = "Turn off Compare to capture the result.";
inline constexpr const char* kReasonDebugView = "Turn off the debug view to capture the result.";
inline constexpr const char* kReasonProxy = "Not with the driver proxy: it returns before the pass writes the frame.";
inline constexpr const char* kReasonVulkan = "Not on this route yet: the capture is D3D12 only.";
inline constexpr const char* kReasonDirectory = "The capture folder could not be created (see the log).";
inline constexpr const char* kReasonExpired = "No frame reached the pass in time; nothing was captured.";
inline constexpr const char* kReasonRetained = "Earlier captures are still held for the GPU; try again shortly.";
inline constexpr const char* kReasonAllocation = "The readback buffers could not be created.";
inline constexpr const char* kReasonNoCompletion =
    "The GPU never confirmed the copies; nothing was written, and the capture is off for this session.";
inline constexpr const char* kReasonHandOff = "The writer could not be started.";
inline constexpr const char* kReasonWrite = "The files could not be written (see the log).";

// --- State machine ---------------------------------------------------------------------------------------------

enum class Phase : uint32_t
{
    Idle,
    Requested, // asked for; the next frame the pass writes takes it
    Recording, // claimed by the render thread while it records the copies
    Pending,   // copies recorded; waiting for the GPU to be proven past them
    Writing    // handed to the writer thread
};

enum class Completion : uint8_t
{
    Submission,   // the queue fence after the list that carried the copies (DlssNr::Submission)
    AssumedFrames // no tracking: read after kAssumedPolls dispatches, an assumption and labelled one
};

inline const char* CompletionName(Completion completion)
{
    return completion == Completion::Submission ? "submission-fence" : "assumed-8-frames";
}

struct Outcome
{
    bool ok = false;
    const char* reason = nullptr; // a literal, null on success or before the first capture
    std::string stem;             // the file stem written, on success; never a path
    uint64_t seq = 0;
};

// The state machine over a backend that does the GPU work. The backend provides:
//
//   using Readback;  using Ticket;  using Payload;  using Recording;
//   bool Alloc(Recording&, int which, Readback&)       which: 0 original, 1 processed
//   bool Track(Recording&, Ticket&)                     before any copy is recorded
//   void Copy(Recording&, int which, Readback&)
//   bool Completed(const Ticket&)                       the GPU is past the recording (may read)
//   bool Ready(const Ticket&)                           ... and the list can never run again (may free)
//   void Release(Readback&)                             our reference, only when proven safe
//   bool Hand(Readback (&)[2], Payload&&, Completion)   to the writer, which takes references of its own
//   void Leak(Readback (&)[2])                          completion will never be proven: keep them forever
//
// Threads: Request, ExpireIfStale, Current and LastOutcome from any thread; Poll, Record and Shutdown on the render
// thread under the pass's lock; Finished on the writer thread. Nothing here waits for the GPU.
template <class Backend> class Session
{
  public:
    using Readback = typename Backend::Readback;
    using Ticket = typename Backend::Ticket;
    using Payload = typename Backend::Payload;
    using Recording = typename Backend::Recording;

    // Asks for a capture. False when one is already under way -- a second press is ignored -- or the capture gave up
    // on a recording this session.
    bool Request(int64_t nowMs)
    {
        // The time is stored only once idle is seen, so a press that is ignored does not move a waiting request's
        // expiry; the release below publishes it to ExpireIfStale's acquire.
        if (gaveUp_.load(std::memory_order_relaxed) || phase_.load(std::memory_order_acquire) != Phase::Idle)
            return false;
        requestedAt_.store(nowMs, std::memory_order_relaxed);
        Phase idle = Phase::Idle;
        return phase_.compare_exchange_strong(idle, Phase::Requested, std::memory_order_acq_rel);
    }

    // Whether a recording was given up on this session, which turns the capture off.
    bool GaveUp() const { return gaveUp_.load(std::memory_order_relaxed); }

    // A request nothing took in time goes back to idle, with a reason.
    void ExpireIfStale(int64_t nowMs)
    {
        // Acquire, so the time stored before the request's release is the one read below.
        if (phase_.load(std::memory_order_acquire) != Phase::Requested)
            return;
        if (nowMs - requestedAt_.load(std::memory_order_relaxed) < kRequestExpiryMs)
            return;
        Phase requested = Phase::Requested;
        if (phase_.compare_exchange_strong(requested, Phase::Idle, std::memory_order_acq_rel))
            Settle(false, kReasonExpired, {});
    }

    Phase Current() const { return phase_.load(std::memory_order_acquire); }

    Outcome LastOutcome() const
    {
        std::lock_guard<std::mutex> lock(outcomeMutex_);
        return outcome_;
    }

    // Once per dispatch, at the top, before anything can return. Idle with nothing retained is one relaxed load.
    void Poll(Backend& backend, int64_t nowMs)
    {
        if (!Busy())
            return;

        ++polls_;
        const Phase phase = phase_.load(std::memory_order_acquire);

        if (phase == Phase::Requested)
            ExpireIfStale(nowMs);

        if (phase == Phase::Pending && pending_.active)
        {
            const uint64_t age = polls_ - pending_.recordedAt;
            const bool complete = pending_.tracked ? backend.Completed(pending_.ticket) : age >= kAssumedPolls;
            if (complete)
            {
                const Completion completion = pending_.tracked ? Completion::Submission : Completion::AssumedFrames;
                // Writing before the hand-off, so the writer's return to idle can never be overwritten by this.
                phase_.store(Phase::Writing, std::memory_order_release);
                if (!backend.Hand(pending_.readbacks, std::move(pending_.payload), completion))
                {
                    Settle(false, kReasonHandOff, {});
                    phase_.store(Phase::Idle, std::memory_order_release);
                }
                Retain(pending_);
                pending_ = {};
            }
            else if (pending_.tracked && age >= kGiveUpPolls)
            {
                backend.Leak(pending_.readbacks);
                pending_ = {};
                gaveUp_.store(true, std::memory_order_relaxed);
                Settle(false, kReasonNoCompletion, {});
                phase_.store(Phase::Idle, std::memory_order_release);
            }
        }

        for (size_t i = 0; i < retained_.size();)
        {
            Retained& r = retained_[i];
            const bool safe = r.tracked ? backend.Ready(r.ticket) : polls_ - r.recordedAt >= kUntrackedReleasePolls;
            if (!safe)
            {
                ++i;
                continue;
            }
            backend.Release(r.readbacks[0]);
            backend.Release(r.readbacks[1]);
            retained_[i] = std::move(retained_.back());
            retained_.pop_back();
        }
    }

    // At the record site, after the pass wrote the frame. refusal is the caller's reason not to capture this frame
    // (compare on, a format the files cannot hold), or null. Returns whether the copies were recorded; when this call
    // took the request and did not record, why is set to the reason it settled (null otherwise).
    bool Record(Backend& backend, Recording& recording, Payload&& payload, const char* refusal,
                const char** why = nullptr)
    {
        if (why != nullptr)
            *why = nullptr;
        if (phase_.load(std::memory_order_relaxed) != Phase::Requested)
            return false;
        Phase requested = Phase::Requested;
        if (!phase_.compare_exchange_strong(requested, Phase::Recording, std::memory_order_acq_rel))
            return false;

        if (refusal == nullptr && retained_.size() >= kMaxRetained)
            refusal = kReasonRetained;

        if (refusal != nullptr)
        {
            if (why != nullptr)
                *why = refusal;
            Settle(false, refusal, {});
            phase_.store(Phase::Idle, std::memory_order_release);
            return false;
        }

        Pending next;
        if (!backend.Alloc(recording, 0, next.readbacks[0]) || !backend.Alloc(recording, 1, next.readbacks[1]))
        {
            // Never recorded into: nothing on the GPU can be using them.
            backend.Release(next.readbacks[0]);
            backend.Release(next.readbacks[1]);
            if (why != nullptr)
                *why = kReasonAllocation;
            Settle(false, kReasonAllocation, {});
            phase_.store(Phase::Idle, std::memory_order_release);
            return false;
        }

        next.tracked = backend.Track(recording, next.ticket);
        backend.Copy(recording, 0, next.readbacks[0]);
        backend.Copy(recording, 1, next.readbacks[1]);
        next.recordedAt = polls_;
        next.payload = std::move(payload);
        next.active = true;
        pending_ = std::move(next);
        phase_.store(Phase::Pending, std::memory_order_release);
        return true;
    }

    // The writer thread, when it is done with a hand-off.
    void Finished(bool ok, const char* reason, std::string stem)
    {
        Settle(ok, reason, std::move(stem));
        phase_.store(Phase::Idle, std::memory_order_release);
    }

    // Lets go of what is proven done; whatever is not is kept for the rest of the process, not freed.
    void Shutdown(Backend& backend)
    {
        if (pending_.active)
        {
            backend.Leak(pending_.readbacks);
            pending_ = {};
            Phase pending = Phase::Pending;
            phase_.compare_exchange_strong(pending, Phase::Idle, std::memory_order_acq_rel);
        }
        for (Retained& r : retained_)
        {
            if (r.tracked && backend.Ready(r.ticket))
            {
                backend.Release(r.readbacks[0]);
                backend.Release(r.readbacks[1]);
            }
            else
            {
                backend.Leak(r.readbacks);
            }
        }
        retained_.clear();
    }

    size_t RetainedCount() const { return retained_.size(); }

    // Render thread: whether Poll has anything to do. False is one relaxed load and an empty-vector check.
    bool Busy() const { return !retained_.empty() || phase_.load(std::memory_order_relaxed) != Phase::Idle; }

  private:
    struct Pending
    {
        Readback readbacks[2] {};
        Ticket ticket {};
        Payload payload {};
        uint64_t recordedAt = 0;
        bool tracked = false;
        bool active = false;
    };

    struct Retained
    {
        Readback readbacks[2] {};
        Ticket ticket {};
        uint64_t recordedAt = 0;
        bool tracked = false;
    };

    void Retain(Pending& from)
    {
        Retained r;
        r.readbacks[0] = from.readbacks[0];
        r.readbacks[1] = from.readbacks[1];
        r.ticket = from.ticket;
        r.recordedAt = from.recordedAt;
        r.tracked = from.tracked;
        retained_.push_back(std::move(r));
    }

    void Settle(bool ok, const char* reason, std::string stem)
    {
        std::lock_guard<std::mutex> lock(outcomeMutex_);
        outcome_.ok = ok;
        outcome_.reason = reason;
        if (ok)
        {
            outcome_.stem = std::move(stem);
            ++outcome_.seq;
        }
    }

    std::atomic<Phase> phase_ { Phase::Idle };
    std::atomic<int64_t> requestedAt_ { 0 };
    std::atomic<bool> gaveUp_ { false };
    mutable std::mutex outcomeMutex_;
    Outcome outcome_;
    // Render thread only.
    Pending pending_;
    std::vector<Retained> retained_;
    uint64_t polls_ = 0;
};

// --- The menu button -------------------------------------------------------------------------------------------

struct ButtonInputs
{
    bool enabled = false;      // [DlssNr] Enabled
    bool nativeVulkan = false; // the pass runs natively on Vulkan
    bool d3d12Ran = false;     // a D3D12 route has run the model this session
    bool proxy = false;        // [DlssNr] UseProxy
    uint32_t compare = 0;      // [DlssNr] Compare
    uint32_t debugView = 0;    // [DlssNr] DebugView
    Phase phase = Phase::Idle;
    bool directoryFailed = false;
    bool gaveUp = false; // a recording never completed this session
};

struct ButtonState
{
    bool shown = false;
    bool enabled = false;
    const char* reason = nullptr; // why it is disabled; a literal
};

inline constexpr const char* kWaitingWords = "Waiting for the next frame the pass writes...";
inline constexpr const char* kReadingWords = "Waiting for the GPU to finish the copies...";
inline constexpr const char* kWritingWords = "Writing the files...";

// What the button is in each state (DEVELOPMENT.md section 2.2). Absent while there is no frame of the pass to
// capture (rule 2); disabled with the reason in every state where pressing it could not give the pair.
inline ButtonState ButtonFor(const ButtonInputs& in)
{
    ButtonState state;
    if (!in.enabled)
        return state;
    if (in.nativeVulkan)
    {
        state.shown = true;
        state.reason = kReasonVulkan;
        return state;
    }
    if (!in.d3d12Ran)
        return state;

    state.shown = true;
    if (in.proxy)
        state.reason = kReasonProxy;
    else if (in.compare != 0)
        state.reason = kReasonCompare;
    else if (in.debugView != 0)
        state.reason = kReasonDebugView;
    else if (in.phase == Phase::Requested || in.phase == Phase::Recording)
        state.reason = kWaitingWords;
    else if (in.phase == Phase::Pending)
        state.reason = kReadingWords;
    else if (in.phase == Phase::Writing)
        state.reason = kWritingWords;
    else if (in.directoryFailed)
        state.reason = kReasonDirectory;
    else if (in.gaveUp)
        state.reason = kReasonNoCompletion;
    else
        state.enabled = true;
    return state;
}

// --- Names -----------------------------------------------------------------------------------------------------

// A UTC time, as the record has it and as file names have it. UTC so the two agree and a daylight-saving change
// cannot reorder the folder.
struct UtcStamp
{
    char iso[24] = {};     // 2026-10-13T12:34:56Z
    char compact[20] = {}; // 20261013-123456
};

inline UtcStamp FormatUtc(int64_t secondsSinceEpoch)
{
    // Howard Hinnant's civil_from_days, so no gmtime_s / gmtime_r split between Windows and the host test.
    int64_t days = secondsSinceEpoch / 86400;
    int64_t seconds = secondsSinceEpoch % 86400;
    if (seconds < 0)
    {
        seconds += 86400;
        --days;
    }
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    const int hh = static_cast<int>(seconds / 3600);
    const int mm = static_cast<int>(seconds / 60 % 60);
    const int ss = static_cast<int>(seconds % 60);

    UtcStamp stamp;
    std::snprintf(stamp.iso, sizeof(stamp.iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", static_cast<int>(year),
                  static_cast<int>(month), static_cast<int>(day), hh, mm, ss);
    std::snprintf(stamp.compact, sizeof(stamp.compact), "%04d%02d%02d-%02d%02d%02d", static_cast<int>(year),
                  static_cast<int>(month), static_cast<int>(day), hh, mm, ss);
    return stamp;
}

inline bool NameChar(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'_' ||
           c == L'-';
}

// The executable's base name as it goes into file names and the record: what makes a capture identifiable, and
// nothing more. Takes the stem only -- the caller passes no path -- and keeps ASCII letters, digits, '.', '_' and
// '-', with anything else (spaces, other scripts) as '_'. At most 40 characters; "game" when nothing is left.
inline std::string SanitizeExeStem(std::wstring_view stem)
{
    // Whatever the caller hands over, only what follows the last separator is a name.
    const size_t cut = stem.find_last_of(L"\\/:");
    if (cut != std::wstring_view::npos)
        stem.remove_prefix(cut + 1);
    std::string out;
    for (const wchar_t c : stem)
    {
        if (out.size() >= 40)
            break;
        out.push_back(NameChar(c) ? static_cast<char>(c) : '_');
    }
    // No leading dot: a hidden file name on the host, and "." or ".." are not names.
    while (!out.empty() && out.front() == '.')
        out.erase(out.begin());
    return out.empty() ? std::string("game") : out;
}

inline std::string Stem(const char* compactUtc, const std::string& exe, uint32_t seq)
{
    char number[16];
    std::snprintf(number, sizeof(number), "%03u", seq);
    return std::string(compactUtc) + "-" + exe + "-" + number;
}

enum class Part : uint8_t
{
    Original,
    Processed,
    Sidecar
};

inline std::string FileName(const std::string& stem, Part part, ImageFile::FileKind kind)
{
    switch (part)
    {
    case Part::Original:
        return stem + ".original." + ImageFile::Extension(kind);
    case Part::Processed:
        return stem + ".processed." + ImageFile::Extension(kind);
    default:
        return stem + ".json";
    }
}

// Whether name is one of ours -- <yyyymmdd>-<hhmmss>-<exe>-<seq>.{original,processed}.{png,pfm} or .json, or one
// of those with the ".tmp" a write in progress carries -- and its stem. Nothing else in the folder is ever touched.
inline bool ParseName(std::string_view name, std::string* stem = nullptr)
{
    static constexpr std::string_view kTmp = ".tmp";
    if (name.size() > kTmp.size() && name.substr(name.size() - kTmp.size()) == kTmp)
        name.remove_suffix(kTmp.size());

    static constexpr std::string_view kSuffixes[] = { ".original.png", ".original.pfm", ".processed.png",
                                                      ".processed.pfm", ".json" };
    bool matched = false;
    for (const std::string_view suffix : kSuffixes)
    {
        if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
        {
            name.remove_suffix(suffix.size());
            matched = true;
            break;
        }
    }
    if (!matched || name.size() < 16 + 1 + 1 + 3)
        return false;

    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (size_t i = 0; i < 15; ++i)
    {
        if (i == 8 ? name[i] != '-' : !digit(name[i]))
            return false;
    }
    if (name[15] != '-')
        return false;

    const size_t dash = name.rfind('-');
    if (dash == std::string_view::npos || dash <= 16)
        return false;
    const std::string_view seq = name.substr(dash + 1);
    const std::string_view exe = name.substr(16, dash - 16);
    if (seq.size() < 3 || !std::all_of(seq.begin(), seq.end(), digit) || exe.empty() || exe.size() > 40 ||
        exe.front() == '.' ||
        !std::all_of(exe.begin(), exe.end(), [](char c) { return NameChar(static_cast<unsigned char>(c)); }))
        return false;

    if (stem != nullptr)
        stem->assign(name);
    return true;
}

// The files to delete so that, with one more capture written, at most keepPairs stems remain: the oldest stems by
// name -- which is by time, the stamp leading every name -- and every file of each. Names that are not ours are
// never returned.
inline std::vector<std::string> RotationVictims(const std::vector<std::string>& names, size_t keepPairs)
{
    std::set<std::string> stems;
    for (const std::string& name : names)
    {
        std::string stem;
        if (ParseName(name, &stem))
            stems.insert(stem);
    }

    const size_t keepExisting = keepPairs > 0 ? keepPairs - 1 : 0;
    if (stems.size() <= keepExisting)
        return {};

    std::set<std::string> doomed;
    size_t excess = stems.size() - keepExisting;
    for (const std::string& stem : stems)
    {
        if (excess-- == 0)
            break;
        doomed.insert(stem);
    }

    std::vector<std::string> victims;
    for (const std::string& name : names)
    {
        std::string stem;
        if (ParseName(name, &stem) && doomed.count(stem) != 0)
            victims.push_back(name);
    }
    std::sort(victims.begin(), victims.end());
    return victims;
}

// Writes a file under its name plus ".tmp" and renames it into place, so a half-written file never sits under a
// final name: a write that fails, or a process that ends mid-write, leaves at most the ".tmp" (which ParseName
// counts with its stem, so the cap clears it). open(path) returns a FILE* opened for binary writing -- _wfopen on
// Windows -- and write(FILE*) returns whether every byte went out.
template <class Open, class Write>
bool WriteViaTemporary(const std::filesystem::path& final, Open&& open, Write&& write)
{
    std::filesystem::path temporary = final;
    temporary += ".tmp";
    std::FILE* file = open(temporary);
    if (file == nullptr)
        return false;
    const bool wrote = write(file);
    const bool closed = std::fclose(file) == 0;
    std::error_code ec;
    if (!wrote || !closed)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::filesystem::rename(temporary, final, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

// --- Provenance ------------------------------------------------------------------------------------------------

// What a value may hold once it is in the record: printable ASCII, and no ';', which separates the pairs in the
// one-line form. Anything else becomes '?' (';' becomes ','), so the tEXt chunk is always valid Latin-1.
inline std::string Printable(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char ch : value)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        out.push_back(c == ';' ? ',' : (c >= 0x20 && c <= 0x7E) ? ch : '?');
    }
    return out;
}

inline std::string JsonString(std::string_view value)
{
    std::string out = "\"";
    for (const char ch : value)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(ch);
        }
        else if (c < 0x20 || c > 0x7E)
        {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        }
        else
        {
            out.push_back(ch);
        }
    }
    out.push_back('"');
    return out;
}

// One record, written three ways from one source (comparison-capture.md, section 4.4): the PNG's tEXt chunk and
// the log as key=value pairs joined by ';', the sidecar as a JSON object of strings, in the order the keys were set.
//
// What goes in is decided by the caller, key by key; this never takes a path, a user name, a host name, a window
// title or an environment variable, and tests/nr-capture runs the repository's privacy patterns over what it makes.
class Record
{
  public:
    void Set(std::string_view key, std::string_view value)
    {
        const std::string clean = Printable(value);
        for (auto& item : items_)
        {
            if (item.first == key)
            {
                item.second = clean;
                return;
            }
        }
        items_.emplace_back(std::string(key), clean);
    }

    const std::string* Find(std::string_view key) const
    {
        for (const auto& item : items_)
            if (item.first == key)
                return &item.second;
        return nullptr;
    }

    std::string Line() const
    {
        std::string out;
        for (const auto& item : items_)
        {
            if (!out.empty())
                out += ';';
            out += item.first;
            out += '=';
            out += item.second;
        }
        return out;
    }

    std::string Json() const
    {
        std::string out = "{\n";
        for (size_t i = 0; i < items_.size(); ++i)
        {
            out += "  ";
            out += JsonString(items_[i].first);
            out += ": ";
            out += JsonString(items_[i].second);
            out += i + 1 < items_.size() ? ",\n" : "\n";
        }
        out += "}\n";
        return out;
    }

    const std::vector<std::pair<std::string, std::string>>& Items() const { return items_; }

  private:
    std::vector<std::pair<std::string, std::string>> items_;
};

// The keyword the PNG's tEXt chunk carries the record under.
inline constexpr const char* kTextKeyword = "Retoque.Provenance";
} // namespace DlssNr::PairCapture
