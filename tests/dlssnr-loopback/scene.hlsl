// Deterministic scene for the DLSS-NR loopback harness.
//
// The point of this shader is the motion vector. Both the current and the previous clip-space
// position are computed here from matrices the host supplies, so the vector is exact rather
// than estimated. That is the property a game cannot give us: there, motion vectors are an
// input we have to trust.

cbuffer Frame : register(b0)
{
    float4x4 gViewProj;      // current frame, jittered
    float4x4 gViewProjPrev;  // previous frame, jittered with the previous frame's offset
    float4x4 gModel;
    float4x4 gModelPrev;
    float2   gJitter;        // in NDC, already divided by render extent
    float2   gJitterPrev;
    float2   gRenderExtent;
    float    gFrameIndex;
    float    gPad;
};

struct VSOut
{
    float4 pos      : SV_POSITION;
    float3 world    : WORLD;
    float3 normal   : NORMAL;
    float4 clipCur  : CLIPCUR;   // unjittered, for the motion vector
    float4 clipPrev : CLIPPREV;
};

VSOut VSMain(float3 p : POSITION, float3 n : NORMAL)
{
    VSOut o;
    float4 wCur  = mul(float4(p, 1.0), gModel);
    float4 wPrev = mul(float4(p, 1.0), gModelPrev);

    o.clipCur  = mul(wCur,  gViewProj);
    o.clipPrev = mul(wPrev, gViewProjPrev);

    // Jitter is applied to the rasterised position only. Feeding a jittered position into the
    // motion vector is a classic mistake: the jitter is not scene motion, and the upscaler
    // removes it itself using the offset we report.
    o.pos = o.clipCur;
    o.pos.xy += gJitter * o.pos.w;

    o.world  = wCur.xyz;
    o.normal = normalize(mul(float4(n, 0.0), gModel).xyz);
    return o;
}

struct PSOut
{
    float4 colour : SV_TARGET0;
    float2 motion : SV_TARGET1;
};

PSOut PSMain(VSOut i)
{
    PSOut o;

    // Deliberately simple lighting with a strong specular term: the NR model reacts to
    // material and light response, so a flat albedo would give it nothing to work with.
    float3 lightDir = normalize(float3(0.4, 0.8, -0.45));
    float3 viewDir  = normalize(float3(0.0, 0.0, -1.0));
    float3 h        = normalize(lightDir + viewDir);
    float  ndotl    = saturate(dot(i.normal, lightDir));
    float  spec     = pow(saturate(dot(i.normal, h)), 48.0);

    float3 albedo = 0.5 + 0.5 * sin(i.world * float3(1.7, 2.3, 3.1) + gFrameIndex * 0.01);
    float3 lit    = albedo * (0.08 + 0.92 * ndotl) + spec * 0.6;

    o.colour = float4(lit, 1.0);

    // NDC -> UV, then the difference. Sign convention matches NVSDK_NGX motion vectors:
    // the vector points from the current pixel back to where it was in the previous frame.
    float2 ndcCur  = i.clipCur.xy  / max(i.clipCur.w,  1e-6);
    float2 ndcPrev = i.clipPrev.xy / max(i.clipPrev.w, 1e-6);
    float2 uvCur   = float2(0.5, -0.5) * ndcCur  + 0.5;
    float2 uvPrev  = float2(0.5, -0.5) * ndcPrev + 0.5;
    o.motion = (uvPrev - uvCur) * gRenderExtent;

    return o;
}
