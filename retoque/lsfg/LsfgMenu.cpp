#include "pch.h"

#include "LsfgMenu.h"
#include "Lsfg.h"

#include <Config.h>
#include <imgui/imgui.h>

namespace Lsfg
{

void RenderMenu(float menuResScale)
{
    auto config = Config::Instance();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("LSFG 3.1 Interpolator (Lossless Scaling)");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Replaces FSR-FG's internal optical flow interpolation with Lossless Scaling's LSFG 3.1.\n"
                          "Needs lsfg-vk.dll from Lossless Scaling (auto-detected or configured in [LSFG] DllPath).");
    }

    // 1. Enabled checkbox
    bool enabled = config->LsfgEnabled.value_or_default();
    if (ImGui::Checkbox("Enable LSFG##fsrfg", &enabled))
    {
        config->LsfgEnabled = enabled;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Enable LSFG 3.1 frame interpolation under FSR-FG.\n"
                          "When disabled or building, FSR-FG uses standard FFX interpolation.");
    }

    // 2. Flow Scale Slider (0.25 .. 1.0)
    ImGui::BeginDisabled(!enabled);
    ImGui::PushItemWidth(180.0f * menuResScale);
    float flow = config->LsfgFlowScale.value_or_default();
    if (ImGui::SliderFloat("Flow Scale##lsfg", &flow, 0.25f, 1.0f, "%.2f"))
    {
        config->LsfgFlowScale = flow;
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Internal optical flow pyramid resolution relative to display frame.\n"
                          "1.00 = Native resolution (highest quality)\n"
                          "0.85 = Balanced quality/performance\n"
                          "0.50 = Half resolution (faster, lower VRAM)");
    }

    // 3. Performance Mode Checkbox
    bool perf = config->LsfgPerformanceMode.value_or_default();
    if (ImGui::Checkbox("Performance Mode##lsfg", &perf))
    {
        config->LsfgPerformanceMode = perf;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Halves feature channels across motion pyramids.\n"
                          "Cheaper on GPU with slightly softer motion edges.");
    }
    ImGui::EndDisabled();

    // 4. Status Line
    const auto status = GetStatus();
    ImGui::Spacing();
    if (!status.enabled)
    {
        ImGui::TextDisabled("Status: Disabled (using FFX interpolation)");
    }
    else if (status.active)
    {
        if (status.gpuMs > 0.0)
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Status: Active at %ux%u (%.2f ms/frame)", status.width,
                               status.height, status.gpuMs);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Status: Active at %ux%u", status.width, status.height);
        }
    }
    else if (!status.fallbackReason.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Status: %s", status.fallbackReason.c_str());
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "Status: Waiting for frames");
    }
}

} // namespace Lsfg
