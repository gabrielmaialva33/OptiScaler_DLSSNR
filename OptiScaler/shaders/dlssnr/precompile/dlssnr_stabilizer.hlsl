// The output stabilizer: keep the shown picture where the game's frame did not change, and follow
// the model where it did. dlssnr/design/output-stabilizer.md is the reasoning; the gate is
// DLSS5-Feeder's feed_hold12.h (MIT, Jean-Laurent Rouzies); the witness, the creep and the weights are not.
//
// Precompiled like dlssnr.hlsl: editing this alone changes nothing. From this directory:
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_stabilizer.hlsl -Fo DlssNr_Stabilizer_Shader.cso
//   python ../../shader_tools/create_header.py DlssNr_Stabilizer_Shader.cso DlssNr_Stabilizer_Shader.h DlssNr_Stabilizer_cso

Texture2D<float4>   gInput      : register(t0); // the frame before the edit, as the upscaler left it
Texture2D<float4>   gAnchorPrev : register(t1); // per pixel, the input box as it was when it last moved
Texture2D<float4>   gShownPrev  : register(t2); // what went out last frame
Texture2D<float4>   gModel      : register(t3); // what the resolve wrote this frame
RWTexture2D<float4> gShownOut   : register(u0);
RWTexture2D<float4> gAnchorOut  : register(u1);
SamplerState        gLinear     : register(s0);

cbuffer Params : register(b0)
{
    float gStrength;
    float gTolerance;
    uint  gWidth;
    uint  gHeight;
    uint  gValid;      // 0 on a reset: show the model and start the anchor over
    float gQuantAbs;   // one unit of a UNORM output, 0 for float
    float gQuantRelR;  // one unit of a float output, relative to the value, per channel
    float gQuantRelG;
    float gQuantRelB;
    float gWhitePoint; // what the composition scales the frame by, so that 1 is paper white
    uint  gPassthrough; // 1 when the frame is already a finished, encoded picture
};

// What the gate compares: the frame as the composition sees it, on a curve that does not saturate.
//
// Not the proxy the model is shown. That goes through a soft knee that reaches its asymptote within a
// stop of white, so stops of real change in a highlight read as nothing there -- and the resolve takes
// those highlights from this linear frame, not from the model. Gating on the proxy froze a muzzle
// flash on a still pixel. Scaled by the white point because that is what the composition divides by
// (a white point that moves changes the output, so it should read as change), and encoded with sRGB's
// curve continued past 1 instead of kneed, so the tolerance keeps the meaning it was tuned with on
// encoded frames.
float3 Witness(float3 c)
{
    if (gPassthrough != 0)
        return c;

    c = max(c, 0.0) / gWhitePoint;
    const float3 low = c * 12.92;
    const float3 high = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(c, 0.0031308));
}

// Rec.709 weights. Only the denominator of a relative difference, on an encoded value.
float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const float4 m = gModel.Load(int3(id.xy, 0));
    const float2 px = 1.0 / float2(gWidth, gHeight);
    const float2 uv = (float2(id.xy) + 0.5) * px;

    // A 3x3 box of the input, so one shimmering texel is not change. Sampled by uv because the input
    // need not be the output's size.
    float3 a = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            a += Witness(gInput.SampleLevel(gLinear, uv + float2(x, y) * px, 0).rgb);
    a /= 9.0;

    if (gValid == 0)
    {
        gShownOut[id.xy] = m;
        gAnchorOut[id.xy] = float4(a, 1.0);
        return;
    }

    const float3 b = gAnchorPrev.Load(int3(id.xy, 0)).rgb;
    const float3 d = abs(a - b);

    // Relative to local brightness, so a global exposure drift -- tiny in absolute terms on a dark
    // pixel -- does not unlock the whole picture.
    const float rel = max(d.r, max(d.g, d.b)) / max(max(Luma(a), Luma(b)), 0.02);

    // 0 = still, 1 = moved: from the tolerance up to twice it, so the gate has a soft edge.
    const float change = saturate((rel - gTolerance) / max(gTolerance, 1e-4));
    const float keep = gStrength * (1.0 - change);

    // The creep toward the model's new answer, at (1 - keep) of the distance per frame -- but never
    // less than one unit of the output format, or the store rounds it away and the pixel sits short of
    // the model forever (5 LSB at strength 0.9 in 8 bits, ~8% on R11G11B10). Never past the target
    // either. keep == 1 is a hold, so it does not creep; keep == 0 is the model, exactly.
    const float3 prev = gShownPrev.Load(int3(id.xy, 0)).rgb;
    const float3 delta = m.rgb - prev;
    float3 step = delta * (1.0 - keep);
    if (keep < 1.0)
    {
        const float3 unit = gQuantAbs + float3(gQuantRelR, gQuantRelG, gQuantRelB) * max(abs(prev), abs(m.rgb));
        step = sign(delta) * max(abs(step), min(abs(delta), unit));
    }
    const float3 shown = keep > 0.0 ? prev + step : m.rgb;

    gShownOut[id.xy] = float4(shown, m.a);
    gAnchorOut[id.xy] = float4(lerp(b, a, change), 1.0);
}
