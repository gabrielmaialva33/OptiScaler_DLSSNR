#pragma once

#include <filesystem>

namespace DlssNr::ModelLog
{
// Route the model's own diagnostics into OptiScaler.log.
//
// nvngx_dlssnr.dll reports what it actually did through OutputDebugString: the network size and
// weight set it built for a feature, and when it reset its temporal history after a control change.
// Under Proton that text goes nowhere unless WINEDEBUG is set, so questions the model answers itself
// -- does a Style change apply without a rebuild, do the presets select different weights -- could
// only be settled by measuring pixels. This patches the model's import of OutputDebugStringA/W to
// log each line as "DLSS-NR model: ..." and then call the real function.
//
// Loads the model from `snippet` if it is not loaded yet, so the hook is in place before the first
// feature is created. Idempotent: a second call finds its own hook in the import table and does
// nothing. Only NR paths call it, so it is inert when NR is off.
void Install(const std::filesystem::path& snippet);

// How many weight sets the model says it carries, from its own "N config(s) available" line at a
// feature build; -1 until it has said. The preset is an index into these, so with one there is
// nothing to choose (310.8 reports one, and a request for another falls back to it).
int ConfigCount();
} // namespace DlssNr::ModelLog
