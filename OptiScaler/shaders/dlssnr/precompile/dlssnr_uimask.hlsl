// The static-overlay mask for DLSS-NR's UI correction: a pixel that stays the same while the scene around
// it moves is interface, and the model is told to leave it alone (DLSSNR.UIAlpha, with UICorrection on and
// the model's own input as DLSSNR.Backbuffer). dlssnr/design/hud-protection.md is the reasoning; the idea is
// afmf-linux's (MIT) static-overlay test, the gates and hysteresis are ours. The per-pixel rule lives in
// dlssnr_uimask_rule.h, shared with the host test tests/nr-uimask-rule.
//
// Precompiled like dlssnr.hlsl: editing this or the rule alone changes nothing. From this directory:
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_uimask.hlsl -Fo DlssNr_UiMask_Shader.cso
//   python ../../shader_tools/create_header.py DlssNr_UiMask_Shader.cso DlssNr_UiMask_Shader.h DlssNr_UiMask_cso

Texture2D<float4>         gInput    : register(t0); // the model's input at the working size
Texture2D<float>          gLumaPrev : register(t1); // last frame's luma of that input
Texture2D<float2>         gAccPrev  : register(t2); // last frame's state: .x protection, .y candidate streak
RWTexture2D<float>        gLumaOut  : register(u0);
RWTexture2D<float2>       gAccOut   : register(u1);
RWTexture2D<unorm float4> gMask     : register(u2); // the UI alpha handed to the model

cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    uint  gValid;      // 0 on the first frame and after a reset: no previous frame to compare with
    uint  gStreakMin;  // consecutive candidate frames before a new pixel is protected
    float gStaticEps;  // own luma change below which a pixel counts as unchanged
    float gMotionTau;  // luma change above which a side's sample counts as moving
    float gDetailMin;  // local contrast a pixel needs to be taken for interface
    float gDecay;      // per-frame decay of the protection once the evidence stops
    float gDropTau;    // own luma change above which the protection is dropped at once
    float gCoreEps;    // luma change below which every pixel within 2 px counts as unchanged
    float gSupportMin; // protected pixels (last frame) a pixel needs in its 5x5 to be exported
    float gSidesMin;   // axis sides whose surroundings must move
};

float Luma(float3 c)
{
    // The input is the model's proxy, encoded and paper-white relative; saturate so a highlight above
    // white cannot pass for motion.
    return saturate(dot(saturate(c), float3(0.2126f, 0.7152f, 0.0722f)));
}

int2 Clamped(int2 p) { return clamp(p, int2(0, 0), int2(int(gWidth) - 1, int(gHeight) - 1)); }

float LumaAt(int2 p) { return Luma(gInput.Load(int3(Clamped(p), 0)).rgb); }

float ChangeAt(int2 p)
{
    p = Clamped(p);
    return abs(Luma(gInput.Load(int3(p, 0)).rgb) - gLumaPrev.Load(int3(p, 0)));
}

float2 AccPrevAt(int2 p) { return gAccPrev.Load(int3(Clamped(p), 0)); }

#define UM_FN
#define UM_OUT(T) out T
#define UM_UNROLL [unroll]
#define UM_LUMA(x, y) LumaAt(int2(x, y))
#define UM_CHANGE(x, y) ChangeAt(int2(x, y))
#define UM_PROT(x, y) AccPrevAt(int2(x, y)).x
#define UM_STREAK(x, y) AccPrevAt(int2(x, y)).y
#include "dlssnr_uimask_rule.h"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
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
    gMask[p] = float4(m, m, m, m);
}
