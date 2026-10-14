#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): the LSFG 3.1 pass graph on a D3D12 device, recorded on
// a command list someone else owns (FSR-FG's interpolation list, retoque/lsfg/Lsfg.cpp).
//
// The harness (tools/lsfg-study/harness/main.cpp) is the reference: per-pass immutable, full-length descriptor
// tables with typed null views for the elements a dispatch does not use, explicit transitions to
// NON_PIXEL_SHADER_RESOURCE / UNORDERED_ACCESS around every dispatch and a UAV barrier after every write.
// Two departures, both from tools/lsfg-study/OPTI_INTEGRATION.md section 4.2: the root signatures come from
// the DXIL's PSV0 part instead of dxcompiler.dll's reflection (LsfgDxil.h), and both cbuffers are root
// constants, so no CPU-written GPU memory exists and nothing needs a fence on a queue we cannot signal.
//
// Objects are built on a worker thread (Build) and only recorded on the present thread (Record). Nothing here
// frees anything the GPU may still use: Lsfg.cpp parks a retired context and destroys it only once FFX has
// provably drained, or 32 presents later.

#include <lsfg/LsfgGraph.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Lsfg
{
class IShaderSource;

// Root signatures and pipelines for one device and one variant (quality/performance, FP32/FP16).
class ProgramSet_Dx12
{
  public:
    enum class ParamKind : uint8_t
    {
        Table,       // one descriptor table: an SRV or UAV array in its own register space
        FrameValues, // `cb` as root constants
        PushValues,  // the push constants as root constants
    };

    struct Param
    {
        ParamKind kind = ParamKind::Table;
        bool uav = false;
        uint32_t space = 0;
        uint32_t count = 0;
        bool array = false; // Texture2DArray rather than Texture2D
    };

    struct Program
    {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
        std::vector<Param> params; // root parameter i
    };

    // Loads, checks and compiles the 27 shaders the graph uses. Worker thread. Null with a reason on failure.
    static std::shared_ptr<ProgramSet_Dx12> Build(ID3D12Device* device, bool performance, bool fp16,
                                                  IShaderSource& source, std::string& error);

    const Program* Find(uint32_t rid) const;
    ID3D12Device* Device() const { return _device.Get(); }
    bool Performance() const { return _performance; }
    bool Fp16() const { return _fp16; }

  private:
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    bool _performance = false;
    bool _fp16 = false;
    std::map<uint32_t, Program> _programs;
};

// What a context is built for. A context serves exactly this and is rebuilt for anything else.
struct ContextKey
{
    ID3D12Device* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // the working (UNORM) format, LsfgGraph.h WorkingFormat
    float flow = 1.0f;
    bool performance = false;
    bool fp16 = false;

    bool operator==(const ContextKey& o) const
    {
        return device == o.device && width == o.width && height == o.height && format == o.format && flow == o.flow &&
               performance == o.performance && fp16 == o.fp16;
    }
    bool operator!=(const ContextKey& o) const { return !(*this == o); }
};

// The frame FFX hands over: its copy of the backbuffer and the outputs to fill, with the states they are in
// and must be left in.
struct FrameIo
{
    ID3D12Resource* presentColor = nullptr;
    D3D12_RESOURCE_STATES presentState = D3D12_RESOURCE_STATE_COMMON;
    std::array<ID3D12Resource*, 4> outputs {};
    std::array<D3D12_RESOURCE_STATES, 4> outputStates {};
    uint32_t generate = 0;        // generated frames to record now (0: ingest and pre-pass only)
    uint32_t generatedFrames = 0; // how many FFX asked for, which sets the timestamps k / (n + 1)
};

class Context_Dx12
{
  public:
    // Worker thread. Null with a reason on failure.
    static std::unique_ptr<Context_Dx12> Build(std::shared_ptr<ProgramSet_Dx12> programs, const ContextKey& key,
                                               std::string& error);

    ~Context_Dx12();

    // Present thread, on the callback's list (DIRECT, or COMPUTE with FFX's async workloads). Every resource
    // of ours is left where the tracked state says; presentColor and outputs[] are restored to their states.
    void Record(ID3D12GraphicsCommandList* list, const FrameIo& io, const FrameConstants& frame);

    const ContextKey& Key() const { return _key; }
    History& GetHistory() { return _history; }
    bool EverRecorded() const { return _recorded; }
    uint32_t DescriptorCount() const { return _descriptorCount; }
    uint32_t ImageCount() const { return uint32_t(_textures.size()); }
    bool HasTimestamps() const { return _readbackData != nullptr; }
    uint64_t VramBytes() const { return _vramBytes; }

    // GPU time of the most recent recorded frames whose timestamps are old enough to have landed, in ms;
    // appended to `out` and consumed. Timestamps are read without a fence, a few frames late, and implausible
    // values (a slot the GPU had not written yet) are dropped.
    void CollectGpuTimes(std::vector<double>& out);

  private:
    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        GraphImage shape;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool clear = false;     // zeroed on the first Record (the feature history must start at zero)
        uint32_t clearSlot = 0; // index among the cleared textures
    };

    struct Pass
    {
        const ProgramSet_Dx12::Program* program = nullptr;
        const GraphPass* graph = nullptr;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> tables; // per root parameter (unused for root constants)
    };

    Context_Dx12() = default;
    bool _Create(std::string& error);
    void _Transition(std::vector<D3D12_RESOURCE_BARRIER>& barriers, Texture& t, D3D12_RESOURCE_STATES next);
    void _Execute(ID3D12GraphicsCommandList* list, std::vector<Pass>& passes, const FrameConstants& frame);

    std::shared_ptr<ProgramSet_Dx12> _programs;
    ContextKey _key;
    Graph _graph;
    std::vector<Texture> _textures;
    std::vector<Pass> _pre;
    std::vector<Pass> _main;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _heap;      // shader visible: the pass tables, then the clears
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _clearHeap; // CPU only: the clears' second handle
    uint32_t _descriptorSize = 0;
    uint32_t _descriptorCount = 0;
    uint32_t _tableDescriptors = 0; // the pass tables; the clears follow them
    uint64_t _vramBytes = 0;
    History _history;
    bool _recorded = false;
    std::vector<D3D12_RESOURCE_BARRIER> _barriers;

    // GPU timing: two timestamps per recorded frame, kTimingSlots frames deep, resolved into a persistently
    // mapped readback buffer and read kTimingLag frames later.
    static constexpr uint32_t kTimingSlots = 8;
    static constexpr uint32_t kTimingLag = 4;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> _readback;
    const uint64_t* _readbackData = nullptr;
    uint64_t _timestampFrequency = 0;
    uint64_t _timedFrames = 0;     // frames that recorded timestamps
    uint64_t _collectedFrames = 0; // the oldest not yet read
    uint64_t _lastCollectedBegin = 0;
};

} // namespace Lsfg
