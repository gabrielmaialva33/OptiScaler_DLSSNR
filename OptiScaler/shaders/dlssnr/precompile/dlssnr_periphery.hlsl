// Peripheral compression: the model's input packed denser in the centre than at the edges, and its answer
// brought back onto the uniform grid. dlssnr/design/peripheral-compression.md.
//
// One source, three passes, chosen at compile time so each binds exactly the resource types it writes:
//
//   PERIPHERY_COLOUR  the frame's proxy -> the packed proxy: the exact area average over each packed texel's
//                     warped footprint (the downsample's box integral with warped bounds)
//   PERIPHERY_GUIDES  depth and motion -> packed depth and motion: point-sampled at the packed texel's frame
//                     position; motion converted by moving both ends of the vector through the mapping
//   PERIPHERY_UNPACK  the packed proxy and the packed answer -> the uniform grid, bilinear
//
// The mapping is dlssnr_periphery_warp.h (BeliyG3's PeripheralWarp, MIT), the same file the host tests run.
//
// Build (from OptiScaler/shaders/dlssnr/precompile, dxc under the msvc-wine prefix on Linux, never beside a
// running DLL build):
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -D PERIPHERY_COLOUR -Fo DlssNr_PeripheryColour_Shader.cso dlssnr_periphery.hlsl
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -D PERIPHERY_GUIDES -Fo DlssNr_PeripheryGuides_Shader.cso dlssnr_periphery.hlsl
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -D PERIPHERY_UNPACK -Fo DlssNr_PeripheryUnpack_Shader.cso dlssnr_periphery.hlsl
//   python3 ../../shader_tools/create_header.py DlssNr_PeripheryColour_Shader.cso DlssNr_PeripheryColour_Shader.h DlssNr_PeripheryColour_cso
//   python3 ../../shader_tools/create_header.py DlssNr_PeripheryGuides_Shader.cso DlssNr_PeripheryGuides_Shader.h DlssNr_PeripheryGuides_cso
//   python3 ../../shader_tools/create_header.py DlssNr_PeripheryUnpack_Shader.cso DlssNr_PeripheryUnpack_Shader.h DlssNr_PeripheryUnpack_cso
// DlssNr_Periphery_Dx12.cpp picks the three headers up through __has_include; without all three the feature
// reports itself unavailable and the pass runs exactly as it did before.

#if defined(PERIPHERY_COLOUR)
Texture2D<float4> gSource : register(t0);  // the frame's proxy, frame size
RWTexture2D<float4> gPacked : register(u0); // the model's input, packed extent
#elif defined(PERIPHERY_GUIDES)
Texture2D<float> gDepth : register(t0);
Texture2D<float2> gMotion : register(t1);
RWTexture2D<float> gDepthOut : register(u0);   // R32_FLOAT, packed extent
RWTexture2D<float2> gMotionOut : register(u1); // R32G32_FLOAT, packed extent, in packed pixels
#elif defined(PERIPHERY_UNPACK)
Texture2D<float4> gPackedProxy : register(t0);
Texture2D<float4> gPackedAnswer : register(t1);
RWTexture2D<float4> gProxyOut : register(u0);  // R16G16B16A16_FLOAT, uniform grid
RWTexture2D<float4> gAnswerOut : register(u1); // R16G16B16A16_FLOAT, uniform grid
SamplerState gLinearClamp : register(s0);
#else
#error define one of PERIPHERY_COLOUR, PERIPHERY_GUIDES, PERIPHERY_UNPACK
#endif

#define PW_FN
#define PW_ABS(v) abs(v)
#define PW_FLOOR(v) floor(v)
#define PW_CEIL(v) ceil(v)
#define PW_MINF(a, b) min((a), (b))
#define PW_MAXF(a, b) max((a), (b))
#define PW_MINI(a, b) min((a), (b))
#define PW_MAXI(a, b) max((a), (b))

#if defined(PERIPHERY_COLOUR)
#define PW_COLOUR float3
#define PW_COLOUR_ZERO float3(0.0f, 0.0f, 0.0f)
#define PW_LOAD_COLOUR(ci, cj) gSource.Load(int3((ci), (cj), 0)).rgb
#endif

#include "dlssnr_periphery_warp.h"

// One ordered list with DlssNr::Periphery::PeripheryConstants (DlssNr_Periphery.h); tests/nr-periphery holds
// the two together.
cbuffer Params : register(b0)
{
    uint gOutWidth;
    uint gOutHeight;
    uint gNativeWidth;
    uint gNativeHeight;
    uint gModelWidth;
    uint gModelHeight;
    uint gDepthBaseX;
    uint gDepthBaseY;
    uint gDepthWidth;
    uint gDepthHeight;
    uint gMotionBaseX;
    uint gMotionBaseY;
    uint gMotionWidth;
    uint gMotionHeight;
    float gMotionScaleX;
    float gMotionScaleY;
    PeripheryAxis gAxisX;
    PeripheryAxis gAxisY;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutWidth || id.y >= gOutHeight)
        return;

#if defined(PERIPHERY_COLOUR)
    // The packed texel's footprint in the frame: its two edges, unpacked. Integrated exactly, so every frame
    // pixel reaches the model in proportion to how much of the packed texel it covers -- at the edges of the
    // frame a packed texel covers several, and a tap filter there would alias.
    const float x0 = PeripheryUnpack((float) id.x, gAxisX);
    const float x1 = PeripheryUnpack((float) id.x + 1.0f, gAxisX);
    const float y0 = PeripheryUnpack((float) id.y, gAxisY);
    const float y1 = PeripheryUnpack((float) id.y + 1.0f, gAxisY);
    const float3 rgb = PeripheryAreaAverage(x0, x1, y0, y1, (int) gNativeWidth, (int) gNativeHeight);

    // Alpha is the centre texel's, as the downsample takes it.
    const int cx = clamp((int) floor(PeripheryUnpack((float) id.x + 0.5f, gAxisX)), 0, (int) gNativeWidth - 1);
    const int cy = clamp((int) floor(PeripheryUnpack((float) id.y + 0.5f, gAxisY)), 0, (int) gNativeHeight - 1);
    gPacked[id.xy] = float4(rgb, gSource.Load(int3(cx, cy, 0)).a);
#elif defined(PERIPHERY_GUIDES)
    // Point, not linear, for the reasons the guide resample gives: a depth between two surfaces is a surface
    // that does not exist, and a vector averaged across an edge belongs to neither side.
    const float nx = PeripheryUnpack((float) id.x + 0.5f, gAxisX);
    const float ny = PeripheryUnpack((float) id.y + 0.5f, gAxisY);

    const int2 depthAt = int2(PeripheryGuideTexel(nx, (float) gNativeWidth, (int) gDepthBaseX, (int) gDepthWidth),
                              PeripheryGuideTexel(ny, (float) gNativeHeight, (int) gDepthBaseY, (int) gDepthHeight));
    const int2 motionAt =
        int2(PeripheryGuideTexel(nx, (float) gNativeWidth, (int) gMotionBaseX, (int) gMotionWidth),
             PeripheryGuideTexel(ny, (float) gNativeHeight, (int) gMotionBaseY, (int) gMotionHeight));

    // The game's units to frame pixels first -- the host's FrameMotionScale -- then both ends through the
    // mapping. The model is handed a scale of 1: these are packed pixels already.
    const float2 motion = gMotion.Load(int3(motionAt, 0)) * float2(gMotionScaleX, gMotionScaleY);

    gDepthOut[id.xy] = gDepth.Load(int3(depthAt, 0));
    gMotionOut[id.xy] = float2(PeripheryPackMotion(nx, motion.x, gAxisX), PeripheryPackMotion(ny, motion.y, gAxisY));
#elif defined(PERIPHERY_UNPACK)
    // The uniform grid's texel centre in the frame, then where that landed in the packed raster. The resolve
    // reads the pair this writes exactly as it reads the uniform path's proxy and answer.
    const float2 native = (float2(id.xy) + 0.5f) * float2(gNativeWidth, gNativeHeight) / float2(gOutWidth, gOutHeight);
    const float2 packed = float2(PeripheryPack(native.x, gAxisX), PeripheryPack(native.y, gAxisY));
    const float2 uv = packed / float2(gModelWidth, gModelHeight);

    gProxyOut[id.xy] = gPackedProxy.SampleLevel(gLinearClamp, uv, 0);
    gAnswerOut[id.xy] = gPackedAnswer.SampleLevel(gLinearClamp, uv, 0);
#endif
}
