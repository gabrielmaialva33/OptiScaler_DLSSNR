#include "misc/Localization.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "menu-labels.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

using Localization::Dictionary;
int main(int argc, char** argv)
{
    assert(argc == 2);
    Dictionary dictionary;
    const char* english = "Unknown label";
    assert(dictionary.Find(english) == english);
    for (const auto* label : menuLabels) assert(dictionary.LabelValue(label) == label);
    assert(dictionary.LabelValue("Bare") == "Bare");
    assert(ImHashStr(dictionary.LabelValue("Bare###bare").c_str()) !=
           ImHashStr(dictionary.LabelValue("Other###other").c_str()));
    assert(dictionary.LabelValue("Reset##first") == "Reset##first");
    std::istringstream data("Reset\tRedefinir\nBare\tRótulo\nOther\tRótulo\n"
                            "Value: %*.*f %%\tValor: %*.*f %%\nHello\\nworld\tOlá\\nmundo\n");
    assert(dictionary.Load(data));
    assert(dictionary.Size() == 5);
    assert(std::string(dictionary.Find("Hello\nworld")) == "Olá\nmundo");
    assert(std::string(dictionary.Find("Value: %*.*f %%")) == "Valor: %*.*f %%");
    assert(dictionary.Find(english) == english);
    // Use the repository's real ImHashStr; seeds model independent windows/PushID scopes.
    for (const auto* label : { "Bare", "Other", "Reset##first", "Reset##second", "Reset###fixed",
                               "Reset###first###last", "Bare###bare", "Other###other", "Unknown", "##hidden", "###hidden" })
        for (const auto seed : { 0u, 1u, 0x12345678u, 0xFFFFFFFFu })
            assert(ImHashStr(label, 0, seed) == ImHashStr(dictionary.LabelValue(label).c_str(), 0, seed));
    assert(ImHashStr(dictionary.LabelValue("Bare").c_str()) !=
           ImHashStr(dictionary.LabelValue("Other").c_str()));
    assert(dictionary.LabelValue("Reset##first") == "Reset##first");
    assert(dictionary.LabelValue("Reset###first") == "Redefinir###first");
    assert(ImHashStr(dictionary.LabelValue("Bare###bare").c_str()) !=
           ImHashStr(dictionary.LabelValue("Other###other").c_str()));
    assert(std::string(dictionary.Find("Reset")) == "Redefinir"); // Display text never has hidden IDs.
    for (const auto* format : { "%u", "%llu", "%zu", "%.*s", "%*.*f", "%hhd", "%I64u", "%%", "%Lf" })
        assert(Dictionary::Compatible(format, format));
    for (const auto* format : { "%n", "%hhn", "%ln", "%I64n", "%1$s", "%*2$s", "%", "%q", "%999$u" })
        assert(!Dictionary::Compatible(format, format));
    assert(!Dictionary::Compatible("%*.*f", "%.*f"));
    assert(!Dictionary::Compatible("%llu", "%u"));
    assert(!Dictionary::Compatible("%u %s", "%s %u"));
    assert(!Dictionary::Compatible("%08.2f", "%f"));
    assert(!Dictionary::Compatible("%%", "%s"));
    assert(!Dictionary::Compatible("plain", "plain##injected"));
    assert(!Dictionary::Compatible("%s", "%s %n"));
    for (const auto* malformed : { "missing tab", "a\tb\tc", "a\t", "a\tb\na\tc", "a\\q\tb",
                                   "a\tb\\", "a\t%n", "a\tb###bad", "a\t\xC0\xAF", "a\t\xED\xA0\x80",
                                   "a\t\xF4\x90\x80\x80", "a\t\xE2\x82", "a\t\xFF" })
    {
        Dictionary bad;
        std::istringstream stream(malformed);
        assert(!bad.Load(stream));
        assert(bad.Size() == 0);
    }
    std::istringstream partial("first\tprimeiro\ninvalid");
    assert(!dictionary.Load(partial));
    assert(dictionary.Size() == 5); // Failed load is transactional, including when replacing a pack.
    std::istringstream nul(std::string("a\tb\0c\n", 7));
    Dictionary bad;
    assert(!bad.Load(nul));
    std::istringstream huge(std::string(16385, 'a') + "\tb\n");
    assert(!bad.Load(huge));
    std::string manyEntries;
    for (int i = 0; i < 4097; ++i) manyEntries += std::to_string(i) + "\tentry\n";
    std::istringstream excessiveEntries(manyEntries);
    assert(!bad.Load(excessiveEntries));
    std::string largeFile;
    for (int i = 0; i < 1100; ++i) largeFile += "#" + std::string(1000, 'a') + "\n";
    std::istringstream excessiveBytes(largeFile);
    assert(!bad.Load(excessiveBytes));
    std::istringstream bom("\xEF\xBB\xBF# comment\r\na\tá\r\n");
    assert(bad.Load(bom));
    assert(std::string(bad.Find("a")) == "á");
    assert(Dictionary::ValidUtf8("ação órgão referência 😀"));
    std::ifstream pack(argv[1], std::ios::binary);
    Dictionary shipped;
    assert(pack && shipped.Load(pack));
    assert(shipped.Size() >= 100);
    assert(std::string(shipped.Find("Save Settings")) == "Salvar configurações");
    for (const auto* label : menuLabels)
        for (const auto seed : { 0u, 1u, 0x12345678u, 0xFFFFFFFFu })
            assert(ImHashStr(label, 0, seed) == ImHashStr(shipped.LabelValue(label).c_str(), 0, seed));
    std::cout << "PASS: real parser and ImHashStr; default English, UTF-8, printf signatures, malformed packs, "
                 "transactional fallback, ##/### identities across four seeds; " << shipped.Size()
              << " shipped pt-BR entries; " << std::size(menuLabels) << " production menu labels checked\n";
}
