#pragma once

// The game's own depth and motion, from its native D3D11 DLSS, carried to the present pass on the D3D11-to-D3D12
// bridge (docs/dlssnr/design/dx11-native-dlss-guides.md, option (c), "Step 3 as built").
//
// A D3D11 game that calls DLSS and keeps the driver's own D3D11 DLSS (Dx11Upscaler=dlss) never hands its guides to a
// D3D12 list, so with HookMethod=2 the bridge's present host ran on zero or synthesized guides. Here, right after the
// native evaluate, the depth and motion are copied into the bridge's shared textures (upstream's Dx11WithDx12 cache,
// the transport dlss_12 uses, or for a depth-stencil a copy of the transport's own: DepthCopy), the bridge's D3D12
// queue is made to wait for those copies on the GPU, and a D3D12 list
// on that queue copies them into the present guide ring, stamped like any capture. The present host then takes them,
// or not, through the identity rules of present-real-guides.md.
//
// The decisions are here, standard library only, so tests/nr-dx11-native-guides compiles this header as it is:
//
//   Decide        whether this evaluate is carried, and if not, why (a refusal is a reason, never a guess)
//   DepthCopy     which copy carries the depth across: upstream's, or one of the transport's own without the
//                 depth-stencil bind, which is the only one D3D12 is seen to read correctly under Proton
//   RunsShader    whether that copy runs upstream's depth conversion on the game's context, which then has its
//                 bindings saved and restored
//   SlotFree      the transport's slot rotation: a slot is written again only once the GPU has read it
//   CacheFree     whether upstream's cache may be prepared, which releases every set when the devices have changed
//   NoHost        NR, HookMethod 2 and native DLSS with no present host to carry to: said once in the log
//   Window        one log line per window with what was carried and what was refused
//
// With NR off, HookMethod other than 2, any upscaler but native DLSS, or no bridge present host, Decide answers
// NotWanted before anything else is looked at, and the caller returns without touching the device, the context or
// the parameters: the evaluate is exactly what it was before this header existed.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace DlssNr::Dx11Guides
{
// Shared texture sets the transport rotates through. A set is written by D3D11 only once the D3D12 list that last read
// it has completed (checked on the CPU, never waited for on the GPU): four, like the bridge's own input sets.
constexpr size_t kSlots = 4;

enum class Reason : uint8_t
{
    Carried,       // copied into the present guide ring
    NotWanted,     // NR off, HookMethod not 2, not native DLSS, or no present host: nothing is looked at
    NotEvaluated,  // the native evaluate failed: its guides are not this frame's picture
    Deferred,      // a deferred context: its commands run when the game says, not before the present
    NoDepth,       // the evaluate named no depth, or not a D3D11 2D texture
    NoMotion,      // the same for motion
    Shape,         // a guide is multisampled, an array, or has mips the copy would not carry whole
    NoOutput,      // no output texture to say what picture the guides belong to
    NotPresented,  // the output is not the size of the frame the bridge presents (another view of the game)
    SlotInFlight,  // the set due next is still being read by the GPU: refused rather than waited for
    AlreadyShared, // the game's own texture is shared already, so the bridge would read it live, racing the game
    Transport,     // the bridge could not make or open a shared texture, or sync it
    D3D12,         // the bridge's D3D12 list could not be recorded or submitted
    Ring,          // the present guide ring did not take it (no slot free, allocation or tracking refused, or NR
                   // switched off for the session after a fault inside the model)
    DepthStencil,  // a depth-stencil depth with a stencil plane, shared through NT handles: no copy here is safe
};
constexpr size_t kReasons = 15;

inline const char* ReasonName(Reason reason)
{
    switch (reason)
    {
    case Reason::Carried:
        return "carried";
    case Reason::NotWanted:
        return "not wanted";
    case Reason::NotEvaluated:
        return "evaluate failed";
    case Reason::Deferred:
        return "deferred context";
    case Reason::NoDepth:
        return "no depth";
    case Reason::NoMotion:
        return "no motion";
    case Reason::Shape:
        return "shape";
    case Reason::NoOutput:
        return "no output";
    case Reason::NotPresented:
        return "not the presented size";
    case Reason::SlotInFlight:
        return "slot in flight";
    case Reason::AlreadyShared:
        return "already shared";
    case Reason::Transport:
        return "transport";
    case Reason::D3D12:
        return "d3d12";
    case Reason::Ring:
        return "ring";
    case Reason::DepthStencil:
        return "depth-stencil";
    }
    return "?";
}

// A D3D11 texture as far as the transport cares. Zero dimension means "not a 2D texture".
struct Texture
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    uint32_t arraySize = 0;
    uint32_t sampleCount = 0;
    uint32_t format = 0; // DXGI_FORMAT
    bool present = false;
    bool texture2D = false;
    bool depthStencil = false; // bound as a depth-stencil (D3D11_BIND_DEPTH_STENCIL)
};

struct Inputs
{
    bool nrEnabled = false;
    uint32_t hookMethod = 1;       // [DlssNr] HookMethod as configured
    bool nativeDlss = false;       // the feature is the driver's own D3D11 DLSS (Upscaler::DLSS)
    bool presentHost = false;      // the D3D11 bridge hosts the pass at present (built at swapchain creation)
    bool evaluated = false;        // the native evaluate succeeded
    bool immediateContext = false; // the evaluate's context is the device's immediate one
    Texture depth {};
    Texture motion {};
    Texture output {};
    uint32_t presentedWidth = 0; // the frame the bridge presented last; zero before its first present
    uint32_t presentedHeight = 0;
    bool ntHandles = false; // [Dx11withDx12] DontUseNTShared=false: the bridge shares through NT handles
};

// The guides fit the copy: one 2D slice, single-sampled. Mips are allowed (the copy takes the whole resource, and the
// ring keeps only mip 0), but a texture with none is not a texture.
inline bool Copyable(const Texture& t)
{
    return t.present && t.texture2D && t.width != 0 && t.height != 0 && t.arraySize == 1 && t.sampleCount == 1 &&
           t.mipLevels >= 1;
}

// How the depth crosses. Upstream's copy path (Dx11WithDx12::CopyTextureFrom11To12) shares a copy with the source's
// own bind flags, except typeless formats with a stencil plane, which its DepthTransfer_Dx11 pass converts into a
// plain texture first (legacy handles only). Under GE-Proton11-7, D3D12 never sees D3D11's copy into a shared texture
// that keeps D3D11_BIND_DEPTH_STENCIL: zero frames of six with the shared fence, zero with a CPU wait for D3D11 as
// well, and every frame only once D3D11 accesses the shared texture again after its copy; the same depth shared as a
// plain shader resource crossed all six (tests/dx11-guides-transport). So a depth bound as a depth-stencil is never
// handed to upstream's plain copy: the transport copies it into a shader-resource-only texture of its own, and shares
// that.
enum class DepthRoute : uint8_t
{
    Upstream, // not a depth-stencil, or one upstream converts (DepthTransfer): upstream's copy path
    OwnCopy,  // a depth-stencil without a stencil plane: a shader-resource-only copy of the transport's own
    Refuse,   // a depth-stencil with a stencil plane over NT handles, which upstream would share as it is
};

// DXGI_FORMAT values, so the header stays standard-library only.
constexpr uint32_t kR32G8X24Typeless = 19;
constexpr uint32_t kD32FloatS8X24Uint = 20;
constexpr uint32_t kR32Typeless = 39;
constexpr uint32_t kD32Float = 40;
constexpr uint32_t kR24G8Typeless = 44;
constexpr uint32_t kD24UnormS8Uint = 45;
constexpr uint32_t kR16Typeless = 53;
constexpr uint32_t kD16Unorm = 55;

inline bool HasStencil(uint32_t format)
{
    return format == kR32G8X24Typeless || format == kD32FloatS8X24Uint || format == kR24G8Typeless ||
           format == kD24UnormS8Uint;
}

inline DepthRoute DepthCopy(const Texture& depth, bool ntHandles)
{
    if (!depth.depthStencil)
        return DepthRoute::Upstream;

    if (HasStencil(depth.format))
    {
        // Upstream's DepthTransfer takes exactly the two typeless ones, and only over legacy handles.
        const bool transferred = !ntHandles && (depth.format == kR24G8Typeless || depth.format == kR32G8X24Typeless);
        return transferred ? DepthRoute::Upstream : DepthRoute::Refuse;
    }

    return DepthRoute::OwnCopy;
}

// Whether upstream's copy of this depth runs DepthTransfer_Dx11, a compute pass on the game's context: the two typeless
// formats with a stencil plane, over legacy handles (Dx11WithDx12::CopyTextureFrom11To12), whatever the bind. The
// only step of the transport that changes a binding, so the only one that saves and restores them.
inline bool RunsShader(const Texture& depth, bool ntHandles)
{
    return DepthCopy(depth, ntHandles) == DepthRoute::Upstream && !ntHandles &&
           (depth.format == kR24G8Typeless || depth.format == kR32G8X24Typeless);
}

// The format of the transport's own copy: the source's family, typeless where the source is typed (a typed
// depth-stencil format cannot be a shader resource), so the D3D12 side and the present pass read it as they read a
// D3D12 game's typeless depth.
inline uint32_t OwnCopyFormat(uint32_t format)
{
    switch (format)
    {
    case kD32Float:
        return kR32Typeless;
    case kD16Unorm:
        return kR16Typeless;
    default:
        return format;
    }
}

// Whether this evaluate's guides are carried, before any device work. Carried means "go on": the transport can still
// refuse later (SlotInFlight, AlreadyShared, Transport, D3D12, Ring), each with its own reason.
inline Reason Decide(const Inputs& in)
{
    if (!in.nrEnabled || in.hookMethod != 2 || !in.nativeDlss || !in.presentHost)
        return Reason::NotWanted;

    if (!in.evaluated)
        return Reason::NotEvaluated;

    if (!in.immediateContext)
        return Reason::Deferred;

    if (!in.depth.present || !in.depth.texture2D)
        return Reason::NoDepth;

    if (!in.motion.present || !in.motion.texture2D)
        return Reason::NoMotion;

    if (!Copyable(in.depth) || !Copyable(in.motion))
        return Reason::Shape;

    if (DepthCopy(in.depth, in.ntHandles) == DepthRoute::Refuse)
        return Reason::DepthStencil;

    if (!in.output.present || !in.output.texture2D || in.output.width == 0 || in.output.height == 0)
        return Reason::NoOutput;

    // Another view of the game (a portrait, a scene capture, a second window's view) is not the picture the present
    // pass edits. Rule 2 of the identity would refuse it at the present; carrying it would also make it the ring's
    // newest capture, and the newest is what the present is offered, so the main view's capture would be lost with it.
    // Before the bridge's first present the size is unknown, and the identity's rule 2 decides.
    if (in.presentedWidth != 0 && in.presentedHeight != 0 &&
        (in.output.width != in.presentedWidth || in.output.height != in.presentedHeight))
    {
        return Reason::NotPresented;
    }

    return Reason::Carried;
}

// The transport's rotation. completed: the value the transport's D3D12 fence has reached; lastRead: the value it
// signalled after the list that last read this slot (zero for never). A slot is free when that list has completed.
inline bool SlotFree(uint64_t lastRead, uint64_t completed) { return lastRead == 0 || completed >= lastRead; }

// Upstream's Dx11WithDx12::PrepareUpscalerResources releases every set of a cache whose generation is not the current
// one (Dx11WithDx12::Init bumps it when the D3D11 or D3D12 device or context changes), whichever set it was asked to
// prepare. So a stale cache is prepared only once every list the transport submitted has completed: until then the
// frame is refused as a set in flight. fenceValue: the last value the transport signalled.
inline bool CacheFree(uint64_t cacheGeneration, uint64_t currentGeneration, uint64_t completed, uint64_t fenceValue)
{
    return cacheGeneration == currentGeneration || completed >= fenceValue;
}

// NR on, HookMethod 2 and native DLSS, but no present host: the bridge decides at swapchain creation whether it hosts
// the pass, so there is nothing to carry the guides to. Not a refusal (Decide answers NotWanted and nothing is looked
// at); the step 1 report points at the log for why the guides are missing, and this is the line it finds.
inline bool NoHost(const Inputs& in) { return in.nrEnabled && in.hookMethod == 2 && in.nativeDlss && !in.presentHost; }

inline constexpr const char* kNoHostText =
    "DLSS-NR native D3D11 guides: not carried: no D3D11 bridge hosts the pass at present in this session. The bridge "
    "is built when the game creates its swapchain with [DlssNr] Enabled=true and HookMethod=2, and not beside frame "
    "generation fed by the upscaler; after changing either, restart the game.";

// ---------------------------------------------------------------------------------------------------------------
// The log: one line per window, in the style of the identity's, so a run can be read from the log alone.
// ---------------------------------------------------------------------------------------------------------------

using Clock = std::chrono::steady_clock;
constexpr Clock::duration kWindow = std::chrono::seconds(2);
constexpr Clock::duration kSteadyWindow = std::chrono::seconds(30);

struct Counts
{
    uint32_t evaluates = 0;                    // evaluates that were wanted (NotWanted is not counted)
    std::array<uint32_t, kReasons> reasons {}; // by answer, Carried included
    Texture depth {};                          // the last carried evaluate's guides and output, for the line
    Texture motion {};
    Texture output {};
    bool haveCarried = false;
    Texture lastDepth {}; // the last evaluate's, whatever its answer: what a window with no carry shows
    Texture lastMotion {};
    Texture lastOutput {};
};

inline std::string TextureText(const Texture& t)
{
    return "format " + std::to_string(t.format) + " " + std::to_string(t.width) + "x" + std::to_string(t.height);
}

inline std::string FormatLine(const Counts& counts, Clock::duration elapsed)
{
    const auto n = [](uint32_t value) { return std::to_string(value); };
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const uint32_t carried = counts.reasons[static_cast<size_t>(Reason::Carried)];

    std::string line = "DLSS-NR native D3D11 guides: " + n(counts.evaluates) + " evaluates in " +
                       std::to_string(ms / 1000) + "." + std::to_string(ms % 1000 / 100) + " s | " + n(carried) +
                       " carried to the present pass";

    std::string refused;
    uint32_t refusedTotal = 0;
    for (size_t r = static_cast<size_t>(Reason::NotEvaluated); r < kReasons; ++r)
    {
        if (counts.reasons[r] == 0)
            continue;

        refusedTotal += counts.reasons[r];
        refused += refused.empty() ? " (" : ", ";
        refused += std::string(ReasonName(static_cast<Reason>(r))) + " " + n(counts.reasons[r]);
    }

    line += ", " + n(refusedTotal) + " not" + (refused.empty() ? "" : refused + ")");

    if (counts.haveCarried)
    {
        line += " | depth " + TextureText(counts.depth) + ", motion " + TextureText(counts.motion) + ", output " +
                n(counts.output.width) + "x" + n(counts.output.height);
    }
    else if (carried == 0 && counts.evaluates != 0)
    {
        // Nothing carried: the last refused evaluate's guides, so a refusal that depends on them (a format the
        // transport could not share, a depth-stencil it would not copy) says which.
        line += " | last refused: depth " + TextureText(counts.lastDepth) + ", motion " +
                TextureText(counts.lastMotion) + ", output " + n(counts.lastOutput.width) + "x" +
                n(counts.lastOutput.height);
    }

    return line;
}

// Which answers occurred: what makes a window look like the last one written.
inline uint32_t Shape(const Counts& counts)
{
    uint32_t shape = 0;
    for (size_t r = 0; r < kReasons; ++r)
        shape |= (counts.reasons[r] != 0 ? 1u : 0u) << r;
    return shape;
}

// One evaluate's answer; true when a line is due, written to *line. A window lasts at least kWindow; one that looks
// like the last line written is held to kSteadyWindow and carries its whole interval. NotWanted is never counted, so
// a title on another route writes nothing.
class Window
{
    Counts _counts {};
    Clock::time_point _start {};
    bool _started = false;
    uint32_t _writtenShape = 0;
    bool _haveWritten = false;

  public:
    bool Note(Reason reason, const Inputs& in, Clock::time_point now, std::string* line)
    {
        if (reason == Reason::NotWanted)
            return false;

        if (!_started)
        {
            _start = now;
            _started = true;
        }

        ++_counts.evaluates;
        ++_counts.reasons[static_cast<size_t>(reason)];
        _counts.lastDepth = in.depth;
        _counts.lastMotion = in.motion;
        _counts.lastOutput = in.output;

        if (reason == Reason::Carried)
        {
            _counts.haveCarried = true;
            _counts.depth = in.depth;
            _counts.motion = in.motion;
            _counts.output = in.output;
        }

        const Clock::duration elapsed = now - _start;
        if (elapsed < kWindow)
            return false;

        const uint32_t shape = Shape(_counts);
        if (_haveWritten && shape == _writtenShape && elapsed < kSteadyWindow)
            return false;

        if (line != nullptr)
            *line = FormatLine(_counts, elapsed);

        _writtenShape = shape;
        _haveWritten = true;
        _counts = {};
        _start = now;
        return true;
    }

    const Counts& Open() const { return _counts; }
};
} // namespace DlssNr::Dx11Guides
