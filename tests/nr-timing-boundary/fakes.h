#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include "OptiScaler/dlssnr/DlssNr_PassSettings.h"
#define LOG_INFO(...) do {} while (false)

template<class T> struct Option {
    T value{};
    T value_or_default() const { return value; }
};
struct TimingSettings { bool Enabled=false; uint32_t Interval=30; };
struct Config {
    Option<bool> DlssNrEnabled{true}, DlssNrApplyModel{true}, DlssNrHoldFrame, DlssNrScanInverted, DlssNrCompareSwap,
                 DlssNrScanExposure, DlssNrAutoCapture, DlssNrUseProxy;
    Option<uint32_t> DlssNrWhitePointSource, DlssNrTransfer, DlssNrReversibleMode, DlssNrDebugView,
                     DlssNrCompare, DlssNrScalingDownscaler;
    Option<float> DlssNrWhitePointScale{1}, DlssNrWhitePointTrim{1}, DlssNrScanTrim{1},
                  DlssNrTransferStrength{1}, DlssNrColourStrength{1}, DlssNrMaxRatio{1},
                  DlssNrCompareSplit{0.5f}, DlssNrCompareZoom{1};
    TimingSettings settings;
    int writes=0;
    TimingSettings GetDlssNrGpuTimingSettings() const { return settings; }
    void SetDlssNrGpuTimingEnabled(bool value) { settings.Enabled=value; ++writes; }
    void SetDlssNrGpuTimingInterval(uint32_t value) { settings.Interval=value; ++writes; }
};
std::string TimingModelIdentity() { return "fixture-file-attributes"; }
namespace Localization {
const char* Tr(const char* text) { return text; }
const char* Label(const char* text) { return text; }
}
namespace ImGui {
std::vector<std::string> output;
int toggle=-1, interval=-1;
int checkboxCalls=0, sliderCalls=0;
void SeparatorText(const char*) {}
template<class... T> void Add(const char* format, T... values) {
    char text[2048];
    std::snprintf(text, sizeof(text), format, values...);
    output.emplace_back(text);
}
template<class... T> void TextWrapped(const char* text, T... values) { Add(text,values...); }
template<class... T> void TextDisabled(const char* text, T... values) { Add(text,values...); }
template<class... T> void Text(const char* text, T... values) { Add(text,values...); }
bool Checkbox(const char*, bool* value) {
    ++checkboxCalls;
    if(toggle < 0) return false;
    *value=toggle!=0; toggle=-1; return true;
}
bool SliderInt(const char*, int* value, int, int) {
    ++sliderCalls;
    if(interval < 0) return false;
    *value=interval; interval=-1; return true;
}
bool Contains(const std::string& value) {
    return std::any_of(output.begin(),output.end(),[&](const auto& text){ return text.find(value)!=std::string::npos; });
}
void Clear() { output.clear(); checkboxCalls=sliderCalls=0; }
}
static void HelpMarker(const char*) {}
