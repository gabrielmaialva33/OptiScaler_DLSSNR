#pragma once

#include <shaders/dlssnr/DlssNr_GuideMatch.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// Transfer 2, "Matched residual + DLSS": the model's edit at the working size, enlarged to the frame by a
// private DLSS Super Resolution rather than by the resolve's bilinear tap. design/dlss-enlargement.md.
//
// The mode is wilsjo2's (OptiScaler-DLSSNR-PreSR-Multipass v0.8.91, GPL-3.0; upstream PR #1158). The rules
// here are re-derived for this tree rather than copied: when it runs and what runs instead, the carrier's
// arithmetic, when its history resets. Changed from his: matched residual instead of a clean frame while the
// SR warms up or after it fails, refusal at and above the frame's size (his ran and released the feature
// every frame there), before-upscale and zero guides refused, and a reset on frame-hold edges.
//
// Pure functions of numbers and flags, so tests/nr-enlarge can pin them without a device.
namespace DlssNr::Enlarge
{
constexpr uint32_t kTransferClassic = 0;
constexpr uint32_t kTransferMatched = 1;
constexpr uint32_t kTransferDlss = 2;
constexpr uint32_t kTransferSharp = 3;

// The carrier. The edit d is stored as 0.5 + 0.5 * d / (kCarrierScale + |d|) and read back as
// kCarrierScale * c / (1 - |c|) with c = clamp(2 * carrier - 1, +-kCarrierClamp). The shader spells both
// numbers itself; tests/nr-enlarge holds its text to these.
constexpr float kCarrierScale = 1.0f / 64.0f;
constexpr float kCarrierClamp = 0.999f;

// Where the private SR stands, as the decision sees it.
enum class State : uint8_t
{
    Missing, // nothing built yet; it is built on this frame if it is wanted
    Waiting, // built, and its creation has not yet crossed a present (or completed, where tracked)
    Ready,   // built and evaluable
    Failed,  // failed this session; stays so until Retry
};

// Why the resolve gets what it gets. The order is the order Decide tests them in.
enum class Why : uint8_t
{
    NotYet,          // nothing has dispatched since start or Retry
    NotSelected,     // Transfer is not 2
    Vulkan,          // native Vulkan has no private SR
    FullSize,        // the model works at or above the frame's size: nothing to enlarge
    BeforeUpscale,   // before the upscaler there is no frame-sized edit to make
    NoGuides,        // the caller has no real depth and motion (zero guides)
    GuidesUnmatched, // the guides are not at the working size and this build cannot resample them
    ModelView,       // debug view "model" shows the model's own answer, which needs no enlargement
    Failed,          // the SR failed this session
    WarmingUp,       // the SR is being built, or its creation has not been submitted yet
    Running,         // the SR enlarged the edit
};

struct Inputs
{
    uint32_t configured = kTransferMatched; // [DlssNr] Transfer
    bool vulkan = false;
    uint32_t width = 0, height = 0;         // the frame
    uint32_t workWidth = 0, workHeight = 0; // the model
    bool beforeUpscale = false;
    bool realGuides = true;   // DlssNrFrameInfo::AllowSupersampling
    bool guidesUsable = true; // the guides are at the working size, or can be resampled to it
    uint32_t debugView = 0;
    State state = State::Missing;
};

struct Decision
{
    uint32_t transfer = kTransferMatched; // what the resolve is sent
    Why why = Why::NotYet;
    bool keep = false;     // the private SR should exist: build it if missing, keep it if built
    bool evaluate = false; // the private SR runs this frame
};

// Below the frame on one axis and above it on neither. The working scale is uniform, so this is "scale
// below 1" without trusting a rounded float: at 0.999 a small frame rounds back to its own size.
constexpr bool BelowFrame(uint32_t width, uint32_t height, uint32_t workWidth, uint32_t workHeight)
{
    return workWidth <= width && workHeight <= height && (workWidth < width || workHeight < height);
}

constexpr Decision Decide(const Inputs& in)
{
    if (in.configured != kTransferDlss)
        return { in.configured, Why::NotSelected, false, false };

    if (in.vulkan)
        return { kTransferMatched, Why::Vulkan, false, false };

    if (!BelowFrame(in.width, in.height, in.workWidth, in.workHeight))
        return { kTransferMatched, Why::FullSize, false, false };

    // From here on the SR is wanted for this configuration, so a frame that cannot use it keeps it idle
    // rather than releasing it: a title that alternates routes would otherwise rebuild it every frame.
    if (in.beforeUpscale)
        return { kTransferMatched, Why::BeforeUpscale, in.state != State::Failed, false };

    if (!in.realGuides)
        return { kTransferMatched, Why::NoGuides, in.state != State::Failed, false };

    if (!in.guidesUsable)
        return { kTransferMatched, Why::GuidesUnmatched, in.state != State::Failed, false };

    if (in.debugView == 2)
        return { kTransferMatched, Why::ModelView, in.state != State::Failed, false };

    switch (in.state)
    {
    case State::Failed:
        return { kTransferMatched, Why::Failed, false, false };
    case State::Missing:
    case State::Waiting:
        return { kTransferMatched, Why::WarmingUp, true, false };
    case State::Ready:
        break;
    }

    return { kTransferDlss, Why::Running, true, true };
}

// The native Vulkan resolve has no private SR, and its shader would read a plain answer as a carrier if it
// were sent 2. Every other value goes through as it always did.
constexpr uint32_t VulkanTransfer(uint32_t configured)
{
    return configured == kTransferDlss ? kTransferMatched : configured;
}

// The Enlargement combo, index by index. Index 2 stays "Matched residual, sharp" (Transfer 3), as it was
// before this mode existed; Transfer 2 is appended. A value the shader does not know runs Classic and is
// shown as Classic, as before.
constexpr uint32_t kMenuTransfers[] = { kTransferClassic, kTransferMatched, kTransferSharp, kTransferDlss };
constexpr int kMenuEntries = 4;

constexpr int MenuIndex(uint32_t transfer)
{
    for (int i = 0; i < kMenuEntries; ++i)
        if (kMenuTransfers[i] == transfer)
            return i;
    return 0;
}

constexpr uint32_t MenuTransfer(int index)
{
    return index >= 0 && index < kMenuEntries ? kMenuTransfers[index] : kTransferClassic;
}

// When the private SR forgets what it accumulated. Its own history is not the model's: everything that
// reset the model resets it, and so does anything that broke the run of frames it was fed. frame is the
// pass's own dispatch counter, so "not the one after the last" means at least one NR frame went by
// without the SR. lastFrame 0 is a feature that has never evaluated.
constexpr bool ResetHistory(bool modelReset, bool gameReset, bool holdChanged, uint64_t frame, uint64_t lastFrame)
{
    return modelReset || gameReset || holdChanged || lastFrame == 0 || frame != lastFrame + 1;
}

// The motion-vector scale the SR is given. Every motion texture it is handed is at the working size and in
// the game's units, so this is the model's own rule (reduced-scale-guides.md). Held: the frame does not
// move, whatever the vectors left in the texture say.
constexpr float MotionScale(float gameScale, uint32_t workExtent, uint32_t referenceExtent, bool hold)
{
    return hold ? 0.0f : GuideMatch::ModelMotionScale(gameScale, workExtent, referenceExtent);
}

// Where the SR's depth and motion come from this frame.
enum class GuideSource : uint8_t
{
    Matched,  // the pair the model was given, already resampled to the working size
    Game,     // the game's own textures: both regions already are the working size at the origin
    Resample, // the existing GuideMatch pass into a private pair
    None,     // none of those: the SR cannot run
};

constexpr bool AtWorkingSize(uint32_t baseX, uint32_t baseY, uint32_t width, uint32_t height, uint32_t workWidth,
                             uint32_t workHeight)
{
    return baseX == 0 && baseY == 0 && width == workWidth && height == workHeight;
}

constexpr GuideSource Guides(bool matched, bool resampleAvailable, uint32_t workWidth, uint32_t workHeight,
                             uint32_t depthBaseX, uint32_t depthBaseY, uint32_t depthWidth, uint32_t depthHeight,
                             uint32_t motionBaseX, uint32_t motionBaseY, uint32_t motionWidth, uint32_t motionHeight)
{
    if (matched)
        return GuideSource::Matched;

    if (AtWorkingSize(depthBaseX, depthBaseY, depthWidth, depthHeight, workWidth, workHeight) &&
        AtWorkingSize(motionBaseX, motionBaseY, motionWidth, motionHeight, workWidth, workHeight))
        return GuideSource::Game;

    return resampleAvailable && depthWidth != 0 && depthHeight != 0 && motionWidth != 0 && motionHeight != 0
               ? GuideSource::Resample
               : GuideSource::None;
}

// The carrier's arithmetic on the CPU, the same expressions the shader evaluates per channel (modes 5 and
// the resolve's decode), for the suite to hold the numbers to.
inline float EncodeCarrier(float edit)
{
    const float d = std::isfinite(edit) ? edit : 0.0f;
    return 0.5f + 0.5f * d / (kCarrierScale + std::fabs(d));
}

inline float DecodeCarrier(float carrier)
{
    const float c = std::clamp(2.0f * (std::isfinite(carrier) ? carrier : 0.5f) - 1.0f, -kCarrierClamp, kCarrierClamp);
    return kCarrierScale * c / (1.0f - std::fabs(c));
}

// For the log and the timing contract: one word per reason.
constexpr const char* Token(Why why)
{
    switch (why)
    {
    case Why::NotYet:
        return "not-yet";
    case Why::NotSelected:
        return "not-selected";
    case Why::Vulkan:
        return "vulkan";
    case Why::FullSize:
        return "full-size";
    case Why::BeforeUpscale:
        return "before-upscale";
    case Why::NoGuides:
        return "no-guides";
    case Why::GuidesUnmatched:
        return "guides-unmatched";
    case Why::ModelView:
        return "model-view";
    case Why::Failed:
        return "failed";
    case Why::WarmingUp:
        return "warming-up";
    case Why::Running:
        return "dlss-sr";
    }
    return "unknown";
}

// What the menu reads: the transfer the resolve was last sent and why.
struct Status
{
    uint32_t transfer = kTransferMatched;
    Why why = Why::NotYet;
};
} // namespace DlssNr::Enlarge
