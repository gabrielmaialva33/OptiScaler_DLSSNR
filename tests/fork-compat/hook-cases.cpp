
BOOL query(const WCHAR* path, DWORD type = CERT_QUERY_OBJECT_FILE)
{
    return hkCryptQueryObject(type, path, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}
LONG verify(const WCHAR* path, GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2)
{
    WINTRUST_FILE_INFO_ info { path };
    WINTRUST_DATA data { &info, nullptr };
    const auto result = hkWinVerifyTrust(nullptr, &action, &data);
    assert(info.pcwszFilePath == path);
    if (calls)
        assert(data.hWVTStateData == reinterpret_cast<void*>(0x1234));
    return result;
}
int main()
{
    State::Instance().nvngxReplacement = L"C:\\signed\\nvngx_dlss.dll";
    struct Case
    {
        const WCHAR* path;
        bool redirected;
    };
    const Case cases[] = {
        { L"nvngx.dll", true },
        { L"NvNgX.DlL", true },
        { L"C:\\game\\NVNGX.DLL", true },
        { L"Z:/game/nvngx.dll", true },
        { L"C:\\game/mixed\\nvngx.dll", true },
        { L"_nvngx.dll", false },
        { L"C:\\driver\\_nvngx.dll", false },
        { L"C:\\game\\nvngx.dll_dlssnr.dll", false },
        { L"nvngx.dll.old", false },
        { L"nvngx.dll/other.dll", false },
        { L"C:\\nvngx.dll\\other.dll", false },
        { L"C:\\nvngx.dll\\", false },
        { L"not_nvngx.dll", false },
        { L"", false },
    };
    for (const auto& test : cases)
    {
        const std::wstring want = test.redirected ? L"C:\\signed\\nvngx_dlss.dll" : test.path;
        calls = 0;
        assert(query(test.path) == 73);
        assert(calls == 1);
        assert(observedPath == want);
        calls = 0;
        assert(verify(test.path) == 97);
        assert(calls == 1);
        assert(observedPath == want);
    }
    // Existing Crypt32 gates must remain in force for the exact basename.
    State::Instance().nvngxExists = true;
    assert(query(L"nvngx.dll") == 73 && observedPath == L"nvngx.dll");
    State::Instance().nvngxExists = false;
    Config::Instance()->DxgiSpoofing.enabled = false;
    Config::Instance()->StreamlineSpoofing.enabled = false;
    assert(query(L"nvngx.dll") == 73 && observedPath == L"nvngx.dll");
    Config::Instance()->StreamlineSpoofing.enabled = true;
    assert(query(L"nvngx.dll") == 73 && observedPath == L"C:\\signed\\nvngx_dlss.dll");
    State::Instance().nvngxReplacement.reset();
    assert(query(L"nvngx.dll") == 73 && observedPath == L"nvngx.dll");
    assert(verify(L"nvngx.dll") == 97 && observedPath == L"nvngx.dll");
    assert(query(nullptr) == 73 && observedObject == nullptr);
    assert(query(L"nvngx.dll", 2) == 73 && observedObject != nullptr && observedPath.empty());
    assert(verify(L"nvngx.dll", 999) == 97 && observedPath == L"nvngx.dll");
    assert(verify(nullptr) == 97 && observedPath.empty());
    FSR4Upgrade::module = reinterpret_cast<void*>(1);
    assert(query(L"C:\\game\\amd_fidelityfx_dx12.dll") == 73);
    assert(observedPath == L"C:\\signed\\amd_fidelityfx_dx12.dll");
    calls = 0;
    assert(verify(L"C:\\game\\amd_fidelityfx_dx12.dll") == ERROR_SUCCESS && calls == 0);
    std::cout << "PASS: exact NGX basename in both hooks, trust-state propagation, gates and FFX behavior\n";
}
