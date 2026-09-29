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

} // namespace DlssNr
