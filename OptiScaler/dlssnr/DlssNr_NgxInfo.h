#pragma once

#include "DlssNr_PeScan.h"

#include <filesystem>
#include <optional>
#include <string>

namespace DlssNr::NgxInfo
{
// What NR is about to run against, said once per model path at the first feature build.
//
// Written after NIGos/dlss5-bridge showed that NVIDIA 32.0.16.1664 (616.64) changed which DLL
// creates feature 18: the driver's own NGX loader (_nvngx.dll) now routes it into nvngx_dlssnr.dll,
// and model 310.8.0.0 faults inside D3D12 on that route. Until then nothing in OptiScaler.log said
// which driver, which loader or which model a report came from -- Rafael's driver version is in
// none of this repository's logs. See dlssnr/design/ngx-driver-616.md.
struct Snapshot
{
    std::filesystem::path loaderPath; // the _nvngx.dll the NGX proxy loaded; empty if none
    uint16_t loaderVersion[4] {};     // its file version, all zero when unreadable
    PeScan::Feature18Route route = PeScan::Feature18Route::Unknown;
    std::filesystem::path modelPath;   // the nvngx_dlssnr.dll NR is about to use
    uint16_t modelVersion[4] {};       //
    unsigned long long modelBytes = 0; //
    std::filesystem::path loadedModel; // a module named nvngx_dlssnr.dll already in the process, if any
};

// Reads the loaded loader image and the model file. Cheap: an in-memory scan of the loader, a file
// version and a size. Logs the result once per distinct model path.
const Snapshot& Describe(const std::filesystem::path& modelPath);

// "616.64" from a loader file version 32.0.16.1664 (NVIDIA's numbering: the last five digits of the
// third and fourth fields). Empty when the version does not have that shape.
//
// On Windows that is the driver. Under Proton it is not: the Linux driver ships a wine build of the
// loader that carries a Windows-branch number of its own -- 615.71.09's /usr/lib/nvidia/wine/_nvngx.dll
// is 32.0.16.1691, which reads as "616.91". So the log calls it the loader's build there, and it is the
// number that decides the feature-18 route, since that route lives in this file.
std::string DriverFromLoaderVersion(const uint16_t version[4]);

// Whether creating feature 18 through the loader -- DlssNr_Proxy, [DlssNr] UseProxy -- is the pairing
// dlss5-bridge measured faulting. The forwarder and the direct runtime call the model themselves and
// never take that route, so they do not ask.
bool LoaderRouteKnownToFault(const std::filesystem::path& modelPath);

// nvngx_dlssnr.dll beside OptiScaler, else beside the executable: the host's own search order.
std::optional<std::filesystem::path> FindModel();
} // namespace DlssNr::NgxInfo
