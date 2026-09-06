#include "pch.h"
#include <misc/Localization.h>
#include "DlssNrFeature_Vk.h"

#include "DlssNr.h"
#include "DlssNr_ExposureScan.h"
#include "DlssNr_GpuTiming.h"

#include <Config.h>
#include <State.h>
#include <menu/menu_common.h>

#include <imgui/imgui.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace DlssNr
{

// The "(?)" marker every control carries, matching the rest of the menu.
static void HelpMarker(const char* tip)
{
    ImGui::SameLine();
    ImGui::TextDisabled(Localization::Tr("(?)"));

    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        ImGui::TextUnformatted(Localization::Tr(tip));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Historical confirmed samples have their own immutable contract. Never present them as the
// current frame's cost, or as an average of the active configuration.
static void RenderGpuTiming(Config* config, bool nativeVulkan)
{
    ImGui::SeparatorText(Localization::Tr("Fence-confirmed GPU timing"));
    if (nativeVulkan || config->DlssNrUseProxy.value_or_default())
    {
        GpuTiming::SetEnabled(false);
        ImGui::TextWrapped(Localization::Tr("Confirmed timing is available on the D3D12 NR path and its bridges. "
                                            "This backend is not instrumented."));
        return;
    }
    auto settings = config->GetDlssNrGpuTimingSettings();
    if (ImGui::Checkbox(Localization::Label("Measure NR on the GPU###nrGpuTiming"), &settings.Enabled))
        config->SetDlssNrGpuTimingEnabled(settings.Enabled);
    HelpMarker("Each published sample belongs to one NR evaluation and one observed queue submission. "
               "It is read only after that submission completes and the command-list recording is sealed "
               "against replay. No GPU wait is introduced. Unsupported or ambiguous samples are discarded."
               "\n\nThe interval covers GPU commands inside the NR Dispatch, including copies, model calls, "
               "composition and resource restoration. Caller-side pre-upscale preparation, the upscaler, "
               "and frame generation are outside this interval. Queue elapsed time is not pure kernel busy time.");
    GpuTiming::SetEnabled(settings.Enabled && config->DlssNrEnabled.value_or_default());
    if (!settings.Enabled)
    {
        ImGui::TextDisabled(Localization::Tr("Measurement off. Pending samples may still be retained safely."));
        return;
    }
    int interval = static_cast<int>(settings.Interval);
    if (ImGui::SliderInt(Localization::Label("Sample every N eligible evaluations###nrGpuTimingInterval"), &interval, 1,
                         10000))
        config->SetDlssNrGpuTimingInterval(static_cast<uint32_t>(interval));
    ImGui::TextWrapped(Localization::Tr("The first eligible evaluation is sampled, then every %u evaluations. "
                                        "An evaluation is not necessarily a presentation frame."),
                       static_cast<unsigned>(interval));
    const auto sample = GpuTiming::GetSnapshot();
    ImGui::Text(Localization::Tr("Accepted %llu | discarded %llu | pending %u"),
                static_cast<unsigned long long>(sample.accepted), static_cast<unsigned long long>(sample.discarded),
                sample.pending);
    ImGui::TextWrapped("%s", Localization::Tr(sample.reason));
    if (!sample.valid)
    {
        ImGui::TextDisabled(Localization::Tr("No confirmed GPU sample yet."));
        return;
    }
    ImGui::TextWrapped(
        Localization::Tr("Last confirmed sample: %.3f ms dispatch = %.3f ms model + %.3f ms outside model"),
        sample.totalMs, sample.modelMs, sample.otherMs);
    ImGui::TextWrapped(Localization::Tr("Historical sample %llu, %.1f s since recording; %s, model %ux%u, %u passes"),
                       static_cast<unsigned long long>(sample.sampleId), sample.sampleAgeMs / 1000.0,
                       sample.metadata.stage, sample.metadata.modelWidth, sample.metadata.modelHeight,
                       sample.actualPasses);
    ImGui::TextWrapped(Localization::Tr("Sample contract %llu, evaluation %llu, run %llu; interval was %u evaluations"),
                       static_cast<unsigned long long>(sample.metadata.settingsGeneration),
                       static_cast<unsigned long long>(sample.metadata.evaluationId),
                       static_cast<unsigned long long>(sample.runId), sample.sampleInterval);
    ImGui::TextWrapped(Localization::Tr("This is one historical sample, not the current frame or an average. "
                                        "Use matching contracts in the INFO log for comparisons."));
}

// A slider that only writes its value when the handle is released.
//
// Some controls -- intensity, the structure and tone strengths -- are read by the model once, when
// the feature is built, so changing one rebuilds the whole feature. Writing on every pixel of a drag
// meant a rebuild per frame, felt as the picture hitching while you scrub. The slider still tracks
// live under the cursor; only the commit that triggers the rebuild waits for release. Cheap controls
// that are just shader constants (detail, colour, paper white) do not use this -- they can afford to
// apply live.
static bool DeferredSlider(const char* label, float* current, float mn, float mx, float def, const char* fmt = "%.2f",
                           bool showReset = true)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : *current;
    bool changed = false;

    if (ImGui::SliderFloat(Localization::Label(label), &value, mn, mx, fmt))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *current = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
            changed = true;
        }
    }

    if (!ImGui::IsItemActive())
        pending.erase(id);
    if (!showReset)
        return changed;

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(Localization::Label(resetId.c_str())))
    {
        *current = def;
        pending.erase(id); // drop any in-flight drag so the reset actually sticks
        changed = true;
    }

    return changed;
}

// Every field is either a sparse override or a live master value, never an implicit copy.
static bool PassFloat(const char* label, std::optional<float>& field, float master, float minimum)
{
    ImGui::PushID(label);
    bool overrideValue = field.has_value();
    bool changed = ImGui::Checkbox(Localization::Label("Override###override"), &overrideValue);
    if (changed)
        field = overrideValue ? std::optional<float>(master) : std::nullopt;
    if (field)
    {
        float value = *field;
        if (DeferredSlider(label, &value, minimum, 2.0f, master, "%.2f", false))
        {
            field = value;
            changed = true;
        }
    }
    else
        ImGui::Text(Localization::Tr("%s: inherit %.2f"), Localization::Tr(label), master);
    ImGui::PopID();
    return changed;
}

static bool PassChoice(const char* label, std::optional<uint32_t>& field, uint32_t master, const char* const* names,
                       int count)
{
    ImGui::PushID(label);
    bool overrideValue = field.has_value();
    bool changed = ImGui::Checkbox(Localization::Label("Override###override"), &overrideValue);
    if (changed)
        field = overrideValue ? std::optional<uint32_t>(master) : std::nullopt;
    if (field)
    {
        int value = static_cast<int>(*field);
        if (ImGui::Combo(label, &value, names, count))
        {
            field = static_cast<uint32_t>(value);
            changed = true;
        }
    }
    else
        ImGui::Text(Localization::Tr("%s: inherit %s"), Localization::Tr(label),
                    names[std::min(master, uint32_t(count - 1))]);
    ImGui::PopID();
    return changed;
}

static void RenderPassControls(Config* config, bool vulkan)
{
    ImGui::SeparatorText(Localization::Tr("Sequential model passes"));
    if (vulkan || config->DlssNrUseProxy.value_or_default())
    {
        ImGui::TextWrapped(Localization::Tr("This backend uses one pass with master settings. "
                                            "Sequential passes and individual settings are unavailable."));
        return;
    }

    auto snapshot = config->GetDlssNrPassSnapshot();
    int count = static_cast<int>(snapshot.Count);
    if (ImGui::SliderInt(Localization::Label("Requested passes###nrPassCount"), &count, 1, 3))
    {
        config->SetDlssNrPassCount(static_cast<uint32_t>(count));
        snapshot = config->GetDlssNrPassSnapshot();
    }
    ImGui::Text(Localization::Tr("Last evaluation: %u successful model passes"), DlssNr::ActivePassCount());
    ImGui::TextWrapped("%s", Localization::Tr(DlssNr::MultipassStatus()));
    HelpMarker("Each extra pass processes the previous answer and has its own model history. "
               "Cost and VRAM grow. A failed extra pass keeps the last successful answer. "
               "Requested passes are not proof of completed work; check the last evaluation and INFO log."
               "\n\nEnabling this after untracked NR has already run requires restarting the application. "
               "Retry cannot make earlier GPU work tracked. Unsupported submission paths refuse extra passes.");

    bool individual = snapshot.Individual;
    if (ImGui::Checkbox(Localization::Label("Individual pass settings###nrIndividualPassSettings"), &individual))
        config->SetDlssNrIndividualPassSettings(individual);
    HelpMarker("The Model controls above are the master settings. Unchecked overrides inherit their "
               "current values. Turning individual settings off or reducing the pass count keeps saved edits.");
    if (!individual)
        return;

    static int selected = 1;
    selected = std::clamp(selected, 1, static_cast<int>(snapshot.Count));
    if (snapshot.Count > 1)
        ImGui::SliderInt(Localization::Label("Edit pass###nrEditPass"), &selected, 1, static_cast<int>(snapshot.Count));
    else
        ImGui::TextUnformatted(Localization::Tr("Editing pass 1"));
    const uint32_t pass = static_cast<uint32_t>(selected - 1);
    auto sparse = config->GetDlssNrPassOverrides(pass);
    const auto master = config->GetDlssNrMasterSettings();
    ImGui::PushID(selected);
    ImGui::TextUnformatted(Localization::Tr("Uncheck Override to inherit the live master value."));
    bool changed = false;
    changed |= PassFloat("Intensity", sparse.Intensity, master.Intensity, 0.0f);
    changed |= PassFloat("Local structure", sparse.LocalStructure, master.LocalStructure, 0.0f);
    changed |= PassFloat("Local tone", sparse.LocalTone, master.LocalTone, 0.0f);
    changed |= PassFloat("Skin structure", sparse.SkinStructure, master.SkinStructure, -1.0f);
    const char* presets[] = { Localization::Tr("Default"), Localization::Tr("Preset 1"), Localization::Tr("Preset 2"),
                              Localization::Tr("Preset 3") };
    const char* styles[] = { Localization::Tr("Default (standard)"), Localization::Tr("Natural"),
                             Localization::Tr("Cinematic") };
    changed |= PassChoice("Model preset", sparse.Preset, master.Preset, presets, IM_ARRAYSIZE(presets));
    changed |= PassChoice("Style", sparse.Style, master.Style, styles, IM_ARRAYSIZE(styles));
    ImGui::PushID("autoMask");
    bool maskOverride = sparse.AutoMask.has_value();
    if (ImGui::Checkbox(Localization::Label("Override auto skin mask###nrMaskOverride"), &maskOverride))
    {
        sparse.AutoMask = maskOverride ? std::optional<bool>(master.AutoMask) : std::nullopt;
        changed = true;
    }
    if (sparse.AutoMask)
    {
        bool mask = *sparse.AutoMask;
        if (ImGui::Checkbox(Localization::Label("Auto skin mask###nrPassMask"), &mask))
        {
            sparse.AutoMask = mask;
            changed = true;
        }
    }
    else
        ImGui::Text(Localization::Tr("Auto skin mask: inherit %s"), Localization::Tr(master.AutoMask ? "on" : "off"));
    ImGui::PopID();
    if (sparse != DlssNrPassSettings {} &&
        ImGui::SmallButton(Localization::Label("Inherit all master settings###nrClearPass")))
    {
        sparse = {};
        changed = true;
    }
    if (changed)
        config->SetDlssNrPassOverrides(pass, sparse);
    ImGui::PopID();
}

void RenderMenu(Config* config, float menuResScale)
{

    // DLSS Neural Rendering -----------------------------
    ImGui::Spacing();
    if (auto ch = ScopedCollapsingHeader(Localization::Label("DLSS Neural Rendering")); ch.IsHeaderOpen())
    {
        ScopedIndent indent {};
        ImGui::Spacing();

        bool enabled = config->DlssNrEnabled.value_or_default();
        if (ImGui::Checkbox(Localization::Label("Enable Neural Rendering"), &enabled))
            config->DlssNrEnabled = enabled;

        HelpMarker("Synthesises detail in the upscaler's output, before frame generation sees it."
                   "\n\nNeeds two similarly named files beside OptiScaler, one character apart:"
                   "\n  nvngx_dlssnr.dll       NVIDIA's model (~165 MB) -- you supply it"
                   "\n  nvngx.dll_dlssnr.dll   the forwarder (~13 KB) -- ships in this package"
                   "\nUndocumented and driven directly, so none of this is officially supported.");

        // The toggle can be bound to a key, and nobody would think to look for it under Keybinds
        // unless told. Dimmed, because it is a note rather than a setting.
        ImGui::TextDisabled(
            Localization::Tr("Can be toggled with a key -- bind it under Keybinds, \"Neural Rendering\"."));

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (ImGui::Checkbox(Localization::Label("Apply the model"), &applyModel))
            config->DlssNrApplyModel = applyModel;

        HelpMarker("Whether the model's edit is applied. Off shows the clean upscaler frame while the"
                   "\npass keeps running -- so with Hold frame (under Compare) you can freeze a"
                   "\nframe and toggle this to see the same frozen frame with and without Neural"
                   "\nRendering. Leave it on for normal use.");

        // Either backend. The two keep separate state, and on a native Vulkan game the D3D12 side
        // is never touched -- so asking only that one reports "waiting for the upscaler" over a pass
        // that is demonstrably running.
        const bool vulkan = DlssNr::IsRunningVk();

        // Turning the pass off does not release the model, so the feature handle stays alive and
        // IsRunning keeps answering yes. Reporting a cost from that was wrong in the way that matters
        // most: the toggle is how anyone A/Bs this, so the one moment the number is read is the one
        // moment it describes the frame before last.
        if (!enabled)
        {
            ImGui::TextDisabled(Localization::Tr("Off. The model stays loaded, so turning this back on is immediate."));
        }
        else if (!DlssNr::IsRunning() && !vulkan)
        {
            const char* reason = DlssNr::FailureReason();

            if (reason[0] != 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), Localization::Tr("Off for this session: %s."),
                                   Localization::Tr(reason));
                ImGui::SameLine();

                if (ImGui::SmallButton(Localization::Label("Retry")))
                    DlssNr::RetryAfterFailure();
            }
            else if (enabled)
                ImGui::TextUnformatted(Localization::Tr("Waiting for the upscaler to run."));
        }
        else
        {
            // Native Vulkan retains its explicitly unconfirmed legacy timer. D3D12 measurements
            // belong to the separate panel below, which can show sample age and provenance.
            const auto ms = vulkan ? DlssNr::LastGpuTimeVk() : std::nullopt;

            // With "Apply the model" off the pass STILL RUNS (so Hold-frame A/B can toggle its edit on
            // a frozen frame) -- it only outputs the clean frame. So the cost is real, and saying so
            // stops the reading looking like a bug. Enable Neural Rendering off is what zeroes it.
            const char* runSuffix =
                !config->DlssNrApplyModel.value_or_default() ? Localization::Tr("  (model running, edit hidden)") : "";

            if (ms.has_value())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   Localization::Tr("Running%s - legacy timer %.2f ms (unconfirmed)%s"),
                                   vulkan ? Localization::Tr(" natively on Vulkan") : "", ms.value(), runSuffix);
            else if (vulkan)
                // Measured but not yet read: the first few frames are still in the query ring.
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f),
                                   Localization::Tr("Running natively on Vulkan - %llu frames%s"), DlssNr::FramesVk(),
                                   runSuffix);
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), Localization::Tr("Running.%s"), runSuffix);

            ImGui::SameLine();
            ImGui::TextDisabled(Localization::Tr("(?)"));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(Localization::Tr("The native Vulkan timer is legacy and is not fence-confirmed."
                                                   "\nFor D3D12, use the separate Fence-confirmed GPU timing panel."
                                                   "\nA historical sample is not the current frame's cost."));
        }

        ImGui::Spacing();
        ImGui::PushItemWidth(220.0f * menuResScale);

        // Native Vulkan has no pre-upscale implementation; show its effective stage as status.
        if (vulkan)
        {
            ImGui::TextDisabled(Localization::Tr("Stage: after the upscaler (native Vulkan)"));
        }
        else
        {
            static const char* stageNames[] = { Localization::Label("After the upscaler"),
                                                Localization::Label("Before the upscaler (experimental)") };
            int stage = config->DlssNrStage.value_or_default() == 1 ? 1 : 0;
            if (ImGui::Combo(Localization::Label("Stage"), &stage, stageNames, IM_ARRAYSIZE(stageNames)))
            {
                config->DlssNrStage = static_cast<uint32_t>(stage);
                DlssNr::RetryAfterFailure();
            }
            HelpMarker("Before: runs Neural Rendering on the game's render-resolution color, then"
                       "\nupscales the edited copy. This may reduce GPU cost, but can introduce shimmer"
                       "\nand unstable detail on moving edges. Compare the picture in motion."
                       "\n\nExperimental on D3D12 and the D3D12 bridges. Ray reconstruction, Hold frame,"
                       "\nthe driver proxy, and unsupported inputs use the after-upscale path."
                       "\nChanging render dimensions pauses NR until they stay stable for 500 ms."
                       "\nThe original game color is used while the model is being built or skipped.");
            if (enabled && stage == 1)
                ImGui::TextWrapped(Localization::Tr("%s"), Localization::Tr(DlssNr::BeforeUpscaleStatus()));

            bool applyAfterRR = config->DlssNrApplyAfterRR.value_or_default();
            if (ImGui::Checkbox(Localization::Label("Apply after Ray Reconstruction###nrApplyAfterRR"), &applyAfterRR))
                config->DlssNrApplyAfterRR = applyAfterRR;

            HelpMarker("Runs Neural Rendering over native Ray Reconstruction (DLSSD) output."
                       "\n\nDisabled by default: Ray Reconstruction already synthesizes denoised output."
                       "\nEnable if you want to run DLSS-NR over the final RR result at configured scale.");

            if (applyAfterRR)
            {
                int rrPasses = static_cast<int>(config->DlssNrRRPasses.value_or_default());
                if (ImGui::SliderInt(Localization::Label("RR Passes###nrRRPasses"), &rrPasses, 1, 3))
                    config->DlssNrRRPasses = static_cast<unsigned int>(std::clamp(rrPasses, 1, 3));

                // The engine clamps this to 0.25..2.0, the same range the after-upscale slider offers;
                // stopping the control at 1.0 hid half of what the route accepts. Above 1 the model
                // supersamples -- its input is enlarged, denoised, and sampled back down -- so it buys
                // edge stability, not detail, and the cost grows with the area.
                float rrScale = config->DlssNrRRWorkingScale.value_or_default();
                if (ImGui::SliderFloat(Localization::Label("RR Working scale###nrRRWorkingScale"), &rrScale, 0.25f,
                                       2.0f, "%.2fx"))
                    config->DlssNrRRWorkingScale = std::clamp(rrScale, 0.25f, 2.0f);

                if (rrScale > 1.001f)
                    ImGui::TextDisabled(Localization::Tr(
                        "Above 1x the model supersamples: no new detail, and cost grows with the area. "
                        "Ray Reconstruction already denoises this frame."));
            }
        }

        // Any percentage, rather than a handful of steps somebody chose in advance. The lower bound
        // is 25%: below that the model is working on so little of the picture that its answer no
        // longer survives being enlarged onto it.
        // Applied when the handle is let go, not while it is moving.
        //
        // Every distinct value here is a different working size, and a different working size tears
        // down the scratch textures and rebuilds the model. Writing it on each pixel of a drag meant
        // dozens of rebuilds in a second, which is felt as the whole frame hitching. The slider still
        // reads live; only the commit waits.
        static int pendingScale = -1;

        int scalePercent =
            pendingScale >= 0 ? pendingScale : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

        // On the ray-reconstruction route the model works at RRWorkingScale, and this slider is not
        // read at all. Leaving it live was worse than hiding it: it moved, it accepted 200%, and the
        // cost did not change, so the only way to find out it governed nothing was to read the log.
        // The route decides, not the ApplyAfterRR checkbox -- ticking that in a title without ray
        // reconstruction leaves this slider in charge.
        if (DlssNr::RunningAfterRayReconstruction())
        {
            ImGui::TextDisabled(Localization::Tr("Model resolution: set by RR Working scale on this route"));
        }
        else
        {
            if (ImGui::SliderInt(Localization::Label("Model resolution"), &scalePercent, 25, 200, "%d%%"))
                pendingScale = scalePercent;

            if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
            {
                config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
                pendingScale = -1;
            }
        }

        if (scalePercent > 100)
            ImGui::TextDisabled(
                Localization::Tr("Supersampling %.2fx: the model runs ABOVE native, then\n"
                                 "is sampled back down. Experimental, and costly -- time grows with the area."),
                scalePercent / 100.0f);

        if (scalePercent > 100)
        {
            static const char* dsNames[] = { Localization::Label("FSR1"),        Localization::Label("Bicubic"),
                                             Localization::Label("Catmull-Rom"), Localization::Label("Lanczos2"),
                                             Localization::Label("Lanczos3"),    Localization::Label("Kaiser2"),
                                             Localization::Label("Kaiser3"),     Localization::Label("MAGIC") };
            int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
            if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
                ds = (int) Scaler::Lanczos3;

            if (ImGui::Combo(Localization::Label("Downscaler (NR)"), &ds, dsNames, IM_ARRAYSIZE(dsNames)))
                config->DlssNrScalingDownscaler = (Scaler) ds;

            HelpMarker("The filter that averages the model's above-native answer back to display size --"
                       "\nthis is what turns supersampling into LESS noise rather than more. Sharper"
                       "\nfilters (Lanczos3, Kaiser3) keep the most detail; softer ones (Bicubic,"
                       "\nCatmull-Rom) are gentler on ringing. Independent of the Output Scaling"
                       "\ndownscaler, so the two can differ and run at the same time.");
        }

        HelpMarker("What fraction of the frame the model works at. Cost falls with the square of"
                   "\nthis, so half resolution is roughly a quarter of the time."
                   "\n\nThe frame is never reduced. Only the model's contribution is computed small"
                   "\nand enlarged, so the picture underneath is untouched whatever this says."
                   "\n\nWhat it trades: the shading the model adds is broad and survives enlargement;"
                   "\nthe fine structure it synthesises does not, and softens. Worth having when the"
                   "\npass costs more than you want to pay for the detail it returns."
                   "\n\nThe frame itself stays at full detail whatever this says -- only the"
                   "\nmodel's own work is done small.");

        // Meaningful only when the model runs BELOW the frame's size. At 100% -- and above, where
        // supersampling composites its down-legged answer at native -- the residual collapses to the
        // model's own picture and the two modes are identical, so the control says so by going grey.
        {
            const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

            if (!reduced)
                ImGui::BeginDisabled();

            static const char* enlargeNames[] = { Localization::Label("Classic"),
                                                  Localization::Label("Matched residual") };
            int enlarge = config->DlssNrTransfer.value_or_default() == 1 ? 1 : 0;

            if (ImGui::Combo(Localization::Label("Enlargement"), &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
                config->DlssNrTransfer = (uint32_t) enlarge;

            if (!reduced)
                ImGui::EndDisabled();

            HelpMarker("How the model's work is brought back up when it ran below the frame's size."
                       "\n\nClassic composes the model's small picture directly against the full-size"
                       "\nframe. Those two disagree by the shrink's blur as well as by the model's edit,"
                       "\nand the composition cannot tell them apart -- it reads the blur as brightness"
                       "\nthe frame has and the model never saw. The lower the model resolution the"
                       "\nlarger that error, and it is the colour shift that shows up at 50%."
                       "\n\nMatched residual carries up only the model's difference and lays it on the"
                       "\nframe's own proxy, so both pictures being compared are full size and the only"
                       "\nthing that came from the small raster is the edit itself."
                       "\n\nNo effect at 100% or above: there is no residual to carry and the two are"
                       "\nidentical (supersampling brings its answer down to frame size before this)."
                       "\n\nFrom hhkbble's multi-pass work on this fork.");
        }

        ImGui::SeparatorText(Localization::Tr("How much of it lands"));

        float transfer = config->DlssNrTransferStrength.value_or_default();
        if (ImGui::SliderFloat(Localization::Label("Detail strength"), &transfer, 0.0f, 2.0f, "%.2f"))
            config->DlssNrTransferStrength = transfer;

        ImGui::SameLine();
        if (ImGui::SmallButton(Localization::Label("Reset##detail")))
            config->DlssNrTransferStrength = 1.0f;

        HelpMarker("How far the frame moves toward the model's picture."
                   "\n\nThe model's answer is not added to the frame -- it is a complete picture of its"
                   "\nown, rescaled so its luminance sits where the original says it should. This"
                   "\nblends between the two, so both ends are real pictures and everything between"
                   "\nthem is one too."
                   "\n\n0 gives back exactly what the upscaler produced. 1 is the model's picture."
                   "\n\nAbove 1 carries on past it in the same direction, which is not something the"
                   "\nmodel asked for -- use it to see what it is doing, then come back down. This"
                   "\nis the control to push if you want more effect: Intensity belongs to the model"
                   "\nand it decides what to do with it.");

        float colour = config->DlssNrColourStrength.value_or_default();
        if (ImGui::SliderFloat(Localization::Label("Colour strength"), &colour, 0.0f, 4.0f, "%.2f"))
            config->DlssNrColourStrength = colour;

        ImGui::SameLine();
        if (ImGui::SmallButton(Localization::Label("Reset##colour")))
            config->DlssNrColourStrength = 1.0f;

        HelpMarker("Whether the model's colour arrives with its light."
                   "\n\n0 keeps the game's own hue exactly -- every pixel is the original colour with"
                   "\nonly its brightness carrying the model's verdict. Game-accurate colour, with"
                   "\nthe detail. 1 brings the model's colour as well, in its own hue, clamped into"
                   "\nAP1 so nothing unreachable is asked for."
                   "\n\nThis cannot shift hue on its own: it interpolates between two finished"
                   "\npictures rather than adding a colour difference to one, which is what used to"
                   "\nlet a warm subject come back green."
                   "\n\nAbove 1 it OVER-SATURATES: the colour keeps its hue but grows more vivid,"
                   "\nand rolls off at the edge of what the display can show rather than clipping"
                   "\ninto a flat blown patch. 1 is the model's own colour; push past it for punch.");

        // Experimental. 0 off (soft knee), 1 Neutwo + our composition, 2 Neutwo + pure-inverse replace,
        // 3 hybrid+composed, 4 hybrid+replace (identity midtones + unclipped highlights). Always shown.
        static const char* reversibleNames[] = { Localization::Label("Off (soft knee)"),
                                                 Localization::Label("Neutwo proxy + composed"),
                                                 Localization::Label("Neutwo proxy + replace"),
                                                 Localization::Label("Hybrid proxy + composed"),
                                                 Localization::Label("Hybrid proxy + replace") };
        int reversible = (int) config->DlssNrReversibleMode.value_or_default();
        if (reversible < 0 || reversible > 4)
            reversible = 0;
        if (ImGui::Combo(Localization::Label("Reversible proxy (experimental)"), &reversible, reversibleNames,
                         IM_ARRAYSIZE(reversibleNames)))
            config->DlssNrReversibleMode = (uint32_t) reversible;

        HelpMarker("What the model is shown, and how its answer comes back."
                   "\n\nOff (soft knee): the default. It rolls highlights off so hard the model"
                   "\ncannot resolve detail in them -- fine in soft-lit scenes, weak in bright ones."
                   "\n\nNeutwo composed: an unclipped curve so the model sees highlight detail, then"
                   "\neverything above (Detail/Colour strength, highlight guard, palette). It wins in"
                   "\nbright scenes, but the curve compresses MIDTONES too, so in soft-lit content it"
                   "\ncan be worse than Off. It also shifts paper white -- re-check it when you switch."
                   "\n\nHybrid composed: the best of both, and the one to use. Identity in the"
                   "\nmidtones -- as good as Off there -- and the unclipped roll only in the"
                   "\nhighlights, so it recovers the detail Off crushes without giving up the"
                   "\nmidtones Neutwo does. It barely shifts paper white."
                   "\n\nReplace: the raw model straight back through the exact inverse, none of the"
                   "\ncomposition -- no guard, no palette, no strengths. Gorgeous where there are no"
                   "\nbright lights, but they FLASH in motion. A reference, not a daily setting."
                   "\n\nHybrid replace: the raw model like Replace, but on the hybrid curve -- the"
                   "\ndecode is identity in the midtones, so the flashing is confined to genuine"
                   "\nbright highlights instead of everywhere. Most of Replace's detail, far more"
                   "\nstable. If you love the Replace look but the flicker bothers you, use this."
                   "\n\nOff is byte-identical to before.");

        ImGui::SeparatorText(Localization::Tr("Model"));

        ImGui::TextUnformatted(
            Localization::Tr("Read when the model is built, so a change rebuilds it after a moment."));

        static const char* nrPresetNames[] = { Localization::Label("Default"), Localization::Label("Preset 1"),
                                               Localization::Label("Preset 2"), Localization::Label("Preset 3") };
        auto master = config->GetDlssNrMasterSettings();
        int preset = (int) master.Preset;
        if (ImGui::Combo(Localization::Label("Model preset"), &preset, nrPresetNames, IM_ARRAYSIZE(nrPresetNames)))
            config->SetDlssNrMasterSetting(&Config::DlssNrPreset, (uint32_t) preset);

        HelpMarker("Default leaves the choice to the model."
                   "\n\nNot the same scale as the super resolution or ray reconstruction presets --"
                   "\nthe same number means something different here.");

        static const char* nrStyleNames[] = { Localization::Label("Default (standard)"), Localization::Label("Natural"),
                                              Localization::Label("Cinematic") };
        int style = (int) master.Style;

        if (style > 2)
            style = 2;

        if (ImGui::Combo(Localization::Label("Style"), &style, nrStyleNames, IM_ARRAYSIZE(nrStyleNames)))
            config->SetDlssNrMasterSetting(&Config::DlssNrStyle, (uint32_t) style);

        HelpMarker("The model's own processing profiles."
                   "\n\nDefault (standard): the strongest. Boosts local contrast and deepens"
                   "\nlighting, and can oversaturate or look stylised -- most of what reads as"
                   "\n'the model changed my game's look' is this profile."
                   "\n\nNatural: the same detail work with a gentler hand. Keeps skin tones and"
                   "\ntonal balance closer to what the game rendered."
                   "\n\nCinematic: tones down the shine and over-processing for a film-like look."
                   "\n\nRead when the model is built, so a change rebuilds it after a moment. The"
                   "\nnames come from community testing; NVIDIA ships no names in the binaries.");

        if (DeferredSlider("Intensity", &master.Intensity, 0.0f, 2.0f, 1.0f))
            config->SetDlssNrMasterSetting(&Config::DlssNrIntensity, master.Intensity);

        HelpMarker("The model's own strength control, applied inside it. Distinct from detail"
                   "\nstrength above, which scales the result afterwards.");

        if (DeferredSlider("Local structure", &master.LocalStructure, 0.0f, 2.0f, 1.0f))
            config->SetDlssNrMasterSetting(&Config::DlssNrLocalStructure, master.LocalStructure);

        if (DeferredSlider("Local tone", &master.LocalTone, 0.0f, 2.0f, 1.0f))
            config->SetDlssNrMasterSetting(&Config::DlssNrLocalTone, master.LocalTone);

        if (DeferredSlider("Skin structure", &master.SkinStructure, -1.0f, 2.0f, -1.0f))
            config->SetDlssNrMasterSetting(&Config::DlssNrSkinStructure, master.SkinStructure);

        HelpMarker("-1 means follow local structure, and is the model's own default -- it is not a"
                   "\nstrength of zero. 0 and above set skin independently of the rest of the frame.");

        bool autoMask = master.AutoMask;
        if (ImGui::Checkbox(Localization::Label("Auto skin mask"), &autoMask))
            config->SetDlssNrMasterSetting(&Config::DlssNrAutoMask, autoMask);

        HelpMarker("Lets the model find skin itself rather than treating the frame uniformly.");

        // Runtime activity alone cannot identify a native Vulkan backend before its first NR frame.
        const auto& state = State::Instance();
        const bool nativeVulkan =
            vulkan || (state.api == API::Vulkan && (!state.currentFeature || !state.currentFeature->IsWithDx12()));
        RenderPassControls(config, nativeVulkan);
        RenderGpuTiming(config, nativeVulkan);

        ImGui::SeparatorText(Localization::Tr("Colour"));

        ImGui::TextDisabled(Localization::Tr("The model was trained on finished, sRGB-encoded frames. The upscaler's\n"
                                             "output is not one: it is linear and open-ended. These decide how it is\n"
                                             "mapped into something the model recognises. A frame the game reports as\n"
                                             "already tone-mapped is passed over untouched and none of this applies."));

        {
            // Logarithmic, because the useful range is not linear. A quarter to 240: the low end because
            // a frame the game already tone mapped wants roughly 1, the high end because there is no
            // principled ceiling -- this is a divisor on an open-ended linear buffer, and how far up a
            // given game needs to go is a property of that game's exposure, not of anything we can bound.
            // One tester was still improving at 100. A linear slider over that span would spend nine
            // tenths of its travel on values nobody needs and never reach the ones they do.
            // One dropdown, because there is one answer.
            //
            // This was two checkboxes that could both be on, and every attempt to stop that was a patch
            // on a shape that should not have existed. Greying deadlocked -- each disabled the other, so
            // once both were set the only way out was a button the notice never mentioned. Clearing
            // worked but silently undid a setting somebody had made. Both were ways to stop an illegal
            // state being REACHED; a single choice cannot reach it, because there is only one value to
            // be in.
            //
            // Each option also says whether it can actually do anything in THIS game, in colour, so the
            // choice is made on what is available rather than on what sounds best.
            {
                const auto ex = DlssNr::GameExposureStatus();
                const bool vk = DlssNr::IsRunningVk();
                const bool haveExposure = vk ? DlssNr::ExposureOfferedVk() : ex.everOffered;

                const float anchorNow = DlssNr::ExposureScan::BestValue();
                const bool haveAnchor = !DlssNr::ExposureScan::Anchors().empty();

                static const char* sourceNames[] = { Localization::Label("Paper white only"),
                                                     Localization::Label("The game's own exposure"),
                                                     Localization::Label("A buffer the scan found") };

                int source = (int) config->DlssNrWhitePointSource.value_or_default();

                if (source < 0 || source > 2)
                    source = 0;

                if (ImGui::Combo(Localization::Label("White point from"), &source, sourceNames,
                                 IM_ARRAYSIZE(sourceNames)))
                {
                    config->DlssNrWhitePointSource = (uint32_t) source;

                    // Nothing else to set. The scan asks the source whether it is wanted, so choosing
                    // it here is the whole of switching it on -- there is no second flag to keep in
                    // step, and so no way for the two to disagree.
                }

                HelpMarker("Where the number that divides the frame comes from."
                           "\n\nPaper white only -- the slider below and nothing else. Right for a"
                           "\ngame whose exposure never moves, wrong the moment it does: one"
                           "\nconstant cannot serve a cave and a field."
                           "\n\nThe game's own exposure -- read from the texture the game hands"
                           "\nthe upscaler. The best source there is, because it is decided"
                           "\nupstream and nothing this pass does can move it. Not every game"
                           "\nsupplies one."
                           "\n\nA buffer the scan found -- for games that compute an exposure and"
                           "\nnever pass it on. A GUESS: candidates are matched by shape, and in"
                           "\nGTA V the best one tracks the real exposure but at its own scale,"
                           "\nwhich the anchor's ratio cancels. Needs anchoring once, and checking"
                           "\nafterwards.");

                // Availability, in colour, for the option currently chosen.
                if (source == 1)
                {
                    if (!vk && ex.seenFrames == 0)
                        ImGui::TextDisabled(Localization::Tr("Waiting for a frame..."));
                    else if (!haveExposure)
                        ImGui::TextColored(
                            ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                            Localization::Tr("This game supplies no exposure -- paper white is in use. Try "
                                             "the scan instead."));
                    else if (vk)
                        ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                           Localization::Tr("This game supplies an exposure and it is being read."));
                    else if (ex.exposure > 1e-6f)
                    {
                        const float trim = std::clamp(config->DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
                        ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                           Localization::Tr("Game exposure %.4f  ->  white point %.2f%s"), ex.exposure,
                                           ex.preExposure / ex.exposure * trim,
                                           ex.offeredNow ? "" : "  (held: absent this frame)");
                    }
                    else
                        ImGui::TextDisabled(Localization::Tr("Reading the exposure..."));
                }
                else if (source == 2)
                {
                    // "Nothing found" and "found several, none of them moving" are different states,
                    // and this said the first for both. In GTA V the log carried eight candidates while
                    // the panel claimed there were none, which reads as the scan being broken when what
                    // it actually needs is for the light to change.
                    if (anchorNow <= 0.0f)
                    {
                        const unsigned int watching = (unsigned int) DlssNr::ExposureScan::Report().size();

                        if (watching == 0)
                            ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                               Localization::Tr("Nothing in this game is shaped like an exposure."));
                        else
                            ImGui::TextColored(
                                ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                Localization::Tr("Watching %u, none moving yet -- go between light and shade."),
                                watching);
                    }
                    else if (!haveAnchor)
                        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.25f, 1.0f),
                                           Localization::Tr("Found one. Set paper white below until the picture looks "
                                                            "right, then press Anchor here."));
                    // Once anchored, the scan -> white point readout sits above the sliders below; it is
                    // not repeated up here.
                }
                else if (haveExposure)
                {
                    ImGui::TextColored(
                        ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                        Localization::Tr("This game supplies an exposure -- the option above would use it."));
                }
            }

            // A measured suggestion for paper white used to sit here and has been withdrawn.
            //
            // It took the 90th percentile of per-tile peak luminance from the untouched frame, which is a
            // statement about scene content rather than about the buffer's scale. In Nioh 3, where the
            // right answer is about 240, it offered 8 -- because most tiles are shadow and the percentile
            // sits wherever most tiles are. The guard meant to catch that compared each tile against the
            // frame's own brightest, which is scale-free and therefore passes on a black screen: the same
            // relative-threshold mistake the white point meter was removed for, made a second time.
            //
            // A wrong number offered confidently is worse than no number, so nothing is offered. What
            // replaces it has to be a measurement of the game's own exposure rather than of its scenery:
            // the exposure texture where a game supplies one, and otherwise the ratio between the
            // scene-referred buffer and the finished frame, which is that exposure by definition.

            // Two controls, not one control with two meanings.
            //
            // These are different quantities. The manual path wants an absolute divisor on an open-ended
            // linear buffer -- Nioh 3 needs about 240 -- and the exposure path wants a multiplier on a
            // number the game already supplied, where 1 is correct and anything far from it says the read
            // is wrong rather than that somebody prefers it.
            //
            // They used to share one stored value, narrowed to 0.25..4 when the toggle was on. That kept
            // a ruinous value unreachable but left two worse problems: moving the slider in one mode
            // silently destroyed the number found in the other, and there was no way back to "just take
            // the game's answer" short of knowing that the number for it was 1. Separate values fix both.
            // Switching modes is now non-destructive in both directions.
            // The trim belongs to both automatic sources, since both end in "the game's number times a
            // little". Only the manual source gets the absolute slider.
            // One slider per source, each remembering its own number.
            //
            // A trim on the game's exposure and a trim on a buffer the scan found are trims on different
            // things, and a value found against one means nothing against the other. Sharing them meant
            // changing source silently carried a number across, so a picture that had been tuned came
            // back wrong for a reason nothing on screen explained.
            //
            // The scan before it is anchored is the exception, and it has to be: anchoring captures an
            // absolute white point, so there must be an absolute slider to set. Showing a trim there
            // asked people to "set paper white below" next to a control that was not paper white.
            const int wpSource = (int) config->DlssNrWhitePointSource.value_or_default();

            // Which anchor row the paper-white slider edits, or -1 for the live unanchored point. Menu-
            // local and not persisted; the anchor block below sets it when a row is clicked. Declared
            // here because both the slider (this block) and the table (below) read it in the same frame.
            static int selectedAnchor = -1;
            auto anchors = DlssNr::ExposureScan::Anchors();
            if (selectedAnchor >= (int) anchors.size())
                selectedAnchor = -1;

            if (wpSource == 2)
            {
                const bool editingRow = selectedAnchor >= 0 && selectedAnchor < (int) anchors.size();

                // The single scan -> white point readout, above the sliders it explains.
                if (!anchors.empty())
                {
                    const float liveScan = DlssNr::ExposureScan::BestValue();

                    if (liveScan > 0.0f)
                    {
                        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
                            liveScan, config->DlssNrScanInverted.value_or_default(),
                            config->DlssNrScanTrim.value_or_default());

                        ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f),
                                           Localization::Tr("Scan %.5f  ->  white point %.2f   (%u point%s)"), liveScan,
                                           w, (unsigned) anchors.size(), anchors.size() == 1 ? "" : "s");
                    }
                }

                // Paper white shows only when there is a point to set: before the first anchor, or when a
                // row is selected to edit. Once points exist and none is selected, the white point is fixed
                // by the anchors and only the trim adjusts the live picture -- so the trim takes the
                // slider's place, the same shape as the game-exposure source.
                const bool showPaperWhite = anchors.empty() || editingRow;

                if (showPaperWhite)
                {
                    float pw =
                        editingRow ? anchors[selectedAnchor].white : config->DlssNrWhitePointScale.value_or_default();

                    char lbl[48];
                    if (editingRow)
                        snprintf(lbl, sizeof(lbl), "Paper white (editing point %d)", selectedAnchor + 1);
                    else
                        snprintf(lbl, sizeof(lbl), "Paper white");

                    if (ImGui::SliderFloat(lbl, &pw, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                    {
                        if (editingRow)
                        {
                            DlssNr::ExposureScan::AnchorSetWhite(selectedAnchor, pw);
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                        }
                        else
                            config->DlssNrWhitePointScale = pw;
                    }

                    HelpMarker("The white point for the selected calibration point, or -- with no row"
                               "\nselected -- the value the next Anchor press captures."
                               "\n\nSet it until the picture looks right here, then Anchor. Move to very"
                               "\ndifferent light and do it again: two points fix the buffer's real"
                               "\nrelationship and the white point holds between them. Click a row below"
                               "\nto come back and adjust that point; click it again to let go.");
                }

                // The trim multiplies the interpolated result, and in the steady state it is the control
                // that stands in for paper white: adjust it until the picture looks right in the current
                // light, then Anchor bakes that trimmed value into a new point and resets the trim to 1.
                if (!anchors.empty())
                {
                    float trim = config->DlssNrScanTrim.value_or_default();

                    if (ImGui::SliderFloat(Localization::Label("Trim (x the scan)"), &trim, 0.25f, 4.0f, "%.2fx",
                                           ImGuiSliderFlags_Logarithmic))
                        config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);

                    ImGui::SameLine();

                    if (ImGui::SmallButton(Localization::Label("Reset##scantrim")))
                        config->DlssNrScanTrim = 1.0f;

                    HelpMarker("A multiplier on the scan's white point, and the control you adjust between"
                               "\nanchor points: dial it until the picture looks right in the current"
                               "\nlight, then press Anchor here -- it captures the trimmed value as a new"
                               "\npoint and resets the trim to 1.");
                }
            }
            else if (wpSource == 1)
            {
                const bool ofScan = false;

                float trim = ofScan ? config->DlssNrScanTrim.value_or_default()
                                    : config->DlssNrWhitePointTrim.value_or_default();

                if (ImGui::SliderFloat(
                        Localization::Label(ofScan ? "Trim (x the scan)" : "Trim (x the game's exposure)"), &trim,
                        0.25f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
                {
                    if (ofScan)
                        config->DlssNrScanTrim = std::clamp(trim, 0.25f, 4.0f);
                    else
                        config->DlssNrWhitePointTrim = std::clamp(trim, 0.25f, 4.0f);
                }

                ImGui::SameLine();

                // Deliberately always present rather than greyed at 1. The point of it is that the safe
                // value is one click away without having to know what the safe value is.
                if (ImGui::SmallButton(Localization::Label("Reset##wptrim")))
                {
                    if (ofScan)
                        config->DlssNrScanTrim = 1.0f;
                    else
                        config->DlssNrWhitePointTrim = 1.0f;
                }

                HelpMarker("A multiplier on the exposure the game supplied. 1.00x takes its number"
                           "\nexactly, and that is the right answer here."
                           "\n\nThis is not a fudge factor. If a game needs the trim far from 1 to look"
                           "\nright, that is evidence the exposure being read is wrong for that game,"
                           "\nnot that the game wants trimming. Somewhere around 0.8 to 1.25 is honest"
                           "\ntuning; reaching for 4 means something upstream is broken and the trim is"
                           "\nhiding it."
                           "\n\nYour manual paper white is kept separately and comes back untouched if"
                           "\nyou switch the option above off.");
            }
            else
            {
                // Logarithmic, because the useful range is not linear. A quarter to 2000: the low end
                // because a frame the game already tone mapped wants roughly 1, the high end because
                // there is no principled ceiling -- this is a divisor on an open-ended linear buffer, and
                // how far up a given game needs to go is a property of that game's exposure rather than
                // of anything that can be bounded here. One tester was still improving at 100.
                float wpScale = config->DlssNrWhitePointScale.value_or_default();

                if (ImGui::SliderFloat(Localization::Label("Paper white"), &wpScale, 0.25f, 2000.0f, "%.2fx",
                                       ImGuiSliderFlags_Logarithmic))
                    config->DlssNrWhitePointScale = wpScale;

                HelpMarker("What the frame is divided by before the model sees it. There is no other white"
                           "\npoint; this is the whole of it."
                           "\n\nThe model was trained on finished frames where white sits at 1. The"
                           "\nupscaler's output is linear and open-ended, so something has to say where"
                           "\nwhite is -- and where the game's DLSS buffer is linear HDR, that number is"
                           "\nrarely anywhere near 1. Measured in Monster Hunter Wilds it takes 16 or more"
                           "\nbefore the model's detail reaches the frame at all, and the value that suits"
                           "\na shaded camp is still too small for the same game out in daylight."
                           "\n\nToo low and almost every pixel trips the soft knee: the model is shown a"
                           "\nflat near-white picture, its answer is scaled away, and only its hue"
                           "\nsurvives -- which reads as a colour cast rather than as lost detail. Too"
                           "\nhigh and it is shown an underexposed one, its answer degrades, and this same"
                           "\nnumber multiplies that error on the way out."
                           "\n\nRaise it until the picture stops improving. Past that point it does not"
                           "\nplateau, it gets worse in the other direction."
                           "\n\nThis was once a multiplier on a measured white point. The measurement is"
                           "\ngone: it read scene brightness rather than where white belongs, handed the"
                           "\nmodel a picture three times too dark, and left the highlight path nothing to"
                           "\ngive back."
                           "\n\nAt strength zero the frame is still bit-identical whatever this says.");
            }

            // Highlight guard, directly under the white point / trim -- it bounds the model's edit and
            // belongs with the exposure controls it works alongside.
            float maxRatio = config->DlssNrMaxRatio.value_or_default();
            if (ImGui::SliderFloat(Localization::Label("Highlight guard"), &maxRatio, 1.0f, 8.0f, "%.1fx"))
                config->DlssNrMaxRatio = maxRatio;

            ImGui::SameLine();
            if (ImGui::SmallButton(Localization::Label("Reset##guard")))
                config->DlssNrMaxRatio = 2.0f;

            HelpMarker("The most the pass may move any pixel, as a multiple of what it already was, in"
                       "\nboth directions -- a pixel may not be brightened past this nor darkened past"
                       "\nits reciprocal. Lights are where the model has least to say and rescaling its"
                       "\nanswer does the most damage; 2x leaves detail intact while stopping a strip"
                       "\nlight turning into a string of coloured cells. Raise it only if bright areas"
                       "\nlook clipped.");

            // Directly under the white point, because that is the number it moves and the number the
            // anchor captures. It used to sit under Inspect, a whole section away from the slider it
            // reads, which left "Anchor here" looking like a control for something else entirely.
            {
                // No checkbox here any more.
                //
                // The dropdown above says whether the scan is the white point's source, and that is
                // the only reason anybody using this would want it running. A second control could
                // only agree with the dropdown or contradict it, and both were on offer: it began as
                // a redundant question and became a way to switch off the thing the chosen source
                // depended on.
                //
                // The ini key survives as a developer override for the one case a user has no reason
                // to want -- running the scan in a game that supplies a REAL exposure, so the log can
                // compare the two. That is validation, and validation does not need a widget.
                //
                // Worth keeping written down, since the panel no longer says it: the scan matches
                // buffers by SHAPE, and shape is a weak filter. In GTA V -- a game that supplies a
                // real exposure, so the right answer sat visible beside it -- the best candidate was
                // a 1x1 R32_FLOAT that climbed in a straight line for seventeen minutes while the
                // true exposure held still. Their ratio moved 14x. That is an accumulator, not an
                // eye adaptation.

                // Only where it means something. The lamp reads the scan, so offering it beside a
                // white point that comes from the game's own exposure is offering a control that
                // cannot light up.
                bool meter = config->DlssNrScanMeter.value_or_default();

                if (config->DlssNrWhitePointSource.value_or_default() == 2 &&
                    ImGui::Checkbox(Localization::Label("Show the light meter on screen"), &meter))
                    config->DlssNrScanMeter = meter;

                HelpMarker("A lamp in the corner: red for dark, green for full light, and the"
                           "\nshades between, with the reading beside it."
                           "\n\nIt is how you see at a glance that the scan is TRACKING rather"
                           "\nthan merely running. Walk into shade and it should slide toward"
                           "\nred; step out and it should go green. If it moves the wrong way,"
                           "\nthat is what the setting above is for."
                           "\n\nPurely a readout. It changes nothing.");

                // Shown when the scan is actually running, whichever way it got switched on.
                if (DlssNr::ExposureScan::Scanning())
                {
                    // Anchoring: one press, then it never needs touching again.
                    //
                    // The absolute white point cannot come out of a buffer whose units are unknown.
                    // Every value AFTER the first can: only the ratio against the anchor is used, so
                    // whatever the number means, it cancels. That is why this is a button and not a
                    // measurement -- the one thing a person can supply that no amount of cleverness
                    // can is "this looks right to me".
                    int which = 0;
                    float low = 0.0f, high = 0.0f;
                    const float live = DlssNr::ExposureScan::BestValue(&which, &low, &high);

                    const bool isSource = config->DlssNrWhitePointSource.value_or_default() == 2;

                    // Anchor captures (currentScan, currentPaperWhite) and ADDS a row -- it does not
                    // replace. One row is the old single-anchor ratio law; add a second in different
                    // light and the white point is interpolated between the points, so it holds across
                    // the whole range instead of only near one anchor. Greyed unless the scan is the
                    // chosen source and it currently has a value to capture.
                    ImGui::BeginDisabled(live <= 0.0f || !isSource);

                    if (ImGui::Button(Localization::Label("Anchor here")))
                    {
                        // What to capture. Before the first point, the paper white above (an absolute value
                        // with the wide range a fresh game needs). After that, the EFFECTIVE white point the
                        // picture is showing right now -- the interpolated value times the Trim the user just
                        // dialed in -- so a second point in different light captures the trimmed look, not a
                        // frozen paper white (which would make two equal whites and a flat, non-tracking
                        // curve). The trim is reset afterwards: the new point, which the picture now passes
                        // through exactly, must not be multiplied by it a second time.
                        const float captureWhite =
                            anchors.empty() ? std::max(0.01f, config->DlssNrWhitePointScale.value_or_default())
                                            : std::max(0.01f, DlssNr::ExposureScan::AnchoredWhitePoint(
                                                                  live, config->DlssNrScanInverted.value_or_default(),
                                                                  config->DlssNrScanTrim.value_or_default()));

                        if (DlssNr::ExposureScan::AnchorAdd(live, captureWhite))
                        {
                            config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                            config->DlssNrScanTrim = 1.0f;
                            selectedAnchor = -1;
                        }
                    }

                    ImGui::EndDisabled();

                    HelpMarker("Make the picture look right, then press this -- it captures the current look"
                               "\nas a point. For the first point use the Paper white slider above; for"
                               "\nevery point after, move to different light and use the Trim, which the"
                               "\nAnchor then bakes into a new point."
                               "\n\nThe first press calibrates one point -- the white point then"
                               "\nfollows the scan by ratio from there, as before. Walk into very"
                               "\ndifferent light, set paper white again, and press it again: the"
                               "\nsecond point pins down the buffer's real curve and everything"
                               "\nbetween the two is right, not just near one anchor. Up to eight."
                               "\n\nThe table is per game and shareable: one person calibrates a game"
                               "\nand the numbers are the same for everyone who takes the profile.");

                    if (!isSource)
                        ImGui::TextDisabled(
                            Localization::Tr("(the scan is only watching -- the white point above comes "
                                             "from somewhere else)"));

                    if (!anchors.empty())
                    {
                        // The row nearest the live scan value (in log space) is the one driving the
                        // picture right now; mark it so the user can see which calibration is in effect.
                        int active = 0;
                        float bestDist = 1e30f;
                        const float liveLog = std::log(std::max(live, 1e-6f));

                        for (size_t i = 0; i < anchors.size(); ++i)
                        {
                            const float d = std::fabs(std::log(std::max(anchors[i].scan, 1e-6f)) - liveLog);
                            if (d < bestDist)
                            {
                                bestDist = d;
                                active = (int) i;
                            }
                        }

                        for (size_t i = 0; i < anchors.size(); ++i)
                        {
                            ImGui::PushID((int) i);

                            // Delete first, so its click is never swallowed by the row-wide Selectable.
                            if (ImGui::SmallButton(Localization::Label("x")))
                            {
                                DlssNr::ExposureScan::AnchorRemove((int) i);
                                config->DlssNrScanAnchors = DlssNr::ExposureScan::SerializeAnchors();
                                if (selectedAnchor == (int) i)
                                    selectedAnchor = -1;
                                else if (selectedAnchor > (int) i)
                                    --selectedAnchor;
                                ImGui::PopID();
                                continue;
                            }

                            ImGui::SameLine();

                            const bool sel = (int) i == selectedAnchor;
                            char row[96];
                            snprintf(row, sizeof(row), "%s scan %.4f  ->  white %.2f%s",
                                     ((int) i == active && isSource) ? ">" : "  ", anchors[i].scan, anchors[i].white,
                                     sel ? "   [editing]" : "");

                            // Click selects the row (slider edits it); click again deselects (slider
                            // returns to the live unanchored point).
                            if (ImGui::Selectable(row, sel))
                                selectedAnchor = sel ? -1 : (int) i;

                            ImGui::PopID();
                        }

                        ImGui::TextDisabled(
                            Localization::Tr("Click a row to edit it with the slider above; click it again"
                                             " to control the live point. > is the point in use now."));
                    }

                    // The direction flag only means anything with a single point; with two or more the
                    // direction the white point moves is already fixed by the data.
                    if (anchors.size() == 1)
                    {
                        bool inverted = config->DlssNrScanInverted.value_or_default();
                        if (ImGui::Checkbox(Localization::Label("The number runs the other way"), &inverted))
                            config->DlssNrScanInverted = inverted;

                        HelpMarker("Flip this if the picture gets worse in the direction it should be"
                                   "\ngetting better. Most engines store an exposure that falls as"
                                   "\nthe scene brightens; some store its reciprocal, and a buffer"
                                   "\nfound by shape does not say which. Add a second anchor point in"
                                   "\ndifferent light and this is decided for you, so it disappears.");
                    }

                    // The scan -> white point readout is shown above the sliders now, not here.

                    // Everything below is read-out rather than control: what the scan is looking at and
                    // how to tell whether it found the right thing. Folded away because the two decisions
                    // that matter -- anchor, and which way the number runs -- are above it.
                    if (ImGui::TreeNode(Localization::Label("Advanced")))
                    {

                        const auto found = DlssNr::ExposureScan::Report();
                        const char* why = DlssNr::ExposureScan::Status();

                        if (found.empty())
                        {
                            ImGui::TextDisabled(Localization::Tr("%s"),
                                                why != nullptr && why[0] != 0 ? why : "nothing matched yet.");
                        }
                        else
                        {
                            for (size_t i = 0; i < found.size(); ++i)
                            {
                                const auto& c = found[i];

                                if (c.reads == 0)
                                {
                                    ImGui::TextDisabled(Localization::Tr("%zu. %s -- not read yet"), i + 1,
                                                        c.shape.c_str());
                                    continue;
                                }

                                // Moving is the whole signal, so it is the thing that is coloured.
                                ImGui::TextColored(
                                    c.moves ? ImVec4(0.45f, 0.8f, 0.45f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                    Localization::Tr("%zu. %s = %.5f  (seen %.5f..%.5f) %s"), i + 1, c.shape.c_str(),
                                    c.latest, c.lowest, c.highest, c.moves ? "MOVES" : "flat so far");
                            }

                            ImGui::TextDisabled(
                                Localization::Tr("Walk from shade into daylight. A real exposure moves."));
                            ImGui::TextDisabled(
                                Localization::Tr("One that only ever climbs is a counter, not an exposure."));
                        }

                        ImGui::TreePop();
                    }
                }
            }
        }

        ImGui::SeparatorText(Localization::Tr("Compare"));

        // Freeze the frame the model works on, so a setting change re-renders it in place -- the only
        // clean way to A/B our own settings (a moving scene confounds every other comparison). See
        // design/frame-hold.md.
        bool held = config->DlssNrHoldFrame.value_or_default();
        if (ImGui::Checkbox(Localization::Label("Hold frame"), &held))
            config->DlssNrHoldFrame = held;

        HelpMarker("Freezes the frame the model works on. While held, change paper white, the"
                   "\nstrengths, the reversible mode, the model preset -- anything below the"
                   "\nupscaler -- and only that setting moves; the scene does not."
                   "\n\nWhat it CANNOT show: DLSS/FSR/XeSS upscaler presets or anything upstream"
                   "\n(the upscaler is not re-run on a held frame), and the game's own HUD and"
                   "\npost-processing, which run after this pass and keep updating. The white"
                   "\npoint stops being measured and holds its value while frozen, so it cannot"
                   "\ndrift and confound the comparison."
                   "\n\nHide the menu and it stays held. Untoggle to resume.");

        static const char* compareNames[] = { Localization::Label("Off"), Localization::Label("Side by side"),
                                              Localization::Label("Wipe") };
        int compare = (int) config->DlssNrCompare.value_or_default();
        if (ImGui::Combo(Localization::Label("Compare"), &compare, compareNames, IM_ARRAYSIZE(compareNames)))
            config->DlssNrCompare = (uint32_t) compare;

        HelpMarker("Shows the pass against itself, so the two can be seen at once rather than"
                   "\ntoggled and remembered."
                   "\n\nSide by side puts the whole frame in each half, untouched on the left and"
                   "\nedited on the right. Both halves are squeezed horizontally to fit, so it is"
                   "\nfor looking at rather than playing in."
                   "\n\nWipe cuts a single frame at the split and resamples nothing, so the picture"
                   "\nis the right shape and can be played normally. Drag the split below; it is a"
                   "\nstored setting and stays put once the menu is closed."
                   "\n\nNeither needs the menu open to keep working. A hairline marks the join.");

        if (compare != 0)
        {
            bool swap = config->DlssNrCompareSwap.value_or_default();
            if (ImGui::Checkbox(Localization::Label("Swap sides"), &swap))
                config->DlssNrCompareSwap = swap;

            bool tags = config->DlssNrCompareTags.value_or_default();
            if (ImGui::Checkbox(Localization::Label("Label the sides"), &tags))
                config->DlssNrCompareTags = tags;

            HelpMarker("Writes which side is which onto the frame itself, so a screenshot still"
                       "\nsays so after it has left this machine. Drawn into the picture's own"
                       "\nplane: in the wipe the split reveals and hides the label exactly as it"
                       "\ndoes the images, and there is nothing to drag. Swap sides moves the"
                       "\nlabels with their pictures.");

            if (tags)
            {
                float tagScale = config->DlssNrTagScale.value_or_default();
                if (ImGui::SliderFloat(Localization::Label("Label size"), &tagScale, 0.5f, 5.0f, "%.1fx"))
                    config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
            }

            HelpMarker("Puts the edited frame on the other side."
                       "\n\nWorth doing once you have decided which you prefer: the eye is not"
                       "\neven-handed about left and right, and a difference can read as an"
                       "\nimprovement purely from where it sits. If the same side still wins after"
                       "\nswapping, it is the pass you are seeing and not the placement.");
        }

        if (compare == 1)
        {
            float zoom = config->DlssNrCompareZoom.value_or_default();
            if (ImGui::SliderFloat(Localization::Label("Zoom"), &zoom, 1.0f, 2.0f, "%.2f"))
                config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

            HelpMarker("How much of the frame each half shows."
                       "\n\nA half is half as wide as the frame and just as tall, so the frame"
                       "\ncannot fill it and keep its shape."
                       "\n\nAt 1 the whole frame is there at its right proportions, with bars above"
                       "\nand below. At 2 the half is filled and the sides are cropped away"
                       "\ninstead. Anything between trades one for the other.");
        }

        if (compare == 2)
        {
            float split = config->DlssNrCompareSplit.value_or_default();
            if (ImGui::SliderFloat(Localization::Label("Split"), &split, 0.0f, 1.0f, "%.2f"))
                config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

            HelpMarker("Where the wipe cuts. Left of it is the frame as the upscaler produced it,"
                       "\nright of it is the frame the model edited.");
        }

        static const char* debugNames[] = { Localization::Label("Off"),
                                            Localization::Label("Proxy (what the model sees)"),
                                            Localization::Label("Model output (raw)"),
                                            Localization::Label("Difference (amplified)") };
        int debugView = (int) config->DlssNrDebugView.value_or_default();
        if (ImGui::Combo(Localization::Label("Debug view"), &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
            config->DlssNrDebugView = (uint32_t) debugView;

        HelpMarker("Proxy is the picture handed to the model -- if that looks wrong, the white point"
                   "\nis wrong and nothing downstream can be judged."
                   "\n\nDifference shows what the model actually changed, amplified twenty times and"
                   "\ncentred on grey. A flat grey frame there means it is doing nothing.");

        ImGui::PopItemWidth();
    }
}

} // namespace DlssNr
