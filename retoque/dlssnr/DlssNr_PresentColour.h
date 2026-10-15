#pragma once

#include <dxgi1_4.h>

namespace DlssNr
{

// Why a presented frame is not one the model may be shown, or nullptr when it is.
//
// Every present host tells the pass its frame is display-referred SDR (PresentFrameDefaults:
// ColourIsLinearHdr = false, so the encode is the identity). That is true of an sRGB swapchain and
// false of an HDR one. PQ code values read as sRGB are a different picture, and scRGB is linear light
// with 1.0 at 80 nits, open-ended above and negative outside Rec.709; the model shown either answers for
// a picture that is not on screen, and the composition writes that answer back over the frame. Until the
// hosts convert PQ and scRGB into what the model was trained on and back again, such a frame is declined
// and the game's own picture goes to the screen untouched.
//
// colourSpace is the last one a successful SetColorSpace1 set on the swapchain, or
// DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 when there was none: DXGI has no getter, and sRGB is its
// default for every format below. The format answers first where it decides the matter on its own:
// - FP16 is composed as linear scRGB on a flip-model swapchain, whatever it was told;
// - eight bits cannot carry an HDR signal, and DXGI refuses an HDR colour space on them.
// Ten-bit and anything else is decided by the colour space.
//
// The reasons are literals: the callers log each one once, keyed on the text.
//
// This is the answer with [DlssNr] HdrPresent off, and it stays exactly what it was before the conversion
// existed (docs/dlssnr/design/DEVELOPMENT.md rule 1). With HdrPresent on, ResolvePresentColour below decides.
inline const char* PresentColourRefusal(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colourSpace)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "the swapchain is scRGB (FP16, linear light) and the present pass has no HDR conversion yet; the "
               "frame is shown as the game rendered it";

    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return nullptr;

    default:
        break;
    }

    switch (colourSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709:
        return nullptr;

    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        return "the swapchain is HDR10 (PQ, Rec.2020) and the present pass has no HDR conversion yet; the frame is "
               "shown as the game rendered it";

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "the swapchain is scRGB (linear light) and the present pass has no HDR conversion yet; the frame is "
               "shown as the game rendered it";

    default:
        return "the swapchain's colour space is not sRGB (HLG, Rec.2020 or another) and the present pass has no "
               "conversion for it; the frame is shown as the game rendered it";
    }
}

// --- HDR10 and scRGB at present (docs/dlssnr/design/hdr-present-conversion.md) ---------------------------------

// What a present host does with a frame. Sdr runs the pass as it always has; Hdr10 and ScRgb decode the frame into
// linear Rec.709 in scRGB units, run the pass on that, and encode it back with the original as the reference; Declined
// shows the game's own frame and says why.
enum class PresentColour
{
    Sdr,
    Hdr10,
    ScRgb,
    Declined,
};

struct PresentColourDecision
{
    PresentColour profile = PresentColour::Sdr;
    const char* reason = nullptr; // a literal, set only when Declined
};

// What is known about the colour space of the swapchain being presented. DXGI has no GetColorSpace1, so this is
// learned from the calls the hosts see, per swapchain (DlssNr_SwapchainColour.h for the D3D12 route, PresentHost for
// the D3D11 bridge).
struct PresentColourRecord
{
    bool known = false;          // the host has a record of this swapchain at all
    bool valid = false;          // not invalidated since: a resize to another format waits for the next SetColorSpace1
    bool colourSpaceSet = false; // a SetColorSpace1 succeeded; without one the format's own default applies
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;                                    // the format the record is for
    DXGI_COLOR_SPACE_TYPE colourSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // meaningful when colourSpaceSet
};

// The colour space a flip-model swapchain has before anything sets one: scRGB for FP16, sRGB for everything else.
inline DXGI_COLOR_SPACE_TYPE DefaultColourSpace(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
                                                    : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

// The reasons ResolvePresentColour gives with HdrPresent on, and the ones the hosts add once a profile is decided. One
// text per reason, so each is logged once and the menu can show it.
namespace PresentColourText
{
inline constexpr const char kResourceContract[] =
    "HDR conversion resource contract unavailable; the frame stays original";
inline constexpr const char kNoDescription[] = "HDR swapchain description unavailable; the frame stays original";
inline constexpr const char kTypeless[] =
    "the swapchain's format is typeless, which the HDR conversion does not take; the frame is shown as the game "
    "rendered it";
inline constexpr const char kNoRecord[] =
    "nothing is known about this swapchain's colour space, and the HDR conversion does not guess; the frame is shown "
    "as the game rendered it";
inline constexpr const char kResized[] =
    "the swapchain changed format after its colour space was set, and the HDR conversion waits for it to be set "
    "again; the frame is shown as the game rendered it";
inline constexpr const char kOtherFormat[] =
    "the swapchain's colour space was recorded for another format; the frame is shown as the game rendered it";
inline constexpr const char kPqVariant[] =
    "the swapchain is PQ in studio range or YCbCr, which the HDR conversion does not take; the frame is shown as "
    "the game rendered it";
inline constexpr const char kTenBitLinear[] =
    "the swapchain is ten-bit with a linear (scRGB) colour space, which the HDR conversion does not take; the frame "
    "is shown as the game rendered it";
inline constexpr const char kFp16NotScRgb[] =
    "the swapchain is FP16 with a colour space other than scRGB, which the HDR conversion does not take; the frame "
    "is shown as the game rendered it";
inline constexpr const char kOtherSpace[] =
    "the swapchain's colour space is HLG, Rec.2020 with a gamma curve or another the HDR conversion does not take; "
    "the frame is shown as the game rendered it";

// From the hosts, after a profile was decided.
inline constexpr const char kNoShader[] =
    "this build carries no HDR conversion shader; the frame is shown as the game rendered it";
inline constexpr const char kNotBuilt[] =
    "the HDR conversion's pass could not be built; the frame is shown as the game rendered it";
inline constexpr const char kNoTypedStore[] =
    "this GPU cannot read this swapchain's format in a shader or store to it from one (typed UAV store), so the HDR "
    "conversion cannot run; the frame is shown as the game rendered it";
inline constexpr const char kNoScratch[] =
    "the HDR conversion's textures could not be created; the frame is shown as the game rendered it";
inline constexpr const char kOtherDevice[] =
    "the HDR conversion was built for another D3D12 device; the frame is shown as the game rendered it";
inline constexpr const char kBufferFormat[] =
    "the backbuffer's format is not the swapchain's, so the HDR conversion does not trust its colour space; the "
    "frame is shown as the game rendered it";
} // namespace PresentColourText

// The decision, in the order of authority the design gives: the swapchain's actual format, then what is known of the
// colour space set on this swapchain, then the switch.
//
// hdrPresent false is PresentColourRefusal(actual, legacyColourSpace), word for word: the decline texts and the frame
// are what they were before the conversion existed. legacyColourSpace is what each host has always passed there.
//
// hdrPresent true:
// - eight bits run as SDR, whatever is recorded (DXGI refuses an HDR colour space on them);
// - a typeless ten-bit or FP16 format declines (the views would have to guess a type);
// - ten bits and FP16 need a record for this format that nothing has invalidated, then:
//   R10G10B10A2_UNORM: full-range PQ Rec.2020 is HDR10; the Rec.709 gamma spaces are SDR; anything else declines;
//   R16G16B16A16_FLOAT: scRGB, set or by default, is scRGB; anything else set declines;
// - any other format keeps PresentColourRefusal's answer, from the same colour space as with the switch off.
inline PresentColourDecision ResolvePresentColour(DXGI_FORMAT actual, const PresentColourRecord& record,
                                                  bool hdrPresent, DXGI_COLOR_SPACE_TYPE legacyColourSpace)
{
    const auto declined = [](const char* reason) { return PresentColourDecision { PresentColour::Declined, reason }; };

    if (!hdrPresent)
    {
        const char* refusal = PresentColourRefusal(actual, legacyColourSpace);
        return refusal != nullptr ? declined(refusal) : PresentColourDecision {};
    }

    switch (actual)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return {};

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return declined(PresentColourText::kTypeless);

    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        break;

    default:
    {
        const char* refusal = PresentColourRefusal(actual, legacyColourSpace);
        return refusal != nullptr ? declined(refusal) : PresentColourDecision {};
    }
    }

    if (!record.known)
        return declined(PresentColourText::kNoRecord);
    if (!record.valid)
        return declined(PresentColourText::kResized);
    if (record.format != actual)
        return declined(PresentColourText::kOtherFormat);

    const DXGI_COLOR_SPACE_TYPE space = record.colourSpaceSet ? record.colourSpace : DefaultColourSpace(actual);

    if (actual == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        return space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? PresentColourDecision { PresentColour::ScRgb }
                                                                : declined(PresentColourText::kFp16NotScRgb);
    }

    switch (space)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return { PresentColour::Hdr10 };

    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709:
        return {};

    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020:
    case DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020:
        return declined(PresentColourText::kPqVariant);

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return declined(PresentColourText::kTenBitLinear);

    default:
        return declined(PresentColourText::kOtherSpace);
    }
}

// Whether a profile is one the conversion runs on.
inline bool IsHdrProfile(PresentColour profile)
{
    return profile == PresentColour::Hdr10 || profile == PresentColour::ScRgb;
}

// Whether a frame belongs in the menu's HDR status (DlssNr::NotePresentHdr): a swapchain the conversion would take, or,
// with the switch on, a ten-bit or FP16 one it declines -- the readout then says why. An eight-bit swapchain never
// does, and neither does a ten-bit one the switch-on decision runs as SDR.
inline bool ReportsHdrStatus(DXGI_FORMAT actual, const PresentColourDecision& switchOn, bool hdrPresent)
{
    const bool hdrCapable = actual == DXGI_FORMAT_R10G10B10A2_UNORM || actual == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
                            actual == DXGI_FORMAT_R16G16B16A16_FLOAT || actual == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    return hdrCapable &&
           (IsHdrProfile(switchOn.profile) || (hdrPresent && switchOn.profile == PresentColour::Declined));
}

} // namespace DlssNr
