#include "pch.h"
#include "DlssNr_Session.h"

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

// The real environment. Every failure comes back as a plain false: the game never learns the file exists.
struct WinEnv
{
    std::filesystem::path path;
    bool reportedWrite = false;

    bool Read(std::string& out)
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
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

    static bool WriteFileText(const std::filesystem::path& to, const std::string& text)
    {
        const HANDLE file =
            CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        DWORD put = 0;
        const bool ok = WriteFile(file, text.data(), (DWORD) text.size(), &put, nullptr) != FALSE && put == text.size();
        CloseHandle(file);
        return ok;
    }

    bool Write(const std::string& text)
    {
        // Beside the marker, then moved over it, so a crash mid-write cannot leave half a marker.
        std::filesystem::path temporary = path;
        temporary += L".tmp";

        bool ok = false;
        if (WriteFileText(temporary, text))
        {
            ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
            if (!ok)
                DeleteFileW(temporary.c_str());
        }

        if (!ok)
            ok = WriteFileText(path, text);

        if (!ok && !reportedWrite)
        {
            reportedWrite = true;
            LOG_DEBUG("DLSS-NR session guard: cannot write {} (error {})", path.string(), GetLastError());
        }
        return ok;
    }

    void Remove()
    {
        if (!DeleteFileW(path.c_str()))
        {
            // Could not delete: say it is over instead, so it is not read as a crash.
            Marker over;
            Write(Serialize(over));
        }
    }

    Owner Probe(uint32_t pid, uint64_t created)
    {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr)
            return GetLastError() == ERROR_INVALID_PARAMETER ? Owner::Dead : Owner::Unknown;

        DWORD exitCode = 0;
        uint64_t started = 0;
        Owner owner = Owner::Unknown;

        if (GetExitCodeProcess(process, &exitCode) && exitCode != STILL_ACTIVE)
            owner = Owner::Dead; // a handle somebody kept to a finished process
        else if (CreationTime(process, started))
            owner = started == created ? Owner::Same : Owner::Reused;

        CloseHandle(process);
        return owner;
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
} // namespace

namespace DlssNr
{
const char* SessionGuardReasonText(SessionGuard::Reason reason)
{
    switch (reason)
    {
    case SessionGuard::Reason::EarlyEnd:
        return "The last session ended without closing, soon after the model started.";
    case SessionGuard::Reason::RepeatedEnd:
        return "The last two sessions ended without closing.";
    case SessionGuard::Reason::StillBlocked:
        return "An earlier session ended without closing, and this has not been confirmed since.";
    case SessionGuard::Reason::MiddleEnd:
        return "The last session ended without closing. If it was a crash, this one may too.";
    default:
        return "";
    }
}

void SessionGuardBegin()
{
    auto* config = Config::Instance();

    // Off means off: nothing is read or written, and the file is left as it is.
    if (!config->DlssNrSessionGuard.value_or_default())
        return;

    try
    {
        Env().path = Util::DllPath().parent_path() / L"retoque_nr_session.state";
        g_begun = true;
    }
    catch (...)
    {
        return;
    }

    const auto verdict = TheGuard().Begin(true, config->DlssNrEnabled.value_or_default());

    switch (verdict.reason)
    {
    case SessionGuard::Reason::EarlyEnd:
    case SessionGuard::Reason::RepeatedEnd:
    case SessionGuard::Reason::StillBlocked:
    case SessionGuard::Reason::MiddleEnd:
        spdlog::warn("DLSS-NR session guard: {} (ran {} s, {} unclean in a row){}",
                     SessionGuardReasonText(verdict.reason), verdict.ranSeconds, verdict.streak,
                     verdict.action == SessionGuard::Action::Disable
                         ? "; Neural Rendering is off for this session (the ini is unchanged)"
                         : "");
        break;
    case SessionGuard::Reason::LongEnd:
        spdlog::info("DLSS-NR session guard: the last session ended without closing after {} s of NR; not blamed",
                     verdict.ranSeconds);
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

void SessionGuardOff()
{
    if (g_begun)
        TheGuard().NoteOff();
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
