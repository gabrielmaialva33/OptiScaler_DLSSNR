#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Localization
{
// Portable parser shared by production and host tests. Files use escaped TSV: source<TAB>translation.
class Dictionary
{
    std::unordered_map<std::string, std::string> entries_;

    static bool Decode(std::string_view input, std::string& output)
    {
        for (size_t i = 0; i < input.size(); ++i)
        {
            char c = input[i];
            if (c == '\\')
            {
                if (++i == input.size())
                    return false;
                switch (input[i])
                {
                case 'n':
                    c = '\n';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case '\\':
                    c = '\\';
                    break;
                default:
                    return false;
                }
            }
            if (c == '\0')
                return false;
            output += c;
        }
        return true;
    }

  public:
    static bool ValidUtf8(std::string_view value)
    {
        for (size_t i = 0; i < value.size();)
        {
            uint32_t c = static_cast<unsigned char>(value[i++]);
            if (c < 0x80)
            {
                if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t'))
                    return false;
                continue;
            }
            unsigned extra;
            uint32_t minimum;
            if (c >= 0xC2 && c <= 0xDF)
            {
                extra = 1;
                minimum = 0x80;
                c &= 0x1F;
            }
            else if (c >= 0xE0 && c <= 0xEF)
            {
                extra = 2;
                minimum = 0x800;
                c &= 0x0F;
            }
            else if (c >= 0xF0 && c <= 0xF4)
            {
                extra = 3;
                minimum = 0x10000;
                c &= 7;
            }
            else
                return false;
            if (i + extra > value.size())
                return false;
            for (unsigned j = 0; j < extra; ++j)
            {
                const auto next = static_cast<unsigned char>(value[i++]);
                if ((next & 0xC0) != 0x80)
                    return false;
                c = (c << 6) | (next & 0x3F);
            }
            if (c < minimum || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
                return false;
        }
        return true;
    }

    static bool FormatSignature(std::string_view value, std::vector<std::string>& signature)
    {
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] != '%')
                continue;
            const size_t start = i++;
            if (i == value.size())
                return false;
            if (value[i] == '%')
            {
                signature.emplace_back("%%");
                continue;
            }
            while (i < value.size() && std::string_view("-+ #0'").find(value[i]) != std::string_view::npos)
                ++i;
            if (i < value.size() && value[i] == '*')
                ++i;
            else
                while (i < value.size() && value[i] >= '0' && value[i] <= '9')
                    ++i;
            if (i < value.size() && value[i] == '.')
            {
                ++i;
                if (i < value.size() && value[i] == '*')
                    ++i;
                else
                    while (i < value.size() && value[i] >= '0' && value[i] <= '9')
                        ++i;
            }
            if (value.substr(i, 3) == "I64" || value.substr(i, 3) == "I32")
                i += 3;
            else if (value.substr(i, 2) == "ll" || value.substr(i, 2) == "hh")
                i += 2;
            else if (i < value.size() && std::string_view("hljztLw").find(value[i]) != std::string_view::npos)
                ++i;
            // %n and positional ($) directives are never accepted, even if both strings contain them.
            if (i == value.size() || std::string_view("diuoxXfFeEgGaAcCsSp").find(value[i]) == std::string_view::npos)
                return false;
            signature.emplace_back(value.substr(start, i - start + 1));
        }
        return true;
    }

    static bool Compatible(std::string_view source, std::string_view translated)
    {
        std::vector<std::string> a, b;
        return ValidUtf8(source) && ValidUtf8(translated) && !translated.empty() &&
               translated.find("##") == std::string_view::npos && FormatSignature(source, a) &&
               FormatSignature(translated, b) && a == b;
    }

    // Fail closed on malformed files rather than installing an unpredictable partial language pack.
    bool Load(std::istream& stream)
    {
        std::unordered_map<std::string, std::string> parsed;
        std::string line;
        size_t total = 0;
        bool first = true;
        while (stream.good())
        {
            line.clear();
            char byte;
            while (stream.get(byte))
            {
                if (++total > 1024 * 1024)
                    return false;
                if (byte == '\n')
                    break;
                if (line.size() >= 16384)
                    return false;
                line += byte;
            }
            if (!stream && line.empty())
                break;
            if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                line.erase(0, 3);
            first = false;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.front() == '#')
                continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos || line.find('\t', tab + 1) != std::string::npos)
                return false;
            std::string source, translated;
            if (!Decode(std::string_view(line).substr(0, tab), source) ||
                !Decode(std::string_view(line).substr(tab + 1), translated) || source.empty() ||
                !Compatible(source, translated) || !parsed.emplace(source, translated).second || parsed.size() > 4096)
                return false;
        }
        if (stream.bad())
            return false;
        entries_ = std::move(parsed);
        return true;
    }

    const char* Find(const char* source) const
    {
        if (!source)
            return source;
        const auto it = entries_.find(source);
        return it == entries_.end() ? source : it->second.c_str();
    }

    std::string LabelValue(const char* source) const
    {
        if (!source)
            return {};
        const std::string original(source);
        const auto stable = original.find("###");
        // ### itself contributes to ImHashStr. Adding it to a bare/## label changes the original
        // hash even if the original text follows it. Such controls keep their original labels.
        if (stable == std::string::npos)
            return original;
        const std::string visible = original.substr(0, original.find("##"));
        const char* translated = Find(visible.c_str());
        if (visible == translated)
            return original;
        return std::string(translated) + original.substr(stable);
    }

    size_t Size() const { return entries_.size(); }
};

// Exact-source lookup for display text; Label additionally preserves the original ImGui hash.
const char* Tr(const char* source);
const char* Label(const char* source);
} // namespace Localization
