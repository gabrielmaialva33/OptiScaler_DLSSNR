#pragma once

// Model cadence for Neural Rendering: the model runs on one frame in N, and this pass carries the edit it made
// onto the frames in between -- along the game's motion, with the depth and colour tests that stop it landing
// on the wrong surface. docs/dlssnr/design/model-cadence.md is the reasoning; dlssnr/DlssNr_Cadence.h decides which
// frames carry.
//
// One object serves one working size on one device. Its surfaces are sized at construction; a different size
// gets a new object, and the old one is parked with the pass's other retired objects rather than freed,
// because the recordings that used it may still be on the GPU.
//
// Its bytecode may be absent: DlssNr_Cadence_Dx12.cpp includes the precompiled header only if it exists, and
// without it Available() is false, nothing is built, and the model runs every frame.
//
// With smoothing ([DlssNr] CadenceSmoothing, docs/dlssnr/design/cadence-smoothing.md) it also keeps the fade, the shown
// answer and the light limiter's grid and state; whether it has them is part of what it was built for, so a toggle
// builds a new object (and the scheduler, which folds the switch into the settings it watches, drops the stored edit
// with it).

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Up to seven dispatches a frame (a smoothed model frame: model motion, fade, record, coarse, light, limit, apply),
// reused round-robin with no fence: sixteen frames of coverage, as the stabilizer keeps for one.
#define DLSSNR_CADENCE_NUM_OF_HEAPS 112

class DlssNr_Cadence_Dx12 : public Shader_Dx12
{
  public:
    // Where in a guide texture this frame's guide lies; it is mapped point-wise onto the working size, the
    // guide resample's rule.
    struct Region
    {
        uint32_t baseX = 0;
        uint32_t baseY = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // What every dispatch of one frame reads. Every resource arrives readable (NON_PIXEL_SHADER_RESOURCE) and is
    // left so.
    struct Frame
    {
        ID3D12Resource* proxy = nullptr; // the model's input this frame, at the working size
        ID3D12Resource* depth = nullptr;
        Region depthRegion {};
        ID3D12Resource* motion = nullptr;
        Region motionRegion {};
        float mvToWorkX = 1.0f; // the game's vectors into working pixels
        float mvToWorkY = 1.0f;
        float scale = 1.0f; // DlssNr::Cadence::TapScale
    };

    // What smoothing does on one frame (DlssNr::Cadence::Decision). Inert when the object has no smoothing surfaces.
    struct Smooth
    {
        float fadeWeight = 0.0f;  // the share of the previous answer still shown
        bool limit = false;       // run the light limiter
        bool limitReset = false;  // ... with nothing before this frame to hold it to
        float limitStep = 0.0f;   // DlssNr::Cadence::LimitStep
        bool passthrough = false; // the frame is already tone mapped: its values are its light
    };

  private:
    // The Params cbuffer of dlssnr_cadence.hlsl, in order.
    struct alignas(256) Constants
    {
        uint32_t Mode = 0;
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t LowWidth = 0;
        uint32_t LowHeight = 0;
        uint32_t LowBlock = 0;
        uint32_t ChainStart = 0;
        uint32_t HaveUi = 0;
        uint32_t DepthBaseX = 0;
        uint32_t DepthBaseY = 0;
        uint32_t DepthWidth = 0;
        uint32_t DepthHeight = 0;
        uint32_t MotionBaseX = 0;
        uint32_t MotionBaseY = 0;
        uint32_t MotionWidth = 0;
        uint32_t MotionHeight = 0;
        float MvToWorkX = 1.0f;
        float MvToWorkY = 1.0f;
        float DepthTol = 0.0f;
        float ColourTol = 0.0f;
        float Scale = 1.0f;
        float DepthStepX = 1.0f;
        float DepthStepY = 1.0f;
        float FadeWeight = 0.0f;
        uint32_t Passthrough = 0;
        uint32_t LimitValid = 0;
        float LimitStep = 0.0f;
        float LimitInputTol = 0.0f;
        float LimitMaxGain = 0.0f;
    };

    static constexpr uint32_t kSrvCount = 11;
    static constexpr uint32_t kUavCount = 3;

    FrameDescriptorHeap _frameHeaps[DLSSNR_CADENCE_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_CADENCE_NUM_OF_HEAPS] = {};
    void* _mappedConstants[DLSSNR_CADENCE_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;

    // All rest in NON_PIXEL_SHADER_RESOURCE between dispatches.
    ID3D12Resource* _residual = nullptr;    // RGBA16F: the model's final answer - its proxy
    ID3D12Resource* _proxyThen = nullptr;   // RGBA16F: the proxy the model saw, for the colour test
    ID3D12Resource* _depthThen = nullptr;   // R32F: its depth at the working size, for the depth test
    ID3D12Resource* _residualLow = nullptr; // RGBA16F, 1/16 per axis: the fill
    ID3D12Resource* _chain[2] = {};         // R32G32F: displacement back to the model frame, working pixels
    int _current = 0;                       // which chain holds the last carried frame's

    // Smoothing only. The light state rests in UNORDERED_ACCESS: four R32F texels, read and written as a UAV.
    // _smoothingAsked is what the object was built for (Fits); _smoothing is whether its surfaces exist. A failed
    // allocation leaves the carry running without smoothing rather than refusing the cadence.
    bool _smoothingAsked = false;
    bool _smoothing = false;
    ID3D12Resource* _fade = nullptr;       // RGBA16F: the model frame's carried answer minus its own
    ID3D12Resource* _shown = nullptr;      // RGBA16F: what this frame shows, before the light limit
    ID3D12Resource* _lightGrid = nullptr;  // R32G32F, kLightGrid square: summed light of the shown answer, the proxy
    ID3D12Resource* _lightState = nullptr; // R32F 4x1: shown light, frame light, valid, gain (stops)

    UINT _width = 0;
    UINT _height = 0;
    UINT _lowWidth = 0;
    UINT _lowHeight = 0;

    ID3D12Resource* CreateTexture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                                  const wchar_t* name,
                                  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // One dispatch. srvs/uavs are slot-indexed; a null entry gets a null descriptor of the slot's type.
    bool Run(ID3D12GraphicsCommandList* cmdList, const Constants& constants, ID3D12Resource* const (&srvs)[kSrvCount],
             ID3D12Resource* const (&uavs)[kUavCount], UINT groupsX, UINT groupsY);

    Constants Base(uint32_t mode, const Frame& frame) const;

    // The light limiter over _shown, then _shown times its gain into InOut (UNORDERED_ACCESS, left so). Interface
    // pixels (InUiAlpha, may be null) are left out of the light and keep their answer as it is.
    bool Show(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InUiAlpha,
              const Smooth& InSmooth, ID3D12Resource* InOut);

  public:
    // Whether this build carries the pass's bytecode at all.
    static bool Available();

    // Whether that bytecode runs smoothing's modes: false for the binary committed before them, which writes nothing
    // in modes 5 to 8 and would leave every smoothed carried frame unwritten.
    static bool SmoothingAvailable();

    DlssNr_Cadence_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight, bool InSmoothing);
    ~DlssNr_Cadence_Dx12();

    // Whether this object was built for this working size, with or without smoothing, on this device. A failed object
    // still fits, so it is not rebuilt, and logged again, every frame.
    bool Fits(ID3D12Device* InDevice, UINT InWidth, UINT InHeight, bool InSmoothing) const
    {
        return InDevice == _device && InWidth == _width && InHeight == _height && InSmoothing == _smoothingAsked;
    }

    bool Smoothing() const { return _smoothing; }

    // A model frame: keep the model's final answer minus its proxy, the proxy, the depth and the coarse edit.
    // InAnswer is the answer at the working size, readable.
    bool Record(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InAnswer);

    // A carried frame: one link of the chain, then the proxy plus the moved edit into InOut -- the surface the
    // model writes, in its format, which arrives in UNORDERED_ACCESS and is left there. InUiAlpha may be null. With
    // smoothing, the walk's share of the stored fade is added, and under the limiter the answer goes through Show.
    bool Carry(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, bool InChainStart, ID3D12Resource* InUiAlpha,
               const Smooth& InSmooth, ID3D12Resource* InOut);

    // The model frame of a smoothed cadence, after ModelMotion and the model, before Record overwrites the edit it
    // reads: the carried answer minus the model's is stored, and the model's answer with InSmooth.fadeWeight of the
    // way back to the carried one is kept as the shown answer. InAnswer is readable.
    bool Fade(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InUiAlpha,
              ID3D12Resource* InAnswer, const Smooth& InSmooth);

    // After Fade and Record: the shown answer, held to the light limit, into InOut -- the surface the resolve reads,
    // at the working size, in UNORDERED_ACCESS, left so.
    bool ShowFaded(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InUiAlpha,
                   const Smooth& InSmooth, ID3D12Resource* InOut);

    // The model frame after carried ones: the chain summed back to the last model frame, as the model's motion
    // vectors (working pixels, scale 1, off the picture where broken). Readable, valid until the next call;
    // nullptr if this object could not be built.
    ID3D12Resource* ModelMotion(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame);
};
