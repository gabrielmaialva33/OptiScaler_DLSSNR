// Synthesized frame generation's UI layer: the frame as it will be presented, with the layer's own mask, grown
// into a band, as alpha. FFX's frame-generation swapchain composes it over every frame it presents,
// lerp(backbuffer, layer.rgb, layer.a): in a generated frame the pixels under the alpha are the base frame's, and in
// a real frame lerp(x, x, a) leaves the frame as it was, whatever a is. Design: dlssnr/design/
// synthesized-frame-generation.md, "The HUD: near depth and a UI layer" and "The HUD layer's own mask: recall and a
// margin". Class: SynthMotion::Overlay_Dx12.
//
// The alpha is the core (synth_overlay_detect.hlsl's gCore: the strict mask with the grown set) dilated and
// feathered by gMargin px: the largest core * UiLayerBandWeight(|dx|) * UiLayerBandWeight(|dy|) around the pixel,
// so 1 within gMargin / 2 px and 0 beyond gMargin + 1. A separable maximum in group-shared memory, rows then
// columns. gMargin 0 is the core itself. Most of a frame has no core in reach: a group first reads the tile map the
// same Record wrote, whose UL_TILE_CORE bit says which 8x8 tiles hold some, and without one it loads nothing more.
//
// The colour is read through a view of its own format, as FFX reads the backbuffer, so the two agree: an sRGB
// view gives linear light here and there. Precompiled by build.sh beside this file.

Texture2D<float4>   gColour : register(t0); // the frame as it will be presented
Texture2D<float>    gCore   : register(t1); // this frame's layer core, 0..1
Texture2D<float4>   gTiles  : register(t2); // this frame's tile map (synth_overlay_detect.hlsl): .z bits / 255
RWTexture2D<float4> gLayer  : register(u0); // RGBA8 UNORM for an 8-bit frame, RGBA16F otherwise

cbuffer Params : register(b0) // sixteen root constants; the first three are used
{
    uint gWidth;
    uint gHeight;
    uint gMargin; // px; above UL_MAX_MARGIN it is UL_MAX_MARGIN
};

#define UL_FN
#include "static_overlay_layer.h"

// 16x16 groups. At the largest margin the apron is 64x64 floats (16 KB) and the row pass 64x16 (4 KB), well under
// the 32 KB of group-shared memory D3D12 guarantees.
#define GROUP 16
#define SPAN_MAX (GROUP + 2 * UL_MAX_MARGIN)
groupshared float gsCore[SPAN_MAX * SPAN_MAX]; // the core over the group and its apron, row-major at the span
groupshared float gsRows[SPAN_MAX * GROUP];    // the row pass: every apron row, the group's own columns
groupshared float gsWeight[UL_MAX_MARGIN + 1]; // UiLayerBandWeight(d) for d = 0..margin
groupshared uint gsAny;

// Every thread reaches every barrier; the maximum is skipped by the loop bounds, which are the group's own.
[numthreads(GROUP, GROUP, 1)]
void CS(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID,
        uint gi : SV_GroupIndex)
{
    const int margin = int(min(gMargin, uint(UL_MAX_MARGIN)));
    const int span = GROUP + 2 * margin;
    const int2 origin = int2(group.xy) * GROUP - margin;

    if (gi == 0)
        gsAny = 0;
    if (int(gi) <= margin)
        gsWeight[gi] = UiLayerBandWeight(int(gi), margin);
    GroupMemoryBarrierWithGroupSync();

    // The tiles the apron covers, at most 7x7 at the largest margin: any holding some core?
    const int2 tilesMax = int2((int(gWidth) + UL_TILE - 1) / UL_TILE, (int(gHeight) + UL_TILE - 1) / UL_TILE) - 1;
    const int2 tile0 = clamp(int2(floor(float2(origin) / UL_TILE)), int2(0, 0), tilesMax);
    const int2 tile1 = clamp((origin + span - 1) / UL_TILE, int2(0, 0), tilesMax);
    const int2 tiles = tile1 - tile0 + 1;
    if (int(gi) < tiles.x * tiles.y)
    {
        const int2 t = tile0 + int2(int(gi) % tiles.x, int(gi) / tiles.x);
        if ((int(round(gTiles.Load(int3(t, 0)).z * 255.0f)) & UL_TILE_CORE) != 0)
            InterlockedOr(gsAny, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    // The core over the group and its apron, only when some is in reach.
    const int loaded = gsAny != 0 ? span * span : 0;
    for (int i = int(gi); i < loaded; i += GROUP * GROUP)
    {
        const int2 q = origin + int2(i % span, i / span);
        const bool inImage = q.x >= 0 && q.y >= 0 && q.x < int(gWidth) && q.y < int(gHeight);
        gsCore[i] = inImage ? gCore.Load(int3(q, 0)) : 0.0f;
    }
    GroupMemoryBarrierWithGroupSync();

    // Rows: every apron row, at the group's columns. None when nothing is in reach.
    const int rows = gsAny != 0 ? span : 0;
    for (int j = int(gi); j < rows * GROUP; j += GROUP * GROUP)
    {
        const int row = j / GROUP;
        const int col = j % GROUP;
        float h = 0.0f;
        for (int dx = -margin; dx <= margin; ++dx)
            h = max(h, gsCore[row * span + col + margin + dx] * gsWeight[abs(dx)]);
        gsRows[row * GROUP + col] = h;
    }
    GroupMemoryBarrierWithGroupSync();

    // Columns, at the thread's own pixel.
    float alpha = 0.0f;
    if (rows != 0)
    {
        for (int dy = -margin; dy <= margin; ++dy)
            alpha = max(alpha, gsRows[(int(local.y) + margin + dy) * GROUP + int(local.x)] * gsWeight[abs(dy)]);
    }

    if (id.x < gWidth && id.y < gHeight)
        gLayer[id.xy] = float4(gColour.Load(int3(id.xy, 0)).rgb, alpha);
}
