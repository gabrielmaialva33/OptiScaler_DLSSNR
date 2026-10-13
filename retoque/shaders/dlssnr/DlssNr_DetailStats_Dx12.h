#pragma once

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <dlssnr/DlssNr_DetailStats.h>

#include <memory>
#include <string>
#include <vector>

#define DLSSNR_DETAIL_STATS_SLOTS 16

class DlssNr_DetailStats_Dx12 : public Shader_Dx12
{
  public:
    struct alignas(256) Params
    {
        uint32_t mode = 0;
        float measureWhitePoint = 1.0f;
        uint32_t width = 0;
        uint32_t height = 0;
        float shoulderThreshold = 1.0f;
        float floorThreshold = 0.05f;
        uint32_t pad[58] {};
    };

    static bool Available();

    DlssNr_DetailStats_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_DetailStats_Dx12();

    bool Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* output, ID3D12Resource* prevOutput,
                ID3D12Resource* input, ID3D12Resource* prevInput, ID3D12Resource* proxy, uint32_t width,
                uint32_t height, float whitePoint, uint32_t slot);

    bool Readback(uint32_t slot, DlssNr::DetailStats::Stats& outStats);

    ID3D12Resource* GetGrid() const { return _grid; }

  private:
    static constexpr uint32_t kSrvCount = 5;
    static constexpr uint32_t kUavCount = 1;
    static constexpr uint32_t kGridWidth = 256;
    static constexpr uint32_t kGridHeight = 64;

    ID3D12Resource* _grid = nullptr;
    ID3D12Resource* _constantBuffers[DLSSNR_DETAIL_STATS_SLOTS] = {};
    void* _mappedConstants[DLSSNR_DETAIL_STATS_SLOTS] = {};
    ID3D12Resource* _readbackBuffers[DLSSNR_DETAIL_STATS_SLOTS] = {};
    void* _mappedReadback[DLSSNR_DETAIL_STATS_SLOTS] = {};
    FrameDescriptorHeap _frameHeaps[DLSSNR_DETAIL_STATS_SLOTS];
};
