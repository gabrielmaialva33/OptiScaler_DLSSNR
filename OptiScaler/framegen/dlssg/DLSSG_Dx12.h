#pragma once

#include <framegen/IFGFeature_Dx12.h>

#include <proxies/Streamline_Proxy.h>

class DLSSG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    uint32_t _width = 0;
    uint32_t _height = 0;
    std::optional<bool> _haveHudless = std::nullopt;

    // The DLSS-G feature latches numFramesToGenerate only on an eOff->eOn
    // transition ("DLSS-G interpolation state changed" / NGX ReportOverrideStates).
    // Changing the multiplier while FG stays eOn is ignored by the plugin, so 3x/4x
    // would silently keep generating at the original count. When the requested count
    // changes we set this so the next Dispatch sends one eOff frame (below) and the
    // frame after re-enters eOn with the new count, forcing the feature to rebuild.
    bool _mfgReinitPending = false;

    sl::ViewportHandle viewport { 0 };
    sl::FrameToken* frameToken = nullptr;

    ID3D12Fence* dlssgFence[BUFFER_COUNT] = {};
    UINT64 lastOptionFrame = 0;

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

    bool Dispatch();

  protected:
    void ReleaseObjects() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final { return "DLSSG"; };
    feature_version Version() override final;
    HWND Hwnd() override final;

    // IFGFeature_Dx12
    bool CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                         IDXGISwapChain** swapChain, bool readyToRelease) override final;
    bool CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd, DXGI_SWAP_CHAIN_DESC1* desc,
                          DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGISwapChain1** swapChain,
                          bool readyToRelease) override final;

    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    DLSSG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        if (StreamlineProxy::Module() == nullptr)
            StreamlineProxy::LoadStreamline();

        if (StreamlineProxy::Module() != nullptr && !StreamlineProxy::IsD3D12Inited() &&
            State::Instance().currentD3D12Device != nullptr)
        {
            StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device);
        }
    }

    ~DLSSG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
};
