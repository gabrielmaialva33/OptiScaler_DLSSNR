#include "pch.h"

#include "DLSSG_Common.h"

#include <hooks/Reflex_Hooks.h>
#include <hooks/Streamline_Hooks.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

using namespace DirectX;

feature_version DLSSG_Common::Version()
{
    if (StreamlineProxy::LoadStreamline())
        return StreamlineProxy::Version();

    return { 0, 0, 0 };
}

void DLSSG_Common::Activate()
{
    LOG_DEBUG("");

    if (!_isActive)
    {
        UpdateTarget();
        _isActive = true;
    }
}

void DLSSG_Common::Deactivate()
{
    LOG_DEBUG("");

    _gameCamera.Clear();

    if (_isActive)
    {
        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOff;
        options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

        if (StreamlineProxy::DLSSGSetOptions() != nullptr)
            StreamlineProxy::DLSSGSetOptions()(_viewport, options); // Potential crash point on exit

        sl::ReflexOptions reflexConst = {};
        reflexConst.mode = sl::ReflexMode::eOff;
        reflexConst.useMarkersToOptimize = false;
        ScopedOptiScalerReflex optiScalerCall {};

        if (StreamlineProxy::ReflexSetOptions() != nullptr)
            StreamlineProxy::ReflexSetOptions()(reflexConst);

        // The cache mirrors what the plugin now holds (eOff), so the next Dispatch re-sends the
        // active values instead of assuming they are still latched from before the deactivate.
        _lastDlssgMode = sl::DLSSGMode::eOff;
        _lastDlssgFrames = 0;
        _lastDlssgDynamicTarget = 0.0f;
        _lastDlssgFlags = {};
        _lastReflexMode = sl::ReflexMode::eOff;
        _lastReflexMarkers = false;
        _lastDlssgGeneration = StreamlineHooks::dlssgOptionsGeneration();
        _lastReflexGeneration = StreamlineHooks::reflexOptionsGeneration();

        _isActive = false;
    }
}

void DLSSG_Common::PauseAfterError()
{
    State::Instance().fgChanged = true;
    UpdateTarget();
    Deactivate();
}

void DLSSG_Common::QueryState()
{
    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(_viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        State::Instance().dlssgMfgMax = dlssgState.numFramesToGenerateMax;
        _maxInterpolationCount = dlssgState.numFramesToGenerateMax;
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }
}

bool DLSSG_Common::SetOptions(bool gameMarkersReachStreamline)
{
    if (Config::Instance()->FGDLSSGInterpolationCount.value_or_default() > _maxInterpolationCount)
    {
        Config::Instance()->FGDLSSGInterpolationCount = _maxInterpolationCount;
        LOG_WARN("Requested interpolation count is higher than max supported, setting to max: {}",
                 _maxInterpolationCount);
    }

    const int targetCount = std::clamp(
        Config::Instance()->FGInterpolationCount(Config::Instance()->FGDLSSGInterpolationCount.value_or_default()), 1,
        std::max(_maxInterpolationCount, 1));

    if (_framesToInterpolate != targetCount)
    {
        LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate, targetCount);

        _framesToInterpolate = targetCount;

        // The SL plugin only latches numFramesToGenerate on an eOff -> eOn transition
        // (its presentCommon "DLSS-G interpolation state changed" / the NGX
        // ReportOverrideStates). Changing the count while FG stays eOn is ignored, so a
        // mid-session 3x/4x request would silently keep generating at the original count.
        // Drop to eOff this frame; the next Dispatch re-enters eOn with the new count and
        // the feature rebuilds at the requested rate.
        _mfgReinitPending = true;
    }

    if (_mfgReinitPending)
    {
        sl::DLSSGOptions off {};
        off.mode = sl::DLSSGMode::eOff;
        off.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        const auto result = StreamlineProxy::DLSSGSetOptions()(_viewport, off);
        if (result != sl::Result::eOk)
        {
            LOG_ERROR("Couldn't disable DLSSG for MFG count change, error: {}", magic_enum::enum_name(result));
            return false;
        }

        _mfgReinitPending = false;
        // This write also changes the plugin's cached state. In particular, returning to the
        // previous count before resuming must not mistake the old eOn options for current ones.
        _lastDlssgMode = off.mode;
        _lastDlssgFrames = off.numFramesToGenerate;
        _lastDlssgDynamicTarget = off.dynamicTargetFrameRate;
        _lastDlssgFlags = off.flags;
        _lastDlssgGeneration = StreamlineHooks::dlssgOptionsGeneration();
        LOG_INFO(
            "DLSSG MFG count change: sent eOff to force a feature rebuild; next frame resumes eOn at the new count");
        return false;
    }

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = _framesToInterpolate;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    float dynamicTarget = 0.0f;
    if (Config::Instance()->FGDLSSGForceDMFG.value_or_default())
    {
        options.mode = sl::DLSSGMode::eDynamic;
        dynamicTarget = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
        options.dynamicTargetFrameRate = dynamicTarget;
    }

    // StreamlineProxy holds the raw export, so this push bypasses hkslDLSSGSetOptions and its
    // interlock. Apply it here too.
    StreamlineHooks::applyMenuDlssgInterlock(options, true);

    // The plugin latches these options (the count is only re-read on an eOff->eOn transition), so
    // push them through the proxy only when they actually change. For a non-Streamline game the
    // Reflex marker flag is always false, so after the first frame both option sets are static and
    // this skips two proxy -> NvAPI calls every frame.
    // flags is in the comparison because applyMenuDlssgInterlock sets it alongside the mode: today
    // the two always move together, so leaving it out would work by coincidence rather than by
    // construction, and the day a flag moves on its own it would go unsent with nothing to show.
    const unsigned int dlssgGeneration = StreamlineHooks::dlssgOptionsGeneration();

    if (options.mode != _lastDlssgMode || options.numFramesToGenerate != _lastDlssgFrames ||
        dynamicTarget != _lastDlssgDynamicTarget || options.flags != _lastDlssgFlags ||
        dlssgGeneration != _lastDlssgGeneration)
    {
        auto dlssgSetOptionsResult = StreamlineProxy::DLSSGSetOptions()(_viewport, options);

        if (dlssgSetOptionsResult != sl::Result::eOk)
        {
            LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(dlssgSetOptionsResult));
        }
        else
        {
            _lastDlssgMode = options.mode;
            _lastDlssgFrames = options.numFramesToGenerate;
            _lastDlssgDynamicTarget = dynamicTarget;
            _lastDlssgFlags = options.flags;
            _lastDlssgGeneration = dlssgGeneration;
        }
    }

    sl::ReflexOptions reflexConst = {};
    // Boost holds the GPU at high clocks while a frame is in flight -- the remaining input-latency
    // lever on top of frame generation, paid for in power and heat, and worth nothing in a scene
    // that is waiting on the CPU. Which side of that trade to take is the user's, so it is a
    // setting. Either way the mode is a low-latency one: eOff is what turns the image pink.
    reflexConst.mode = Config::Instance()->FGDLSSGReflexBoost.value_or_default() ? sl::ReflexMode::eLowLatencyWithBoost
                                                                                 : sl::ReflexMode::eLowLatency;
    reflexConst.useMarkersToOptimize = gameMarkersReachStreamline && ReflexHooks::gameIsSendingMarkers();

    const unsigned int reflexGeneration = StreamlineHooks::reflexOptionsGeneration();

    if (reflexConst.mode != _lastReflexMode || reflexConst.useMarkersToOptimize != _lastReflexMarkers ||
        reflexGeneration != _lastReflexGeneration)
    {
        ScopedOptiScalerReflex optiScalerCall {};
        auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

        if (reflexSetOptionsResult != sl::Result::eOk)
        {
            LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(reflexSetOptionsResult));
        }
        else
        {
            _lastReflexMode = reflexConst.mode;
            _lastReflexMarkers = reflexConst.useMarkersToOptimize;
            _lastReflexGeneration = reflexGeneration;
        }
    }

    return true;
}

bool DLSSG_Common::SetConstants(int fIndex, uint64_t willDispatchFrame, uint32_t mvWidth, uint32_t mvHeight)
{
    sl::Constants constData = {};

    // Streamline wants near < far, the upscaler input has them swapped for inverted depth like FFX wants
    float cameraNear = std::min(_cameraNear[fIndex], _cameraFar[fIndex]);
    float cameraFar = std::max(_cameraNear[fIndex], _cameraFar[fIndex]);

    if (IsInfiniteDepth())
        cameraFar = std::numeric_limits<float>::infinity();

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        constData.cameraPos.x = _cameraPosition[fIndex][0];
        constData.cameraPos.y = _cameraPosition[fIndex][1];
        constData.cameraPos.z = _cameraPosition[fIndex][2];

        constData.cameraUp.x = _cameraUp[fIndex][0];
        constData.cameraUp.y = _cameraUp[fIndex][1];
        constData.cameraUp.z = _cameraUp[fIndex][2];

        constData.cameraFwd.x = _cameraForward[fIndex][0];
        constData.cameraFwd.y = _cameraForward[fIndex][1];
        constData.cameraFwd.z = _cameraForward[fIndex][2];

        constData.cameraRight.x = _cameraRight[fIndex][0];
        constData.cameraRight.y = _cameraRight[fIndex][1];
        constData.cameraRight.z = _cameraRight[fIndex][2];
    }
    else
    {
        constData.cameraPos = { 0.0f, 0.0f, 0.0f };
        constData.cameraUp = { 0.0f, 0.0f, 1.0f };
        constData.cameraRight = { 0.0f, 1.0f, 0.0f };
        constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
        constData.cameraPinholeOffset = { 0.0f, 0.0f };

        auto prev = XMMatrixIdentity();

        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, prev);
        memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
    }

    {
        XMMATRIX cameraViewToClip {};
        XMMATRIX clipToCameraView {};

        float vFov = _cameraVFov[fIndex];

        // XMMatrixPerspectiveFovRH will fail if input values are incorrect
        if (cameraNear > 0.f && std::isfinite(cameraFar) && !XMScalarNearEqual(vFov, 0.0f, 0.00001f) &&
            !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
        {
            if (XMScalarNearEqual(cameraNear, cameraFar, 0.00001f))
                cameraFar++;

            // A reverse-Z projection for inverted depth, like the game's. DLSSG replacements like Nukem's read near
            // and far back out of it, a regular one loses the far plane to float precision.
            auto projectionNear = IsInvertedDepth() ? cameraFar : cameraNear;
            auto projectionFar = IsInvertedDepth() ? cameraNear : cameraFar;

            cameraViewToClip =
                XMMatrixPerspectiveFovRH(vFov, _cameraAspectRatio[fIndex], projectionNear, projectionFar);
            clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);
        }
        else
        {
            LOG_WARN("Can't calculate projectionMatrix");
        }

        // Convert to sl::float4x4 for Streamline
        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, cameraViewToClip);
        memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
        XMStoreFloat4x4(&temp, clipToCameraView);
        memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));
    }

    constData.cameraAspectRatio = _cameraAspectRatio[fIndex];
    constData.cameraFOV = _cameraVFov[fIndex];
    constData.cameraNear = cameraNear;
    constData.cameraFar = cameraFar;

    constData.jitterOffset.x = _jitterX[fIndex];
    constData.jitterOffset.y = _jitterY[fIndex];

    constData.mvecScale.x = _mvScaleX[fIndex] / (float) mvWidth;
    constData.mvecScale.y = _mvScaleY[fIndex] / (float) mvHeight;

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[fIndex] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    _gameCamera.Apply(willDispatchFrame, constData);

    auto frameId = static_cast<uint32_t>(willDispatchFrame);

    auto tokenResult = StreamlineProxy::GetNewFrameToken()(_frameToken, &frameId);
    if (tokenResult != sl::Result::eOk)
    {
        LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
        PauseAfterError();
        return false;
    }

    auto result = StreamlineProxy::SetConstants()(constData, *_frameToken, _viewport);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("SetConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);
        PauseAfterError();
        return false;
    }

    return true;
}
