#pragma once

#include <Windows.h>

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>

namespace OptiInput
{
// High-frequency keyboard capture + missed-press resend for games whose input
// sampling is frame dependent (e.g. Kingdom Come: Deliverance II, where a fast
// physical key-down can be dropped by the game's once-per-frame DirectInput poll,
// or occasionally double-counted).
//
// A key-down is captured either by a high-rate GetAsyncKeyState polling thread
// (mode "poll") or by a global WH_KEYBOARD_LL hook (mode "hook", event driven,
// no polling latency). Whatever captures it, the press is queued and folded into
// the DirectInput keyboard state/events the game reads (GetDeviceState /
// GetDeviceData), and a held key is kept marked down for a short window so a
// quick tap is not lost between samples.
//
// Everything is gated on [KcdInputFix] in OptiScaler.ini and the in-game menu.
// Update() reconciles the running capture mechanism with the config on demand, so
// switching poll<->hook starts/stops the right thread and installs/uninstalls the
// low-level hook. The capture is suppressed while the overlay menu is visible.
namespace KcdInputFix
{
// Capture modes. 0 = poll (GetAsyncKeyState thread), 1 = hook (WH_KEYBOARD_LL).
enum class Mode : int
{
    Poll = 0,
    Hook = 1,
};

// Reconcile the running state with the config: start/stop the poll thread and/or
// install/uninstall the low-level hook to match [KcdInputFix] Enabled + Mode.
// Idempotent and cheap when nothing changed. Call once per frame.
void Update();

// Fully tear down the capture (thread + hook). Called when the DirectInput hooks
// go away (shutdown). Idempotent.
void Stop();

// Suppress/allow key capture (set false while the overlay menu is open so menu
// typing is never replayed into the game). The low-level hook thread and the poll
// thread both respect this; it does not stop the thread, only the recording.
void SetCaptureAllowed(bool allowed);

bool IsRunning();
Mode ActiveMode();

// Fold any pending presses into a 256-byte DirectInput keyboard level-state buffer
// (one byte per virtual key, bit 7 = pressed). No-op when disabled or nothing is
// pending. Called from the GetDeviceState hook after the real state has been read.
void ApplyPendingToLevelState(BYTE* state);

// Fold any pending presses into a DirectInput keyboard event buffer. Returns the
// number of events appended (0 when disabled, the buffer is full, or nothing is
// pending). Called from the GetDeviceData hook after the real events are read.
int ApplyPendingToEventBuffer(DIDEVICEOBJECTDATA* events, int capacity, int count);
} // namespace KcdInputFix
} // namespace OptiInput
