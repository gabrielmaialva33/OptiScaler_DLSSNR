#include "pch.h"

#include "DlssNr_ModelLog.h"

#include <Logger.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace DlssNr::ModelLog
{
namespace
{
using PFN_OutputDebugStringA = void(WINAPI*)(LPCSTR);
using PFN_OutputDebugStringW = void(WINAPI*)(LPCWSTR);

std::atomic<PFN_OutputDebugStringA> g_realA { nullptr };
std::atomic<PFN_OutputDebugStringW> g_realW { nullptr };
std::mutex g_installMutex;
std::atomic<int> g_configCount { -1 };

// The model is not expected to talk per frame, but nothing guarantees it, and a per-frame line would
// bury everything else in the log. Forty lines per ten seconds is far above anything event-driven
// (a feature build, a reset) and far below a frame rate; whatever is dropped is counted and reported
// with the next line that gets through.
constexpr unsigned int kLinesPerWindow = 40;
constexpr auto kWindow = std::chrono::seconds(10);
std::mutex g_rateMutex;
std::chrono::steady_clock::time_point g_windowStart {};
unsigned int g_inWindow = 0;
unsigned int g_dropped = 0;

// "DLSSNR: 1 config(s) available:" -- read before the rate limit, so a dropped line still counts.
void NoteConfigCount(const std::string& text)
{
    constexpr std::string_view kMarker = " config(s) available";
    const auto at = text.find(kMarker);

    if (at == std::string::npos)
        return;

    size_t start = at;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1])) && at - start < 4)
        --start;

    if (start != at)
        g_configCount = std::stoi(text.substr(start, at - start));
}

void Emit(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();

    if (text.empty())
        return;

    NoteConfigCount(text);

    unsigned int dropped = 0;
    {
        std::lock_guard<std::mutex> lock(g_rateMutex);
        const auto now = std::chrono::steady_clock::now();

        if (g_windowStart.time_since_epoch().count() == 0 || now - g_windowStart >= kWindow)
        {
            dropped = g_dropped;
            g_dropped = 0;
            g_inWindow = 0;
            g_windowStart = now;
        }

        if (g_inWindow >= kLinesPerWindow)
        {
            ++g_dropped;
            return;
        }

        ++g_inWindow;
    }

    if (dropped != 0)
        LOG_INFO("DLSS-NR model: {} lines dropped by the rate limit", dropped);

    LOG_INFO("DLSS-NR model: {}", text);
}

// Called from inside the model, on whatever thread it logs from. Nothing may unwind out of here
// into its frames.
void WINAPI CaptureA(LPCSTR text)
{
    try
    {
        if (text != nullptr)
            Emit(text);
    }
    catch (...)
    {
    }

    if (const auto real = g_realA.load())
        real(text);
}

void WINAPI CaptureW(LPCWSTR text)
{
    try
    {
        if (text != nullptr && *text != L'\0')
        {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

            if (bytes > 1)
            {
                std::string utf8((size_t) bytes, '\0');
                WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), bytes, nullptr, nullptr);
                utf8.resize((size_t) bytes - 1);
                Emit(std::move(utf8));
            }
        }
    }
    catch (...)
    {
    }

    if (const auto real = g_realW.load())
        real(text);
}

// Point one import of `module` at `replacement`. Returns the address the slot held before, the
// replacement itself when the slot already pointed at it, or nullptr when the module does not import
// that function from that DLL.
void* PatchImport(HMODULE module, const char* importDll, const char* function, void* replacement)
{
    auto* base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return nullptr;

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (directory.VirtualAddress == 0)
        return nullptr;

    for (auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         descriptor->Name != 0; ++descriptor)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), importDll) != 0)
            continue;

        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (descriptor->OriginalFirstThunk != 0 ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;

            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);

            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0)
                continue;

            void* previous = reinterpret_cast<void*>(slots->u1.Function);

            if (previous == replacement)
                return replacement;

            DWORD oldProtect = 0;

            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &oldProtect))
                return nullptr;

            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);

            DWORD ignored = 0;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), oldProtect, &ignored);
            return previous;
        }
    }

    return nullptr;
}

} // namespace

int ConfigCount() { return g_configCount.load(); }

std::string DescribeFault(unsigned long code, void* address)
{
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &owner);

    wchar_t path[MAX_PATH] = {};
    if (owner != nullptr)
        GetModuleFileNameW(owner, path, MAX_PATH);

    const std::string module = owner != nullptr ? std::filesystem::path(path).filename().string() : "unknown module";
    const auto offset = owner != nullptr ? reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(owner)
                                         : reinterpret_cast<uintptr_t>(address);

    return std::format("exception 0x{:08X} at {}+0x{:X}", code, module, offset);
}

void Install(const std::filesystem::path& snippet)
{
    std::lock_guard<std::mutex> lock(g_installMutex);

    HMODULE model = GetModuleHandleW(snippet.c_str());

    // The forwarder loads it with these flags from this same path, so this is the module it will get
    // back; loading it here only moves the load earlier, ahead of the first feature build.
    if (model == nullptr)
        model = LoadLibraryExW(snippet.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (model == nullptr)
        return;

    void* previousA = PatchImport(model, "KERNEL32.dll", "OutputDebugStringA", reinterpret_cast<void*>(&CaptureA));
    void* previousW = PatchImport(model, "KERNEL32.dll", "OutputDebugStringW", reinterpret_cast<void*>(&CaptureW));

    const bool newA = previousA != nullptr && previousA != reinterpret_cast<void*>(&CaptureA);
    const bool newW = previousW != nullptr && previousW != reinterpret_cast<void*>(&CaptureW);

    if (newA)
        g_realA = reinterpret_cast<PFN_OutputDebugStringA>(previousA);

    if (newW)
        g_realW = reinterpret_cast<PFN_OutputDebugStringW>(previousW);

    if (newA || newW)
        LOG_INFO("DLSS-NR model diagnostics routed to this log (OutputDebugStringA {}, OutputDebugStringW {})",
                 previousA != nullptr ? "hooked" : "not imported", previousW != nullptr ? "hooked" : "not imported");
}

} // namespace DlssNr::ModelLog
