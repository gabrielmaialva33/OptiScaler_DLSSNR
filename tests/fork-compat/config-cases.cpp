
void expect(Config& config, const std::optional<std::wstring>& sr, const std::optional<std::wstring>& fg,
            const std::optional<std::wstring>& ll)
{
    assert(config.XeSSLibrary.has_value() == sr.has_value());
    if (sr)
        assert(config.XeSSLibrary.value() == sr.value());
    assert(config.XeFGLibrary.has_value() == fg.has_value());
    if (fg)
        assert(config.XeFGLibrary.value() == fg.value());
    assert(config.XeLLLibrary.has_value() == ll.has_value());
    if (ll)
        assert(config.XeLLLibrary.value() == ll.value());
}

int main()
{
    struct Case
    {
        const char* sr;
        const char* fg;
        const char* ll;
        std::optional<std::wstring> wantSr;
        std::optional<std::wstring> wantFg;
        std::optional<std::wstring> wantLl;
    };
    const Case cases[] = {
        { "C:\\SR\\libxess.dll", "D:\\FG\\libxess_fg.dll", "E:\\LL\\libxell.dll", L"C:\\SR\\libxess.dll",
          L"D:\\FG\\libxess_fg.dll", L"E:\\LL\\libxell.dll" },
        { "auto", "D:\\FG\\libxess_fg.dll", "auto", std::nullopt, L"D:\\FG\\libxess_fg.dll", std::nullopt },
        { "auto", "auto", "E:\\LL\\libxell.dll", std::nullopt, std::nullopt, L"E:\\LL\\libxell.dll" },
        { "C:\\SR\\libxess.dll", "auto", "auto", L"C:\\SR\\libxess.dll", std::nullopt, std::nullopt },
        { "auto", "AUTO", "Auto", std::nullopt, std::nullopt, std::nullopt },
        { nullptr, nullptr, nullptr, std::nullopt, std::nullopt, std::nullopt },
    };
    for (const auto& test : cases)
    {
        Config first;
        if (test.sr)
            first.ini.SetValue("Libraries", "XeSSPath", test.sr);
        if (test.fg)
            first.ini.SetValue("Libraries", "XeFGPath", test.fg);
        if (test.ll)
            first.ini.SetValue("Libraries", "XeLLPath", test.ll);
        first.Read();
        expect(first, test.wantSr, test.wantFg, test.wantLl);
        first.ini.Reset();
        first.Save();
        std::string saved;
        assert(first.ini.Save(saved) >= 0);
        Config second;
        assert(second.ini.LoadData(saved) >= 0);
        second.Read();
        expect(second, test.wantSr, test.wantFg, test.wantLl);
        for (const auto* key : { "XeSSPath", "XeFGPath", "XeLLPath" })
            assert(std::string(second.ini.GetValue("Libraries", key)) != "AUTO");
    }
    std::cout << "PASS: distinct XeSS/XeFG/XeLL paths, single overrides, auto/missing and read/save/read\n";
}
