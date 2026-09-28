#pragma once

// Which of synthesized frame generation's two HUD fixes run for a base frame, and what FG is handed.
// Pure logic with no D3D in it, so the host tests drive it directly (tests/fg-synth-policy). Design:
// dlssnr/design/synthesized-frame-generation.md, "The HUD: near depth and a UI layer" and "DLSS-G output".
//
// - Fix A, [FrameGen] SynthesizedHudDepth: the HUD mask's depth (1.0, near, where the mask holds) instead of
//   the constant zero depth. Any output takes it: it is only the depth FG is given.
// - Fix B, [FrameGen] SynthesizedHudLayer: the frame with the mask as alpha, registered as FFX's UI resource.
//   FSR-FG only: DLSS-G recomposes UI only with a HUD-less colour this input does not have.

struct SynthHudPlan
{
    bool depth = false;  // Fix A runs
    bool layer = false;  // Fix B runs
    bool detect = false; // the mask is recorded at all
};

// hudDepth, hudLayer: the two keys. motion: [FrameGen] SynthesizedMotion. disableUi: [FrameGen] DisableUI.
// layerComposed: the active output composes a registered UI layer over every frame it presents (FSR-FG).
inline SynthHudPlan PlanSynthHud(bool hudDepth, bool hudLayer, bool motion, bool disableUi, bool layerComposed)
{
    SynthHudPlan plan {};

    // With zero game vectors FSR samples every pixel in place, so no side is ever occluded, and every vector
    // ties at zero anyway: near depth would change nothing, and the mask would be paid for nothing.
    plan.depth = hudDepth && motion;

    // FSRFG_Dx12::SetResource refuses every UI resource under DisableUI, and an output that does not compose
    // the layer (DLSS-G) would take it and do nothing with it; either way a layer nobody uses is waste.
    plan.layer = hudLayer && !disableUi && layerComposed;

    plan.detect = plan.depth || plan.layer;
    return plan;
}

struct SynthHudFeed
{
    bool maskDepth = false; // hand FG the mask's depth; otherwise the constant zero depth
    bool layer = false;     // hand FSR the layer as its UI resource
};

// maskThisFrame: the mask was recorded for this base frame, its list executed, at the presenter's extent.
// layerReady: a layer recording at that extent has executed at least once. On native D3D12 the layer is
// written after this decision (after DLSS-NR's pass has edited the backbuffer), so without this the first
// frame would hand FFX memory nothing ever wrote.
inline SynthHudFeed ChooseSynthHudFeed(const SynthHudPlan& plan, bool maskThisFrame, bool layerReady)
{
    SynthHudFeed feed {};
    feed.maskDepth = plan.depth && maskThisFrame;
    feed.layer = plan.layer && maskThisFrame && layerReady;
    return feed;
}
