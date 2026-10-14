#pragma once

// Device Removed Extended Data, as a developer switch.
//
// A device removed on a player's PC says only why, in one word: DXGI_ERROR_DEVICE_HUNG, DXGI_ERROR_INVALID_CALL.
// Which command list, which operation, which allocation is what DRED keeps: auto-breadcrumbs (every operation a list
// recorded, and how far the GPU got through it), their contexts (the event names around them), and on a page fault the
// address and the allocations that were, or had just stopped being, there. DRED is part of the retail D3D12 runtime;
// it needs no SDK layers, which a player's PC does not have (Rafael's, 2026-10-14: Divinity hung after two minutes with
// the neural pass, synthesized frame generation and DLSS-G all on the bridge's device).
//
// On with RETOQUE_DRED=1 in the environment, or a file named retoque_dred beside OptiScaler.dll (the pattern of
// dllmain.cpp's optiscaler_skip_vulkan_hooks marker). No ini key. Off by default, and off nothing here calls anything:
// every function below returns at its first line.
//
// Windows only in effect. Where the interfaces are missing -- an older Windows, vkd3d-proton -- it says so once and
// everything carries on as it would have.

#include <d3d12.h>

namespace Dred
{

// Whether the switch is on. Read once, and said once when it is.
bool Enabled();

// Before a D3D12 device is created: auto-breadcrumbs, breadcrumb contexts and page-fault reporting forced on, through
// the real d3d12.dll's D3D12GetDebugInterface. Once per process; later calls do nothing. Given the device factory a
// device is being created through (the Agility SDK's ID3D12DeviceFactory), its own configuration as well, once per
// factory. Settings made after a device exists apply only to devices created after them.
void BeforeDeviceCreation(ID3D12DeviceFactory* factory = nullptr);

// At the first sign that a device was removed: everything DRED kept about it, into the log. Once per device; a device
// that is not removed, or one already reported, costs one call to GetDeviceRemovedReason and nothing else. Never per
// frame by construction, so a caller in a loop that fails every frame can call it every time.
void ReportRemoval(ID3D12Device* device, const char* where);

// A debug name for an object of ours, so a breadcrumb names the list or queue it came from. With an index, the index is
// appended. Only with the switch on: off, objects keep exactly the names they had.
void Name(ID3D12Object* object, const wchar_t* name, int index = -1);

// A breadcrumb context around a span of our own work on an open list: BeginEvent at construction, EndEvent at
// destruction. Only with the switch on. The scope must end before the list is closed.
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

} // namespace Dred
