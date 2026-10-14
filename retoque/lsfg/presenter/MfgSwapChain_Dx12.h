// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// Modified for Retoque's LSFG study (branch study/lsfg-d3d12, tools/lsfg-study/MFG_PRESENTER.md): copied from
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.h and widened to N
// generated frames per real frame (N = 1..3). The three ABI instantiations of TFrameInterpolationSwapChainDX12 became
// one concrete class; the FFX COM interfaces (IFrameInterpolationSwapChainDX12, IID_IFfxFrameInterpolationSwapChain),
// the ABI version private data, the registered interpolation command list and AntiLag 2 marking are gone, because
// nothing outside OptiScaler.dll drives this swapchain. The pacing record holds N generated frames and the real one.

#pragma once

#include "MfgSwapChain_Helpers.h"
#include "MfgSwapChain_UiComposition.h"
#include <lsfg/presenter/MfgPacing.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>

#include <ffx_api.h>
#include <ffx_framegeneration.h>

namespace LsfgMfg
{

constexpr uint32_t kMaxBufferCount = DXGI_MAX_SWAP_CHAIN_BUFFERS;

// FFX's FfxFrameGenerationConfig (framegeneration/internal/ffx_framegeneration_internal.h), the fields this swapchain
// reads. Filled by Lsfg::Presenter from the ffxConfigureDescFrameGeneration OptiScaler hands FFX's FG context, the way
// FFX's provider fills its own (ffx_provider_fsr3framegeneration.cpp:216-436).
struct FrameGenerationConfig
{
    void* swapChain = nullptr;
    FfxApiPresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackContext = nullptr;
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackContext = nullptr;
    bool frameGenerationEnabled = false;
    bool allowAsyncWorkloads = false;
    uint32_t flags = 0;
    bool onlyPresentInterpolated = false;
    FfxApiRect2D interpolationRect {};
    uint64_t frameID = 0;
    bool drawDebugPacingLines = false;
};

// One frame of an entry. FFX's TFrameInterpolationFrameInfo, plus whether it is generated (FFX knew that from the
// frame's slot in a two-slot array).
struct FrameInfo
{
    bool doPresent;
    bool generated;
    FfxApiResource resource;
    UINT64 interpolationCompletedFenceValue;
    UINT64 presentIndex;
    UINT64 presentQpcDelta;
};

// What the game's Present hands the interpolation thread, and that thread the presenter thread. FFX's
// TFrameInterpolationPacingData with frames[2] widened to the generated frames in display order, then the real one.
struct PacingData
{
    FfxApiPresentCallbackFunc presentCallback;
    void* presentCallbackContext;
    FfxApiResource uiSurface;

    bool vsync;
    bool tearingSupported;
    bool usePremulAlphaComposite;
    bool drawDebugPacingLines;

    UINT64 interpolationCompletedFenceValue;
    UINT64 replacementBufferFenceSignal;
    UINT64 numFramesSentForPresentationBase;
    UINT32 numFramesToPresent;
    UINT64 currentFrameID;

    uint32_t generatedFrames; // how many of frames[] are generated; frames[generatedFrames] is the real one
    uint32_t frameCount;      // generatedFrames + 1
    FrameInfo frames[kMaxFramesPerEntry];

    void invalidate() { memset(this, 0, sizeof(PacingData)); }
};

// FFX's FrameinterpolationPresentInfoExt: what the two threads share.
struct PresentInfo
{
    CRITICAL_SECTION criticalSectionScheduledFrame;
    ID3D12Device* device = nullptr;
    IDXGISwapChain4* swapChain = nullptr;
    Dx12CommandPool<64> commandPool;

    PacingData scheduledInterpolations;
    PacingData scheduledPresents;

    FfxApiResource currentUiSurface;
    uint32_t uiCompositionFlags = 0;

    ID3D12CommandQueue* interpolationQueue = nullptr;
    ID3D12CommandQueue* asyncComputeQueue = nullptr;
    ID3D12CommandQueue* gameQueue = nullptr;
    ID3D12CommandQueue* presentQueue = nullptr;

    ID3D12Fence* gameFence = nullptr;
    ID3D12Fence* interpolationFence = nullptr;
    ID3D12Fence* presentFence = nullptr;
    ID3D12Fence* replacementBufferFence = nullptr;
    ID3D12Fence* compositionFenceCPU = nullptr;
    ID3D12Fence* compositionFenceGPU = nullptr;

    HANDLE presentEvent = 0;
    HANDLE interpolationEvent = 0;
    HANDLE pacerEvent = 0;

    volatile bool resetTimer = false;
    volatile bool shutdown = false;

    volatile double safetyMarginInSec = 0.0001; // 0.1ms
    volatile double varianceFactor = 0.1;
    volatile bool allowHybridSpin = false;
    volatile uint32_t hybridSpinTime = 2; // Measured in system timer resolution units. Default is 2. Below 1 will
                                          // frequently result in overshoot. Overshoots stop showing up >=2.
    volatile bool allowWaitForSingleObjectOnFence = false;

    FfxWaitCallbackFunc waitCallback = nullptr;

    volatile int64_t previousPresentQpc = 0;

    HANDLE waitableObjectSemaphoreHandle = 0; // will be passed to app as waitableObject
    HANDLE frameDoneEventHandle = 0;          // event to notify the semaphore
    HANDLE waitableThreadHandle = 0;          // thread to update semaphore when event gets triggered by GPU
    volatile bool shutdownWaitableSignalThread = false;

    // Owned by the swapchain, used by the presenter thread; released after it has exited.
    UiCompositionGpu uiComposition;
    DebugPacingGpu debugPacing;

    // The presenter thread's 10 s summary (MfgPacing.h PresentWindow).
    PresentWindow window;
    int64_t windowStartQpc = 0;
    int64_t lastStatsPresentQpc = 0;
};

struct ReplacementResource
{
    ID3D12Resource* resource = nullptr;
    UINT64 availabilityFenceValue = 0;

    void destroy()
    {
        SafeRelease(resource);
        availabilityFenceValue = 0;
    }
};

// One present's generated frames: up to kMaxGeneratedFrames outputs and when the set may be written again.
struct OutputSet
{
    ID3D12Resource* resource[kMaxGeneratedFrames] = {};
    UINT64 availabilityFenceValue = 0;
};

// {6E0F3A27-9C51-4B8E-A3D4-1F7C2B5E8D90}: answered by QueryInterface so Lsfg::Presenter can tell its own swapchains
// apart; no FFX interface is answered.
class DECLSPEC_UUID("6E0F3A27-9C51-4B8E-A3D4-1F7C2B5E8D90") MfgSwapChainDX12 : public IDXGISwapChain4
{
  public:
    MfgSwapChainDX12();
    virtual ~MfgSwapChainDX12();

    HRESULT init(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* swapChainDesc1,
                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* queue,
                 IDXGIFactory2* dxgiFactory);

    // Lsfg::Presenter's side (FFX's swapchain configure entry points, without the ABI dance).
    void setFrameGenerationConfig(const FrameGenerationConfig* config);
    void registerUiResource(FfxApiResource uiResource, uint32_t flags);
    void setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc);
    void setFramePacingTuning(const FfxApiSwapchainFramePacingTuning* framePacingTuning);
    // Generated frames asked for per real frame from the next present on: 1..kMaxGeneratedFrames.
    void setGeneratedFrames(uint32_t generatedFrames);
    bool waitForPresents();

    // IUnknown
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();

    // IDXGIObject
    virtual HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData);
    virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown);
    virtual HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData);
    virtual HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent);

    // IDXGIDeviceSubObject
    virtual HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice);

    // IDXGISwapChain
    virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface);
    virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget);
    virtual HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                    UINT SwapChainFlags);
    virtual HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters);
    virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput);
    virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats);
    virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount);

    // IDXGISwapChain1
    virtual HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc);
    virtual HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd);
    virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk);
    virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters);
    virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported(void);
    virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput);
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor);
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor);
    virtual HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation);
    virtual HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation);

    // IDXGISwapChain2
    virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height);
    virtual HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight);
    virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency);
    virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency);
    virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject(void);
    virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix);
    virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix);

    // IDXGISwapChain3
    virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex(void);
    virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                             UINT* pColorSpaceSupport);
    virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace);
    virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                     UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                     IUnknown* const* ppPresentQueue);

    // IDXGISwapChain4
    virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData);

  private:
    HRESULT shutdown();
    UINT getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags);
    DXGI_SWAP_CHAIN_DESC1 getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc);

    void applyFrameGenerationConfig(const FrameGenerationConfig* config, bool applyChangesNow);
    void presentWithUiComposition(UINT SyncInterval, UINT Flags);
    uint32_t dispatchInterpolationCommands(FfxApiResource* pInterpolatedFrames, FfxApiResource* pRealFrame);
    void presentInterpolated(UINT SyncInterval, UINT Flags);

    bool verifyUiDuplicateResource();
    void copyUiResource();

    bool verifyBackbufferDuplicateResources();
    bool verifyInterpolationOutputs(uint32_t set, uint32_t count);
    bool destroyReplacementResources();
    bool killPresenterThread();
    bool spawnPresenterThread();

    IDXGISwapChain4* real();

    PresentInfo presentInfo = {};
    FrameGenerationConfig nextFrameGenerationConfig = {};

    CRITICAL_SECTION criticalSection {};
    CRITICAL_SECTION criticalSectionUpdateConfig {};

    UINT gameBufferCount = 0;
    UINT gameFlags = 0;
    DXGI_SWAP_EFFECT gameSwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT gameMaximumFrameLatency = 1;          // default is 1
    ID3D12Resource* realBackBuffer0 = nullptr; // hold reference to avoid DXGI deadlock on Windows 10

    HANDLE replacementFrameLatencyWaitableObjectHandle = 0;

    UINT64 interpolationFenceValue = 0;
    UINT64 gameFenceValue = 0;
    bool frameInterpolationResetCondition = false;
    FfxApiRect2D interpolationRect {};

    ReplacementResource replacementSwapBuffers[kMaxBufferCount] = {};
    OutputSet outputSets[kOutputSets] = {};
    ReplacementResource uiReplacementBuffer = {};
    int replacementSwapBufferIndex = 0;
    uint32_t outputSetIndex = 0;
    UINT64 presentCount = 0;

    uint32_t configFlags = 0;

    bool tearingSupported = false;
    bool interpolationEnabled = false;
    bool presentInterpolatedOnly = false;
    bool previousFrameWasInterpolated = false;
    bool drawDebugPacingLines = false;
    std::atomic<uint32_t> generatedFramesWanted { 1 };

    UINT64 currentFrameID = 0;

    UINT64 framesSentForPresentation = 0;
    UINT64 nextPresentWaitValue = 0;
    HANDLE interpolationThreadHandle = 0;
    ULONG refCount = 1;

    UINT backBufferTransferFunction = 0;
    float minLuminance = 0.0f;
    float maxLuminance = 0.0f;

    FfxApiPresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackContext = nullptr;
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackContext = nullptr;

    UINT64 totalUsageInBytes = 0;
};

} // namespace LsfgMfg
