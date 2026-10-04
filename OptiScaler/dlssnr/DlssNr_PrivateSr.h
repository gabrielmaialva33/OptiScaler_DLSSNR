#pragma once

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>

#include <cstdint>

// The private DLSS Super Resolution that Transfer 2 enlarges the model's edit with. design/dlss-enlargement.md.
//
// One SR feature with its own parameter table and its own handle, so its history is its own and never the
// game's. Reached through the raw driver core (NVNGXProxy), not through OptiScaler's exported NVSDK_NGX_*
// entry points: those would make an IFeature of it and run the NR hook over its evaluate.
//
// Only the NGX half lives here. The textures it reads and writes, and when it may run, belong to the pass
// (shaders/dlssnr/DlssNr_Dx12.cpp); this file makes no D3D12 call and knows nothing of the frame.
//
// The creation and evaluation parameters are wilsjo2's (DlssNr_Upscaler_Dx12.cpp in v0.8.91 of
// OptiScaler-DLSSNR-PreSR-Multipass, GPL-3.0), the SR branch only. Changed from his: the value types follow
// NVIDIA's own helper (nvsdk_ngx_helpers: SetI for the flags, the quality and the reset), the motion scale is
// passed in rather than fixed at 1 over a pre-scaled texture, and the NGX entry points are handed in, so the
// host suite can stand in for the driver and so a core the game has shut down is a null pointer here, not a
// call into it.
namespace DlssNr::PrivateSr
{
using AllocateParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter** OutParameters);
using DestroyParameters = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter* InParameters);
using CreateFeature = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Feature InFeatureID,
                                           NVSDK_NGX_Parameter* InParameters, NVSDK_NGX_Handle** OutHandle);
using EvaluateFeature = NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList* InCmdList,
                                             const NVSDK_NGX_Handle* InFeatureHandle,
                                             const NVSDK_NGX_Parameter* InParameters,
                                             PFN_NVSDK_NGX_ProgressCallback InCallback);
using ReleaseFeature = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle* InHandle);

// The five entry points, as the core's getters hand them out at the moment of the call. A null one is a
// core that is not initialised (or was shut down by the game); nothing is called through it.
struct Api
{
    AllocateParameters allocate = nullptr;
    DestroyParameters destroy = nullptr;
    CreateFeature create = nullptr;
    EvaluateFeature evaluate = nullptr;
    ReleaseFeature release = nullptr;

    bool Complete() const
    {
        return allocate != nullptr && destroy != nullptr && create != nullptr && evaluate != nullptr &&
               release != nullptr;
    }
};

// What a feature is built for. A different shape is a different feature.
struct Shape
{
    uint32_t width = 0, height = 0;       // the carrier: the model's working size
    uint32_t outWidth = 0, outHeight = 0; // the frame
    bool depthInverted = false;

    bool operator==(const Shape& o) const
    {
        return width == o.width && height == o.height && outWidth == o.outWidth && outHeight == o.outHeight &&
               depthInverted == o.depthInverted;
    }
    bool operator!=(const Shape& o) const { return !(*this == o); }
};

// One evaluation. Every resource is the pass's, in the state NGX requires: inputs readable
// (NON_PIXEL_SHADER_RESOURCE), the output a UAV.
struct Frame
{
    ID3D12Resource* color = nullptr;    // the carrier, RGBA16F at the working size
    ID3D12Resource* output = nullptr;   // RGBA16F at the frame's size
    ID3D12Resource* depth = nullptr;    // at the working size
    ID3D12Resource* motion = nullptr;   // at the working size, in the game's units
    ID3D12Resource* exposure = nullptr; // 1x1 R32 holding 1
    uint32_t width = 0, height = 0;     // the render subrect: the working size
    bool reset = false;
    float mvScaleX = 1.0f, mvScaleY = 1.0f;
    float frameTimeMs = 16.67f;
};

// The parameter writes, apart from the calls, so the suite can read them back from a recording table.
void WriteCreate(NVSDK_NGX_Parameter* parameters, const Shape& shape);
void WriteEvaluate(NVSDK_NGX_Parameter* parameters, const Frame& frame);

class Feature
{
  public:
    Feature() = default;
    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;

    // Allocates the table, writes the creation parameters and creates the feature on cmdList. Never
    // evaluate on the list a feature was created on: that is the caller's to keep. On any failure the
    // table is destroyed again and the result says why (FAIL_NotInitialized for a missing entry point).
    NVSDK_NGX_Result Create(const Api& api, ID3D12GraphicsCommandList* cmdList, const Shape& shape);

    NVSDK_NGX_Result Evaluate(const Api& api, ID3D12GraphicsCommandList* cmdList, const Frame& frame);

    // The handle first, then the table. Only once the GPU is done with every recording that used it.
    // An entry point the api lacks is skipped and what it would have freed is left to the process.
    void Release(const Api& api);

    // Forget both without calling NGX. For process exit, when the core may already be gone.
    void Abandon();

    bool Live() const { return _handle != nullptr; }
    const Shape& Built() const { return _shape; }

  private:
    NVSDK_NGX_Parameter* _parameters = nullptr;
    NVSDK_NGX_Handle* _handle = nullptr;
    Shape _shape {};
};
} // namespace DlssNr::PrivateSr
