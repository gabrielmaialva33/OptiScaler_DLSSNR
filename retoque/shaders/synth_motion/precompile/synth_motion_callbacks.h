// Derived from the FidelityFX SDK's ffx_opticalflow_callbacks_hlsl.h (SDK 2.3.0, commit 60f4ea8),
// Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/. The original's licence follows.
//
// Changes for OptiScaler's synthesized-motion estimator (shaders/synth_motion/README.md):
//  - Every internal texture is read through a UAV, never an SRV, so all of them stay in
//    UNORDERED_ACCESS for their whole life and only UAV barriers separate the passes. FFX's own
//    passes already load R8_UINT and R16G16_SINT through UAVs (the pyramid and the search), so this
//    asks nothing of the device that FFX does not.
//  - Bindings come from the register numbers each pass file defines; the SRV-named bindings are
//    u registers too. Only the colour input is a real SRV (t0).
//  - The constant buffers are root constants (b0, b1). Their layouts are FFX's, unchanged.
//  - The colour input is saturated before the luma is taken, and the transfer function is always 0.
//  - Accessors no pass here uses (game motion vectors, histograms for global motion, additional info,
//    debug visualisation) are left out.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef SYNTH_MOTION_CALLBACKS_H
#define SYNTH_MOTION_CALLBACKS_H

#if defined(FFX_GPU)
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#endif //__hlsl_dx_compiler
#include "ffx_core.h"
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic pop
#endif //__hlsl_dx_compiler

#define FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION 1
#define FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS 1
#define FFX_OPTICALFLOW_USE_HEURISTICS 1
#define FFX_OPTICALFLOW_BLOCK_SIZE 8
#define FFX_LOCAL_SEARCH_FALLBACK 1

// As in FFX: msad4 only on the wave64 permutation, which this build does not make.
#if !defined(FFX_PREFER_WAVE64) && defined(FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION)
#undef FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION
#endif

#include "opticalflow/ffx_opticalflow_common.h"

#ifndef FFX_PREFER_WAVE64
#define FFX_PREFER_WAVE64
#endif

#pragma warning(disable : 3205) // conversion from larger type to smaller

// Internal textures read-only in a pass are still UAVs (see the header comment).
#define SM_DECLARE_RO(regIndex) : register(DECLARE_UAV_REGISTER(regIndex))

cbuffer cbOF : register(b0)
{
    FfxInt32x2 iInputLumaResolution;
    FfxUInt32 uOpticalFlowPyramidLevel;
    FfxUInt32 uOpticalFlowPyramidLevelCount;

    FfxUInt32 iFrameIndex;
    FfxUInt32 backbufferTransferFunction;
    FfxFloat32x2 minMaxLuminance;
};

#if defined(FFX_OPTICALFLOW_BIND_CB_SPD)
cbuffer cbOF_SPD : register(b1)
{
    FfxUInt32 mips;
    FfxUInt32 numWorkGroups;
    FfxUInt32x2 workGroupOffset;
    FfxUInt32 numWorkGroupOpticalFlowInputPyramid;
    FfxUInt32 pad0_;
    FfxUInt32 pad1_;
    FfxUInt32 pad2_;
};

FfxUInt32 NumWorkGroups()
{
    return numWorkGroupOpticalFlowInputPyramid;
}
#endif // FFX_OPTICALFLOW_BIND_CB_SPD

FfxInt32x2 DisplaySize()
{
    return iInputLumaResolution;
}

FfxUInt32 FrameIndex()
{
    return iFrameIndex;
}

FfxUInt32 BackbufferTransferFunction()
{
    return backbufferTransferFunction;
}

FfxFloat32x2 MinMaxLuminance()
{
    return minMaxLuminance;
}

FfxBoolean CrossedSceneChangeThreshold(FfxFloat32 sceneChangeValue)
{
    return sceneChangeValue > 0.45f;
}

FfxUInt32 OpticalFlowPyramidLevel()
{
    return uOpticalFlowPyramidLevel;
}

FfxUInt32 OpticalFlowPyramidLevelCount()
{
    return uOpticalFlowPyramidLevelCount;
}

// SRV declarations. The colour is the only real SRV.
#if defined FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR
Texture2D<FfxFloat32x4> r_input_color FFX_DECLARE_SRV(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR);
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT
RWTexture2D<FfxUInt32> r_optical_flow_input SM_DECLARE_RO(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT);
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT
RWTexture2D<FfxUInt32> r_optical_flow_previous_input SM_DECLARE_RO(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT);
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW
RWTexture2D<FfxInt32x2> r_optical_flow SM_DECLARE_RO(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW);
#endif
#if defined FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS
RWTexture2D<FfxInt32x2> r_optical_flow_previous SM_DECLARE_RO(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS);
#endif

// UAV declarations
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT
RWTexture2D<FfxUInt32> rw_optical_flow_input FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_1 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_2
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_2 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_2);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_3
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_3 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_3);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_4
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_4 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_4);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_5
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_5 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_5);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_6
globallycoherent RWTexture2D<FfxUInt32> rw_optical_flow_input_level_6 FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_6);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW
RWTexture2D<FfxInt32x2> rw_optical_flow FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL
RWTexture2D<FfxInt32x2> rw_optical_flow_next_level FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM
RWTexture2D<FfxUInt32> rw_optical_flow_scd_histogram FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM
RWTexture2D<FfxFloat32> rw_optical_flow_scd_previous_histogram FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP
RWTexture2D<FfxUInt32> rw_optical_flow_scd_temp FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP);
#endif
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT
RWTexture2D<FfxUInt32> rw_optical_flow_scd_output FFX_DECLARE_UAV(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT);
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR)
FfxFloat32x4 LoadInputColor(FfxUInt32x2 iPxHistory)
{
    // Saturated: a float frame's highlights above 1 would otherwise wrap in the R8_UINT luma.
    return saturate(r_input_color[iPxHistory]);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT)
void StoreOpticalFlowInput(FfxInt32x2 iPxPos, FfxUInt32 fLuma)
{
    rw_optical_flow_input[iPxPos] = fLuma;
}

FfxUInt32 LoadRwOpticalFlowInput(FfxInt32x2 iPxPos)
{
    return rw_optical_flow_input[iPxPos];
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT)
FfxUInt32 LoadOpticalFlowInput(FfxInt32x2 iPxPos)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1
    return max(1, r_optical_flow_input[iPxPos]);
#else
    return r_optical_flow_input[iPxPos];
#endif
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT)
FfxUInt32 LoadOpticalFlowPreviousInput(FfxInt32x2 iPxPos)
{
#if FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION == 1
    return max(1, r_optical_flow_previous_input[iPxPos]);
#else
    return r_optical_flow_previous_input[iPxPos];
#endif
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW)
FfxInt32x2 LoadOpticalFlow(FfxInt32x2 iPxPos)
{
    return r_optical_flow[iPxPos];
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW)
FfxInt32x2 LoadRwOpticalFlow(FfxInt32x2 iPxPos)
{
    return rw_optical_flow[iPxPos];
}

void StoreOpticalFlow(FfxInt32x2 iPxPos, FfxInt32x2 motionVector)
{
    rw_optical_flow[iPxPos] = motionVector;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS)
FfxInt32x2 LoadPreviousOpticalFlow(FfxInt32x2 iPxPos)
{
    return r_optical_flow_previous[iPxPos];
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_NEXT_LEVEL)
void StoreOpticalFlowNextLevel(FfxInt32x2 iPxPos, FfxInt32x2 motionVector)
{
    rw_optical_flow_next_level[iPxPos] = motionVector;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_HISTOGRAM)
FfxUInt32 LoadRwSCDHistogram(FfxInt32 iIndex)
{
    return rw_optical_flow_scd_histogram[FfxInt32x2(iIndex, 0)];
}

void StoreSCDHistogram(FfxInt32 iIndex, FfxUInt32 value)
{
    rw_optical_flow_scd_histogram[FfxInt32x2(iIndex, 0)] = value;
}

void AtomicIncrementSCDHistogram(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
    InterlockedAdd(rw_optical_flow_scd_histogram[FfxInt32x2(iIndex, 0)], valueToAdd);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM)
FfxFloat32 LoadRwSCDPreviousHistogram(FfxInt32 iIndex)
{
    return rw_optical_flow_scd_previous_histogram[FfxInt32x2(iIndex, 0)];
}

void StoreSCDPreviousHistogram(FfxInt32 iIndex, FfxFloat32 value)
{
    rw_optical_flow_scd_previous_histogram[FfxInt32x2(iIndex, 0)] = value;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_TEMP)
FfxUInt32 LoadRwSCDTemp(FfxInt32 iIndex)
{
    return rw_optical_flow_scd_temp[FfxInt32x2(iIndex, 0)];
}

void AtomicIncrementSCDTemp(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
    InterlockedAdd(rw_optical_flow_scd_temp[FfxInt32x2(iIndex, 0)], valueToAdd);
}

void ResetSCDTemp()
{
    rw_optical_flow_scd_temp[FfxInt32x2(0, 0)] = 0;
    rw_optical_flow_scd_temp[FfxInt32x2(1, 0)] = 0;
    rw_optical_flow_scd_temp[FfxInt32x2(2, 0)] = 0;
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT)
FfxUInt32 LoadRwSCDOutput(FfxInt32 iIndex)
{
    return rw_optical_flow_scd_output[FfxInt32x2(iIndex, 0)];
}

void StoreSCDOutput(FfxInt32 iIndex, FfxUInt32 value)
{
    rw_optical_flow_scd_output[FfxInt32x2(iIndex, 0)] = value;
}

FfxUInt32 AtomicIncrementSCDOutput(FfxInt32 iIndex, FfxUInt32 valueToAdd)
{
    FfxUInt32 initialValue;
    InterlockedAdd(rw_optical_flow_scd_output[FfxInt32x2(iIndex, 0)], valueToAdd, initialValue);
    return initialValue;
}

FfxFloat32 GetSceneChangeValue()
{
    if (FrameIndex() <= 5)
        return 1.0;
    else
        return ffxAsFloat(LoadRwSCDOutput(SCD_OUTPUT_SCENE_CHANGE_SLOT));
}

FfxBoolean IsSceneChanged()
{
    if (FrameIndex() <= 5)
    {
        return 1.0;
    }
    else
    {
        return (LoadRwSCDOutput(SCD_OUTPUT_HISTORY_BITS_SLOT) & 0xfu) != 0;
    }
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT)
FfxUInt32 LoadFirstImagePackedLuma(FfxInt32x2 iPxPos)
{
    const FfxInt32 lumaTextureWidth = DisplaySize().x >> OpticalFlowPyramidLevel();
    const FfxInt32 lumaTextureHeight = DisplaySize().y >> OpticalFlowPyramidLevel();

    FfxInt32x2 adjustedPos = FfxInt32x2(ffxClamp(iPxPos.x, 0, lumaTextureWidth - 4), ffxClamp(iPxPos.y, 0, lumaTextureHeight - 1));

    FfxUInt32 luma0 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(0, 0));
    FfxUInt32 luma1 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(1, 0));
    FfxUInt32 luma2 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(2, 0));
    FfxUInt32 luma3 = LoadOpticalFlowInput(adjustedPos + FfxInt32x2(3, 0));

    return GetPackedLuma(lumaTextureWidth, iPxPos.x, luma0, luma1, luma2, luma3);
}
#endif

#if defined(FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT)
FfxUInt32 LoadSecondImagePackedLuma(FfxInt32x2 iPxPos)
{
    const FfxInt32 lumaTextureWidth = DisplaySize().x >> OpticalFlowPyramidLevel();
    const FfxInt32 lumaTextureHeight = DisplaySize().y >> OpticalFlowPyramidLevel();

    FfxInt32x2 adjustedPos = FfxInt32x2(ffxClamp(iPxPos.x, 0, lumaTextureWidth - 4), ffxClamp(iPxPos.y, 0, lumaTextureHeight - 1));

    FfxUInt32 luma0 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(0, 0));
    FfxUInt32 luma1 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(1, 0));
    FfxUInt32 luma2 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(2, 0));
    FfxUInt32 luma3 = LoadOpticalFlowPreviousInput(adjustedPos + FfxInt32x2(3, 0));

    return GetPackedLuma(lumaTextureWidth, iPxPos.x, luma0, luma1, luma2, luma3);
}
#endif

void SPD_SetMipmap(int2 iPxPos, int index, float value)
{
    switch (index)
    {
    case 0:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_1
        rw_optical_flow_input_level_1[iPxPos] = value;
#endif
        break;
    case 1:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_2
        rw_optical_flow_input_level_2[iPxPos] = value;
#endif
        break;
    case 2:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_3
        rw_optical_flow_input_level_3[iPxPos] = value;
#endif
        break;
    case 3:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_4
        rw_optical_flow_input_level_4[iPxPos] = value;
#endif
        break;
    case 4:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_5
        rw_optical_flow_input_level_5[iPxPos] = value;
#endif
        break;
    case 5:
#if defined FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT_LEVEL_6
        rw_optical_flow_input_level_6[iPxPos] = value;
#endif
        break;
    }
}

#endif // #if defined(FFX_GPU)

#endif // SYNTH_MOTION_CALLBACKS_H
