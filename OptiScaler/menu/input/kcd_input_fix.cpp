#include "pch.h"

#include <atomic>
#include <mutex>
#include <vector>

#include "kcd_input_fix.h"

namespace OptiInput
{
namespace KcdInputFix
{
namespace
{
struct PendingPress
{
    UINT key;
    DWORD tick;
};

// ---- capture control (thread/hook lifecycle) --------------------------------
std::mutex g_ctl_mtx;
int g_active_mode = -1; // -1 = none, (int)Mode::Poll, (int)Mode::Hook
HANDLE g_poll_thread = nullptr;
DWORD g_poll_tid = 0;
HANDLE g_hook_thread = nullptr;
DWORD g_hook_tid = 0;
HHOOK g_hook = nullptr;
std::atomic<bool> g_poll_running { false };
std::atomic<bool> g_hook_running { false };
std::atomic<bool> g_capture_allowed { true };

// ---- pending-press queue (shared by both capture modes) ---------------------
std::mutex g_mtx;
std::vector<PendingPress> g_pending;
std::atomic<BYTE> g_prev[256] {};
DWORD g_last_press[256] = {};
DWORD g_hold[256] = {};

// winmm is not linked into OptiScaler, so load timeBeginPeriod/timeEndPeriod at
// runtime to get ~1 ms timer granularity for the high-rate poll. Falls back to
// plain Sleep (coarser granularity) if winmm cannot be loaded.
typedef UINT (WINAPI *PFN_TimerPeriod)(UINT);
PFN_TimerPeriod pfnTimeBeginPeriod = nullptr;
PFN_TimerPeriod pfnTimeEndPeriod = nullptr;

void EnsureTimerPeriodFuncs()
{
    if (pfnTimeBeginPeriod != nullptr)
        return;
    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (winmm == nullptr)
        return;
    pfnTimeBeginPeriod = reinterpret_cast<PFN_TimerPeriod>(GetProcAddress(winmm, "timeBeginPeriod"));
    pfnTimeEndPeriod = reinterpret_cast<PFN_TimerPeriod>(GetProcAddress(winmm, "timeEndPeriod"));
}

DWORD now_ms() { return GetTickCount(); }

bool IsFixEnabled()
{
    auto* cfg = Config::Instance();
    return cfg != nullptr && cfg->KcdInputFixEnabled.value_or_default();
}

// Mouse buttons (0x01-0x06) and the wheel (0xE0,0xE1) are not keyboard keys; skip
// them so a mouse click or wheel never injects a phantom key into the DIKEYBOARD
// state.
inline bool is_keyboard_vk(UINT vk)
{
    if (vk >= 0x01 && vk <= 0x06)
        return false;
    if (vk == 0xE0 || vk == 0xE1)
        return false;
    return true;
}

// True while a window of this process owns the foreground (fast: 2 calls). Used so
// key presses in other applications are never captured while the game is hidden.
bool our_process_foreground()
{
    HWND fg = GetForegroundWindow();
    if (fg == nullptr)
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Record a press (called on a rising edge by either capture mode). Applies the
// per-key debounce so a contact bounce does not produce two presses.
void record_press(UINT vk)
{
    if (!is_keyboard_vk(vk))
        return;
    if (!g_capture_allowed.load())
        return;
    if (!our_process_foreground())
        return;

    auto* cfg = Config::Instance();
    const int debounce_ms = cfg->KcdInputFixDebounceMs.value_or_default();
    const int max_pending = cfg->KcdInputFixMaxPending.value_or_default();

    const DWORD t = now_ms();
    if (vk < 256 && debounce_ms > 0 && (t - g_last_press[vk]) < (DWORD) debounce_ms)
        return; // within the debounce window -> treat as a bounce
    if (vk < 256)
        g_last_press[vk] = t;

    std::lock_guard<std::mutex> lk(g_mtx);
    for (auto& p : g_pending) // de-dupe if the same key is still pending
    {
        if (p.key == vk)
        {
            p.tick = t;
            return;
        }
    }
    g_pending.push_back(PendingPress { vk, t });
    while ((int) g_pending.size() > max_pending)
        g_pending.erase(g_pending.begin());
}

// ---- mode: poll (high-rate GetAsyncKeyState) ---------------------------------
DWORD WINAPI poll_thread(LPVOID)
{
    auto* cfg = Config::Instance();
    const int hz = cfg->KcdInputFixPollHz.value_or_default();

    // Seed the sampled state so keys already held are not seen as a fresh press,
    // and reset the debounce/hold bookkeeping. Under the queue mutex so it does
    // not race the apply path that also touches g_hold.
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (int i = 0; i < 256; ++i)
        {
            g_prev[i].store((GetAsyncKeyState((WORD) i) & 0x8000) ? 1 : 0);
            g_last_press[i] = 0;
            g_hold[i] = 0;
        }
    }

    EnsureTimerPeriodFuncs();
    if (pfnTimeBeginPeriod != nullptr)
        pfnTimeBeginPeriod(1);

    LOG_INFO("KcdInputFix capture (poll) thread started (tid {}) rate ~{} Hz debounce={} ms", g_poll_tid, hz,
             cfg->KcdInputFixDebounceMs.value_or_default());

    const DWORD sleep_ms = (hz > 0) ? (1000u / (unsigned) hz) : 2;

    while (g_poll_running.load())
    {
        const DWORD t0 = now_ms();
        for (UINT vk = 0; vk < 256; ++vk)
        {
            if (!is_keyboard_vk(vk))
                continue;
            const BYTE down = (GetAsyncKeyState((WORD) vk) & 0x8000) ? 1 : 0;
            const BYTE prev = g_prev[vk].load();
            if (down && !prev)
                record_press(vk);
            g_prev[vk].store(down);
        }
        const DWORD elapsed = now_ms() - t0;
        const DWORD sleep = (elapsed < sleep_ms) ? (sleep_ms - elapsed) : 0;
        if (sleep != 0)
            Sleep(sleep);
    }

    if (pfnTimeEndPeriod != nullptr)
        pfnTimeEndPeriod(1);

    LOG_INFO("KcdInputFix capture (poll) thread stopped");
    return 0;
}

void start_poll_locked()
{
    if (g_poll_thread != nullptr)
        return;
    g_poll_running.store(true);
    g_poll_thread = CreateThread(nullptr, 0, poll_thread, nullptr, 0, &g_poll_tid);
    if (g_poll_thread == nullptr)
    {
        g_poll_running.store(false);
        LOG_ERROR("KcdInputFix capture (poll) thread failed to start (err {})", GetLastError());
    }
}

void stop_poll_locked()
{
    if (g_poll_thread == nullptr)
        return;
    g_poll_running.store(false);
    WaitForSingleObject(g_poll_thread, 2000);
    CloseHandle(g_poll_thread);
    g_poll_thread = nullptr;
    g_poll_tid = 0;
}

// ---- mode: hook (global WH_KEYBOARD_LL, event driven) ------------------------
LRESULT CALLBACK ll_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const bool up = (ks->flags & LLKHF_UP) != 0;
        const bool injected = (ks->flags & LLKHF_INJECTED) != 0;
        // Capture only physical key-downs; record_press re-checks the allow gate.
        if (!up && !injected)
            record_press(ks->vkCode);
    }
    // Always pass through so we never swallow or reorder input for anyone else.
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

DWORD WINAPI hook_thread(LPVOID)
{
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc, GetModuleHandleW(nullptr), 0);
    if (g_hook == nullptr)
    {
        LOG_ERROR("KcdInputFix WH_KEYBOARD_LL install failed (err {})", GetLastError());
        return 1;
    }

    LOG_INFO("KcdInputFix capture (hook) installed (tid {})", g_hook_tid);

    MSG msg;
    while (g_hook_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook != nullptr)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }

    LOG_INFO("KcdInputFix capture (hook) removed");
    return 0;
}

void start_hook_locked()
{
    if (g_hook_thread != nullptr)
        return;
    g_hook_running.store(true);
    g_hook_thread = CreateThread(nullptr, 0, hook_thread, nullptr, 0, &g_hook_tid);
    if (g_hook_thread == nullptr)
    {
        g_hook_running.store(false);
        LOG_ERROR("KcdInputFix capture (hook) thread failed to start (err {})", GetLastError());
    }
}

void stop_hook_locked()
{
    if (g_hook_thread == nullptr)
        return;
    g_hook_running.store(false);
    // Wake the hook thread's message loop so it unhooks and exits.
    if (g_hook_tid != 0)
        PostThreadMessageW(g_hook_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hook_thread, 2000);
    CloseHandle(g_hook_thread);
    g_hook_thread = nullptr;
    g_hook_tid = 0;
    g_hook = nullptr;
}
} // namespace

void Update()
{
    auto* cfg = Config::Instance();
    if (cfg == nullptr)
        return;

    const int desired = cfg->KcdInputFixEnabled.value_or_default()
                            ? cfg->KcdInputFixMode.value_or_default()
                            : -1;

    std::lock_guard<std::mutex> lk(g_ctl_mtx);

    if (g_active_mode == desired)
        return; // already reconciled

    if (g_active_mode == (int) Mode::Poll)
        stop_poll_locked();
    else if (g_active_mode == (int) Mode::Hook)
        stop_hook_locked();

    if (desired == (int) Mode::Poll)
        start_poll_locked();
    else if (desired == (int) Mode::Hook)
        start_hook_locked();

    g_active_mode = desired;
}

void Stop()
{
    std::lock_guard<std::mutex> lk(g_ctl_mtx);

    if (g_active_mode == (int) Mode::Poll)
        stop_poll_locked();
    else if (g_active_mode == (int) Mode::Hook)
        stop_hook_locked();

    g_active_mode = -1;

    std::lock_guard<std::mutex> plk(g_mtx);
    g_pending.clear();
    for (int i = 0; i < 256; ++i)
        g_hold[i] = 0;
}

void SetCaptureAllowed(bool allowed)
{
    g_capture_allowed.store(allowed);
    if (!allowed)
    {
        // Drop anything queued while the gate was open (e.g. menu typing) so it is
        // never replayed into the game when the menu closes.
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pending.clear();
    }
}

bool IsRunning()
{
    return g_poll_running.load() || g_hook_running.load();
}

Mode ActiveMode()
{
    std::lock_guard<std::mutex> lk(g_ctl_mtx);
    return static_cast<Mode>(g_active_mode);
}

void ApplyPendingToLevelState(BYTE* state)
{
    if (!IsFixEnabled() || state == nullptr)
        return;

    auto* cfg = Config::Instance();
    const int max_age_ms = cfg->KcdInputFixMaxPendingAgeMs.value_or_default();
    const int hold_ms = cfg->KcdInputFixHoldMs.value_or_default();

    const DWORD t = now_ms();
    std::lock_guard<std::mutex> lk(g_mtx);

    // 1) Inject any newly-captured presses (force a rising edge for the game).
    int injected = 0;
    for (auto& p : g_pending)
    {
        if (t - p.tick > (DWORD) max_age_ms)
            continue; // stale -> drop
        if (p.key < 256)
        {
            if (!(state[p.key] & 0x80))
            {
                state[p.key] |= 0x80;
                ++injected;
            }
            g_hold[p.key] = t; // (re)start the hold window
        }
    }
    g_pending.clear(); // consumed (or dropped) -> no duplicates
    if (injected > 0)
        LOG_INFO("KcdInputFix folded {} missed press(es) into level state", injected);

    // 2) Keep held keys marked down for hold_ms so an irregular sample does not
    //    lose a key that is still physically pressed.
    if (hold_ms > 0)
    {
        for (UINT vk = 0; vk < 256; ++vk)
        {
            if (!g_hold[vk])
                continue;
            if (t - g_hold[vk] < (DWORD) hold_ms)
                state[vk] |= 0x80;
            else
                g_hold[vk] = 0;
        }
    }
}

int ApplyPendingToEventBuffer(DIDEVICEOBJECTDATA* events, int capacity, int count)
{
    if (!IsFixEnabled() || events == nullptr || count < 0 || capacity <= 0)
        return 0;

    auto* cfg = Config::Instance();
    const int max_age_ms = cfg->KcdInputFixMaxPendingAgeMs.value_or_default();

    const DWORD t = now_ms();
    std::lock_guard<std::mutex> lk(g_mtx);

    int added = 0;
    for (auto& p : g_pending)
    {
        if (t - p.tick > (DWORD) max_age_ms)
            continue; // stale -> drop
        if (p.key >= 256)
            continue;
        bool present = false;
        for (int i = 0; i < count; ++i)
        {
            if (events[i].dwOfs == p.key)
            {
                present = true;
                break;
            }
        }
        if (present)
            continue;
        if (count + added >= capacity)
            break;

        DIDEVICEOBJECTDATA& e = events[count + added];
        ZeroMemory(&e, sizeof(e));
        e.dwOfs = p.key; // keyboard object number == virtual key code
        // Set the low byte of dwData to 0x80 (pressed). dwData sits immediately
        // after dwOfs in both the ANSI and Unicode DIDEVICEOBJECTDATA layouts.
        *(reinterpret_cast<BYTE*>(&e) + sizeof(e.dwOfs)) = 0x80;
        ++added;
    }
    g_pending.clear();
    if (added > 0)
        LOG_INFO("KcdInputFix folded {} missed press(es) into the event buffer", added);
    return added;
}
} // namespace KcdInputFix
} // namespace OptiInput
