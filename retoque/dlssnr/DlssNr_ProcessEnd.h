#pragma once

#include <cstdint>

// The moment a process ends itself, seen from inside it (docs/dlssnr/design/session-guard.md, "The exit signal").
// ExitProcess, TerminateProcess on itself and the default end of an unhandled exception all reach ntdll's
// NtTerminateProcess in the dying process, on the thread that ends it and before any other thread is stopped; a
// process killed from outside never runs it. Windows and Detours only, no OptiScaler dependency, so
// tests/nr-session-exit builds this file as it ships and ends a real process every way under Wine.

namespace DlssNr::ProcessEnd
{
// Called once per process, on the thread that ends it, with the status it ends with: the exit code, or the exception
// code an unhandled exception ends it with. It runs inside NtTerminateProcess: no logging, no allocation of its own,
// no waiting on anything another thread may hold.
using Callback = void (*)(uint32_t status);

// Puts the hook in place, once per process, and says whether it is. Only the first call attaches and only its
// callback is used; later calls return the first one's answer.
bool Watch(Callback callback);
} // namespace DlssNr::ProcessEnd
