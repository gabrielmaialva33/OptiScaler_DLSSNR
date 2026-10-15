#include "pch.h"
#include "DlssNr_Session.h"
#include "DlssNr_ProcessEnd.h"

#include <Config.h>
#include <Logger.h>
#include <Util.h>

#include <ctime>
#include <string>

namespace
{
using namespace DlssNr::SessionGuard;

uint64_t Ticks(const FILETIME& t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }

bool CreationTime(HANDLE process, uint64_t& out)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return false;
    out = Ticks(created);
    return true;
}

void OnProcessEnd(uint32_t status);

// The real environment. Every failure comes back as a plain false: the game never learns the file exists. Both paths
// are made once at start, so the end of the process writes without allocating.
struct WinEnv
{
    std::wstring marker;
    std::wstring temporary;
    bool reportWrites = false; // off during Begin, whose own line says a verdict could not be written
    bool reportedWrite = false;
    bool reportedWatch = false;

    bool Read(std::string& out)
    {
        const HANDLE file = CreateFileW(marker.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        char buffer[1024];
        DWORD got = 0;
        const bool ok = ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != FALSE && got < sizeof(buffer);
        CloseHandle(file);
        if (ok)
            out.assign(buffer, got);
        return ok;
    }

    static bool WriteFileBytes(const wchar_t* to, const char* text, size_t size)
    {
        const HANDLE file = CreateFileW(to, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD put = 0;
        const bool ok = WriteFile(file, text, (DWORD) size, &put, nullptr) != FALSE && put == size;
        CloseHandle(file);
        return ok;
    }

    // Beside the marker, then moved over it, so a crash mid-write cannot leave half a marker (under Wine the move is a
    // rename(2), atomic on one filesystem). A direct write if the move fails; Parse refuses a torn one.
    bool WriteBytes(const char* text, size_t size)
    {
        bool ok = WriteFileBytes(temporary.c_str(), text, size) &&
                  MoveFileExW(temporary.c_str(), marker.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (!ok)
            DeleteFileW(temporary.c_str()); // a partial one too (a full disk)

        if (!ok)
            ok = WriteFileBytes(marker.c_str(), text, size);
        return ok;
    }

    bool Write(const std::string& text)
    {
        const bool ok = !text.empty() && WriteBytes(text.data(), text.size());
        if (!ok && reportWrites && !reportedWrite)
        {
            reportedWrite = true;
            LOG_INFO("DLSS-NR session guard: cannot write {} (error {}); a read-only folder leaves the guard inert",
                     wstring_to_string(marker), GetLastError());
        }
        return ok;
    }

    bool WriteAtExit(const char* text, size_t size) { return WriteBytes(text, size); }

    bool RemoveAtExit() { return DeleteFileW(marker.c_str()) != FALSE; }

    void Remove()
    {
        if (!DeleteFileW(marker.c_str()))
        {
            // Could not delete: say it is over instead, so it is not read as a crash.
            Marker over;
            Write(Serialize(over));
        }
    }

    bool WatchExit()
    {
        if (DlssNr::ProcessEnd::Watch(&OnProcessEnd))
            return true;

        if (!reportedWatch)
        {
            reportedWatch = true;
            LOG_INFO("DLSS-NR session guard: cannot see this process end (the NtTerminateProcess hook did not attach); "
                     "no marker this session");
        }
        return false;
    }

    void Pause() { Sleep(1); }

    Owner Probe(uint32_t pid, uint64_t created)
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
            return ClassifyProbe(false, GetLastError() == ERROR_INVALID_PARAMETER, false, false, 0, created);

        DWORD exitCode = 0;
        uint64_t started = 0;
        const bool exited = GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE;
        const bool haveCreation = !exited && CreationTime(process, started);
        CloseHandle(process);
        return ClassifyProbe(true, false, exited, haveCreation, started, created);
    }

    uint32_t SelfPid() { return GetCurrentProcessId(); }

    uint64_t SelfCreated()
    {
        uint64_t started = 0;
        CreationTime(GetCurrentProcess(), started);
        return started;
    }

    int64_t Now() { return (int64_t) std::time(nullptr); }
};

WinEnv& Env()
{
    static WinEnv env;
    return env;
}

Guard<WinEnv>& TheGuard()
{
    static Guard<WinEnv> guard(Env());
    return guard;
}

bool g_begun = false;

// Inside NtTerminateProcess, on the thread that ends the process.
void OnProcessEnd(uint32_t status)
{
    if (g_begun)
        TheGuard().NoteProcessEnd(status);
}

} // namespace

namespace DlssNr
{
const char* SessionGuardReasonText(const SessionGuard::Verdict& verdict) { return SessionGuard::ReasonText(verdict); }

void SessionGuardBegin()
{
    auto* config = Config::Instance();

    // Off means off: nothing is read or written, no hook, and the file is left as it is.
    if (!config->DlssNrSessionGuard.value_or_default())
        return;

    try
    {
        auto& env = Env();
        env.marker = (Util::DllPath().parent_path() / L"retoque_nr_session.state").wstring();
        env.temporary = env.marker + L".tmp";
        g_begun = true;
    }
    catch (...)
    {
        return;
    }

    const auto verdict = TheGuard().Begin(true, config->DlssNrEnabled.value_or_default());
    Env().reportWrites = true;

    switch (verdict.reason)
    {
    case SessionGuard::Reason::EarlyEnd:
    case SessionGuard::Reason::RepeatedEnd:
    case SessionGuard::Reason::StillBlocked:
    case SessionGuard::Reason::MiddleEnd:
    case SessionGuard::Reason::LongEnd:
        if (verdict.action == SessionGuard::Action::None)
        {
            spdlog::info("DLSS-NR session guard: the last session did not end normally, after {} s of NR ({}); not "
                         "blamed",
                         verdict.ranSeconds, SessionGuard::EndText(verdict));
            break;
        }

        if (verdict.reason == SessionGuard::Reason::StillBlocked)
            spdlog::warn("DLSS-NR session guard: {} ({} unclean in a row); Neural Rendering is off for this session "
                         "(the ini is unchanged)",
                         SessionGuardReasonText(verdict), verdict.streak);
        else
            spdlog::warn("DLSS-NR session guard: {} ({}; ran {} s, {} unclean in a row){}",
                         SessionGuardReasonText(verdict), SessionGuard::EndText(verdict), verdict.ranSeconds,
                         verdict.streak,
                         verdict.action == SessionGuard::Action::Disable
                             ? "; Neural Rendering is off for this session (the ini is unchanged)"
                             : "");
        break;
    case SessionGuard::Reason::LiveInstance:
        spdlog::info("DLSS-NR session guard: another instance owns the marker; not writing one");
        break;
    case SessionGuard::Reason::CannotPersist:
        spdlog::info("DLSS-NR session guard: the marker cannot be written here; the guard is inert");
        break;
    default:
        break;
    }

    if (verdict.action == SessionGuard::Action::Disable)
        config->DlssNrEnabled.set_volatile_value(false);
}

void SessionGuardEvaluated()
{
    if (g_begun)
        TheGuard().NoteEvaluated();
}

void SessionGuardDeviceRemoved()
{
    if (g_begun)
        TheGuard().NoteDeviceRemoved();
}

void SessionGuardOff()
{
    if (g_begun)
        TheGuard().NoteOff();
}

void SessionGuardOffAtExit()
{
    if (g_begun)
        TheGuard().NoteOffAtExit();
}

SessionGuard::Verdict SessionGuardNotice()
{
    if (!g_begun)
        return {};
    return TheGuard().Last();
}

void SessionGuardEnableAnyway()
{
    if (g_begun)
        TheGuard().Acknowledge();

    Config::Instance()->DlssNrEnabled = true;
}
} // namespace DlssNr
