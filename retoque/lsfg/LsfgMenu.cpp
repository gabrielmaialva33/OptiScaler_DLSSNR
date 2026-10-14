#include "pch.h"

#include "LsfgMenu.h"
#include "Lsfg.h"
#include "presenter/LsfgPresenter.h"
#include "presenter/MfgLatch.h"

#include <Config.h>
#include <imgui/imgui.h>
#include <misc/Localization.h>

#include <cstdio>

namespace Lsfg
{
namespace
{
const ImVec4 kNoteColor(1.0f, 0.6f, 0.2f, 1.0f);

// One item of the Multiplier combo: "3x", or "3x (after a restart)" when choosing it now cannot reach the swapchain the
// process latched.
void MultiplierText(LsfgMfg::Latch latch, int multiplier, char* text, size_t size)
{
    if (LsfgMfg::AppliesLive(latch, multiplier))
        std::snprintf(text, size, "%dx", multiplier);
    else
        std::snprintf(text, size, Localization::Tr("%dx (after a restart)"), multiplier);
}

// [LSFG] Multiplier. Which swapchain presents the frames was latched when FSR-FG created its first FG swapchain
// (tools/lsfg-study/MFG_PRESENTER.md 1.4): ours takes every choice at the next real frame, FFX's shows 2x only, so 3x
// and 4x are saved for the next start. The row says which, and what is presented now when that is not the choice.
void RenderMultiplier(Config* config, const Status& status, float menuResScale)
{
    const auto latch = Presenter::CurrentLatch();
    const int configured = LsfgMfg::MultiplierOrDefault(config->LsfgMultiplier.value_or_default());

    char preview[96];
    MultiplierText(latch, configured, preview, sizeof(preview));

    ImGui::PushItemWidth(180.0f * menuResScale);
    if (ImGui::BeginCombo(Localization::Label("Multiplier##lsfg"), preview))
    {
        for (int multiplier = 2; multiplier <= 4; ++multiplier)
        {
            char item[96];
            MultiplierText(latch, multiplier, item, sizeof(item));
            ImGui::PushID(multiplier);
            if (ImGui::Selectable(item, multiplier == configured) && multiplier != configured)
                config->LsfgMultiplier = multiplier;
            if (multiplier == configured)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s",
                          Localization::Tr("Frames shown per real frame. 2x adds one generated frame; 3x and 4x add "
                                           "two or three,\npresented by OptiScaler's own frame-generation "
                                           "swapchain, a fork of FFX's.\n\nThe swapchain is chosen when the game "
                                           "creates it. Started at 3x or 4x, every choice\napplies at the next "
                                           "frame. Started at 2x, 3x and 4x are saved and need a restart."));
    }

    if (!status.enabled)
        return;

    const int active = LsfgMfg::ActiveMultiplier(latch, configured);
    if (active != configured)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kNoteColor);
        ImGui::TextWrapped(Localization::Tr("Now %dx: the game started with FFX's swapchain, which shows one generated "
                                            "frame. Save Settings and restart the game for %dx."),
                           active, configured);
        ImGui::PopStyleColor();
    }
    else if (latch == LsfgMfg::Latch::Ours && configured > 2 && !status.active)
    {
        // Our swapchain asks for N, but FFX writes one frame while it interpolates in LSFG's place (Lsfg::TakesFrame).
        ImGui::TextDisabled("%s", Localization::Tr("Now 2x, while FFX interpolates in LSFG's place (see Status)."));
    }
}
} // namespace

void RenderMenu(float menuResScale)
{
    auto config = Config::Instance();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(Localization::Tr("LSFG 3.1 Interpolator (Lossless Scaling)"));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", Localization::Tr("Replaces FSR-FG's internal optical flow interpolation with Lossless "
                                                 "Scaling's LSFG 3.1.\nNeeds lsfg-vk.dll from Lossless Scaling "
                                                 "(auto-detected or configured in [LSFG] DllPath)."));
    }

    // 1. Enabled checkbox
    bool enabled = config->LsfgEnabled.value_or_default();
    if (ImGui::Checkbox(Localization::Label("Enable LSFG##fsrfg"), &enabled))
    {
        config->LsfgEnabled = enabled;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", Localization::Tr("Enable LSFG 3.1 frame interpolation under FSR-FG.\nWhen disabled or "
                                                 "building, FSR-FG uses standard FFX interpolation."));
    }

    // 2. Flow Scale Slider (0.25 .. 1.0)
    ImGui::BeginDisabled(!enabled);
    ImGui::PushItemWidth(180.0f * menuResScale);
    float flow = config->LsfgFlowScale.value_or_default();
    if (ImGui::SliderFloat(Localization::Label("Flow Scale##lsfg"), &flow, 0.25f, 1.0f, "%.2f"))
    {
        config->LsfgFlowScale = flow;
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", Localization::Tr("Internal optical flow pyramid resolution relative to display "
                                                 "frame.\n1.00 = Native resolution (highest quality)\n0.85 = Balanced "
                                                 "quality/performance\n0.50 = Half resolution (faster, lower VRAM)"));
    }

    // 3. Performance Mode Checkbox
    bool perf = config->LsfgPerformanceMode.value_or_default();
    if (ImGui::Checkbox(Localization::Label("Performance Mode##lsfg"), &perf))
    {
        config->LsfgPerformanceMode = perf;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", Localization::Tr("Halves feature channels across motion pyramids.\nCheaper on GPU with "
                                                 "slightly softer motion edges."));
    }

    // 4. Multiplier
    const auto status = GetStatus();
    RenderMultiplier(config, status, menuResScale);
    ImGui::EndDisabled();

    // 5. Status Line
    ImGui::Spacing();
    if (!status.enabled)
    {
        ImGui::TextDisabled("%s", Localization::Tr("Status: Disabled (using FFX interpolation)"));
    }
    else if (status.active)
    {
        if (status.gpuMs > 0.0)
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f),
                               Localization::Tr("Status: Active at %ux%u (%.2f ms/frame)"), status.width, status.height,
                               status.gpuMs);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), Localization::Tr("Status: Active at %ux%u"),
                               status.width, status.height);
        }
    }
    else if (!status.fallbackReason.empty())
    {
        ImGui::TextColored(kNoteColor, Localization::Tr("Status: %s"), status.fallbackReason.c_str());
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "%s", Localization::Tr("Status: Waiting for frames"));
    }
}

} // namespace Lsfg
