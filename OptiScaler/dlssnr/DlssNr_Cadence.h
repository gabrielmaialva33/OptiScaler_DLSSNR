#pragma once

#include <cstddef>
#include <cstdint>

// Model cadence's scheduling: which frames run the model, which carry its last edit, and why.
// design/model-cadence.md is the reasoning.
//
// Pure: flags, sizes, ids and times in, a decision out, so tests/nr-cadence pins it without a device. The
// renderer (DlssNr_Dx12::Dispatch) asks once per call and then tells the scheduler what actually happened --
// the edit was recorded, the frame was carried, or nothing usable is stored any more.
namespace DlssNr::Cadence
{
constexpr uint32_t kMaxCadence = 4;

// A gap longer than this since the pass last ran means the stored edit no longer describes the screen: the
// pass or the cadence was off, the stage moved, the game paused. The stabilizer and the UI mask use the same.
constexpr int64_t kGapMs = 250;

// The carry's fixed numbers, as BeliyG3 fixed everything but N. A relative depth mismatch from 0.05 rejects,
// fully at twice that; the colour gate's tolerance is the display-referred one (the proxy is an encoded,
// display-referred picture); the coarse edit averages 16x16 blocks.
constexpr float kDepthTolerance = 0.05f;
constexpr float kColourTolerance = 0.08f;
constexpr uint32_t kLowBlock = 16;

// BeliyG3's taps, rings and edge ramp are native pixels; the carry runs at the model's working size.
constexpr float TapScale(uint32_t workWidth, uint32_t frameWidth)
{
    const float scale = frameWidth != 0 ? (float) workWidth / (float) frameWidth : 1.0f;
    return scale < 1.0f / 3.0f ? 1.0f / 3.0f : (scale > 2.0f ? 2.0f : scale);
}

// The coarse edit's extent for a working extent.
constexpr uint32_t LowExtent(uint32_t workExtent) { return (workExtent + kLowBlock - 1) / kLowBlock; }

// The configured cadence as the pass uses it. 1 is the model every frame, which is off; 2..4 are allowed.
// Anything else -- 0, a hand-edited 9 -- reads as off rather than as the nearest legal value, so a typo
// never turns on an option that costs stability.
constexpr uint32_t Requested(uint32_t configured)
{
    return configured >= 2 && configured <= kMaxCadence ? configured : 1;
}

enum class Reason : uint8_t
{
    None,

    // The cadence is running.
    Carried,   // this frame carries the last edit
    Scheduled, // the model's turn: the cadence's age was reached

    // A forced model frame. The stored edit is dropped; the cadence resumes once the model has run.
    NoEdit,      // nothing stored: the first frame, or after a drop
    Reset,       // a reset is pending -- held until a model frame runs, never lost on a carried one
    Changed,     // size, format, route, what the model reads, or the feature itself changed
    Gap,         // more than kGapMs since the pass last ran
    SamePresent, // a second call in one present: one view's edit must not land on another

    // A refusal: the model runs every frame for as long as the condition holds.
    Off,
    NativeVulkan,
    DriverProxy,
    Unavailable,    // this build carries no cadence bytecode
    NoPresentCount, // the route never presents through the wrapped swapchain, so presents are not counted
    BeforeUpscale,
    NoGameGuides,
    FrameGeneration,
    FrameHold,
    Capture,
    NoSurfaces,        // the surfaces could not be allocated
    ReversibleReplace, // model cadence does not run with replace curves (causes highlight flashing)
};

constexpr bool IsRefusal(Reason reason) { return reason >= Reason::Off; }
constexpr bool IsForced(Reason reason) { return reason >= Reason::NoEdit && reason < Reason::Off; }

// What decides whether the cadence may run at all, gathered by the caller.
struct Inputs
{
    uint32_t requested = 1;         // Requested(config)
    bool nativeVulkan = false;      // the D3D12 pass never sees this; the menu and the tests do
    bool driverProxy = false;       // [DlssNr] UseProxy: it returns before the resolve
    bool available = true;          // the bytecode is in this build
    bool presentsCounted = true;    // State::frameCount advances: the route presents through the wrapped swapchain
    bool beforeUpscale = false;     // stage 1: jittered input the upscaler would accumulate
    bool gameGuides = true;         // the game's own depth and motion, not zero or synthesized guides
    bool frameGeneration = false;   // any frame generation active
    bool allowWithFrameGen = false; // [DlssNr] CadenceWithFrameGen
    bool hold = false;              // Hold frame on, or a hold being released this frame
    bool capture = false;           // a matched before/after capture is running
    bool surfaces = true;           // the cadence's surfaces exist
    bool reversibleReplace = false; // ReversibleMode 2 or 4 (replace curves flash under carried edits)
};

// Why the cadence cannot run, or None. When several hold, the first one a user can do least about is named.
constexpr Reason Refusal(const Inputs& in)
{
    if (in.requested <= 1)
        return Reason::Off;
    if (in.nativeVulkan)
        return Reason::NativeVulkan;
    if (in.driverProxy)
        return Reason::DriverProxy;
    if (!in.available)
        return Reason::Unavailable;
    // Age, and a second call in one present, are both read off the present counter. Where it never advances,
    // every call after the first would look like a second call in one present: refused under its own name
    // instead, rather than counting calls, which would let one view's edit land on another.
    if (!in.presentsCounted)
        return Reason::NoPresentCount;
    if (in.beforeUpscale)
        return Reason::BeforeUpscale;
    if (!in.gameGuides)
        return Reason::NoGameGuides;
    if (in.reversibleReplace)
        return Reason::ReversibleReplace;
    if (in.frameGeneration && !in.allowWithFrameGen)
        return Reason::FrameGeneration;
    if (in.hold)
        return Reason::FrameHold;
    if (in.capture)
        return Reason::Capture;
    if (!in.surfaces)
        return Reason::NoSurfaces;
    return Reason::None;
}

// What the renderer does with the surfaces, given the refusal worked out as if they existed.
//
// Built only when the cadence would run, so a title that cannot carry -- zero guides, before the upscaler --
// never pays for them. Given back when the cadence is off or cannot exist here (native Vulkan, the driver
// proxy, a build without the shader): off allocates nothing. Every other refusal keeps what is there, so a
// route, a frame-generation toggle or a hold that comes and goes does not reallocate ~130 MB each time; each
// discarded copy would sit in the retired list for 32 evaluates. A failed allocation keeps its failed object,
// so it is not retried and logged every frame.
constexpr bool BuildsSurfaces(Reason refusal) { return refusal == Reason::None; }

constexpr bool ReleasesSurfaces(Reason refusal)
{
    return refusal == Reason::Off || refusal == Reason::NativeVulkan || refusal == Reason::DriverProxy ||
           refusal == Reason::Unavailable;
}

// The menu's line, and the log's. String literals, so the pointer outlives everything. Forced reasons are
// transient and say what happened to one frame; the renderer does not publish them as the status.
constexpr const char* Describe(Reason reason, uint32_t cadence)
{
    switch (reason)
    {
    case Reason::Carried:
    case Reason::Scheduled:
        return cadence >= 4   ? "Running the model every 4th frame; the frames between carry its last edit."
               : cadence == 3 ? "Running the model every 3rd frame; the frames between carry its last edit."
                              : "Running the model every 2nd frame; the frame between carries its last edit.";
    case Reason::NoEdit:
        return "The model runs this frame: no edit is stored to carry.";
    case Reason::Reset:
        return "The model runs this frame: a history reset is pending.";
    case Reason::Changed:
        return "The model runs this frame: its size, format, route or settings changed.";
    case Reason::Gap:
        return "The model runs this frame: the pass did not run for a while.";
    case Reason::SamePresent:
        return "The model runs every frame: the pass ran more than once in one present.";
    case Reason::Off:
        return "Off: the model runs every frame.";
    case Reason::NativeVulkan:
        return "The model runs every frame: model cadence is not available on native Vulkan.";
    case Reason::DriverProxy:
        return "The model runs every frame: model cadence does not run with the driver proxy.";
    case Reason::Unavailable:
        return "The model runs every frame: this build has no model cadence shader.";
    case Reason::ReversibleReplace:
        return "The model runs every frame: model cadence does not run with replace curves.";
    case Reason::NoPresentCount:
        return "The model runs every frame: this route does not present through OptiScaler's swapchain, so model "
               "cadence cannot count frames.";
    case Reason::BeforeUpscale:
        return "The model runs every frame: model cadence does not run before the upscaler.";
    case Reason::NoGameGuides:
        return "The model runs every frame: this frame has no game depth and motion to carry the edit with.";
    case Reason::FrameGeneration:
        return "The model runs every frame: frame generation is active (see Allow with frame generation).";
    case Reason::FrameHold:
        return "The model runs every frame while Hold frame is on.";
    case Reason::Capture:
        return "The model runs every frame while a capture is running.";
    case Reason::NoSurfaces:
        return "The model runs every frame: the model cadence surfaces could not be allocated.";
    case Reason::None:
        break;
    }
    return "Waiting for the pass to run.";
}

// Everything whose change makes a stored edit describe another picture. The caller folds what the model
// reads -- preset, style, intensity, structure, tone, skin, auto mask, the passes, the colour transform,
// the reversible proxy, the supersampling filter, UI protection, guide matching, the motion-scale rule, the
// game's depth orientation and vector scale -- into settings with Mix, and counts feature creations.
// Composition sliders stay out: the edit is taken before composition and the resolve reruns every frame.
struct Signature
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t workWidth = 0;
    uint32_t workHeight = 0;
    uint32_t format = 0;
    uint32_t route = 0; // Route()
    uint64_t settings = 0;
    uint64_t feature = 0;

    constexpr bool operator==(const Signature&) const = default;
};

constexpr uint32_t Route(bool beforeUpscale, bool afterRayReconstruction, bool presentSource)
{
    return (beforeUpscale ? 1u : 0u) | (afterRayReconstruction ? 2u : 0u) | (presentSource ? 4u : 0u);
}

// FNV-1a over the bytes of a value, chained.
constexpr uint64_t kMixSeed = 14695981039346656037ull;

inline uint64_t Mix(uint64_t hash, const void* bytes, size_t size)
{
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

template <class T> uint64_t Mix(uint64_t hash, const T& value) { return Mix(hash, &value, sizeof(value)); }

struct Decision
{
    bool runModel = true;
    bool handChain = false;  // a scheduled model frame after carried ones: hand the model the summed chain
    bool chainStart = false; // the first carried frame since a model frame: the chain starts from this frame
    bool record = false;     // a model frame whose edit is to be stored for the frames that carry it
    Reason reason = Reason::None;
    uint32_t cadence = 1; // what is in effect: 1 whenever the model runs every frame because of a refusal
    uint32_t age = 0;     // presents the pass ran at since the last model frame, this one included
};

// Age is counted in presents the pass ran at, never by parity. A raw present delta would be wrong: the
// present counter advances on generated frames too, so with frame generation every rendered frame would be
// two or more presents old and the model would run every frame. A dropped call shifts the pattern by one
// frame; it cannot flip it.
class Scheduler
{
    Signature _signature {};
    uint64_t _lastPresent = 0;
    int64_t _lastMs = 0;
    uint32_t _carried = 0; // carried frames since the last model frame
    bool _seen = false;
    bool _edit = false; // an edit is stored, from the last model frame

  public:
    // One call per Dispatch. resetPending is anything that will reset the model's history this frame -- the
    // game's flag, a tuning pulse, a released hold, a route switch, an extra pass's own reset.
    Decision Decide(const Inputs& in, const Signature& signature, uint64_t present, int64_t nowMs, bool resetPending)
    {
        const bool samePresent = _seen && present == _lastPresent;
        const bool gap = _seen && nowMs - _lastMs > kGapMs;
        const bool changed = _seen && !(signature == _signature);

        _seen = true;
        _lastPresent = present;
        _lastMs = nowMs;
        _signature = signature;

        Decision d;
        d.cadence = in.requested;

        const Reason refusal = Refusal(in);
        if (refusal != Reason::None)
        {
            Drop();
            d.reason = refusal;
            d.cadence = 1;
            return d;
        }

        const Reason forced = samePresent    ? Reason::SamePresent
                              : gap          ? Reason::Gap
                              : changed      ? Reason::Changed
                              : resetPending ? Reason::Reset
                              : !_edit       ? Reason::NoEdit
                                             : Reason::None;
        if (forced != Reason::None)
        {
            Drop();
            d.reason = forced;
            d.age = 0;
            // A second call in one present is another view: its edit must not be carried onto the first one's
            // next frame, so it is not stored, and the next present runs the model again.
            d.record = forced != Reason::SamePresent;
            return d;
        }

        d.age = _carried + 1;
        if (d.age >= in.requested)
        {
            d.reason = Reason::Scheduled;
            d.handChain = _carried > 0;
            d.record = true;
            return d;
        }

        d.runModel = false;
        d.reason = Reason::Carried;
        d.chainStart = _carried == 0;
        return d;
    }

    // The model ran -- every requested pass -- and its edit is stored: the cadence starts counting from here.
    void Recorded()
    {
        _edit = true;
        _carried = 0;
    }

    // A carried frame was recorded.
    void Carried() { ++_carried; }

    // Nothing usable is stored: a failed or partial model frame, a failed carry, any forced frame.
    void Drop()
    {
        _edit = false;
        _carried = 0;
    }

    bool HasEdit() const { return _edit; }
    uint32_t CarriedSinceModel() const { return _carried; }
};
} // namespace DlssNr::Cadence
