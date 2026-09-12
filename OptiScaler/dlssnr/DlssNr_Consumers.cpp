#include "pch.h"

#include "DlssNr_Consumers.h"
#include <Logger.h>
#include <filesystem>
#include <mutex>
#include <string>

namespace DlssNr::Consumers
{
namespace
{

// A marker that appears in the module name of another neural consumer and in nothing else. Matched
// case-insensitively against the base file name of every module in the process.
//
// Not a substring of "nvngx" anywhere: the driver core (_nvngx.dll), our own module under whatever
// name it was installed as, and our forwarder (nvngx.dll_dlssnr.dll) all legitimately carry that,
// and our dlss backend calling the core is exactly how the upscaler is supposed to work.
struct Competitor
{
    const wchar_t* marker;
    const char* project;
};

constexpr Competitor kCompetitors[] = {
    { L"deep-fried-chicken", "Deep Fried Chicken" },
    { L"renodx", "RenoDX (renodx-dlss / renodx-dlss5)" },
    { L"dlss5-feed", "DLSS5-Feeder" },
    { L"dlss5_feed", "DLSS5-Feeder" },
};

// Lowercase in place, ASCII only. The markers are ASCII and module names that matter here are too.
void Fold(std::wstring& s)
{
    for (wchar_t& c : s)
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
}

void Scan()
{
    HMODULE modules[1024] {};
    DWORD needed = 0;

    // K32EnumProcessModules rather than the psapi import: kernel32 is already loaded and this keeps
    // the module list out of our own link-time dependencies.
    using PFN_EnumModules = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto enumModules = kernel32 != nullptr
                                 ? reinterpret_cast<PFN_EnumModules>(GetProcAddress(kernel32, "K32EnumProcessModules"))
                                 : nullptr;

    if (enumModules == nullptr || !enumModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
    {
        LOG_DEBUG("could not enumerate process modules; the competing-consumer check did not run");
        return;
    }

    DWORD count = needed / sizeof(HMODULE);

    if (count > _countof(modules))
        count = _countof(modules);

    for (DWORD i = 0; i < count; ++i)
    {
        wchar_t buffer[MAX_PATH] {};

        if (GetModuleFileNameW(modules[i], buffer, MAX_PATH) == 0)
            continue;

        const std::filesystem::path module = buffer;
        std::wstring folded = module.filename().wstring();
        Fold(folded);

        for (const auto& competitor : kCompetitors)
        {
            if (folded.find(competitor.marker) == std::wstring::npos)
                continue;

            LOG_ERROR("another DLSS 5 neural consumer is in this process: {} (module {}). It reaches the "
                      "model by detouring NVIDIA's core, and we ARE the module the process talks to, so "
                      "our upscaler's call into the core fires its detour as well -- the frame gets TWO "
                      "neural passes. Remove one of them: either that add-on, or set [DlssNr] "
                      "Enabled=false here.",
                      competitor.project, module.filename().string());
            return;
        }
    }

    LOG_DEBUG("no competing DLSS 5 neural consumer found in this process");
}

} // namespace

void WarnIfCompeting()
{
    static std::once_flag once;
    std::call_once(once, Scan);
}

} // namespace DlssNr::Consumers
