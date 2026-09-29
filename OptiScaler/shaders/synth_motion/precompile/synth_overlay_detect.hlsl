// Synthesized frame generation's HUD mask: DLSS-NR's static-overlay rule (static_overlay_rule.h beside this
// file, the same header dlssnr_uimask.hlsl includes) run at display size on the game's frame. A pixel that
// stays the same while the scene around it moves is interface. Two outputs besides the rule's own state:
//   gMask   the mask, 0..1 (R8_UNORM)
//   gDepth  1.0 where the mask is at least gNearMin, 0.0 elsewhere (R32_FLOAT): the depth FSR-FG is handed,
//           with inverted depth, so the interface is at the near plane and the scene at the far one
// Design: dlssnr/design/synthesized-frame-generation.md, "The HUD: near depth and a UI layer"; the rule's
// own reasoning is hud-protection.md. Class: SynthMotion::Overlay_Dx12 (SynthOverlay_Dx12.cpp).
//
// Two permutations of this source. Without SYNTH_OVERLAY_LAYER (SynthOverlay_Detect) it is the mask alone, the
// bytecode that shipped before the layer had a mask of its own. With SYNTH_OVERLAY_LAYER=1
// (SynthOverlay_DetectLayer, run only with [FrameGen] SynthesizedHudLayer) it also computes the UI layer's own
// mask (static_overlay_layer.h): its state, a tile map, and gCore, the core synth_overlay_layer.hlsl grows the
// band from. The mask and depth are the same in both. Design: synthesized-frame-generation.md, "The HUD layer's
// own mask: recall and a margin".
//
// Precompiled by build.sh beside this file: editing this or the rules alone changes nothing.
//
// The previous frame's luma and state are read through SRVs, not typed UAV loads, which beyond the R32
// formats are optional in D3D12. Constants are root constants: no constant-buffer view to size.

Texture2D<float4> gColour   : register(t0); // the game's frame at display size
Texture2D<float>  gLumaPrev : register(t1); // last frame's luma of it
Texture2D<float2> gAccPrev  : register(t2); // last frame's state: .x protection, .y candidate streak

RWTexture2D<float>        gLumaOut : register(u0);
RWTexture2D<float2>       gAccOut  : register(u1);
RWTexture2D<unorm float>  gMask    : register(u2);
RWTexture2D<float>        gDepth   : register(u3);

cbuffer Params : register(b0) // sixteen root constants (Overlay_Dx12's DetectConstants, in order)
{
    uint  gWidth;
    uint  gHeight;
    uint  gValid;       // 0 on the first frame and after a reset: no previous frame to compare with
    uint  gStreakMin;   // consecutive candidate frames before a new pixel is protected
    float gStaticEps;   // own luma change below which a pixel counts as unchanged
    float gCoreEps;     // luma change below which every pixel within 2 px counts as unchanged
    float gMotionTau;   // luma change above which a side's sample counts as moving
    float gDetailMin;   // local contrast a pixel needs to be taken for interface
    float gDecay;       // per-frame decay of the protection once the evidence stops
    float gDropTau;     // own luma change above which the protection is dropped at once
    float gSupportMin;  // protected pixels (last frame) a pixel needs in its 5x5 to be exported
    float gSidesMin;    // axis sides whose surroundings must move
    float gNearMin;     // mask at or above which the pixel is near
    uint  gLinearInput; // 1 when the colour reads as linear light (an sRGB view, a float format)
#if SYNTH_OVERLAY_LAYER
    uint  gLayerValid;  // 0 while the layer's state and tile map hold no executed frame
#else
    uint  gPad0;
#endif
    uint  gPad1;
};

float Luma(float3 c)
{
    // The thresholds are DLSS-NR's, tuned on an encoded image. A swapchain's UNORM frame is encoded
    // already; a linear one (scRGB, an sRGB view) is brought close with a 2.2 power. Saturated first, so
    // a highlight above white cannot pass for motion.
    float3 e = saturate(c);
    if (gLinearInput != 0)
        e = pow(e, 1.0f / 2.2f);
    return saturate(dot(e, float3(0.2126f, 0.7152f, 0.0722f)));
}

int2 Clamped(int2 p) { return clamp(p, int2(0, 0), int2(int(gWidth) - 1, int(gHeight) - 1)); }

float LumaAt(int2 p) { return Luma(gColour.Load(int3(Clamped(p), 0)).rgb); }

float ChangeAt(int2 p)
{
    p = Clamped(p);
    return abs(Luma(gColour.Load(int3(p, 0)).rgb) - gLumaPrev.Load(int3(p, 0)));
}

float2 AccPrevAt(int2 p) { return gAccPrev.Load(int3(Clamped(p), 0)); }

// The parameters are px, py, never x, y: a function-like macro substitutes every token that matches a
// parameter, the swizzle included, so `AccPrevAt(int2(x, y)).x` called as UM_PROT(x + sx, y + sy) reads
// `.x + sx`. The rule's support and growth loops then count every neighbour as protected, and the mask is 1
// everywhere. DLSS-NR's dlssnr_uimask.hlsl has exactly that (2026-09-28); tests/fg-synth-policy refuses it
// here.
#define UM_FN
#define UM_OUT(T) out T
#define UM_UNROLL [unroll]
#define UM_LUMA(px, py) LumaAt(int2(px, py))
#define UM_CHANGE(px, py) ChangeAt(int2(px, py))
#define UM_PROT(px, py) AccPrevAt(int2(px, py)).x
#define UM_STREAK(px, py) AccPrevAt(int2(px, py)).y
// A 3x3 still core, where DLSS-NR's pass uses 5x5: a crosshair arm of 2 px or a 3 px stroke with a 1 px outline
// has moving scene within 2 px of every pixel, so a 5x5 core marks none of it, and those are what frame generation
// smears. The fixtures that made the core necessary (sand beside a swaying coat, a concave gap) still mark nothing
// at 3x3 (tests/fg-synth-policy runs them).
#define UM_CORE_RADIUS 1
#include "static_overlay_rule.h"

#if !SYNTH_OVERLAY_LAYER

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int2 p = int2(id.xy);
    gLumaOut[p] = LumaAt(p);

    UiMaskParams params;
    params.staticEps = gStaticEps;
    params.coreEps = gCoreEps;
    params.motionTau = gMotionTau;
    params.detailMin = gDetailMin;
    params.decay = gDecay;
    params.dropTau = gDropTau;
    params.streakMin = float(gStreakMin);
    params.supportMin = gSupportMin;
    params.sidesMin = gSidesMin;

    float prot;
    float streak;
    float m;
    UiMaskPixel(p.x, p.y, gValid != 0, params, prot, streak, m);

    gAccOut[p] = float2(prot, streak);
    gMask[p] = m;
    gDepth[p] = m >= gNearMin ? 1.0f : 0.0f;
}

#else // SYNTH_OVERLAY_LAYER: the mask as above, and the UI layer's own mask beside it

Texture2D<float4> gLayerPrev : register(t3); // last frame's layer state: .x still frames, .y hold, .z distance (/255), .w anchor
Texture2D<float4> gTilesPrev : register(t4); // last frame's tile map, /255: .x moved, .y textured, .z bits, .w pixels

RWTexture2D<unorm float4> gLayerOut : register(u4);
RWTexture2D<unorm float4> gTilesOut : register(u5); // one texel per thread group
RWTexture2D<unorm float>  gCore     : register(u6); // max(mask, grown): what synth_overlay_layer.hlsl grows the band from

// static_overlay_layer.h's UL_TILE, which is included below, after everything its macros reach; checked there.
#define LAYER_TILE 8

int TilesW() { return int((gWidth + LAYER_TILE - 1) / LAYER_TILE); }
int TilesH() { return int((gHeight + LAYER_TILE - 1) / LAYER_TILE); }
// A tile's counts, whole numbers: 8-bit UNORM holds n / 255 exactly.
float4 TileAt(int2 q)
{
    return round(gTilesPrev.Load(int3(clamp(q, int2(0, 0), int2(TilesW() - 1, TilesH() - 1)), 0)) * 255.0f);
}

// Last frame's layer state over the group's pixels and a 2 px apron: the growth step reads the distances of a 5x5,
// and the orientation test asks whether each of the 8 neighbours is steady.
#define APRON (LAYER_TILE + 4)
groupshared float gsDist[APRON * APRON];
groupshared float gsStill[APRON * APRON];
groupshared float gsAnchor[APRON * APRON];
static int2 sOrigin; // the group's first pixel

int ApronIndex(int2 q) { return (q.y - sOrigin.y + 2) * APRON + (q.x - sOrigin.x + 2); }

groupshared uint gsSides;       // a bit per axis side with a moving tile in reach
groupshared uint gsAround;      // orientation bits of the 5x5 tiles around
groupshared uint gsCamTextured; // camera samples that are textured
groupshared uint gsCamMoving;   // and of those, moving
groupshared uint gsMoved;       // this frame's tile: pixels that moved,
groupshared uint gsTextured;    // that are textured,
groupshared uint gsTileBits;    // the orientations of its steady structure,
groupshared uint gsPixels;      // and pixels inside the image

// Parameters named qx, qy for the reason given above UM_PROT.
#define UL_FN
#define UL_OUT(T) out T
#define UL_UNROLL [unroll]
#define UL_TILE_MOVED(qx, qy) TileAt(int2(qx, qy)).x
#define UL_TILE_TEXTURED(qx, qy) TileAt(int2(qx, qy)).y
#define UL_TILE_BITS(qx, qy) int(TileAt(int2(qx, qy)).z)
#define UL_TILE_PIXELS(qx, qy) TileAt(int2(qx, qy)).w
#define UL_DIST_PREV(qx, qy) gsDist[ApronIndex(int2(qx, qy))]
#include "static_overlay_layer.h"
#if UL_TILE != LAYER_TILE
#error "the detect pass's thread group is the layer's tile"
#endif

// Every thread reaches every barrier: those outside the image skip the per-pixel work, not the group's.
[numthreads(UL_TILE, UL_TILE, 1)]
void CS(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint gi : SV_GroupIndex)
{
    const int2 p = int2(id.xy);
    const bool inside = id.x < gWidth && id.y < gHeight;
    const bool layerValid = gValid != 0 && gLayerValid != 0;
    const UiLayerParams L = UiLayerShipped(int(gHeight));
    const int tilesW = TilesW();
    const int tilesH = TilesH();
    const int2 tile = int2(group.xy);
    sOrigin = tile * UL_TILE;

    if (gi == 0)
    {
        gsSides = 0;
        gsAround = 0;
        gsCamTextured = 0;
        gsCamMoving = 0;
        gsMoved = 0;
        gsTextured = 0;
        gsTileBits = 0;
        gsPixels = 0;
    }

    // Outside the image, or without a history: no distance, never steady.
    for (uint i = gi; i < APRON * APRON; i += UL_TILE * UL_TILE)
    {
        const int2 q = sOrigin - 2 + int2(i % APRON, i / APRON);
        const bool inImage = q.x >= 0 && q.y >= 0 && q.x < int(gWidth) && q.y < int(gHeight);
        const float4 state = (layerValid && inImage) ? gLayerPrev.Load(int3(q, 0)) : float4(0.0f, 0.0f, 1.0f, 0.0f);
        gsStill[i] = round(state.x * 255.0f);
        gsDist[i] = round(state.z * 255.0f);
        gsAnchor[i] = state.w;
    }
    GroupMemoryBarrierWithGroupSync();

    // The group's tile tests on last frame's map, spread over the threads: sixteen per axis side (every sixteenth
    // distance each), one per tile of the 5x5 around, and four camera samples each.
    if (layerValid)
    {
        const int side = int(gi & 3);
        const int2 dir = side == 0 ? int2(1, 0) : (side == 1 ? int2(-1, 0) : (side == 2 ? int2(0, 1) : int2(0, -1)));
        if (UiLayerSideMoving(tile.x, tile.y, dir.x, dir.y, int(gi >> 2) + 1, 16, tilesW, tilesH, L))
            InterlockedOr(gsSides, 1u << uint(side));

        if (gi < 25)
            InterlockedOr(gsAround,
                          uint(UiLayerTileBits(tile.x + int(gi % 5) - 2, tile.y + int(gi / 5) - 2, tilesW, tilesH)));

        uint textured = 0;
        uint moving = 0;
        [unroll] for (uint k = 0; k < UL_CAMERA_SAMPLES / (UL_TILE * UL_TILE); ++k)
        {
            float t;
            float m;
            UiLayerCameraSample(int(gi + k * UL_TILE * UL_TILE), tilesW, tilesH, L, t, m);
            textured += t > 0.5f ? 1u : 0u;
            moving += m > 0.5f ? 1u : 0u;
        }
        InterlockedAdd(gsCamTextured, textured);
        InterlockedAdd(gsCamMoving, moving);
    }
    GroupMemoryBarrierWithGroupSync();

    const bool seedable = layerValid && int(countbits(gsSides)) >= L.sidesMin && UiLayerPerpendicular(int(gsAround)) &&
                          UiLayerCameraMoving(float(gsCamMoving), float(gsCamTextured), L);

    // Every load before any store: a store to a UAV keeps the compiler from sharing a colour load between the two
    // rules, which read the same neighbours.
    if (inside)
    {
        const float c = LumaAt(p);

        UiMaskParams params;
        params.staticEps = gStaticEps;
        params.coreEps = gCoreEps;
        params.motionTau = gMotionTau;
        params.detailMin = gDetailMin;
        params.decay = gDecay;
        params.dropTau = gDropTau;
        params.streakMin = float(gStreakMin);
        params.supportMin = gSupportMin;
        params.sidesMin = gSidesMin;

        float prot;
        float streak;
        float m;
        UiMaskPixel(p.x, p.y, gValid != 0, params, prot, streak, m);

        // The layer's own mask. Without a previous frame nothing moved; without the layer's own history the pixel
        // starts over.
        const float own = gValid != 0 ? abs(c - gLumaPrev.Load(int3(p, 0))) : 0.0f;
        const float l = LumaAt(p + int2(-1, 0));
        const float r = LumaAt(p + int2(1, 0));
        const float u = LumaAt(p + int2(0, -1));
        const float d = LumaAt(p + int2(0, 1));
        const float contrast = max(max(abs(c - l), abs(c - r)), max(abs(c - u), abs(c - d)));
        const float hold0 = layerValid ? round(gLayerPrev.Load(int3(p, 0)).y * 255.0f) : 0.0f;
        const int self = ApronIndex(p);

        float still;
        float anchor;
        float hold;
        float dist;
        UiLayerPixel(p.x, p.y, layerValid, seedable, c, contrast, gsStill[self], hold0, gsAnchor[self], L, still,
                     anchor, hold, dist);

        // The orientation of its static structure, with the neighbours that are steady too. Only a steady pixel's
        // counts (UiLayerTileContribution), and in a moving scene nearly none is: the rest skip the diagonal loads.
        int orientation = 0;
        if (still >= L.stillFrames)
        {
            float n[8];
            int steady = 0;
            const int2 offsets[8] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
                                      int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
            [unroll] for (int k = 0; k < 8; ++k)
            {
                const int2 q = p + offsets[k];
                n[k] = k == 0 ? l : (k == 1 ? r : (k == 2 ? u : (k == 3 ? d : LumaAt(q))));
                const int a = ApronIndex(q);
                steady = steady | (UiLayerSteady(layerValid, n[k], gsStill[a], gsAnchor[a], L) ? (1 << k) : 0);
            }
            orientation = UiLayerOrientation(c, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], steady, L.detailMin);
        }
        float moved;
        float textured;
        int bits;
        UiLayerTileContribution(gValid != 0, own, contrast, still, orientation, L, moved, textured, bits);

        gLumaOut[p] = c;
        gAccOut[p] = float2(prot, streak);
        gMask[p] = m;
        gDepth[p] = m >= gNearMin ? 1.0f : 0.0f;
        gLayerOut[p] = float4(still / 255.0f, hold / 255.0f, dist / 255.0f, anchor);
        const float core = max(m, UiLayerGrown(dist, L) ? 1.0f : 0.0f);
        gCore[p] = core;

        InterlockedAdd(gsMoved, moved > 0.5f ? 1u : 0u);
        InterlockedAdd(gsTextured, textured > 0.5f ? 1u : 0u);
        InterlockedOr(gsTileBits, uint(bits) | (core > 0.0f ? uint(UL_TILE_CORE) : 0u));
        InterlockedAdd(gsPixels, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0)
        gTilesOut[tile] = float4(float(gsMoved), float(gsTextured), float(gsTileBits), float(gsPixels)) / 255.0f;
}

#endif
