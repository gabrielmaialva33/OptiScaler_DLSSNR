// Depth and motion vectors, point-resampled from the upscaler's region to the NR model's working size.
// design/reduced-scale-guides.md.
//
// Point, not linear: a depth between two surfaces is a surface that does not exist, and a vector
// averaged across an edge belongs to neither side. The vectors keep the game's units; the host
// re-expresses the motion-vector scale for this texture (DlssNr_GuideMatch.h).
//
// The texel read is the destination texel's centre mapped into the region, floored -- the same integer
// expression as DlssNr::GuideMatch::PointSource, which the host tests pin.
//
// Build (from OptiScaler/shaders/dlssnr/precompile, dxc under the msvc-wine prefix on Linux):
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -Fo DlssNr_Guides_Shader.cso dlssnr_guides.hlsl
//   python3 ../../shader_tools/create_header.py DlssNr_Guides_Shader.cso DlssNr_Guides_Shader.h DlssNr_Guides_cso
// DlssNr_GuideMatch_Dx12.cpp picks the header up through __has_include; without it the pass reports
// itself unavailable and the model keeps the frame-size guides.

cbuffer Params : register(b0)
{
    uint gDepthBaseX;
    uint gDepthBaseY;
    uint gDepthWidth;
    uint gDepthHeight;
    uint gMotionBaseX;
    uint gMotionBaseY;
    uint gMotionWidth;
    uint gMotionHeight;
    uint gOutWidth;
    uint gOutHeight;
    uint gPad0;
    uint gPad1;
};

Texture2D<float> gDepth : register(t0);
Texture2D<float2> gMotion : register(t1);
RWTexture2D<float> gDepthOut : register(u0);
RWTexture2D<float2> gMotionOut : register(u1);

uint PointSource(uint destination, uint destinationExtent, uint regionBase, uint regionExtent)
{
    const uint scaled = ((destination * 2u + 1u) * regionExtent) / (destinationExtent * 2u);
    return regionBase + min(scaled, regionExtent - 1u);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutWidth || id.y >= gOutHeight)
        return;

    const uint2 depthAt = uint2(PointSource(id.x, gOutWidth, gDepthBaseX, gDepthWidth),
                                PointSource(id.y, gOutHeight, gDepthBaseY, gDepthHeight));
    const uint2 motionAt = uint2(PointSource(id.x, gOutWidth, gMotionBaseX, gMotionWidth),
                                 PointSource(id.y, gOutHeight, gMotionBaseY, gMotionHeight));

    gDepthOut[id.xy] = gDepth.Load(int3(depthAt, 0));
    gMotionOut[id.xy] = gMotion.Load(int3(motionAt, 0));
}
