// Derived from the FidelityFX SDK's ffx_opticalflow_prepare_luma_pass.hlsl (SDK 2.3.0, commit 60f4ea8),
// Kits/FidelityFX/framegeneration/fsr3/internal/shaders/. Changed for OptiScaler's synthesized-motion
// estimator: register numbers follow its single root signature (shaders/synth_motion/README.md), and
// the callbacks come from synth_motion_callbacks.h. The pass body is FFX's, unchanged.
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

// t0 colour; u0 current luma (level 0)
#define FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "synth_motion_callbacks.h"

#ifndef FFX_OPTICALFLOW_THREAD_GROUP_WIDTH
#define FFX_OPTICALFLOW_THREAD_GROUP_WIDTH 16
#endif
#ifndef FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT
#define FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT 16
#endif
#ifndef FFX_OPTICALFLOW_THREAD_GROUP_DEPTH
#define FFX_OPTICALFLOW_THREAD_GROUP_DEPTH 1
#endif
#ifndef FFX_OPTICALFLOW_NUM_THREADS
#define FFX_OPTICALFLOW_NUM_THREADS [numthreads(FFX_OPTICALFLOW_THREAD_GROUP_WIDTH, FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT, FFX_OPTICALFLOW_THREAD_GROUP_DEPTH)]
#endif

#include "opticalflow/ffx_opticalflow_prepare_luma.h"

FFX_OPTICALFLOW_NUM_THREADS
void CS(int2 iGlobalId : SV_DispatchThreadID, int iLocalIndex : SV_GroupIndex)
{
    PrepareLuma(iGlobalId, iLocalIndex);
}
