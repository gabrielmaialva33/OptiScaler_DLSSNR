// OptiScaler synthesized-motion estimator: the last pass, ours (not FidelityFX code).
//
// Expands the level-0 optical flow -- one vector per 8x8 block of the colour, in colour pixels --
// into one R16G16_FLOAT vector per colour pixel, the output contract in shaders/synth_motion/README.md
// and dlssnr/design/synthesized-motion.md §4:
//   .rg = displacement from the current frame to the previous one, in colour pixels, +x right, +y down
//         (prev = cur + mv, the DLSS/FSR convention).
// FFX's flow already has exactly that meaning and those units: the search looks for the current
// frame's block in the previous frame's luma, and stores where it found it relative to the block
// (ffx_opticalflow_compute_optical_flow_v5.h: newVector = currentVector + minSadCoord, the offset into
// the SECOND image, which is the previous frame). Level 0 is the colour's own resolution. So the only
// work is the bilinear reconstruction between block centres. No sign or unit conversion.
//
// gZero forces zeros (the estimator has no trustworthy field this frame). FFX already writes zero
// vectors on a scene change and on the first frames after a reset, so this is belt and braces.

cbuffer SynthMotionExpand : register(b0)
{
    uint2 gExtent;   // colour extent
    uint2 gFlowSize; // level-0 flow texture extent (ceil(extent / 8))
    uint gZero;
    uint3 gPad;
};

RWTexture2D<int2> r_flow : register(u0);
RWTexture2D<float2> rw_motion : register(u1);

[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= gExtent))
        return;

    if (gZero != 0)
    {
        rw_motion[id] = float2(0.0f, 0.0f);
        return;
    }

    // Block centres sit at 8 * b + 4 - 0.5 in pixel-centre coordinates.
    const float2 p = (float2(id) + 0.5f) / 8.0f - 0.5f;
    const int2 b0 = int2(floor(p));
    const float2 f = p - float2(b0);
    const int2 hi = int2(gFlowSize) - 1;

    const float2 v00 = float2(r_flow[clamp(b0, int2(0, 0), hi)]);
    const float2 v10 = float2(r_flow[clamp(b0 + int2(1, 0), int2(0, 0), hi)]);
    const float2 v01 = float2(r_flow[clamp(b0 + int2(0, 1), int2(0, 0), hi)]);
    const float2 v11 = float2(r_flow[clamp(b0 + int2(1, 1), int2(0, 0), hi)]);

    rw_motion[id] = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);
}
