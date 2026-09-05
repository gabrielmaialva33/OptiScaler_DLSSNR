#include "misc/Localization.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    assert(argc == 2);
    const bool active = std::string(argv[1]) == "active";
    const char* original = "Save Settings";
    const char* translated = Localization::Tr(original);
    if (!active) assert(translated == original);
    else assert(std::string(translated) == "Salvar configurações");
    assert(std::string(Localization::Label("Reset##first")) == "Reset##first");
    assert(std::string(Localization::Label("Reset###first")) ==
           (active ? "Redefinir###first" : "Reset###first"));
    assert(std::string(Localization::Tr("Reset")) == (active ? "Redefinir" : "Reset"));
    // Loading is once per process. Editing/deleting the file cannot invalidate returned pointers.
    std::ofstream replacement("OptiScaler.lang");
    replacement << "Save Settings\tAnother translation\n";
    replacement.close();
    assert(Localization::Tr(original) == translated);
    std::cout << "PASS: real runtime loader " << argv[1] << "; exact display lookup, immutable pointers and IDs\n";
}
