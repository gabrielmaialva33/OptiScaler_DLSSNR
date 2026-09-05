#include <dlssnr/DlssNr_PassSettings.h>
#include <cassert>
#include <string>
#include <unordered_map>
#include <functional>
#include <iostream>
#include <set>
#define IM_ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
using ImGuiID = size_t;
namespace Localization { const char* Tr(const char* x) { return x; } const char* Label(const char* x) { return x; } }
namespace ImGui {
std::vector<std::string> stack, shown;
std::map<std::string, double> actions;
std::string last;
bool edited = false;
void PushID(const char* id) { stack.emplace_back(id); }
void PushID(int id) { stack.push_back(std::to_string(id)); }
void PopID() { assert(!stack.empty()); stack.pop_back(); }
std::string Key(const char* label) { std::string key; for (auto& x : stack) key += x + '/'; return key + label; }
ImGuiID GetID(const char* label) { return std::hash<std::string>{}(Key(label)); }
template<class T> bool Input(const char* label, T* value) {
 last = Key(label); shown.push_back(last); edited = false;
 auto item = actions.find(last); if (item == actions.end()) return false;
 *value = static_cast<T>(item->second); actions.erase(item); return edited = true;
}
bool Checkbox(const char* label, bool* x) { return Input(label,x); }
bool SliderFloat(const char* label,float* x,float,float,const char*) { return Input(label,x); }
bool SliderInt(const char* label,int* x,int,int) { return Input(label,x); }
bool Combo(const char* label,int* x,const char* const*,int) { return Input(label,x); }
bool SmallButton(const char* label) { bool x = false; return Input(label,&x); }
bool IsItemDeactivatedAfterEdit() { return edited; }
bool IsItemActive() { return false; }
void SameLine() {}
void SeparatorText(const char*) {}
void TextUnformatted(const char*) {}
template<class...T> void Text(const char*, T...) {}
template<class...T> void TextWrapped(const char*, T...) {}
void Reset() { assert(stack.empty()); shown.clear(); actions.clear(); edited = false; }
bool Saw(const std::string& id) { return std::find(shown.begin(),shown.end(),id)!=shown.end(); }
}
struct OptionalBool { bool value = false; bool value_or_default() const { return value; } };
struct Config {
 OptionalBool DlssNrUseProxy;
 DlssNrResolvedPassSettings master;
 DlssNr::PassConfig::Overrides overrides;
 DlssNrPassSnapshot snapshot;
 unsigned writes = 0;
 DlssNrPassSnapshot GetDlssNrPassSnapshot() const { return snapshot; }
 DlssNrResolvedPassSettings GetDlssNrMasterSettings() const { return master; }
 DlssNrPassSettings GetDlssNrPassOverrides(uint32_t i) const { return DlssNr::PassConfig::Get(overrides,i); }
 void SetDlssNrPassCount(uint32_t n) { snapshot.Count=DlssNr::PassConfig::BoundCount(n); ++writes; }
 void SetDlssNrIndividualPassSettings(bool x) { snapshot.Individual=x; ++writes; }
 void SetDlssNrPassOverrides(uint32_t i,DlssNrPassSettings x) { DlssNr::PassConfig::Set(overrides,i,x); ++writes; }
};
namespace DlssNr {
void HelpMarker(const char*) {}
unsigned ActivePassCount() { return 0; }
const char* MultipassStatus() { return "Restart required"; }
}
