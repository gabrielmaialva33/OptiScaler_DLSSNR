#pragma once

struct ID3D12Device;
struct ID3D12DeviceFactory;
struct ID3D12Object;
struct ID3D12GraphicsCommandList;

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
// so this is off by default and only for a run that is chasing a removal. Off, every function here returns
// at its first line: nothing is asked of D3D12, and nothing is named.
//
// vkd3d-proton does not implement DRED. Under Proton the equivalent is VKD3D_CONFIG=breadcrumbs, which
// writes its own report into the vkd3d log; the log line here says so instead of failing silently.

// Turns the recorders on, and breadcrumb contexts where the runtime has them. Call before any D3D12 device is
// created; a no-op unless [DlssNr] Dred is true. The process-wide settings are made once; devices created
// before the first call are not covered. Given the ID3D12DeviceFactory a device is about to come from (the
// Agility SDK's factory, which keeps settings of its own), that factory is configured too, on every call.
void EnableBeforeDeviceCreation(ID3D12DeviceFactory* factory = nullptr);

// Logs what DRED recorded for a removed device. Once per process: the first removal is the one that says
// something, and every later caller would read the same data. A live device costs one GetDeviceRemovedReason
// and spends nothing, so a caller may ask on every failure. A no-op unless [DlssNr] Dred is true.
void Report(ID3D12Device* device);

// A debug name for an object of ours, so a breadcrumb names the queue or list it came from; with an index,
// the index is appended. Only with [DlssNr] Dred on: off, objects keep exactly the names they had.
void Name(ID3D12Object* object, const wchar_t* name, int index = -1);

// A breadcrumb context around a span of our own work on an open list: BeginEvent at construction, EndEvent
// at destruction. Only with [DlssNr] Dred on: off, nothing is recorded. The scope must end before the list
// is closed.
class ScopedEvent
{
  public:
    ScopedEvent(ID3D12GraphicsCommandList* list, const wchar_t* name);
    ~ScopedEvent();

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

  private:
    ID3D12GraphicsCommandList* _list = nullptr;
};
} // namespace DlssNr::Dred
