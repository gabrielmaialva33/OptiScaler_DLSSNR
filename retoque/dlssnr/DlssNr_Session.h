#pragma once

#include "DlssNr_SessionGuard.h"

// The session guard as the rest of the process sees it (docs/dlssnr/design/session-guard.md). The decisions are in
// DlssNr_SessionGuard.h; this layer owns the marker file, the process probe and the config write.

namespace DlssNr
{
// Once at start, after the config is read and the logger exists. Reads the previous session's marker, and when it
// says NR should stay off, switches DlssNrEnabled off for this session without touching the ini.
void SessionGuardBegin();

// An NR evaluate succeeded. Cheap.
void SessionGuardEvaluated();

// NR is not running any more (turned off, faulted) or the process is exiting in order. Cheap when nothing is owned.
void SessionGuardOff();

// What the menu shows; Action::None means nothing to say.
SessionGuard::Verdict SessionGuardNotice();

// The "Enable anyway" button: clears the block and the strikes and turns NR on (not saved as a change).
void SessionGuardEnableAnyway();

// The notice text for a verdict, untranslated; the menu runs it through Tr.
const char* SessionGuardReasonText(SessionGuard::Reason reason);
} // namespace DlssNr
