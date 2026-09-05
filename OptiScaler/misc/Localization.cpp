#include "pch.h"
#include "Localization.h"

#include <Util.h>
#include <Logger.h>
#include <fstream>
#include <mutex>

namespace Localization
{
static const Dictionary& ActiveDictionary()
{
    static const Dictionary dictionary = []
    {
        Dictionary loaded;
        const auto path = Util::DllPath().parent_path() / L"OptiScaler.lang";
        std::ifstream file(path, std::ios::binary);
        const bool opened = file.is_open();
        if (opened && !loaded.Load(file))
            LOG_WARN("Menu localization: invalid OptiScaler.lang; using original English strings");
        else if (opened)
            LOG_INFO("Menu localization: loaded {} entries from OptiScaler.lang", loaded.Size());
        return loaded;
    }();
    return dictionary;
}

const char* Tr(const char* source) { return ActiveDictionary().Find(source); }

const char* Label(const char* source)
{
    if (!source || std::string_view(source).find("###") == std::string_view::npos ||
        ActiveDictionary().Size() == 0) return source;
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> labels;
    const std::lock_guard lock(mutex);
    const auto found = labels.find(source);
    if (found != labels.end()) return found->second.c_str();
    auto translated = ActiveDictionary().LabelValue(source);
    if (translated == source) return source;
    // IDs used here are fixed controls. Bound storage defensively for future dynamic call sites.
    if (labels.size() >= 4096) return source;
    return labels.emplace(source, std::move(translated)).first->second.c_str();
}
}
