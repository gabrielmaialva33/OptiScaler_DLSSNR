#pragma once

struct ID3D12Device;

namespace DlssNr::Dred
{
// D3D12 Device Removed Extended Data, opt-in through [DlssNr] Dred (docs/dlssnr/design/dred.md).
//
// On native Windows a device removal reaches us as one HRESULT. The guide resample that removed Rafael's
// RTX 3060 at its first frame (13f10d60) said only DXGI_ERROR_DEVICE_REMOVED; finding the 48-byte
// constant-buffer view behind it took a review. DRED makes the runtime record, per command list, how far
// the GPU got (auto-breadcrumbs) and, on a page fault, which allocations lived at the faulting address and
// which had just been freed there. A removal then names the queue, the list and the operation it stopped
// at, which is what tells our pass apart from the game's.
//
// Both recorders have to be on before the device exists, and both cost a little on every command list,
// so this is off by default and only for a run that is chasing a removal.
//
// vkd3d-proton does not implement DRED. Under Proton the equivalent is VKD3D_CONFIG=breadcrumbs, which
// writes its own report into the vkd3d log; the log line here says so instead of failing silently.

// Turns the recorders on. Call before any D3D12 device is created; idempotent, once per process, and a
// no-op unless [DlssNr] Dred is true. Devices created before the first call are not covered.
void EnableBeforeDeviceCreation();

// Logs what DRED recorded for a removed device. Once per process: the first removal is the one that says
// something, and every later caller would read the same data. A no-op unless [DlssNr] Dred is true.
void Report(ID3D12Device* device);
} // namespace DlssNr::Dred
