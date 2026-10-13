// OptiScaler synthesized-motion estimator, NVIDIA Optical Flow backend: the input pass, ours.
//
// NVIDIA's optical-flow engine takes an 8-bit grayscale frame (NV_OF_BUFFER_FORMAT_GRAYSCALE8, an
// R8_UNORM texture) at the resolution its session was created for. dxvk-nvapi's D3D12 path accepts no
// other input format, so this is what both platforms get. The session runs at a reduced height
// (NvofaEstimator_Dx12::MaxInputHeight) rather than the colour's own: the engine's cost scales with
// the input, and a guide does not need more than a vector every few pixels.
//
// Each input texel box-averages the colour footprint it covers (at most 4x4 taps), so aliasing in the
// downscale does not become false motion. Luma is Rec.709 on the saturated colour, the same choice the
// FidelityFX backend makes: HDR highlights above 1 clip.

cbuffer SynthMotionNvofaPrep : register(b0)
{
    uint2 gInputSize;  // the session's input extent
    uint2 gColourSize; // the colour's extent
    uint4 gPad0;
    uint4 gPad1;
};

Texture2D<float4> r_colour : register(t0);
RWTexture2D<unorm float> rw_luma : register(u0);

[numthreads(8, 8, 1)]
void CS(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= gInputSize))
        return;

    const float2 scale = float2(gColourSize) / float2(gInputSize);
    const float2 origin = float2(id) * scale;
    const uint taps = clamp(uint(ceil(max(scale.x, scale.y))), 1u, 4u);
    const float2 step = scale / float(taps);
    const uint2 last = gColourSize - 1;

    float sum = 0.0f;
    for (uint y = 0; y < taps; ++y)
    {
        for (uint x = 0; x < taps; ++x)
        {
            const uint2 p = min(uint2(origin + (float2(x, y) + 0.5f) * step), last);
            const float3 c = saturate(r_colour.Load(int3(p, 0)).rgb);
            sum += dot(c, float3(0.2126f, 0.7152f, 0.0722f));
        }
    }

    rw_luma[id] = sum / float(taps * taps);
}
