#pragma once

// Model cadence for Neural Rendering: the model runs on one frame in N, and this pass carries the edit it made
// onto the frames in between -- along the game's motion, with the depth and colour tests that stop it landing
// on the wrong surface. dlssnr/design/model-cadence.md is the reasoning; dlssnr/DlssNr_Cadence.h decides which
// frames carry.
//
// One object serves one working size on one device. Its surfaces are sized at construction; a different size
// gets a new object, and the old one is parked with the pass's other retired objects rather than freed,
// because the recordings that used it may still be on the GPU.
//
// Its bytecode may be absent: DlssNr_Cadence_Dx12.cpp includes the precompiled header only if it exists, and
// without it Available() is false, nothing is built, and the model runs every frame.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Up to three dispatches a frame (model motion, record, coarse), reused round-robin with no fence: sixteen
// frames of coverage, as the stabilizer keeps for one.
#define DLSSNR_CADENCE_NUM_OF_HEAPS 48

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
        uint32_t Pad0 = 0;
    };

    static constexpr uint32_t kSrvCount = 10;
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

    UINT _width = 0;
    UINT _height = 0;
    UINT _lowWidth = 0;
    UINT _lowHeight = 0;

    ID3D12Resource* CreateTexture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                                  const wchar_t* name);

    // One dispatch. srvs/uavs are slot-indexed; a null entry gets a null descriptor of the slot's type.
    bool Run(ID3D12GraphicsCommandList* cmdList, const Constants& constants, ID3D12Resource* const (&srvs)[kSrvCount],
             ID3D12Resource* const (&uavs)[kUavCount], UINT groupsX, UINT groupsY);

    Constants Base(uint32_t mode, const Frame& frame) const;

  public:
    // Whether this build carries the pass's bytecode at all.
    static bool Available();

    DlssNr_Cadence_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight);
    ~DlssNr_Cadence_Dx12();

    // Whether this object was built for this working size on this device. A failed object still fits, so it is
    // not rebuilt, and logged again, every frame.
    bool Fits(ID3D12Device* InDevice, UINT InWidth, UINT InHeight) const
    {
        return InDevice == _device && InWidth == _width && InHeight == _height;
    }

    // A model frame: keep the model's final answer minus its proxy, the proxy, the depth and the coarse edit.
    // InAnswer is the answer at the working size, readable.
    bool Record(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InAnswer);

    // A carried frame: one link of the chain, then the proxy plus the moved edit into InOut -- the surface the
    // model writes, in its format, which arrives in UNORDERED_ACCESS and is left there. InUiAlpha may be null.
    bool Carry(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, bool InChainStart, ID3D12Resource* InUiAlpha,
               ID3D12Resource* InOut);

    // The model frame after carried ones: the chain summed back to the last model frame, as the model's motion
    // vectors (working pixels, scale 1, off the picture where broken). Readable, valid until the next call;
    // nullptr if this object could not be built.
    ID3D12Resource* ModelMotion(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame);
};
