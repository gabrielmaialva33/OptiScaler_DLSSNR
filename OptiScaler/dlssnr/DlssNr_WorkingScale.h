#pragma once

#include <cstdint>

struct _LUID;

namespace DlssNr
{

// What [DlssNr] WorkingScale=auto stands for on the GPU the pass runs on. design/working-scale-auto.md
// is the reasoning; model-cost-across-architectures.md is the measurement it rests on.

// NV_GPU_ARCHITECTURE_TU100 and _GA100, spelled out so this header needs no NVAPI; the .cpp asserts they
// match. NVAPI gives one id per generation, whatever the die. Named one by one rather than as a range
// below Ada, because Hopper (GH100, 0x180, which has FP8 tensor cores) sits between them.
inline constexpr uint32_t kArchTuring = 0x160;
inline constexpr uint32_t kArchAmpere = 0x170;

// Half resolution on Turing and Ampere; full resolution on everything else, including anything NVAPI
// did not name (0), which is what auto meant before. An RTX 3060 measured 25 times an RTX 4090's
// per-pixel cost for the model; Turing is inferred, not measured.
constexpr float AutoWorkingScaleFor(uint32_t architectureId)
{
    return architectureId == kArchTuring || architectureId == kArchAmpere ? 0.5f : 1.0f;
}

// Tell the resolver which adapter the pass records on. Cheap when the adapter has not changed: the GPU
// list is only consulted, and the answer only logged, when it has. D3D12 knows its adapter by LUID and
// notes it every dispatch; Vulkan knows it by PCI id and notes it once per physical device.
void NoteAdapter(const _LUID& luid);
void NoteAdapter(uint32_t pciVendorId, uint32_t pciDeviceId);

// What auto resolves to for the adapter noted last; 1.0 before any.
float AutoWorkingScale();

// [DlssNr] WorkingScale as the pass uses it: the configured value, or AutoWorkingScale() for auto.
// Not clamped -- each route applies its own bounds.
float WorkingScale();

} // namespace DlssNr
