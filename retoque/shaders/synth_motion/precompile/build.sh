#!/usr/bin/env bash
# Rebuilds the DXIL for the synthesized-motion estimator and the SPIR-V for its Vulkan twin, and regenerates the
# headers SynthMotion_Dx12.cpp and SynthMotion_Vk.cpp embed. Needs the msvc-wine prefix (dxc.exe ships in
# OptiScaler/shaders/shader_tools). Never run it while build-local.sh is using the same prefix (CLAUDE.md, build
# stalls); with other agents on the machine, under `flock -o ~/.cache/retoque-wt/msvc-prefix.lock`.
#
# The FFX-derived passes are compiled with the flags AMD's BuildOpticalFlowShaders.bat uses for the
# base (wave32, 32-bit) permutation: cs_6_2, FFX_HLSL_SM=62, FFX_HALF=0. Our two passes use the same.
set -euo pipefail
cd "$(dirname "$0")"

WINEPREFIX="${WINEPREFIX:-$HOME/.local/opt/msvc-wineprefix}"
export WINEPREFIX WINEDEBUG=-all
DXC="../../../../OptiScaler/shaders/shader_tools/dxc.exe"
CREATE_HEADER="../../../../OptiScaler/shaders/shader_tools/create_header.py"
FLAGS=(-T cs_6_2 -E CS -O3 -Qstrip_debug -Qstrip_reflect -I ffx
       -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_HALF=0 -DFFX_HLSL_SM=62 -DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0
       -Wno-for-redefinition -Wno-ambig-lit-shift)

# The Vulkan estimator (SynthMotion_Vk.cpp, nr-vulkan-present-host.md "Stage 5"): the same sources under VK_MODE,
# SPIR-V for Vulkan 1.1 (the search's subgroup arithmetic). u0..u7 are bindings 0..7 and the colour's t0 binding 8, in
# descriptor set 0. The DXIL above does not change with VK_MODE's edits: everything they add is under it.
VK_FLAGS=(-spirv -fspv-target-env=vulkan1.1 -T cs_6_2 -E CS -O3 -Qstrip_debug -I ffx -D VK_MODE
          -DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_HALF=0 -DFFX_HLSL_SM=62 -DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0
          -Wno-for-redefinition -Wno-ambig-lit-shift -fvk-u-shift 0 0 -fvk-t-shift 8 0)

build() { # source, output stem, array name, extra flags...
    local src="$1" stem="$2" array="$3"
    shift 3
    wine "$DXC" "${FLAGS[@]}" "$@" -Fo "$stem.cso" "$src"
    python3 "$CREATE_HEADER" "$stem.cso" "${stem}_Shader.h" "$array" > /dev/null
    echo "$stem.cso $(stat -c %s "$stem.cso") bytes"
}

build_vk() { # source, output stem, extra flags... -> <stem>_Shader_Vk.spv, <stem>_Shader_Vk.h, array <stem>_spv
    local src="$1" stem="$2"
    shift 2
    wine "$DXC" "${VK_FLAGS[@]}" "$@" -Fo "${stem}_Shader_Vk.spv" "$src"
    python3 "$CREATE_HEADER" "${stem}_Shader_Vk.spv" "${stem}_Shader_Vk.h" "${stem}_spv" > /dev/null
    echo "${stem}_Shader_Vk.spv $(stat -c %s "${stem}_Shader_Vk.spv") bytes"
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

# Synthesized frame generation's HUD mask and UI layer (SynthOverlay_Dx12.cpp). Ours; same flags. The mask
# runs DLSS-NR's static-overlay rule, static_overlay_rule.h, which dlssnr_uimask.hlsl includes from here. The
# DetectLayer permutation adds the UI layer's own mask (static_overlay_layer.h, frame generation only); the plain
# Detect is the same source without it, and must stay the bytecode it was.
build synth_overlay_detect.hlsl             SynthOverlay_Detect            SynthOverlay_Detect_cso
build synth_overlay_detect.hlsl             SynthOverlay_DetectLayer       SynthOverlay_DetectLayer_cso -DSYNTH_OVERLAY_LAYER=1
build synth_overlay_layer.hlsl              SynthOverlay_Layer             SynthOverlay_Layer_cso

# The Vulkan estimator's ten passes; no NVOFA and no HUD mask (neither exists on Vulkan).
build_vk synth_motion_prepare_luma.hlsl        SynthMotion_PrepareLuma
build_vk synth_motion_luminance_pyramid.hlsl   SynthMotion_LuminancePyramid
build_vk synth_motion_scd_histogram.hlsl       SynthMotion_ScdHistogram
build_vk synth_motion_scd_divergence.hlsl      SynthMotion_ScdDivergence
build_vk synth_motion_search.hlsl              SynthMotion_Search
build_vk synth_motion_filter.hlsl              SynthMotion_Filter
build_vk synth_motion_scale.hlsl               SynthMotion_Scale
build_vk synth_motion_expand.hlsl              SynthMotion_Expand
build_vk synth_motion_clear.hlsl               SynthMotion_ClearUint
build_vk synth_motion_clear.hlsl               SynthMotion_ClearFloat -DSM_CLEAR_FLOAT=1
