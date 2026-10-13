#include "pch.h"

#include "DlssNr_WorkingScale.h"

#include <Config.h>
#include <Logger.h>
#include <misc/IdentifyGpu.h>

#include <atomic>
#include <mutex>

static_assert(DlssNr::kArchTuring == NV_GPU_ARCHITECTURE_TU100);
static_assert(DlssNr::kArchAmpere == NV_GPU_ARCHITECTURE_GA100);
static_assert(DlssNr::AutoWorkingScaleFor(NV_GPU_ARCHITECTURE_TU100) == 0.5f);
static_assert(DlssNr::AutoWorkingScaleFor(NV_GPU_ARCHITECTURE_GA100) == 0.5f);
static_assert(DlssNr::AutoWorkingScaleFor(0x180) == 1.0f); // GH100, not in this NVAPI header
static_assert(DlssNr::AutoWorkingScaleFor(NV_GPU_ARCHITECTURE_AD100) == 1.0f);
static_assert(DlssNr::AutoWorkingScaleFor(NV_GPU_ARCHITECTURE_GB200) == 1.0f);
static_assert(DlssNr::AutoWorkingScaleFor(0) == 1.0f);

namespace DlssNr
{

namespace
{

std::mutex g_mutex;
std::atomic<float> g_auto { 1.0f };

bool g_haveLuid = false;
LUID g_luid {};
bool g_havePci = false;
uint32_t g_pciVendor = 0;
uint32_t g_pciDevice = 0;

// Settles auto for the adapter `matches` picks out of the GPU list, and says so once. Called with the
// lock held, only when the adapter changed.
template <typename Match> void Resolve(const char* how, Match&& matches)
{
    const auto gpus = IdentifyGpu::getAllGpus();
    const GpuInformation* gpu = nullptr;

    for (const auto& candidate : gpus)
    {
        if (matches(candidate))
        {
            gpu = &candidate;
            break;
        }
    }

    // The model runs only on NVIDIA, so a lone NVIDIA adapter is the one, whatever a translation layer
    // did to its LUID.
    bool inferred = false;
    if (gpu == nullptr)
    {
        for (const auto& candidate : gpus)
        {
            if (candidate.vendorId != VendorId::Nvidia)
                continue;

            if (gpu != nullptr)
            {
                gpu = nullptr;
                break;
            }

            gpu = &candidate;
        }

        inferred = gpu != nullptr;
    }

    if (gpu == nullptr)
    {
        g_auto = 1.0f;
        LOG_INFO("DLSS-NR: WorkingScale=auto is 1.00: the adapter the pass runs on (by {}) is not in the GPU list",
                 how);
        return;
    }

    const uint32_t arch = gpu->vendorId == VendorId::Nvidia ? gpu->nvidiaArchInfo.architecture_id : 0;
    const float scale = AutoWorkingScaleFor(arch);
    g_auto = scale;

    const char* source = inferred ? ", the only NVIDIA adapter" : "";

    // Resolved regardless, so a later switch back to auto has its answer; said, so a log from a session
    // with an explicit value does not read as if auto were in charge.
    const char* inUse = Config::Instance()->DlssNrWorkingScale.has_value() ? " -- not in use, WorkingScale is set" : "";

    if (arch == 0)
        LOG_INFO("DLSS-NR: WorkingScale=auto is {:.2f} on {}{}: NVAPI named no architecture{}", scale, gpu->name,
                 source, inUse);
    else
        LOG_INFO("DLSS-NR: WorkingScale=auto is {:.2f} on {}{} (architecture {:#x}){}", scale, gpu->name, source, arch,
                 inUse);
}

} // namespace

void NoteAdapter(const LUID& luid)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_haveLuid && IsEqualLUID(g_luid, luid))
        return;

    g_haveLuid = true;
    g_luid = luid;
    g_havePci = false;

    Resolve("LUID", [&](const GpuInformation& gpu) { return IsEqualLUID(gpu.luid, luid); });
}

void NoteAdapter(uint32_t pciVendorId, uint32_t pciDeviceId)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_havePci && g_pciVendor == pciVendorId && g_pciDevice == pciDeviceId)
        return;

    g_havePci = true;
    g_pciVendor = pciVendorId;
    g_pciDevice = pciDeviceId;
    g_haveLuid = false;

    Resolve("PCI id", [&](const GpuInformation& gpu)
            { return static_cast<uint32_t>(gpu.vendorId) == pciVendorId && gpu.deviceId == pciDeviceId; });
}

float AutoWorkingScale() { return g_auto; }

float WorkingScale()
{
    const auto& configured = Config::Instance()->DlssNrWorkingScale;
    return configured.has_value() ? configured.value() : AutoWorkingScale();
}

} // namespace DlssNr
