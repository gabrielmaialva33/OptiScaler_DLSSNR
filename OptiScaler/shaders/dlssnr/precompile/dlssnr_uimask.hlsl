// The static-overlay mask for DLSS-NR's UI correction: a pixel that stays the same while the pixels
// around it change is interface, and the model is told to leave it alone (DLSSNR.UIAlpha, with
// UICorrection on and the model's own input as DLSSNR.Backbuffer). dlssnr/design/hud-protection.md is the
// reasoning; the rule is afmf-linux's (MIT) static-overlay test, the hysteresis and gates are ours.
//
// Precompiled like dlssnr.hlsl: editing this alone changes nothing. From this directory:
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_uimask.hlsl -Fo DlssNr_UiMask_Shader.cso
//   python ../../shader_tools/create_header.py DlssNr_UiMask_Shader.cso DlssNr_UiMask_Shader.h DlssNr_UiMask_cso

Texture2D<float4>         gInput    : register(t0); // the model's input at the working size
Texture2D<float>          gLumaPrev : register(t1); // last frame's luma of that input
Texture2D<float>          gAccPrev  : register(t2); // last frame's protection, before the dilation
RWTexture2D<float>        gLumaOut  : register(u0);
RWTexture2D<float>        gAccOut   : register(u1);
RWTexture2D<unorm float4> gMask     : register(u2); // the UI alpha handed to the model

cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    uint  gValid;     // 0 on the first frame and after a reset: no previous frame to compare with
    uint  gPad0;
    float gStaticEps; // own luma change below which a pixel counts as unchanged
    float gMotionTau; // mean luma change on the rings above which the surroundings count as moving
    float gDetailMin; // local contrast a pixel needs to be taken for interface
    float gDecay;     // per-frame decay of the protection once the evidence stops
    float gDropTau;   // own luma change above which the protection is dropped at once
    float gPad1;
    float gPad2;
    float gPad3;
};

float Luma(float3 c)
{
    // The input is the model's proxy, encoded and paper-white relative; saturate so a highlight above
    // white cannot pass for motion.
    return saturate(dot(saturate(c), float3(0.2126f, 0.7152f, 0.0722f)));
}

float LumaAt(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(gWidth) - 1, int(gHeight) - 1));
    return Luma(gInput.Load(int3(p, 0)).rgb);
}

float ChangeAt(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(gWidth) - 1, int(gHeight) - 1));
    return abs(Luma(gInput.Load(int3(p, 0)).rgb) - gLumaPrev.Load(int3(p, 0)));
}

float AccPrevAt(int2 p)
{
    p = clamp(p, int2(0, 0), int2(int(gWidth) - 1, int(gHeight) - 1));
    return gAccPrev.Load(int3(p, 0));
}

static const int2 kRing[8] = { int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1),
                               int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1) };

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int2 p = int2(id.xy);
    const float cur = LumaAt(p);
    gLumaOut[p] = cur;

    if (gValid == 0)
    {
        gAccOut[p] = 0.0f;
        gMask[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float own = abs(cur - gLumaPrev.Load(int3(p, 0)));

    // The surroundings, sampled on two rings: close enough to belong to the same region of the screen,
    // far enough to be outside a glyph.
    float ring = 0.0f;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        ring += ChangeAt(p + kRing[i] * 6);
        ring += ChangeAt(p + kRing[i] * 14);
    }
    ring *= 1.0f / 16.0f;

    const float detail = max(max(abs(cur - LumaAt(p + int2(1, 0))), abs(cur - LumaAt(p - int2(1, 0)))),
                             max(abs(cur - LumaAt(p + int2(0, 1))), abs(cur - LumaAt(p - int2(0, 1)))));

    const bool candidate = own < gStaticEps && ring > gMotionTau && detail > gDetailMin;

    float acc = AccPrevAt(p) * gDecay;
    if (own > gDropTau)
        acc = 0.0f; // the pixel itself changed a lot: whatever was protected here is gone
    acc = candidate ? 1.0f : acc;
    gAccOut[p] = acc;

    // Grow one pixel over glyph edges from last frame's accumulation, so the growth never feeds back.
    float grown = 0.0f;
    [unroll] for (int j = 0; j < 8; ++j)
        grown = max(grown, AccPrevAt(p + kRing[j]));
    const float m = saturate((max(acc, grown * gDecay) - 0.25f) * 2.0f);

    gMask[p] = float4(m, m, m, m);
}
