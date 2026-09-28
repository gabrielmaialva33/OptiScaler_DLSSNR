#!/usr/bin/env bash
# Rebuilds the DXIL for the synthesized-motion estimator and regenerates the headers
# SynthMotion_Dx12.cpp embeds. Needs the msvc-wine prefix (dxc.exe ships in shaders/shader_tools).
# Never run it while build-local.sh is using the same prefix (CLAUDE.md, build stalls).
#
# The FFX-derived passes are compiled with the flags AMD's BuildOpticalFlowShaders.bat uses for the
# base (wave32, 32-bit) permutation: cs_6_2, FFX_HLSL_SM=62, FFX_HALF=0. Our two passes use the same.
set -euo pipefail
cd "$(dirname "$0")"

WINEPREFIX="${WINEPREFIX:-$HOME/.local/opt/msvc-wineprefix}"
export WINEPREFIX WINEDEBUG=-all
DXC="../../shader_tools/dxc.exe"
CREATE_HEADER="../../shader_tools/create_header.py"
FLAGS=(-T cs_6_2 -E CS -O3 -Qstrip_debug -Qstrip_reflect -I ffx
       -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_HALF=0 -DFFX_HLSL_SM=62 -DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0
       -Wno-for-redefinition -Wno-ambig-lit-shift)

build() { # source, output stem, array name, extra flags...
    local src="$1" stem="$2" array="$3"
    shift 3
    wine "$DXC" "${FLAGS[@]}" "$@" -Fo "$stem.cso" "$src"
    python3 "$CREATE_HEADER" "$stem.cso" "${stem}_Shader.h" "$array" > /dev/null
    echo "$stem.cso $(stat -c %s "$stem.cso") bytes"
}

build synth_motion_prepare_luma.hlsl        SynthMotion_PrepareLuma        SynthMotion_PrepareLuma_cso
build synth_motion_luminance_pyramid.hlsl   SynthMotion_LuminancePyramid   SynthMotion_LuminancePyramid_cso
build synth_motion_scd_histogram.hlsl       SynthMotion_ScdHistogram       SynthMotion_ScdHistogram_cso
build synth_motion_scd_divergence.hlsl      SynthMotion_ScdDivergence      SynthMotion_ScdDivergence_cso
build synth_motion_search.hlsl              SynthMotion_Search             SynthMotion_Search_cso
build synth_motion_filter.hlsl              SynthMotion_Filter             SynthMotion_Filter_cso
build synth_motion_scale.hlsl               SynthMotion_Scale              SynthMotion_Scale_cso
build synth_motion_expand.hlsl              SynthMotion_Expand             SynthMotion_Expand_cso
build synth_motion_clear.hlsl               SynthMotion_ClearUint          SynthMotion_ClearUint_cso
build synth_motion_clear.hlsl               SynthMotion_ClearFloat         SynthMotion_ClearFloat_cso -DSM_CLEAR_FLOAT=1

# NVIDIA Optical Flow backend (SynthMotionNvofa_Dx12.cpp). Ours, not FidelityFX code; same flags. Until
# these two headers exist the backend compiles to "unavailable" and [DlssNr] SynthMotionSource=nvofa
# falls back to the FidelityFX estimator.
build synth_motion_nvofa_prep.hlsl          SynthMotion_NvofaPrep          SynthMotion_NvofaPrep_cso
build synth_motion_nvofa_expand.hlsl        SynthMotion_NvofaExpand        SynthMotion_NvofaExpand_cso
