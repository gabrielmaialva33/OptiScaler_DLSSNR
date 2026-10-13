#pragma once

// The pulse metric's GPU half ([DlssNr] PulseMetric, docs/dlssnr/design/pulse-metric.md): one small reduction of the
// pass's output and the game's frame under it into a 16x16 grid of luminance sums, copied to a readback ring. The
// CPU half, which turns a grid into the edit's light and pairs frames into steps, is dlssnr/DlssNr_Pulse.h.
//
// Built only while the metric is on, and parked with the pass's other retired objects when it goes off: the last
// recordings still reference its buffers. Nothing in it depends on the frame's size, so a resize does not rebuild it.
//
// Its bytecode may be absent: DlssNr_Pulse_Dx12.cpp includes the precompiled header only if it exists, and without it
// Available() is false and the menu does not offer the metric.

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <dlssnr/DlssNr_Pulse.h>

// Readbacks in flight. A slot is reused only once its last sample was collected; at sixteen, a frame is skipped
// (and its pair broken) only if the GPU runs sixteen frames behind the CPU.
#define DLSSNR_PULSE_SLOTS 16

class DlssNr_Pulse_Dx12 : public Shader_Dx12
{
  public:
    // The Params cbuffer of dlssnr_pulse.hlsl, padded to the 256 bytes every constant-buffer view must have.
    struct alignas(256) Params
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
    };

    static bool Available();

    DlssNr_Pulse_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_Pulse_Dx12();

    bool BuiltFor(ID3D12Device* device) const { return device == _device; }

    // Records the reduction and its copy into the slot's readback. Both inputs arrive readable
    // (NON_PIXEL_SHADER_RESOURCE) and are left so; they share the frame's size.
    bool Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* output, ID3D12Resource* input, uint32_t width,
                uint32_t height, uint32_t slot);

    // The edit's light from the slot's readback, once the recording that filled it has completed. NaN if there is
    // nothing to compare.
    double Light(uint32_t slot) const;

  private:
    static constexpr uint32_t kSrvCount = 2;
    static constexpr uint32_t kUavCount = 1;
    static constexpr uint32_t kGrid = DlssNr::Pulse::kGrid;

    ID3D12Resource* _grid = nullptr; // RGBA32F kGrid x kGrid, rests in UNORDERED_ACCESS
    ID3D12Resource* _constantBuffers[DLSSNR_PULSE_SLOTS] = {};
    void* _mappedConstants[DLSSNR_PULSE_SLOTS] = {};
    ID3D12Resource* _readbackBuffers[DLSSNR_PULSE_SLOTS] = {};
    void* _mappedReadback[DLSSNR_PULSE_SLOTS] = {};
    FrameDescriptorHeap _frameHeaps[DLSSNR_PULSE_SLOTS];
};
