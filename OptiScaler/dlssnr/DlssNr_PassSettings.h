#pragma once

// Portable model tuning and the owned sparse INI namespace. No renderer or platform dependencies.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct DlssNrResolvedPassSettings
{
    float Intensity = 1.0f;
    float LocalStructure = 1.0f;
    float LocalTone = 1.0f;
    float SkinStructure = -1.0f;
    uint32_t Style = 0;
    uint32_t Preset = 0;
    bool AutoMask = true;
    bool operator==(const DlssNrResolvedPassSettings&) const = default;
};

struct DlssNrPassSettings
{
    std::optional<float> Intensity, LocalStructure, LocalTone, SkinStructure;
    std::optional<uint32_t> Style, Preset;
    std::optional<bool> AutoMask;
    bool operator==(const DlssNrPassSettings&) const = default;
};

namespace DlssNr::PassConfig
{
inline constexpr uint32_t MaxPasses = 3;
using Overrides = std::map<uint32_t, DlssNrPassSettings>;
inline uint32_t BoundCount(uint32_t count) { return std::clamp(count, 1u, MaxPasses); }

inline bool EqualAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

inline std::optional<uint32_t> UInt(const char* text, uint32_t maximum)
{
    if (!text) return std::nullopt;
    std::string_view value(text);
    if (value.starts_with('+')) value.remove_prefix(1);
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) { value.remove_prefix(2); base = 16; }
    if (value.empty()) return std::nullopt;
    uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (error != std::errc() || end != value.data() + value.size() || parsed > maximum) return std::nullopt;
    return parsed;
}

inline std::optional<uint32_t> Count(const char* text)
{
    const auto count = UInt(text, std::numeric_limits<uint32_t>::max());
    return count ? std::optional<uint32_t>(BoundCount(*count)) : std::nullopt;
}

inline std::optional<float> Float(const char* text, float minimum, float maximum)
{
    if (!text) return std::nullopt;
    std::string_view value(text);
    if (value.starts_with('+')) value.remove_prefix(1);
    if (value.empty() || value.starts_with("+-")) return std::nullopt;
    // Reject a second sign after consuming '+', including the malformed '+-1'.
    if (text[0] == '+' && value.starts_with('-')) return std::nullopt;
    float parsed = 0.0f;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc() || end != value.data() + value.size() || !std::isfinite(parsed) ||
        parsed < minimum || parsed > maximum) return std::nullopt;
    return parsed;
}

inline std::optional<bool> Bool(const char* text)
{
    if (!text) return std::nullopt;
    if (EqualAscii(text, "true")) return true;
    if (EqualAscii(text, "false")) return false;
    return std::nullopt;
}

inline std::optional<uint32_t> Index(std::string_view name)
{
    constexpr std::string_view prefix = "DlssNr.Pass";
    if (name.size() <= prefix.size() || !EqualAscii(name.substr(0, prefix.size()), prefix)) return std::nullopt;
    const auto digits = name.substr(prefix.size());
    uint32_t number = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number, 10);
    if (error != std::errc() || end != digits.data() + digits.size() || number == 0) return std::nullopt;
    return number - 1;
}

inline DlssNrPassSettings Validated(DlssNrPassSettings value)
{
    auto finite = [](std::optional<float>& field, float minimum) {
        if (field && (!std::isfinite(*field) || *field < minimum || *field > 2.0f)) field.reset();
    };
    finite(value.Intensity, 0.0f);
    finite(value.LocalStructure, 0.0f);
    finite(value.LocalTone, 0.0f);
    finite(value.SkinStructure, -1.0f);
    if (value.Preset && *value.Preset > 3) value.Preset.reset();
    if (value.Style && *value.Style > 2) value.Style.reset();
    return value;
}

inline DlssNrResolvedPassSettings Resolve(const DlssNrResolvedPassSettings& master,
                                         const DlssNrPassSettings& sparse, bool individual)
{
    if (!individual) return master;
    const auto value = Validated(sparse);
    return { value.Intensity.value_or(master.Intensity), value.LocalStructure.value_or(master.LocalStructure),
             value.LocalTone.value_or(master.LocalTone), value.SkinStructure.value_or(master.SkinStructure),
             value.Style.value_or(master.Style), value.Preset.value_or(master.Preset),
             value.AutoMask.value_or(master.AutoMask) };
}

inline void Set(Overrides& overrides, uint32_t index, DlssNrPassSettings value)
{
    // UINT_MAX cannot be represented by a one-based uint32 section number.
    if (index == std::numeric_limits<uint32_t>::max()) return;
    value = Validated(value);
    if (value == DlssNrPassSettings {}) overrides.erase(index);
    else overrides.insert_or_assign(index, value);
}

inline DlssNrPassSettings Get(const Overrides& overrides, uint32_t index)
{
    const auto found = overrides.find(index);
    return found == overrides.end() ? DlssNrPassSettings {} : found->second;
}

// SimpleIni is supplied by the caller so the exact codec can run in portable host tests.
template <class Ini> Overrides Load(const Ini& ini)
{
    Overrides result;
    typename Ini::TNamesDepend sections;
    ini.GetAllSections(sections);
    sections.sort(typename Ini::Entry::LoadOrder());
    for (const auto& section : sections)
    {
        const auto index = Index(section.pItem);
        if (!index) continue;
        auto value = Get(result, *index);
        auto first = [](auto& destination, auto source) { if (!destination) destination = source; };
        first(value.Intensity, Float(ini.GetValue(section.pItem, "Intensity"), 0.0f, 2.0f));
        first(value.LocalStructure, Float(ini.GetValue(section.pItem, "LocalStructure"), 0.0f, 2.0f));
        first(value.LocalTone, Float(ini.GetValue(section.pItem, "LocalTone"), 0.0f, 2.0f));
        first(value.SkinStructure, Float(ini.GetValue(section.pItem, "SkinStructure"), -1.0f, 2.0f));
        first(value.Style, UInt(ini.GetValue(section.pItem, "Style"), 2));
        first(value.Preset, UInt(ini.GetValue(section.pItem, "Preset"), 3));
        first(value.AutoMask, Bool(ini.GetValue(section.pItem, "AutoMask")));
        Set(result, *index, value);
    }
    return result;
}

inline std::string FloatText(float value)
{
    std::array<char, 64> buffer {};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                          std::chars_format::general, std::numeric_limits<float>::max_digits10);
    return error == std::errc() ? std::string(buffer.data(), end) : "auto";
}

template <class Ini> void Save(Ini& ini, const Overrides& overrides)
{
    typename Ini::TNamesDepend sections;
    ini.GetAllSections(sections);
    // Copy names before deleting: SimpleIni entries refer into its internal storage.
    std::vector<std::string> ownedSections;
    for (const auto& section : sections)
        if (Index(section.pItem)) ownedSections.emplace_back(section.pItem);
    for (const auto& section : ownedSections) ini.Delete(section.c_str(), nullptr);
    for (const auto& [index, raw] : overrides)
    {
        if (index == std::numeric_limits<uint32_t>::max()) continue;
        const auto value = Validated(raw);
        const std::string section = "DlssNr.Pass" + std::to_string(uint64_t(index) + 1);
        auto saveFloat = [&](const char* key, const std::optional<float>& field) {
            if (field) ini.SetValue(section.c_str(), key, FloatText(*field).c_str());
        };
        saveFloat("Intensity", value.Intensity);
        saveFloat("LocalStructure", value.LocalStructure);
        saveFloat("LocalTone", value.LocalTone);
        saveFloat("SkinStructure", value.SkinStructure);
        if (value.Style) ini.SetValue(section.c_str(), "Style", std::to_string(*value.Style).c_str());
        if (value.Preset) ini.SetValue(section.c_str(), "Preset", std::to_string(*value.Preset).c_str());
        if (value.AutoMask) ini.SetValue(section.c_str(), "AutoMask", *value.AutoMask ? "true" : "false");
    }
}
} // namespace DlssNr::PassConfig

struct DlssNrPassSnapshot
{
    uint32_t Count = 1;
    bool Individual = false;
    std::array<DlssNrResolvedPassSettings, DlssNr::PassConfig::MaxPasses> Settings {};
};
