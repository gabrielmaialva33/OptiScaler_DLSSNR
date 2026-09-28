// Synthesized frame generation's UI layer: the frame as it will be presented, with the HUD mask
// (synth_overlay_detect.hlsl) as alpha. FFX's frame-generation swapchain composes it over every frame it
// presents, lerp(backbuffer, layer.rgb, layer.a): in a generated frame the masked pixels are the base frame's,
// and in a real frame lerp(x, x, a) leaves the frame as it was. Design: dlssnr/design/
// synthesized-frame-generation.md, "The HUD: near depth and a UI layer". Class: SynthMotion::Overlay_Dx12.
//
// The colour is read through a view of its own format, as FFX reads the backbuffer, so the two agree: an sRGB
// view gives linear light here and there. Precompiled by build.sh beside this file.

Texture2D<float4>   gColour : register(t0); // the frame as it will be presented
Texture2D<float>    gMask   : register(t1); // this frame's HUD mask, 0..1
RWTexture2D<float4> gLayer  : register(u0); // RGBA8 UNORM for an 8-bit frame, RGBA16F otherwise

cbuffer Params : register(b0) // sixteen root constants; the first two are used
{
    uint gWidth;
    uint gHeight;
};

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int3 p = int3(id.xy, 0);
    gLayer[id.xy] = float4(gColour.Load(p).rgb, gMask.Load(p));
}
