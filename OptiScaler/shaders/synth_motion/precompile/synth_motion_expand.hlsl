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
// the SECOND image, which is the previous frame). Level 0 is the colour's own resolution. So the
// reconstruction is bilinear between block centres, with no sign or unit conversion.
//
// Then one choice per pixel, between that vector and zero (synthesized-motion.md, "Static overlays").
// A thin static overlay -- a crosshair arm, a HUD stroke -- covers a small part of its block, the
// moving world decides the block's vector, and without this the overlay is handed the camera's motion
// and frame generation drags it along (Generation Zero, 2026-09-28). Both hypotheses are scored on the
// level-0 luma pair over the 3x3 around the pixel, weighted 1-2-1 by 1-2-1:
//   S0 = sum w * |cur(p + o) - prev(p + o)|        Sv = sum w * |cur(p + o) - prev(p + o + round(v))|
// and zero is written only when 2 * S0 + 64 < Sv: zero must leave under half of v's error and be at
// least 4 levels better on average. A tie -- flat content, a line along the motion, noise -- keeps v,
// so a moving scene is never frozen. round() here takes halves away from zero, so every vector of half a
// pixel or more is tested. Nothing is tested where it rounds to zero (the same hypothesis) or where
// p + round(v) leaves the frame (content entering at the border has nothing to compare).
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
// Level-0 luma, R8_UINT at the colour extent, read through UAVs like every internal texture of the
// estimator (synth_motion_callbacks.h). u2 is this frame's, u3 the previous frame's.
RWTexture2D<uint> r_luma : register(u2);
RWTexture2D<uint> r_luma_previous : register(u3);

// 4 levels of mean absolute difference, over weights that sum to 16.
static const uint kZeroFloor = 64;

// The group's 8x8 pixels and a 1 px apron, of both frames: the 3x3 taps at the same position come from
// here, 2 loads per thread instead of 18. The displaced taps differ per pixel and stay texture loads.
groupshared uint gsCurrent[10][10];
groupshared uint gsPrevious[10][10];

[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID, uint2 gid : SV_GroupID, uint2 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    // Uniform over the dispatch, so the whole group leaves before the barrier together.
    if (gZero != 0)
    {
        if (all(id < gExtent))
            rw_motion[id] = float2(0.0f, 0.0f);
        return;
    }

    const int2 last = int2(gExtent) - 1;
    const int2 apron = int2(gid * 8) - 1;
    for (uint i = gi; i < 100; i += 64)
    {
        const int2 at = clamp(apron + int2(i % 10, i / 10), int2(0, 0), last);
        gsCurrent[i / 10][i % 10] = r_luma[at];
        gsPrevious[i / 10][i % 10] = r_luma_previous[at];
    }
    GroupMemoryBarrierWithGroupSync();

    if (any(id >= gExtent))
        return;

    // Block centres sit at 8 * b + 4 - 0.5 in pixel-centre coordinates.
    const float2 p = (float2(id) + 0.5f) / 8.0f - 0.5f;
    const int2 b0 = int2(floor(p));
    const float2 f = p - float2(b0);
    const int2 hi = int2(gFlowSize) - 1;

    const float2 v00 = float2(r_flow[clamp(b0, int2(0, 0), hi)]);
    const float2 v10 = float2(r_flow[clamp(b0 + int2(1, 0), int2(0, 0), hi)]);
    const float2 v01 = float2(r_flow[clamp(b0 + int2(0, 1), int2(0, 0), hi)]);
    const float2 v11 = float2(r_flow[clamp(b0 + int2(1, 1), int2(0, 0), hi)]);

    float2 v = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y);

    // Halves away from zero: round() is half to even, so a 1/16 blend weight toward a camera block,
    // (-0.5, -0.5), rounded to no displacement and was never tested (harness, 2026-09-28).
    const int2 pixel = int2(id);
    const int2 d = int2(sign(v)) * int2(floor(abs(v) + 0.5f));
    const int2 target = pixel + d;

    if (any(d != 0) && all(target >= 0) && all(target <= last))
    {
        uint s0 = 0;
        uint sv = 0;

        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                const uint w = uint((2 - abs(x)) * (2 - abs(y)));
                const int2 o = int2(x, y);
                const uint2 tile = tid + uint2(1 + x, 1 + y);
                const int current = int(gsCurrent[tile.y][tile.x]);
                s0 += w * uint(abs(current - int(gsPrevious[tile.y][tile.x])));
                sv += w * uint(abs(current - int(r_luma_previous[clamp(target + o, int2(0, 0), last)])));
            }
        }

        if (2 * s0 + kZeroFloor < sv)
            v = float2(0.0f, 0.0f);
    }

    rw_motion[id] = v;
}
