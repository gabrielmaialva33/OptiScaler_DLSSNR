// "Measure detail" (DlssNr_DetailStats.h): one measured NR evaluation -> a 256x64 grid of tile
// statistics, read back and averaged on the CPU. Only while a measurement runs.
// Ported from janblade (OptiScaler descendants-scan, commit 28daeebf, GPL-3.0).
//
// Separate from dlssnr.hlsl so the shared shader, its SPIR-V twin and the occupancy of every ordinary pass stay as
// they are.
//
// One 8x8 thread group per tile of a 64x64 grid over the frame. Every second pixel in each direction is measured:
// the results are means, and a quarter of the pixels is plenty for a mean. Per tile:
//   (x, y)       raw detail of the output, band detail of the output, band detail of the input, output change
//   (x + 64, y)  input change, shoulder share, floor share, pixels measured
//   (x + 128, y) colour: OkLab chroma of the input, of the output, |output - input| in OkLab's a-b plane, and the
//                output's b minus the input's (+ is warmer, toward yellow)
//   (x + 192, y) shadows: the share of pixels whose input is in the shadows, their input level, their output level (both
//                summed and divided by the pixels measured, like the rest), and the share the model crushed

#ifdef VK_MODE
[[vk::binding(0, 0)]]
#endif
cbuffer Params : register(b0)
{
    uint mode;
    float measureWhitePoint;
    uint width;
    uint height;
    float shoulderThreshold;
    float floorThreshold;
};

#ifdef VK_MODE
[[vk::binding(1, 0)]]
#endif
Texture2D<float4> gOutput     : register(t0); // the edited frame, linear
#ifdef VK_MODE
[[vk::binding(2, 0)]]
#endif
Texture2D<float4> gPrevOutput : register(t1); // the edited frame of the previous evaluation
#ifdef VK_MODE
[[vk::binding(3, 0)]]
#endif
Texture2D<float4> gInput      : register(t2); // the frame before NR (the encode's untouched copy), linear
#ifdef VK_MODE
[[vk::binding(4, 0)]]
#endif
Texture2D<float4> gPrevInput  : register(t3);
#ifdef VK_MODE
[[vk::binding(6, 0)]] [[vk::image_format("rgba16f")]]
RWTexture2D<float4> gProxy;
#else
Texture2D<float4> gProxy      : register(t4);
#endif
#ifdef VK_MODE
[[vk::binding(5, 0)]] [[vk::image_format("rgba32f")]]
#endif
RWTexture2D<float4> gGrid     : register(u0);
#ifdef VK_MODE
[[vk::binding(7, 0)]]
#endif
SamplerState gLinear          : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const uint kGrid = 64;
static const float kShadowLevel = 0.15; // display-encoded luma below which a pixel is in the shadows (~2% of white)
static const float kBlackLevel = 0.02;  // below this the input is black already: nothing left to crush

float Neutwo(float x) { return x * rsqrt(x * x + 1.0); }

// Linear light -> the display-range colour Colour() measures: / the white point, the peak channel rolled off by
// Neutwo with the others scaled alike (NeutwoEncode's hue-keeping shape), no sRGB (OkLab takes linear light).
float3 DisplayColour(float3 rgb)
{
    const float3 x = max(rgb, 0.0) / max(measureWhitePoint, 1e-6);
    const float m = max(x.r, max(x.g, x.b));
    if (!(m > 1e-6) || !isfinite(m))
        return 0.0;
    return x * (Neutwo(m) / m);
}

// Bjorn Ottosson's OkLab (the matrices dlssnr.hlsl's ToOkLab uses).
float3 ToOkLab(float3 c)
{
    const float3x3 rgbToLms = { 0.4122214708, 0.5363325363, 0.0514459929,
                                0.2119034982, 0.6806995451, 0.1073969566,
                                0.0883024619, 0.2817188376, 0.6299787005 };
    const float3x3 lmsToLab = { 0.2104542553, 0.7936177850, -0.0040720468,
                                1.9779984951, -2.4285922050, 0.4505937099,
                                0.0259040371, 0.7827717662, -0.8086757660 };
    const float3 lms = mul(rgbToLms, c);
    return mul(lmsToLab, sign(lms) * pow(abs(lms), 1.0 / 3.0));
}

// Chroma in, chroma out, the a-b distance between them, and the b difference (warmth), for one pixel.
float4 Colour(float3 input, float3 output)
{
    const float2 abIn = ToOkLab(DisplayColour(input)).yz;
    const float2 abOut = ToOkLab(DisplayColour(output)).yz;
    const float4 c = float4(length(abIn), length(abOut), length(abOut - abIn), abOut.y - abIn.y);
    return all(isfinite(c)) ? c : 0.0;
}

float LinearToSrgb(float v)
{
    v = saturate(v);
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055;
}

float Encode(float3 rgb)
{
    const float luma = max(dot(max(rgb, 0.0), kLuma), 0.0);
    const float x = luma / max(measureWhitePoint, 1e-6);
    return isfinite(x) ? LinearToSrgb(Neutwo(x)) : 0.0;
}

float EncodedAt(Texture2D<float4> tex, int2 p)
{
    uint w, h;
    tex.GetDimensions(w, h);
    return Encode(tex.Load(int3(clamp(p, int2(0, 0), int2(w - 1, h - 1)), 0)).rgb);
}

// A box-binomial blur from bilinear taps: a tap half a pixel off a centre averages two pixels, so the offsets
// {-1.5, -0.5, 0.5, 1.5} weigh pixels -2..2 as 1 2 2 2 1 (sigma ~1.2) and {-4.5, -2.5, -0.5, 0.5, 2.5, 4.5}
// weigh -5..5 as 1 1 1 1 1 2 1 1 1 1 1 (sigma ~3.0). Symmetric about the pixel either way.
float3 BlurSmall(Texture2D<float4> tex, float2 centre, float2 texel)
{
    static const float o[4] = { -1.5, -0.5, 0.5, 1.5 };
    float3 sum = 0.0;
    [unroll] for (int j = 0; j < 4; ++j)
        [unroll] for (int i = 0; i < 4; ++i)
            sum += tex.SampleLevel(gLinear, (centre + float2(o[i], o[j])) * texel, 0).rgb;
    return sum / 16.0;
}

float3 BlurLarge(Texture2D<float4> tex, float2 centre, float2 texel)
{
    static const float o[6] = { -4.5, -2.5, -0.5, 0.5, 2.5, 4.5 };
    float3 sum = 0.0;
    [unroll] for (int j = 0; j < 6; ++j)
        [unroll] for (int i = 0; i < 6; ++i)
            sum += tex.SampleLevel(gLinear, (centre + float2(o[i], o[j])) * texel, 0).rgb;
    return sum / 36.0;
}

float Band(Texture2D<float4> tex, int2 p)
{
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 texel = 1.0 / float2(w, h);
    const float2 centre = float2(p) + 0.5;
    return abs(Encode(BlurSmall(tex, centre, texel)) - Encode(BlurLarge(tex, centre, texel)));
}

groupshared float4 gSumA[64];
groupshared float4 gSumB[64];
groupshared float4 gSumC[64];
groupshared float4 gSumD[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
#ifdef VK_MODE
    if (mode == 1u)
    {
        const uint2 p = groupId.xy * 8u + groupThreadId.xy;
        if (p.x < width && p.y < height)
            gProxy[p] = gOutput.Load(int3(p, 0));
        return;
    }
#endif

    if (groupId.x >= kGrid || groupId.y >= kGrid)
        return;

    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const uint x0 = (groupId.x * width) / kGrid;
    const uint x1 = max(((groupId.x + 1u) * width) / kGrid, x0 + 1u);
    const uint y0 = (groupId.y * height) / kGrid;
    const uint y1 = max(((groupId.y + 1u) * height) / kGrid, y0 + 1u);

    float4 a = 0.0; // raw, band out, band in, output change
    float4 b = 0.0; // input change, shoulder, floor, pixels
    float4 c4 = 0.0; // chroma in, chroma out, colour shift, warmth
    float4 d = 0.0;  // shadow pixels, their input level, their output level, crushed pixels

    [loop] for (uint y = y0 + groupThreadId.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + groupThreadId.x * 2u; x < x1; x += 16u)
        {
            const int2 p = int2(x, y);
            const float c = EncodedAt(gOutput, p);
            const float lap = abs(4.0 * c - EncodedAt(gOutput, p + int2(-1, 0)) - EncodedAt(gOutput, p + int2(1, 0)) -
                                  EncodedAt(gOutput, p + int2(0, -1)) - EncodedAt(gOutput, p + int2(0, 1)));

            uint proxyW, proxyH;
            gProxy.GetDimensions(proxyW, proxyH);
            const int2 q = min(int2((float2(p) + 0.5) * float2(proxyW, proxyH) / float2(max(width, 1u), max(height, 1u))),
                               int2(proxyW - 1, proxyH - 1));
            const float3 proxy = gProxy[uint2(q)].rgb;
            const float peak = max(proxy.r, max(proxy.g, proxy.b));

            a += float4(lap, Band(gOutput, p), Band(gInput, p), abs(c - EncodedAt(gPrevOutput, p)));
            b += float4(abs(EncodedAt(gInput, p) - EncodedAt(gPrevInput, p)), peak > shoulderThreshold ? 1.0 : 0.0,
                        peak < floorThreshold ? 1.0 : 0.0, 1.0);
            c4 += Colour(gInput.Load(int3(p, 0)).rgb, gOutput.Load(int3(p, 0)).rgb);
            const float lin = EncodedAt(gInput, p);
            if (lin < kShadowLevel)
                d += float4(1.0, lin, c, lin >= kBlackLevel && c < 0.5 * lin ? 1.0 : 0.0);
        }
    }

    gSumA[lane] = a;
    gSumB[lane] = b;
    gSumC[lane] = c4;
    gSumD[lane] = d;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
        {
            gSumA[lane] += gSumA[lane + stride];
            gSumB[lane] += gSumB[lane + stride];
            gSumC[lane] += gSumC[lane + stride];
            gSumD[lane] += gSumD[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        const float n = max(gSumB[0].w, 1.0);
        gGrid[uint2(groupId.x, groupId.y)] = gSumA[0] / n;
        gGrid[uint2(groupId.x + kGrid, groupId.y)] = float4(gSumB[0].xyz / n, gSumB[0].w);
        gGrid[uint2(groupId.x + 2u * kGrid, groupId.y)] = gSumC[0] / n;
        gGrid[uint2(groupId.x + 3u * kGrid, groupId.y)] = gSumD[0] / n;
    }
}
