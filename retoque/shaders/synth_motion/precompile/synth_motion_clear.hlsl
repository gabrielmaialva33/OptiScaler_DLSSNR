// OptiScaler synthesized-motion estimator: zeroes one texture (ours, not FidelityFX code). Used on a
// reset for the scene-change textures, which FFX clears the same way before their first use. Built
// twice: SM_CLEAR_FLOAT for the R32_FLOAT previous histogram, without it for the R32_UINT ones.

#if defined(VK_MODE)
// One push constant block, and a storage image of a named format (nr-vulkan-present-host.md, "Stage 5").
struct SynthMotionClearConstants
{
    uint2 gSize;
    uint2 gPad0;
    uint4 gPad1;
};
[[vk::push_constant]] SynthMotionClearConstants gClear;
#define gSize gClear.gSize
#define SM_VK_FORMAT(format) [[vk::image_format(format)]]
#else
cbuffer SynthMotionClear : register(b0)
{
    uint2 gSize;
    uint2 gPad0;
    uint4 gPad1;
};
#define SM_VK_FORMAT(format)
#endif

#if defined(SM_CLEAR_FLOAT)
SM_VK_FORMAT("r32f") RWTexture2D<float> rw_target : register(u0);
#else
SM_VK_FORMAT("r32ui") RWTexture2D<uint> rw_target : register(u0);
#endif

[numthreads(64, 1, 1)]
void CS(uint2 id : SV_DispatchThreadID)
{
    if (all(id < gSize))
        rw_target[id] = 0;
}
