// The pulse metric ([DlssNr] PulseMetric, DlssNr_Pulse.h, docs/dlssnr/design/pulse-metric.md): the luminance of the
// pass's output and of the game's frame under it, summed over a 16x16 grid of tiles, read back and turned into one
// number per frame on the CPU -- the edit's light, log2 of the ratio. Only while the metric is on.
//
// Separate from dlssnr.hlsl so the composition, its SPIR-V twin and the occupancy of every ordinary pass stay as
// they are, and from the detail measure, which is a 60-frame session on a still scene rather than a running number.
//
// One 8x8 thread group per tile. Every second pixel in each direction is read: the result is a mean, and a quarter
// of the pixels is plenty for one. Per tile: (luminance of the output, luminance of the frame, pixels read, 0).
//
// Precompiled like dlssnr.hlsl: editing this alone changes nothing. From this directory (dxc under the msvc-wine
// prefix on Linux, never while a build is using it):
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_pulse.hlsl -Fo DlssNr_Pulse_Shader.cso
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_Pulse_Shader.cso DlssNr_Pulse_Shader.h DlssNr_Pulse_cso
// DlssNr_Pulse_Dx12.cpp picks the header up through __has_include; without it the metric reports itself unavailable
// and the menu does not offer it.

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gPad0;
    uint gPad1;
};

Texture2D<float4>   gOutput : register(t0); // the edited frame, as the pass leaves it
Texture2D<float4>   gInput  : register(t1); // the frame before NR (the encode's untouched copy)
RWTexture2D<float4> gGrid   : register(u0); // per tile: (luminance out, luminance in, pixels read, 0)

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const uint kGrid = 16;

// Rec.709 luminance of the non-negative part, in the buffer's own units -- the ratio does not care which. A pixel
// that is not finite counts as black on both sides, rather than poisoning the tile's sum.
float Luma(float3 c)
{
    const float y = dot(max(c, 0.0), kLuma);
    return isfinite(y) ? y : 0.0;
}

groupshared float3 gSum[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    // Uniform across the group: a whole group returns or none of it does, so the barriers below are safe.
    if (groupId.x >= kGrid || groupId.y >= kGrid)
        return;

    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const uint x0 = (groupId.x * gWidth) / kGrid;
    const uint x1 = min(max(((groupId.x + 1u) * gWidth) / kGrid, x0 + 1u), gWidth);
    const uint y0 = (groupId.y * gHeight) / kGrid;
    const uint y1 = min(max(((groupId.y + 1u) * gHeight) / kGrid, y0 + 1u), gHeight);

    float3 sum = 0.0;

    [loop] for (uint y = y0 + groupThreadId.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + groupThreadId.x * 2u; x < x1; x += 16u)
        {
            const int3 p = int3(x, y, 0);
            sum += float3(Luma(gOutput.Load(p).rgb), Luma(gInput.Load(p).rgb), 1.0);
        }
    }

    gSum[lane] = sum;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gSum[lane] += gSum[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
        gGrid[groupId.xy] = float4(gSum[0], 0.0);
}
