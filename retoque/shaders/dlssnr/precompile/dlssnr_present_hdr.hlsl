// HDR10 and scRGB at present ([DlssNr] HdrPresent, docs/dlssnr/design/hdr-present-conversion.md): the two passes on
// either side of the composition when a present host's swapchain is HDR. dlssnr.hlsl is not edited; the composition runs
// between these on a linear-HDR frame, the path it was built for.
//
//   decode (gMode 0): the backbuffer's copy, in its own format, into the FP16 work texture as linear Rec.709 in scRGB
//                     units (1.0 = 80 nits). HDR10: PQ to light, Rec.2020 to Rec.709, negatives kept. scRGB: a copy.
//   encode (gMode 1): the work texture as the composition left it back into the swapchain's format, with the original
//                     as the reference: a texel the pass left alone gets the game's own code back, bit for bit.
//
// One file, one entry point, the mode a constant: both passes need the same decode, and the bypass in the encode is
// only exact if it is the same decode the work texture was made with.
//
// Precompiled like dlssnr.hlsl: editing this alone changes nothing. From this directory (dxc under the msvc-wine prefix
// on Linux, never while a build is using it):
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_present_hdr.hlsl -Fo DlssNr_PresentHdr_Shader.cso
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_PresentHdr_Shader.cso DlssNr_PresentHdr_Shader.h DlssNr_PresentHdr_cso
// DlssNr_PresentHdr_Dx12.cpp picks the header up through __has_include; without it the conversion reports itself
// unavailable, an HDR swapchain stays declined, and the menu offers nothing.
//
// Also rebuilt as SPIR-V with -spirv -D VK_MODE. This validates the shared shader target; the Vulkan host does not
// consume it yet. Vulkan HDR integration is a separate task.

#ifdef VK_MODE
[[vk::binding(0, 0)]]
cbuffer Params : register(b0, space0)
#else
cbuffer Params : register(b0)
#endif
{
    uint gWidth;
    uint gHeight;
    uint gProfile; // 1 HDR10 (PQ, Rec.2020, R10G10B10A2_UNORM), 2 scRGB (linear Rec.709, R16G16B16A16_FLOAT)
    uint gMode;    // 0 decode, 1 encode
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4>   gOriginal  : register(t0); // the backbuffer's copy, in its own format; never written
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4>   gProcessed : register(t1); // encode: the work texture as the composition left it; decode: unread
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
RWTexture2D<float4> gOut       : register(u0); // decode: the FP16 work texture; encode: the swapchain's format

// --- the conversion maths: tests/nr-hdr-math compiles this block as C++, so it stays plain float arithmetic ---

static const uint kProfileHdr10 = 1;
static const uint kProfileScRgb = 2;
static const uint kModeDecode = 0;
static const uint kModeEncode = 1;

// SMPTE ST 2084 (PQ), exactly: each is a dyadic fraction. A code of 1.0 is 10,000 nits; scRGB's 1.0 is 80.
static const float kPqM1 = 0.1593017578125; // 2610 / 16384
static const float kPqM2 = 78.84375;        // 2523 / 4096 * 128
static const float kPqC1 = 0.8359375;       // 3424 / 4096
static const float kPqC2 = 18.8515625;      // 2413 / 4096 * 32
static const float kPqC3 = 18.6875;         // 2392 / 4096 * 32
static const float kPqPeakScRgb = 125.0;    // 10000 / 80

// Rec.2020 to Rec.709 and back, linear light, D65 white, by rows. Derived from the primaries in tests/nr-hdr-math.
static const float3 kRec2020To709R = float3(1.660491002108, -0.587641138789, -0.072849863320);
static const float3 kRec2020To709G = float3(-0.124550474522, 1.132899897126, -0.008349422604);
static const float3 kRec2020To709B = float3(-0.018150763355, -0.100578898008, 1.118729661363);
static const float3 kRec709To2020R = float3(0.627403895935, 0.329283038378, 0.043313065687);
static const float3 kRec709To2020G = float3(0.069097289358, 0.919540395075, 0.011362315566);
static const float3 kRec709To2020B = float3(0.016391438875, 0.088013307877, 0.895595253248);

// The encode's reference bypass: every channel within kBypassEps of the decoded original, relative to its size and
// never below kBypassFloor, and the texel is the original's.
static const float kBypassEps = 0.001953125;        // 2^-9
static const float kBypassFloor = 0.00006103515625; // 2^-14, FP16's smallest normal

float FiniteOr(float v, float fallback)
{
    return isfinite(v) ? v : fallback;
}

// One PQ code (0..1) to scRGB units.
float PqToScRgb(float code)
{
    const float e = pow(saturate(code), 1.0 / kPqM2);
    return kPqPeakScRgb * pow(max(e - kPqC1, 0.0) / (kPqC2 - kPqC3 * e), 1.0 / kPqM1);
}

// scRGB units to one PQ code (0..1), clamped to 0..10,000 nits first.
float ScRgbToPq(float value)
{
    const float y = pow(saturate(value / kPqPeakScRgb), kPqM1);
    return pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

// HDR10 codes to linear Rec.709 in scRGB units. Rec.2020 colours outside Rec.709 come out negative and stay so: the
// composition hands the negative part back (signedRest), and the bypass returns the codes of a pixel it did not change.
float3 DecodeHdr10(float3 code)
{
    const float3 rec2020 = float3(PqToScRgb(code.x), PqToScRgb(code.y), PqToScRgb(code.z));
    return float3(dot(kRec2020To709R, rec2020), dot(kRec2020To709G, rec2020), dot(kRec2020To709B, rec2020));
}

// Linear Rec.709 in scRGB units to HDR10 codes, before the UNORM store rounds them.
float3 EncodeHdr10(float3 light)
{
    const float3 rec2020 = float3(dot(kRec709To2020R, light), dot(kRec709To2020G, light), dot(kRec709To2020B, light));
    return float3(ScRgbToPq(rec2020.x), ScRgbToPq(rec2020.y), ScRgbToPq(rec2020.z));
}

// What the original is in the working space: the work texture before the pass, without FP16's rounding. Nothing
// non-finite reaches the model; an scRGB NaN or Inf is black to it (and comes back as itself through the bypass).
float3 DecodeOriginal(float3 original, uint profile)
{
    const float3 light = profile == kProfileHdr10 ? DecodeHdr10(original) : original;
    return float3(FiniteOr(light.x, 0.0), FiniteOr(light.y, 0.0), FiniteOr(light.z, 0.0));
}

// Rule 1: a texel the pass made non-finite keeps the original. Rule 2: so does one it left alone.
bool KeepsOriginal(float3 processed, float3 decoded)
{
    if (!isfinite(processed.x) || !isfinite(processed.y) || !isfinite(processed.z))
        return true;

    const float3 slack = kBypassEps * max(abs(decoded), kBypassFloor);
    const float3 moved = abs(processed - decoded);
    return moved.x <= slack.x && moved.y <= slack.y && moved.z <= slack.z;
}

// The texel the encode writes, before the store converts it to the swapchain's format. scRGB is stored as it is,
// negatives and values above 1 included: FP16 holds them and the compositor decides what to show.
float3 EncodeTexel(float3 processed, float3 original, uint profile)
{
    if (KeepsOriginal(processed, DecodeOriginal(original, profile)))
        return original;

    return profile == kProfileHdr10 ? EncodeHdr10(processed) : processed;
}

// --- end of the conversion maths ---

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const float4 original = gOriginal.Load(int3(id.xy, 0));

    if (gMode == kModeDecode)
    {
        gOut[id.xy] = float4(DecodeOriginal(original.rgb, gProfile), FiniteOr(original.a, 1.0));
        return;
    }

    // Alpha is the original's in every branch.
    const float3 processed = gProcessed.Load(int3(id.xy, 0)).rgb;
    gOut[id.xy] = float4(EncodeTexel(processed, original.rgb, gProfile), original.a);
}
