// Ends a real process every way docs/dlssnr/design/session-guard.md ("The exit signal") lists, with the shipped
// retoque/dlssnr/DlssNr_ProcessEnd.cpp watching, and records what the hook saw. run.py runs one mode per process and
// judges the record.
//
//   harness.exe <mode> <record file> [child record file]
//
// The record gets one line per callback: "status=0x........ others=alive|stopped". "others" says whether a worker
// thread was still running while the callback ran (in ExitProcess the hook must come before the other threads are
// stopped). The file is appended to, so a second callback would show as a second line.

#include <windows.h>

#include <dlssnr/DlssNr_ProcessEnd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
wchar_t g_record[MAX_PATH];
volatile LONG g_ticks = 0;

DWORD WINAPI Worker(void*)
{
    for (;;)
    {
        InterlockedIncrement(&g_ticks);
        Sleep(1);
    }
}

// Inside NtTerminateProcess: no CRT formatting, no allocation.
void Record(uint32_t status)
{
    const LONG before = g_ticks;
    Sleep(50);
    const bool alive = g_ticks != before;

    char line[64] = "status=0x";
    size_t n = std::strlen(line);
    for (int shift = 28; shift >= 0; shift -= 4)
        line[n++] = "0123456789ABCDEF"[(status >> shift) & 0xF];
    const char* tail = alive ? " others=alive\n" : " others=stopped\n";
    std::memcpy(line + n, tail, std::strlen(tail));
    n += std::strlen(tail);

    const HANDLE file =
        CreateFileW(g_record, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD put = 0;
    WriteFile(file, line, (DWORD) n, &put, nullptr);
    CloseHandle(file);
}

LONG WINAPI ExitThreeFilter(EXCEPTION_POINTERS*)
{
    TerminateProcess(GetCurrentProcess(), 3); // a game's own crash handler choosing an ordinary exit code
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void Fail(const char* why)
{
    OutputDebugStringA(why);
    ExitProcess(200);
}

// Starts this program again in `mode`, recording into `record`, and waits until it has its watch in place.
PROCESS_INFORMATION Spawn(const wchar_t* mode, const wchar_t* record)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t ready[MAX_PATH + 8];
    lstrcpyW(ready, record);
    lstrcatW(ready, L".ready");
    DeleteFileW(ready);

    wchar_t command[3 * MAX_PATH];
    lstrcpyW(command, L"\"");
    lstrcatW(command, self);
    lstrcatW(command, L"\" ");
    lstrcatW(command, mode);
    lstrcatW(command, L" \"");
    lstrcatW(command, record);
    lstrcatW(command, L"\"");

    STARTUPINFOW si { sizeof(si) };
    PROCESS_INFORMATION pi {};
    if (!CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        Fail("CreateProcessW failed");

    for (int i = 0; i < 2000 && GetFileAttributesW(ready) == INVALID_FILE_ATTRIBUTES; ++i)
        Sleep(5);
    if (GetFileAttributesW(ready) == INVALID_FILE_ATTRIBUTES)
        Fail("child never became ready");
    return pi;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
        return 100;

    lstrcpynW(g_record, argv[2], MAX_PATH);
    const wchar_t* mode = argv[1];
    const auto is = [mode](const wchar_t* name) { return lstrcmpW(mode, name) == 0; };

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    if (!DlssNr::ProcessEnd::Watch(&Record))
        return 101;
    if (!DlssNr::ProcessEnd::Watch(&Record)) // a second ask returns the first answer and attaches nothing more
        return 102;

    CloseHandle(CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr));
    Sleep(20);

    if (is(L"exit-process-0"))
        ExitProcess(0);
    if (is(L"exit-process-3"))
        ExitProcess(3);
    if (is(L"crt-exit-5"))
        std::exit(5);
    if (is(L"return-6"))
        return 6;
    if (is(L"terminate-self-0"))
        TerminateProcess(GetCurrentProcess(), 0);
    if (is(L"terminate-self-7"))
        TerminateProcess(GetCurrentProcess(), 7);
    if (is(L"terminate-self-handle-9"))
    {
        const HANDLE self =
            OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
        if (self == nullptr)
            return 103;
        TerminateProcess(self, 9);
    }
    if (is(L"access-violation"))
        *static_cast<volatile int*>(nullptr) = 1;
    if (is(L"abort"))
        std::abort();
    if (is(L"own-filter-exit-3"))
    {
        SetUnhandledExceptionFilter(&ExitThreeFilter);
        *static_cast<volatile int*>(nullptr) = 1;
    }
    if (is(L"sleep"))
    {
        // Waits to be killed from outside. Says it is ready by a file beside its record.
        wchar_t ready[MAX_PATH + 8];
        lstrcpyW(ready, g_record);
        lstrcatW(ready, L".ready");
        const HANDLE file = CreateFileW(ready, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(file);
        Sleep(60000);
        return 104;
    }
    if (is(L"kill-other"))
    {
        // This process watches too; terminating another process must not look like its own end. Then it ends itself
        // normally, which must be the one record here.
        if (argc < 4)
            return 105;
        PROCESS_INFORMATION child = Spawn(L"sleep", argv[3]);
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 10000);
        DWORD code = 0;
        GetExitCodeProcess(child.hProcess, &code);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        ExitProcess(code == 1 ? 0 : 106);
    }

    return 107; // unknown mode, or an end that did not end the process
}
