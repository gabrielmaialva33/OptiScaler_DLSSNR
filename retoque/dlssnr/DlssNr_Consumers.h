#pragma once

// Is another DLSS 5 neural-rendering consumer in this process?
//
// Every other add-on in the scene reaches the neural model by detouring NVIDIA's core
// (_nvngx.dll). We do not: we ARE the NGX implementation the process talks to, and our LoadLibrary
// hooks hand our own module to anything that asks for nvngx.dll or _nvngx.dll. So when both are
// installed, the other add-on's module is handed ours, our dlss backend then calls the real core,
// and its detours fire there -- a second neural pass over a frame that already had one. That is not
// a quality note, it is a wrong image, and nothing about it is visible without being told.
//
// Detection is by module name, because the mechanism itself cannot be observed from here: a detour
// on the core looks like the core. The list is therefore what is known today, not a guarantee.

namespace DlssNr::Consumers
{
// Warn once per process, on the log. Call after the neural forwarder has loaded -- by then anything
// that was going to hook the core has been in the process for a long time, and we know the neural
// pass is actually live, so the warning is never printed at someone who is not running it.
void WarnIfCompeting();
} // namespace DlssNr::Consumers
