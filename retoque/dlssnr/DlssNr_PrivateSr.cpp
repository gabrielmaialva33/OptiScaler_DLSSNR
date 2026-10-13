#include "pch.h"

#include "DlssNr_PrivateSr.h"

// See the header. Ported from wilsjo2's private upscaler (GPL-3.0), SR branch only; the changes are listed
// there.
namespace DlssNr::PrivateSr
{
void WriteCreate(NVSDK_NGX_Parameter* p, const Shape& shape)
{
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_Width, shape.width);
    p->Set(NVSDK_NGX_Parameter_Height, shape.height);
    p->Set(NVSDK_NGX_Parameter_OutWidth, shape.outWidth);
    p->Set(NVSDK_NGX_Parameter_OutHeight, shape.outHeight);

    // The best quality whatever the real ratio: the ratio is the working scale, not a DLSS mode.
    p->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_MaxQuality));

    // LDR: the carrier sits in (0, 1) around 0.5 and is not light, so no IsHDR and no auto exposure. Motion
    // is always handed at the working size, whatever the game's vectors were.
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (shape.depthInverted)
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
}

void WriteEvaluate(NVSDK_NGX_Parameter* p, const Frame& frame)
{
    p->Set(NVSDK_NGX_Parameter_Color, frame.color);
    p->Set(NVSDK_NGX_Parameter_Output, frame.output);
    p->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
    p->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
    p->Set(NVSDK_NGX_Parameter_ExposureTexture, frame.exposure);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, frame.width);
    p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, frame.height);
    p->Set(NVSDK_NGX_Parameter_Reset, frame.reset ? 1 : 0);

    // The frame it enlarges was de-jittered by the game's upscaler before the model saw it.
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_X, frame.mvScaleX);
    p->Set(NVSDK_NGX_Parameter_MV_Scale_Y, frame.mvScaleY);
    p->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, frame.frameTimeMs);

    // The carrier is a signal, not a picture: nothing scales it, nothing sharpens it.
    p->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    p->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
    p->Set(NVSDK_NGX_Parameter_Sharpness, 0.0f);
}

NVSDK_NGX_Result Feature::Create(const Api& api, ID3D12GraphicsCommandList* cmdList, const Shape& shape)
{
    if (_handle != nullptr || _parameters != nullptr)
        return NVSDK_NGX_Result_FAIL_FeatureAlreadyExists;

    if (!api.Complete() || cmdList == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;

    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result allocated = api.allocate(&parameters);

    if (allocated != NVSDK_NGX_Result_Success || parameters == nullptr)
        return allocated != NVSDK_NGX_Result_Success ? allocated : NVSDK_NGX_Result_Fail;

    WriteCreate(parameters, shape);

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result created = api.create(cmdList, NVSDK_NGX_Feature_SuperSampling, parameters, &handle);

    if (created != NVSDK_NGX_Result_Success || handle == nullptr)
    {
        // A feature the core says it made but handed no handle for cannot be released either; it is the
        // core's to keep. The table is ours.
        api.destroy(parameters);
        return created != NVSDK_NGX_Result_Success ? created : NVSDK_NGX_Result_Fail;
    }

    _parameters = parameters;
    _handle = handle;
    _shape = shape;
    return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result Feature::Evaluate(const Api& api, ID3D12GraphicsCommandList* cmdList, const Frame& frame)
{
    if (_handle == nullptr || _parameters == nullptr)
        return NVSDK_NGX_Result_FAIL_FeatureNotFound;

    if (api.evaluate == nullptr || cmdList == nullptr)
        return NVSDK_NGX_Result_FAIL_NotInitialized;

    if (frame.color == nullptr || frame.output == nullptr || frame.depth == nullptr || frame.motion == nullptr ||
        frame.exposure == nullptr || frame.width != _shape.width || frame.height != _shape.height)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;

    WriteEvaluate(_parameters, frame);
    return api.evaluate(cmdList, _handle, _parameters, nullptr);
}

void Feature::Release(const Api& api)
{
    if (_handle != nullptr && api.release != nullptr)
        api.release(_handle);

    if (_parameters != nullptr && api.destroy != nullptr)
        api.destroy(_parameters);

    Abandon();
}

void Feature::Abandon()
{
    _handle = nullptr;
    _parameters = nullptr;
    _shape = {};
}
} // namespace DlssNr::PrivateSr
