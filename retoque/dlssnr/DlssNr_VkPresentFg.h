#pragma once

// The Vulkan present host beside frame generation, stage 4 (docs/dlssnr/design/nr-vulkan-present-host.md, "Stage 4:
// the present host beside frame generation"):
//
// - which presents frame generation makes or takes (the two shapes the host meets), and what the host does with them
//   until the pacer experiment has been run in games: nothing -- they go down untouched, and the model stands down;
// - the pacer trace ([DlssNr] VulkanPresentTrace): per swapchain, the presenting threads and identities, waits and
//   signals, present ids, GPU completion, the frame-time tail, Streamline's messages and the frame-generation shape,
//   one line every kTraceWindowMs;
// - Streamline's log messages, classified for that trace.
//
// Standard library only, so tests/nr-vk-present-host drives all of it on the host.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DlssNr::VkPresentHost
{
// The FG overlay bypass can call the host directly while an export detour is already observing that present.
// Borrow the caller's arrays only while its synchronous call is on the stack. A proxy swapchain's nested real
// present, another image, or another queue is a distinct call; changed wait semaphores alone are not.
template <typename Queue, typename Swapchain> class PresentCallScope
{
    Queue _queue;
    uint32_t _count;
    const Swapchain* _swapchains;
    const uint32_t* _images;
    const PresentCallScope* _previous;
    inline static thread_local const PresentCallScope* _current = nullptr;

    bool Matches(Queue queue, uint32_t count, const Swapchain* swapchains, const uint32_t* images) const
    {
        if (_queue != queue || _count != count || (count != 0 && (_swapchains == nullptr || _images == nullptr)))
            return false;
        for (uint32_t i = 0; i < count; ++i)
            if (_swapchains[i] != swapchains[i] || _images[i] != images[i])
                return false;
        return true;
    }

  public:
    PresentCallScope(Queue queue, uint32_t count, const Swapchain* swapchains, const uint32_t* images)
        : _queue(queue), _count(count), _swapchains(swapchains), _images(images), _previous(_current)
    {
        _current = this;
    }
    ~PresentCallScope() { _current = _previous; }
    PresentCallScope(const PresentCallScope&) = delete;
    PresentCallScope& operator=(const PresentCallScope&) = delete;

    static bool ShouldObserve(bool installed, Queue queue, uint32_t count, const Swapchain* swapchains,
                              const uint32_t* images)
    {
        if (!installed || (count != 0 && (swapchains == nullptr || images == nullptr)))
            return false;
        for (auto scope = _current; scope != nullptr; scope = scope->_previous)
            if (scope->Matches(queue, count, swapchains, images))
                return false;
        return true;
    }
};

// --- Which presents frame generation makes or takes -----------------------------------------------------------

// What the host knows at one present, read from OptiScaler's state by the host.
struct FgFacts
{
    // Process-wide: the signals the D3D12 present pass reads (PresentBoundary::Inputs).
    bool gameDlssgSet = false; // the mode last sent to Streamline's DLSS-G is not off
    int gameDlssgCount = 0;    // DLSSG.MultiFrameCount at the game's last NGX DLSS-G evaluate (0 once it stops)
    bool ownD3d12Fg = false;   // OptiScaler's own D3D12 frame generation output is active and not paused
    // OptiScaler's own Vulkan frame generation output (State::currentVkFG), for this present.
    bool vkFgSwapchain = false; // the present is of the output's swapchain: the game's real frame, going into FFX
    bool vkFgQueue = false;     // the present is on a queue of the output's: FFX's presenter, real or generated
};

enum class FgShape : uint8_t
{
    None,
    VkFgInput,  // shape (b): the game presenting a real frame into OptiScaler's Vulkan FSR-FG output
    VkFgOutput, // shape (b): FSR-FG's presenter presenting a real or a generated frame
    GameDlssg,  // shape (a): the game's own DLSS-G is on (Streamline's mode, or frames counted at its NGX evaluate)
    OwnD3d12Fg, // OptiScaler's own D3D12 frame generation is on in this process
};

// The present's shape. The Vulkan output's own identification comes first: it is exact, per present. The process-wide
// signals cover every other present of every device: none of their calls says which frame it carries.
inline FgShape ClassifyFg(const FgFacts& facts)
{
    if (facts.vkFgQueue)
        return FgShape::VkFgOutput;
    if (facts.vkFgSwapchain)
        return FgShape::VkFgInput;
    if (facts.ownD3d12Fg)
        return FgShape::OwnD3d12Fg;
    if (facts.gameDlssgSet || facts.gameDlssgCount > 0)
        return FgShape::GameDlssg;
    return FgShape::None;
}

inline const char* FgShapeName(FgShape shape)
{
    switch (shape)
    {
    case FgShape::VkFgInput:
        return "OptiScaler's Vulkan FSR-FG output, the game's present into it";
    case FgShape::VkFgOutput:
        return "OptiScaler's Vulkan FSR-FG output, its own present";
    case FgShape::GameDlssg:
        return "the game's DLSS-G";
    case FgShape::OwnD3d12Fg:
        return "OptiScaler's D3D12 frame generation";
    default:
        return "none";
    }
}

// The trace's short words for it.
inline const char* FgShapeShort(FgShape shape)
{
    switch (shape)
    {
    case FgShape::VkFgInput:
        return "FSR-FG input";
    case FgShape::VkFgOutput:
        return "FSR-FG output";
    case FgShape::GameDlssg:
        return "game DLSS-G";
    case FgShape::OwnD3d12Fg:
        return "OptiScaler D3D12 FG";
    default:
        return "none";
    }
}

enum class FgCarry : uint8_t
{
    Normal,        // no frame generation: the stages before this one, unchanged
    Untouched,     // goes down with the caller's own present info: no copy, no submit, no semaphore of the host's
    TransportOnly, // [DlssNr] VulkanPresentFgTransport, the pacer experiment's submission-only arm: the round trip, no
                   // NR
};

// What the host does with a present of this shape. Until stage 4 is proven (no generated frame has been run beside the
// host in a game), never NR beside frame generation; the experiment key only puts the round trip back.
inline FgCarry DecideFg(FgShape shape, bool transportExperiment)
{
    if (shape == FgShape::None)
        return FgCarry::Normal;
    return transportExperiment ? FgCarry::TransportOnly : FgCarry::Untouched;
}

// The device's line, once per device and shape.
inline std::string FgUnsupportedText(FgShape shape, bool transportExperiment)
{
    std::string text = std::string("frame generation active (") + FgShapeName(shape) +
                       "): NR at present is off on this device until stage 4 is proven; ";
    text += transportExperiment ? "its presents keep the round trip without NR ([DlssNr] VulkanPresentFgTransport, the "
                                  "pacer experiment's submission-only arm)"
                                : "its presents go down untouched";
    return text;
}

// --- Streamline's messages --------------------------------------------------------------------------------------

enum class StreamlineKind : uint8_t
{
    Info,
    Warning,
    Error,
    FenceTimeout, // "Wait on gpu fence timed out after 500ms": the marker's recorded failure
    Pacer,        // a warning or error about Streamline's pacer
};

inline bool ContainsNoCase(std::string_view text, std::string_view word)
{
    if (word.empty() || text.size() < word.size())
        return word.empty();

    for (size_t i = 0; i + word.size() <= text.size(); ++i)
    {
        bool same = true;
        for (size_t j = 0; j < word.size() && same; ++j)
            same = std::tolower(static_cast<unsigned char>(text[i + j])) ==
                   std::tolower(static_cast<unsigned char>(word[j]));
        if (same)
            return true;
    }
    return false;
}

// `level`: 0 info, 1 warning, 2 error (the callback's own LogType, mapped at the call site). "timed out" is a timeout
// at any level (the marker's failure: "Wait on gpu fence timed out after 500ms"); "timeout" and "pacer" only in a
// warning or an error (Streamline's info lines name its pacer and its timeouts' settings at start-up).
inline StreamlineKind ClassifyStreamline(int level, std::string_view message)
{
    if (ContainsNoCase(message, "timed out") || (level >= 1 && ContainsNoCase(message, "timeout")))
        return StreamlineKind::FenceTimeout;
    if (level >= 1 && ContainsNoCase(message, "pacer"))
        return StreamlineKind::Pacer;
    if (level >= 2)
        return StreamlineKind::Error;
    if (level == 1)
        return StreamlineKind::Warning;
    return StreamlineKind::Info;
}

// Counted since the process began; the trace reports what changed in each window.
struct StreamlineCounts
{
    uint64_t fenceTimeouts = 0;
    uint64_t pacer = 0;
    uint64_t warnings = 0;
    uint64_t errors = 0;
    std::string last; // the last timeout, pacer message, warning or error, trimmed

    void Add(StreamlineKind kind, std::string_view message)
    {
        switch (kind)
        {
        case StreamlineKind::FenceTimeout:
            ++fenceTimeouts;
            break;
        case StreamlineKind::Pacer:
            ++pacer;
            break;
        case StreamlineKind::Warning:
            ++warnings;
            break;
        case StreamlineKind::Error:
            ++errors;
            break;
        default:
            return;
        }
        last = std::string(message.substr(0, 160));
    }
};

// --- The pacer trace --------------------------------------------------------------------------------------------

inline constexpr uint64_t kTraceWindowMs = 10000;
inline constexpr size_t kTraceThreadsKept = 8; // per window; more are counted together
inline constexpr size_t kTraceQueuesKept = 4;
inline constexpr size_t kTraceResultsKept = 4;

// One present, as the host saw it end to end.
struct TraceSample
{
    uint64_t swapchain = 0;   // the handle
    uint32_t swapchainId = 0; // the host's record, 0 when not on record
    uint64_t device = 0;      // the device's handle (for a swapchain not on record, its queue's), 0 when unknown
    uint32_t deviceId = 0;
    uint64_t nowUs = 0;  // when the present entered the host (a monotonic clock)
    uint64_t callUs = 0; // how long the present took, the host's own work and the call down included
    uint32_t threadId = 0;
    std::string threadName; // GetThreadDescription's, empty when none
    std::string callerName; // the module that called vkQueuePresentKHR
    uint64_t queue = 0;
    uint32_t waits = 0;                // the application's wait semaphores
    bool hostSignal = false;           // the present went down on the host's own semaphore (a round trip)
    bool untouched = false;            // frame generation: carried untouched
    std::optional<uint64_t> presentId; // VkPresentIdKHR, the first swapchain's (0: none was given)
    int32_t result = 0;                // VkResult of the call down
    uint32_t completions = 0;          // the host's round trips seen complete during this present
    uint32_t ring = 0;
    uint64_t waitNs = 0; // CPU time spent waiting for a ring slot
    FgShape fg = FgShape::None;
};

inline const char* ResultName(int32_t result)
{
    switch (result)
    {
    case 0:
        return "VK_SUCCESS";
    case 1000001003:
        return "VK_SUBOPTIMAL_KHR";
    case -1000001004:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case -1000000000:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case -4:
        return "VK_ERROR_DEVICE_LOST";
    case -1000255000:
        return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
    default:
        return nullptr;
    }
}

// The value at fraction `q` of sorted samples (nearest rank), 0 for none.
inline double Percentile(std::vector<double>& values, double q)
{
    if (values.empty())
        return 0.0;
    const size_t rank = std::min(values.size() - 1, static_cast<size_t>(q * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(rank), values.end());
    return values[rank];
}

inline std::string Fixed(double value, int decimals)
{
    char text[48];
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return text;
}

// One swapchain's window.
class TraceWindow
{
  public:
    // Adds a sample. True when the window has run kTraceWindowMs: the caller takes its line (Line) and starts over.
    bool Add(const TraceSample& s)
    {
        if (_presents == 0)
        {
            _startUs = s.nowUs;
            _swapchain = s.swapchain;
        }
        _swapchainId = s.swapchainId != 0 ? s.swapchainId : _swapchainId;
        _deviceId = s.deviceId != 0 ? s.deviceId : _deviceId;
        _endUs = s.nowUs;
        ++_presents;

        if (_lastUs != 0 && s.nowUs >= _lastUs)
            _intervalsMs.push_back(static_cast<double>(s.nowUs - _lastUs) / 1000.0);
        _lastUs = s.nowUs;
        _callsMs.push_back(static_cast<double>(s.callUs) / 1000.0);

        Count(_threads, ThreadKey(s), kTraceThreadsKept);
        Count(_queues, s.queue, kTraceQueuesKept);
        Count(_results, s.result, kTraceResultsKept);
        Count(_shapes, static_cast<uint8_t>(s.fg), 8);

        _waitsMin = std::min(_waitsMin, s.waits);
        _waitsMax = std::max(_waitsMax, s.waits);
        _waits += s.waits;
        _hostSignals += s.hostSignal ? 1 : 0;
        _untouched += s.untouched ? 1 : 0;
        _completions += s.completions;
        _ring = std::max(_ring, s.ring);
        _maxWaitNs = std::max(_maxWaitNs, s.waitNs);

        // VK_KHR_present_id: 0 is no id. A gap is measured from the highest id seen (the previous window's included),
        // so one id out of order is counted once and its successor is not a gap.
        if (s.presentId.has_value() && *s.presentId != 0)
        {
            const uint64_t id = *s.presentId;
            if (!_idFirst.has_value())
                _idFirst = id;
            _idHigh = std::max(_idHigh, id);
            if (_idMax != 0 && id <= _idMax)
                ++_idOutOfOrder;
            else
            {
                if (_idMax != 0 && id > _idMax + 1)
                    ++_idGaps;
                _idMax = id;
            }
            ++_ids;
        }

        return _endUs - _startUs >= kTraceWindowMs * 1000;
    }

    uint64_t Presents() const { return _presents; }
    uint32_t SwapchainId() const { return _swapchainId; }

    // The line, `streamline` being what Streamline said during the window.
    std::string Line(const StreamlineCounts& streamline, const std::string& why)
    {
        const double seconds = static_cast<double>(_endUs - _startUs) / 1e6;
        std::string line = "DLSS-NR Vulkan present host (trace): swapchain " + Hex(_swapchain) +
                           (_swapchainId != 0 ? " #" + std::to_string(_swapchainId) : std::string(" (not on record)")) +
                           (_deviceId != 0 ? " on device #" + std::to_string(_deviceId) : std::string()) + ", " +
                           Fixed(seconds, 1) + " s" + (why.empty() ? "" : " (" + why + ")") + ": " +
                           std::to_string(_presents) + " presents";
        if (seconds > 0.0)
            line += " (" + Fixed(static_cast<double>(_presents) / seconds, 1) + "/s)";

        line += "; threads " + Join(_threads, [](const std::string& key) { return key; });
        line += "; queues " + Join(_queues, [](uint64_t queue) { return Hex(queue); });

        line += "; waits " + std::to_string(_waitsMin == UINT32_MAX ? 0 : _waitsMin) + "-" + std::to_string(_waitsMax) +
                " (" + std::to_string(_waits) + "), host signals " + std::to_string(_hostSignals) + ", untouched " +
                std::to_string(_untouched);

        if (_ids == 0)
            line += "; present ids none";
        else
            line += "; present ids " + std::to_string(*_idFirst) + ".." + std::to_string(_idHigh) + " (gaps " +
                    std::to_string(_idGaps) + ", out of order " + std::to_string(_idOutOfOrder) + ")";

        const double p50 = Percentile(_intervalsMs, 0.50);
        size_t over = 0;
        for (const double interval : _intervalsMs)
            over += p50 > 0.0 && interval > 2.0 * p50 ? 1 : 0;
        if (_intervalsMs.empty())
        {
            line += "; frame time none";
        }
        else
        {
            const double p95 = Percentile(_intervalsMs, 0.95);
            const double p99 = Percentile(_intervalsMs, 0.99);
            const double maxMs = *std::max_element(_intervalsMs.begin(), _intervalsMs.end());
            line += "; frame time p50 " + Fixed(p50, 2) + " p95 " + Fixed(p95, 2) + " p99 " + Fixed(p99, 2) + " max " +
                    Fixed(maxMs, 2) + " ms, over twice the median " + std::to_string(over);
        }

        {
            const double p50Call = Percentile(_callsMs, 0.50);
            const double p99Call = Percentile(_callsMs, 0.99);
            const double maxCall = _callsMs.empty() ? 0.0 : *std::max_element(_callsMs.begin(), _callsMs.end());
            line += "; present call p50 " + Fixed(p50Call, 3) + " p99 " + Fixed(p99Call, 3) + " max " +
                    Fixed(maxCall, 3) + " ms";
        }

        line +=
            "; results " + Join(_results,
                                [](int32_t result)
                                {
                                    const char* name = ResultName(result);
                                    return name != nullptr ? std::string(name) : "VkResult " + std::to_string(result);
                                });

        line += "; GPU completions " + std::to_string(_completions) + ", ring " + std::to_string(_ring) +
                ", max CPU wait " + Fixed(static_cast<double>(_maxWaitNs) / 1e6, 3) + " ms";

        line += "; frame generation " +
                Join(_shapes, [](uint8_t shape) { return std::string(FgShapeShort(static_cast<FgShape>(shape))); });

        line += "; Streamline fence timeouts " + std::to_string(streamline.fenceTimeouts) + ", pacer " +
                std::to_string(streamline.pacer) + ", warnings " + std::to_string(streamline.warnings) + ", errors " +
                std::to_string(streamline.errors);
        if (!streamline.last.empty())
            line += " (last: " + streamline.last + ")";

        Reset();
        return line;
    }

    static std::string Hex(uint64_t value)
    {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
        return text;
    }

  private:
    template <typename K> struct Counted
    {
        std::vector<std::pair<K, uint64_t>> items; // in order of first appearance
        uint64_t others = 0;
    };

    template <typename K> static void Count(Counted<K>& counted, const K& key, size_t kept)
    {
        for (auto& [k, count] : counted.items)
        {
            if (k == key)
            {
                ++count;
                return;
            }
        }
        if (counted.items.size() < kept)
            counted.items.emplace_back(key, 1);
        else
            ++counted.others;
    }

    // "a 3, b 2, others 1", or "none".
    template <typename K, typename Name> static std::string Join(const Counted<K>& counted, Name name)
    {
        std::string text;
        for (const auto& [key, count] : counted.items)
            text += (text.empty() ? "" : ", ") + name(key) + " " + std::to_string(count);
        if (counted.others != 0)
            text += (text.empty() ? "" : ", ") + std::string("others ") + std::to_string(counted.others);
        return text.empty() ? std::string("none") : text;
    }

    static std::string ThreadKey(const TraceSample& s)
    {
        std::string key = "tid " + std::to_string(s.threadId);
        if (!s.threadName.empty())
            key += " \"" + s.threadName + "\"";
        key += " (caller " + (s.callerName.empty() ? std::string("?") : s.callerName) + ")";
        return key;
    }

    void Reset()
    {
        const uint64_t last = _lastUs;
        const uint64_t swapchain = _swapchain;
        const uint32_t id = _swapchainId;
        const uint32_t device = _deviceId;
        const uint64_t idMax = _idMax;
        *this = TraceWindow {};
        // The first interval of the next window is measured from this window's last present, its first present id
        // from this window's highest.
        _lastUs = last;
        _swapchain = swapchain;
        _swapchainId = id;
        _deviceId = device;
        _idMax = idMax;
    }

    uint64_t _swapchain = 0;
    uint32_t _swapchainId = 0;
    uint32_t _deviceId = 0;
    uint64_t _startUs = 0;
    uint64_t _endUs = 0;
    uint64_t _lastUs = 0;
    uint64_t _presents = 0;
    std::vector<double> _intervalsMs;
    std::vector<double> _callsMs;
    Counted<std::string> _threads;
    Counted<uint64_t> _queues;
    Counted<int32_t> _results;
    Counted<uint8_t> _shapes;
    uint32_t _waitsMin = UINT32_MAX;
    uint32_t _waitsMax = 0;
    uint64_t _waits = 0;
    uint64_t _hostSignals = 0;
    uint64_t _untouched = 0;
    uint64_t _completions = 0;
    uint32_t _ring = 0;
    uint64_t _maxWaitNs = 0;
    std::optional<uint64_t> _idFirst; // this window's first id
    uint64_t _idHigh = 0;             // this window's highest id
    uint64_t _idMax = 0;              // the highest id seen on the swapchain, carried across windows
    uint64_t _ids = 0;
    uint64_t _idGaps = 0;
    uint64_t _idOutOfOrder = 0;
};

// Every swapchain's window, by device and handle (handle values may repeat across devices), and Streamline's counts as
// of each swapchain's last line.
class PacerTrace
{
  public:
    // A present; a line when its swapchain's window is over. `streamline`: the process's counts now.
    std::optional<std::string> Add(const TraceSample& sample, const StreamlineCounts& streamline)
    {
        // A swapchain's first window counts Streamline's messages from its own first present, not from the process's.
        const auto [found, inserted] = _windows.try_emplace(Key { sample.device, sample.swapchain });
        Entry& entry = found->second;
        if (inserted)
        {
            entry.seen = streamline;
            entry.seen.last.clear();
        }
        if (!entry.window.Add(sample))
            return std::nullopt;
        return Take(entry, streamline, {});
    }

    // The swapchain is going: its last, partial window, if it presented since its last line.
    std::optional<std::string> Flush(uint64_t device, uint64_t swapchain, const StreamlineCounts& streamline)
    {
        const auto found = _windows.find(Key { device, swapchain });
        if (found == _windows.end())
            return std::nullopt;
        std::optional<std::string> line;
        if (found->second.window.Presents() != 0)
            line = Take(found->second, streamline, "destroyed");
        _windows.erase(found);
        return line;
    }

    // A swapchain was made on the device: the windows that cannot have outlived it end -- one left under the same
    // handle (its destroy was not seen), and the device's swapchains not on record (FSR-FG's proxy is made, and
    // replaced, together with FFX's own swapchain, and its own creation and destruction never reach the host).
    std::vector<std::string> FlushOnCreate(uint64_t device, uint64_t swapchain, const StreamlineCounts& streamline)
    {
        std::vector<std::string> lines;
        for (auto it = _windows.begin(); it != _windows.end();)
        {
            const bool same = it->first.device == device && it->first.swapchain == swapchain;
            const bool unrecorded = it->first.device == device && it->second.window.SwapchainId() == 0;
            if (!same && !unrecorded)
            {
                ++it;
                continue;
            }
            if (it->second.window.Presents() != 0)
                lines.push_back(
                    Take(it->second, streamline, same ? "its handle made again" : "a swapchain made on its device"));
            it = _windows.erase(it);
        }
        return lines;
    }

    // The device is going: the last, partial windows of its swapchains, those not on record included, and of any
    // swapchain whose device was never known.
    std::vector<std::string> FlushDevice(uint64_t device, const StreamlineCounts& streamline)
    {
        std::vector<std::string> lines;
        for (auto it = _windows.begin(); it != _windows.end();)
        {
            if (it->first.device != device && it->first.device != 0)
            {
                ++it;
                continue;
            }
            if (it->second.window.Presents() != 0)
                lines.push_back(Take(it->second, streamline, "device destroyed"));
            it = _windows.erase(it);
        }
        return lines;
    }

    size_t Swapchains() const { return _windows.size(); }

  private:
    struct Key
    {
        uint64_t device = 0;
        uint64_t swapchain = 0;
        bool operator<(const Key& other) const
        {
            return device != other.device ? device < other.device : swapchain < other.swapchain;
        }
    };

    struct Entry
    {
        TraceWindow window;
        StreamlineCounts seen; // the process's counts at this swapchain's last line
    };

    // The counts only grow, but two presents may hand theirs in out of order: an older copy adds nothing.
    static uint64_t Since(uint64_t now, uint64_t seen) { return now > seen ? now - seen : 0; }

    static std::string Take(Entry& entry, const StreamlineCounts& now, const std::string& why)
    {
        StreamlineCounts delta;
        delta.fenceTimeouts = Since(now.fenceTimeouts, entry.seen.fenceTimeouts);
        delta.pacer = Since(now.pacer, entry.seen.pacer);
        delta.warnings = Since(now.warnings, entry.seen.warnings);
        delta.errors = Since(now.errors, entry.seen.errors);
        if (delta.fenceTimeouts + delta.pacer + delta.warnings + delta.errors != 0)
            delta.last = now.last;
        entry.seen.fenceTimeouts = std::max(entry.seen.fenceTimeouts, now.fenceTimeouts);
        entry.seen.pacer = std::max(entry.seen.pacer, now.pacer);
        entry.seen.warnings = std::max(entry.seen.warnings, now.warnings);
        entry.seen.errors = std::max(entry.seen.errors, now.errors);
        entry.seen.last.clear();
        return entry.window.Line(delta, why);
    }

    std::map<Key, Entry> _windows;
};
} // namespace DlssNr::VkPresentHost
