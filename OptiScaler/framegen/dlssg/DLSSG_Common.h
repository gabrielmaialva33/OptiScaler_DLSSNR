#pragma once

#include <framegen/IFGFeature.h>

#include <proxies/Streamline_Proxy.h>
#include <framegen/dlssg/GameCamera.h>

// The Streamline side of the DLSSG output, shared by the D3D12 and Vulkan ones
class DLSSG_Common : public virtual IFGFeature
{
  protected:
    // The DLSS-G feature latches numFramesToGenerate only on an eOff->eOn
    // transition ("DLSS-G interpolation state changed" / NGX ReportOverrideStates).
    // Changing the multiplier while FG stays eOn is ignored by the plugin, so 3x/4x
    // would silently keep generating at the original count. When the requested count
    // changes we set this so Dispatch retries eOff until accepted (below), then the
    // frame after re-enters eOn with the new count, forcing the feature to rebuild.
    bool _mfgReinitPending = false;

    DlssgGameCamera<BUFFER_COUNT> _gameCamera;

    // The DLSSG and Reflex options are latched by the SL plugin, so re-sending identical
    // options every frame costs two proxy -> NvAPI calls for nothing. Track the last-sent
    // values and call the proxy only when one of them actually changes.
    sl::DLSSGMode _lastDlssgMode = sl::DLSSGMode::eOff;
    uint32_t _lastDlssgFrames = 0;
    float _lastDlssgDynamicTarget = 0.0f;
    sl::DLSSGFlags _lastDlssgFlags = {};
    sl::ReflexMode _lastReflexMode = sl::ReflexMode::eOff;
    bool _lastReflexMarkers = false;

    // What the game had pushed the last time this cache was believed. The interlock and the menu
    // reach the plugin through OptiScaler, but a Streamline game also sets its own options behind
    // us; when it does, the plugin no longer holds what we last sent and the cache has to be
    // treated as cold, or our values quietly stop being applied.
    unsigned int _lastDlssgGeneration = 0;
    unsigned int _lastReflexGeneration = 0;

    sl::ViewportHandle _viewport { 0 };
    sl::FrameToken* _frameToken = nullptr;

    // Max interpolation count and DMFG support, once the swapchain exists
    void QueryState();

    // DLSSG and Reflex options for the next present, Reflex optimizes with the game's markers when it has them
    // False while disabling MFG for a count change, including failed eOff retries.
    bool SetOptions(bool gameMarkersReachStreamline);

    // Fills _frameToken and sends the frame's constants, pauses FG when Streamline refuses them
    bool SetConstants(int fIndex, uint64_t willDispatchFrame, uint32_t mvWidth, uint32_t mvHeight);

    void PauseAfterError();

  public:
    void BeginGameCameraFrame(uint64_t frame) { _gameCamera.BeginFrame(frame); }
    void SetGameCamera(uint64_t frame, const sl::Constants& data, bool allow) { _gameCamera.Store(frame, data, allow); }

    // IFGFeature
    const char* Name() override final { return "DLSSG"; }
    feature_version Version() override final;
    void Activate() override final;
    void Deactivate() override final;
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override final { return true; }
};
