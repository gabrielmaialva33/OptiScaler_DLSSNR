#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>
#include <optional>
#include <string>

using BOOL = int;
using DWORD = unsigned;
using WCHAR = wchar_t;
using HCERTSTORE = void*;
using HCRYPTMSG = void*;
using LONG = long;
using HWND = void*;
using LPVOID = void*;
using GUID = int;
constexpr DWORD CERT_QUERY_OBJECT_FILE = 1;
constexpr GUID WINTRUST_ACTION_GENERIC_VERIFY_V2 = 42;
constexpr LONG ERROR_SUCCESS = 0;
bool IsEqualGUID(GUID a, GUID b) { return a == b; }
#define LOG_DEBUG(...) ((void) 0)
std::string wstring_to_string(const std::wstring& value) { return { value.begin(), value.end() }; }
void to_lower_in_place(std::string& value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
}
struct State
{
    bool nvngxExists = false;
    std::optional<std::wstring> nvngxReplacement;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct Option
{
    bool enabled = true;
    bool value_or_default() const { return enabled; }
};
struct Config
{
    Option DxgiSpoofing, StreamlineSpoofing;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
namespace FSR4Upgrade
{
void* module = nullptr;
void* GetFSR4Module() { return module; }
} // namespace FSR4Upgrade
DWORD GetModuleFileNameW(void*, WCHAR* output, DWORD)
{
    std::wstring path = L"C:\\signed\\amd_fidelityfx_dx12.dll";
    path.copy(output, path.size());
    output[path.size()] = L'\0';
    return path.size();
}
struct WINTRUST_FILE_INFO_
{
    const WCHAR* pcwszFilePath;
};
struct WINTRUST_DATA
{
    WINTRUST_FILE_INFO_* pFile;
    void* hWVTStateData;
};
std::wstring observedPath;
const void* observedObject = nullptr;
unsigned calls = 0;
BOOL o_CryptQueryObject(DWORD type, const void* object, DWORD, DWORD, DWORD, DWORD*, DWORD*, DWORD*, HCERTSTORE*,
                        HCRYPTMSG*, const void**)
{
    ++calls;
    observedObject = object;
    observedPath = type == CERT_QUERY_OBJECT_FILE && object ? static_cast<const WCHAR*>(object) : L"";
    return 73;
}
LONG o_WinVerifyTrust(HWND, GUID*, LPVOID data)
{
    ++calls;
    auto* trust = static_cast<WINTRUST_DATA*>(data);
    observedPath = trust && trust->pFile && trust->pFile->pcwszFilePath ? trust->pFile->pcwszFilePath : L"";
    if (trust)
        trust->hWVTStateData = reinterpret_cast<void*>(0x1234);
    return 97;
}
