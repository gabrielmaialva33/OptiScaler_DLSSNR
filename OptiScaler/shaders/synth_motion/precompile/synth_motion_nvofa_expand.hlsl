// OptiScaler synthesized-motion estimator, NVIDIA Optical Flow backend: the output pass, ours.
//
// Turns the engine's flow -- one NV_OF_FLOW_VECTOR per output grid cell (2x2 or 4x4 input pixels),
// signed S10.5 fixed point, in INPUT pixels, read here as R16G16_SINT -- into the estimator's output
// contract (shaders/synth_motion/README.md, dlssnr/design/synthesized-motion.md §4):
//   R16G16_FLOAT at the colour's extent, .rg = displacement from the current frame to the previous one,
//   in colour pixels, +x right, +y down (prev = cur + mv).
//
// No sign conversion. The session runs NV_OF_PRED_DIRECTION_FORWARD with the current frame as
// inputFrame and the previous one as referenceFrame, and forward flow is "each pixel's position change
// from inputFrame to referenceFrame" (nvOpticalFlowCommon.h), which is current -> previous. The only work
// is units -- S10.5 input pixels to colour pixels, gVectorScale = (colour / input) / 32 per axis -- and
// the bilinear reconstruction between cell centres.

cbuffer SynthMotionNvofaExpand : register(b0)
{
    uint2 gExtent;       // colour extent
    uint2 gFlowSize;     // flow texture extent (ceil(input / grid))
    float2 gCellSize;    // colour pixels per flow cell, per axis (grid * colour / input)
    float2 gVectorScale; // S10.5 input pixels -> colour pixels, per axis ((colour / input) / 32)
    uint gZero;          // write zeros: no trustworthy flow this frame
    uint3 gPad;
};

Texture2D<int2> r_flow : register(t0);
RWTexture2D<float2> rw_motion : register(u0);

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

    // Cell centres sit at gCellSize * (c + 0.5) in colour pixel-centre coordinates.
    const float2 p = (float2(id) + 0.5f) / gCellSize - 0.5f;
    const int2 c0 = int2(floor(p));
    const float2 f = p - float2(c0);
    const int2 hi = int2(gFlowSize) - 1;

    const float2 v00 = float2(r_flow.Load(int3(clamp(c0, int2(0, 0), hi), 0)));
    const float2 v10 = float2(r_flow.Load(int3(clamp(c0 + int2(1, 0), int2(0, 0), hi), 0)));
    const float2 v01 = float2(r_flow.Load(int3(clamp(c0 + int2(0, 1), int2(0, 0), hi), 0)));
    const float2 v11 = float2(r_flow.Load(int3(clamp(c0 + int2(1, 1), int2(0, 0), hi), 0)));

    rw_motion[id] = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y) * gVectorScale;
}
