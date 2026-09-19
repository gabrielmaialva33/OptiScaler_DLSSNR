// ===========================================================================================
// UNBUILT SKELETON. Design: OptiScaler/dlssnr/design/synthesized-motion.md.
//
// This shader is compiled by nothing. No _Shader.h (DX12, dxc cs_6_0) or _Shader_Vk.h
// (Vulkan, dxc -spirv -D VK_MODE) has been generated from it, and DlssNr_SynthMotion.cpp does
// not include one. When this is promoted to a real pass, BOTH targets are regenerated together
// (module invariant, DEVELOPMENT.md §1.6) via OptiScaler/shaders/shader_tools, exactly like the
// NR pass rebuilds DlssNr_Shader.h and DlssNr_Shader_Vk.h.
//
// The three entry points below reconstruct a current->previous motion field in working-resolution
// pixels from consecutive frames, with no engine buffers. Bodies are documented intent, not final
// numerics: block sizes, window radii, the noise floor and the scene thresholds are the tuning
// surface, and the sign of the y axis is an OPEN QUESTION (see the design note) to be pinned by a
// before/after capture, not asserted here.
// ===========================================================================================

// ---------------------------------------------------------------------------------------------
// Constants. Kept as one ordered scalar list; append at the end if extended, never reorder (the
// same discipline the NR Params cbuffer follows).
// ---------------------------------------------------------------------------------------------
cbuffer SynthMotionParams : register(b0)
{
    uint  gWorkWidth;      // working-resolution output extent
    uint  gWorkHeight;
    uint  gFlowWidth;      // estimation-resolution grid (~320 wide), where the search runs
    uint  gFlowHeight;

    uint  gLevel;          // current pyramid level for the block-match pass (0 = finest estimation)
    uint  gNumLevels;      // pyramid depth
    uint  gReset;          // 1 => history discontinuity: emit zero, do not trust previous
    uint  gHasPrevious;    // 0 on the first frame after a reset/abandon

    float gNoiseFloorPx;   // vectors shorter than this (in WORKING px) are zeroed (~0.5, guides.py:102)
    float gSceneCutScore;  // mean-abs-luma-diff above this => scene cut (~0.24, guides.py:307)
    float gStaticScore;    // below this => static frame, skip the search entirely (~0.001, guides.py:311)
    float gMvSignY;        // +1 or -1: the working y-axis sign of the emitted vector (OPEN QUESTION)
};

// ---------------------------------------------------------------------------------------------
// Pass 1 - luma + pyramid.  source colour -> estimation-resolution luma (this frame).
// Area-downsample, not a point sample, so aliasing does not become false motion
// (NeuralScreen box-averages 12x12 per cell at 4K -> 320x180, TECHNICAL.md:280).
// A separate reduction (or mip generation) builds the coarser levels.
// ---------------------------------------------------------------------------------------------
Texture2D<float4>   gSourceColour : register(t0);   // the present host's working proxy (RGBA16F)
RWTexture2D<float>  gLumaOut      : register(u0);    // current luma at gFlowWidth x gFlowHeight

[numthreads(8, 8, 1)]
void BuildLuma(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFlowWidth || id.y >= gFlowHeight)
        return;

    // TODO(skeleton): box-average the source block that maps to this flow cell, take Rec.709 luma,
    // write gLumaOut[id.xy]. Pyramid levels above 0 are an area reduction of the level below.
    gLumaOut[id.xy] = 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Pass 2 - scene score.  mean( |current luma - previous luma| ) over the flow grid, reduced to a
// single value the CPU/host reads to pick reset / static / search. Cheap: it reuses the luma the
// search needs anyway (guides.py:306 computes scene_score before deciding whether to run flow).
// ---------------------------------------------------------------------------------------------
Texture2D<float>    gLumaCur      : register(t0);
Texture2D<float>    gLumaPrev     : register(t1);
RWByteAddressBuffer gSceneScore   : register(u0);   // one accumulated value (atomic add / two-stage)

[numthreads(8, 8, 1)]
void SceneScore(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFlowWidth || id.y >= gFlowHeight)
        return;

    // TODO(skeleton): accumulate |gLumaCur - gLumaPrev| into gSceneScore (group reduce then atomic,
    // or a two-pass reduction). Host divides by cell count and compares to gSceneCutScore /
    // gStaticScore.
}

// ---------------------------------------------------------------------------------------------
// Pass 3 - coarse-to-fine block match.  For each cell, search a small window against the previous
// frame's luma at gLevel, seeded by the (upscaled) vector from the coarser level, scoring SAD over
// a patch, with a parabolic sub-pixel fit on the SAD minimum. Emits the raw flow at estimation
// resolution, in ESTIMATION px, current->previous.
//
//   window   +/- 4 px per level is plenty with 2-3 levels
//   patch    7x7 (the window NeuralScreen's trust test settled on, guides.py:143)
//   cost     levels * cells * window^2 * patch^2, fixed and small at 320 wide (design note §8)
// ---------------------------------------------------------------------------------------------
Texture2D<float>    gLumaCurLvl   : register(t0);
Texture2D<float>    gLumaPrevLvl  : register(t1);
Texture2D<float2>   gSeedFlow     : register(t2);    // coarser level's flow, upscaled; unused at coarsest
RWTexture2D<float2> gFlowOut      : register(u0);    // estimation-res flow, current->previous, est px

[numthreads(8, 8, 1)]
void BlockMatch(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gFlowWidth || id.y >= gFlowHeight)
        return;

    if (gReset != 0 || gHasPrevious == 0)
    {
        gFlowOut[id.xy] = float2(0.0f, 0.0f);
        return;
    }

    // TODO(skeleton): seed from gSeedFlow (0 at the coarsest level), search +/- window in
    // gLumaPrevLvl for the SAD-minimising offset of the patch around id.xy in gLumaCurLvl, parabola
    // fit for sub-pixel, write current->previous displacement (this frame minus where it came from)
    // in estimation-level pixels.
    gFlowOut[id.xy] = float2(0.0f, 0.0f);
}

// ---------------------------------------------------------------------------------------------
// Pass 4 - validate, scale, upscale.  Zero sub-noise-floor vectors, scale estimation px -> working
// px (guides.py:334 scales BEFORE the upscale; linear-equivalent and 70x fewer elements), then
// bilinearly upscale to the working-resolution output. The output is the R16G16_FLOAT field the NR
// pass reads as gMotion, with MvScaleX = 1, MvScaleY = gMvSignY at the call site.
//
// The optional static-hypothesis guard (warp previous by the vector, keep it only if it beats
// standing still by a margin; guides.py:186-220) is a SECOND slice, added here or as its own pass
// once the field works at all.
// ---------------------------------------------------------------------------------------------
Texture2D<float2>   gFlowEst      : register(t0);    // estimation-res flow, current->previous, est px
SamplerState        gLinear       : register(s0);
RWTexture2D<float2> gMotionOut    : register(u0);    // working-res R16G16_FLOAT, current->previous, work px

[numthreads(8, 8, 1)]
void UpscaleField(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWorkWidth || id.y >= gWorkHeight)
        return;

    if (gReset != 0 || gHasPrevious == 0)
    {
        gMotionOut[id.xy] = float2(0.0f, 0.0f);
        return;
    }

    // TODO(skeleton): sample gFlowEst bilinearly at this working pixel, multiply by
    // (gWorkWidth/gFlowWidth, gWorkHeight/gFlowHeight) to reach working px, apply gMvSignY to the y
    // component, zero magnitudes below gNoiseFloorPx, and write gMotionOut[id.xy].
    gMotionOut[id.xy] = float2(0.0f, 0.0f);
}
