#include "pch.h"
#include "DlssNr_ProcessEnd.h"

#include <detours/detours.h>

#include <atomic>
#include <mutex>

namespace
{
using TerminateFn = LONG(NTAPI*)(HANDLE process, LONG status);

TerminateFn g_original = nullptr;
std::atomic<DlssNr::ProcessEnd::Callback> g_callback { nullptr };
std::atomic<bool> g_fired { false };
std::mutex g_install;
bool g_tried = false;
bool g_installed = false;

// ExitProcess passes no handle (its first call stops every other thread); TerminateProcess(GetCurrentProcess()) and
// the unhandled-exception end pass the pseudo-handle; a real handle may still name this process.
bool IsSelf(HANDLE process)
{
    if (process == nullptr || process == GetCurrentProcess())
        return true;
    return GetProcessId(process) == GetCurrentProcessId();
}

LONG NTAPI Hook(HANDLE process, LONG status)
{
    // The first call is the one that carries the status: ExitProcess calls again after the DLL detach, and a fault
    // inside the callback would come back through here as a second end.
    if (IsSelf(process) && !g_fired.exchange(true))
    {
        if (const auto callback = g_callback.load())
            callback(static_cast<uint32_t>(status));
    }

    return g_original(process, status);
}
} // namespace

bool DlssNr::ProcessEnd::Watch(Callback callback)
{
    std::lock_guard<std::mutex> lock(g_install);
    if (g_tried)
        return g_installed;
    g_tried = true;

    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto target =
        ntdll != nullptr ? reinterpret_cast<TerminateFn>(GetProcAddress(ntdll, "NtTerminateProcess")) : nullptr;
    if (target == nullptr)
        return false;

    g_callback.store(callback); // before the hook can run
    g_original = target;

    // The pattern used across the tree: a failed attach is reported by the commit, which also closes the transaction.
    if (DetourTransactionBegin() != NO_ERROR)
        return false;
    DetourUpdateThread(GetCurrentThread());
    const LONG attached = DetourAttach(reinterpret_cast<PVOID*>(&g_original), reinterpret_cast<PVOID>(&Hook));
    const LONG committed = DetourTransactionCommit();

    g_installed = attached == NO_ERROR && committed == NO_ERROR;
    return g_installed;
}
