// Synthesized frame generation's HUD mask: DLSS-NR's static-overlay rule (static_overlay_rule.h beside this
// file, the same header dlssnr_uimask.hlsl includes) run at display size on the game's frame. A pixel that
// stays the same while the scene around it moves is interface. Two outputs besides the rule's own state:
//   gMask   the mask, 0..1 (R8_UNORM): the alpha of the UI layer (synth_overlay_layer.hlsl)
//   gDepth  1.0 where the mask is at least gNearMin, 0.0 elsewhere (R32_FLOAT): the depth FSR-FG is handed,
//           with inverted depth, so the interface is at the near plane and the scene at the far one
// Design: dlssnr/design/synthesized-frame-generation.md, "The HUD: near depth and a UI layer"; the rule's
// own reasoning is hud-protection.md. Class: SynthMotion::Overlay_Dx12 (SynthOverlay_Dx12.cpp).
//
// Precompiled by build.sh beside this file: editing this or the rule alone changes nothing.
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
    uint  gPad0;
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
