#pragma once

#if __has_include(<nvsdk_ngx.h>)
#include <nvsdk_ngx.h>
#endif

#if __has_include(<nvsdk_ngx_defs.h>)
#include <nvsdk_ngx_defs.h>
#endif

namespace DlssNr
{

struct RenderSize
{
    unsigned int width = 0;
    unsigned int height = 0;
};

// Resolves the effective render resolution from the game's parameter block.
// Follows IFeature::GetRenderResolution: if the subrect dimensions are missing or both zero,
// fall back to Width / Height when both are present and Width < OutWidth.
inline RenderSize GetRenderSize(const NVSDK_NGX_Parameter* params)
{
    if (params == nullptr)
        return { 0, 0 };

    auto* p = const_cast<NVSDK_NGX_Parameter*>(params);

    unsigned int subW = 0;
    unsigned int subH = 0;
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subW);
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &subH);

    // If either dimension is non-zero, the game specified (or attempted to specify)
    // a render subrect. If only one is non-zero, callers like Stage 1 check
    // ((subW == 0) != (subH == 0)) to decline incomplete subrects.
    if (subW != 0 || subH != 0)
        return { subW, subH };

    // Subrect keys are absent or both zero. Fall back to Width / Height if both
    // are present and describe a render size smaller than OutWidth.
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int outWidth = 0;

    if (p->Get(NVSDK_NGX_Parameter_Width, &width) == NVSDK_NGX_Result_Success &&
        p->Get(NVSDK_NGX_Parameter_Height, &height) == NVSDK_NGX_Result_Success &&
        p->Get(NVSDK_NGX_Parameter_OutWidth, &outWidth) == NVSDK_NGX_Result_Success)
    {
        if (width != 0 && height != 0 && width < outWidth)
            return { width, height };
    }

    return { 0, 0 };
}

} // namespace DlssNr
