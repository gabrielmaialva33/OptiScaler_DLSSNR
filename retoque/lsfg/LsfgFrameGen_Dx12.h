#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): the LSFG 3.1 pass graph on a D3D12 device, recorded on
// a command list someone else owns (FSR-FG's interpolation list, retoque/lsfg/Lsfg.cpp).
//
// The harness (tools/lsfg-study/harness/main.cpp) is the reference for the bindings: per-pass immutable,
// full-length descriptor tables with typed null views for the elements a dispatch does not use. Departures:
// - the root signatures come from the DXIL's PSV0 part instead of dxcompiler.dll's reflection (LsfgDxil.h), and
//   both cbuffers are root constants, so no CPU-written GPU memory exists and nothing needs a fence on a queue we
//   cannot signal (tools/lsfg-study/OPTI_INTEGRATION.md section 4.2);
// - the harness puts a barrier around every dispatch; here the passes run in waves (LsfgGraph.h ScheduleWaves)
//   with one batched barrier in front of each, holding only the transitions and UAV barriers the wave needs;
// - generate writes FFX's output itself when D3D12 allows it (DirectOutputFormat), instead of LSFG's own output
//   and a copy. The ingest copy stays: the shaders sample the frame as one slice of a two-slice array, which
//   FFX's single backbuffer cannot be bound as.
// Schedule::PerPass keeps the harness's ordering and the copy-out, for the study's A/B timing (Lsfg.cpp).
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

#ifdef LSFG_WAVE_TIMING
// Study instrumentation, compiled only into tools/lsfg-study/ctxbench (never defined for OptiScaler.dll): when
// waveQueries is set, Record writes a timestamp after the ingest copy, after every wave and at its end, at
// waveQueryNext onwards.
namespace Study
{
inline ID3D12QueryHeap* waveQueries = nullptr;
inline uint32_t waveQueryNext = 0;
} // namespace Study
#endif

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

    // Where generated frame k is recorded, k >= 1: lists[k] when set, else the list of frame k - 1. Our MFG presenter
    // hands one list per generated frame and executes each with a fence signal of its own (Lsfg.h
    // DispatchDescFrameLists); everything shared by the frames (the clears, ingest, the pre-pass) and frame 0 stay on
    // the list Record is given. Unset, as with FFX's swapchain, everything is recorded on that one list as before.
    std::array<ID3D12GraphicsCommandList*, 4> lists {};
};

// How the last generated frame reached FFX's output.
enum class OutputRoute : uint8_t
{
    None,   // nothing generated yet
    Direct, // generate wrote it through a UAV of its own
    Copy,   // generate wrote LSFG's output, copied into FFX's (OutputRouteReason says why)
};

enum class Schedule : uint8_t
{
    Waves,   // one barrier batch per wave, direct output where allowed
    PerPass, // the harness's: transitions before and UAV barriers after every dispatch, always the copy-out
};

class Context_Dx12
{
  public:
    // Worker thread. Null with a reason on failure.
    static std::unique_ptr<Context_Dx12> Build(std::shared_ptr<ProgramSet_Dx12> programs, const ContextKey& key,
                                               std::string& error);

    ~Context_Dx12();

    // Present thread, on the callback's list (DIRECT, or COMPUTE with FFX's async workloads), and on io.lists for the
    // generated frames that have one. Every resource of ours is left where the tracked state says, which is where the
    // next list on the same queue finds it; presentColor is restored on the first list, and outputs[k] on the list that
    // wrote it, before the next list starts.
    void Record(ID3D12GraphicsCommandList* list, const FrameIo& io, const FrameConstants& frame,
                Schedule schedule = Schedule::Waves);

    // How many lists the last Record recorded into (1 unless io.lists switched it to others).
    uint32_t LastListsUsed() const { return _listsUsed; }

    // Multi-frame generation contract:
    // Records generated frame `index` (0-based: 0 .. count - 1) of `count` total generated frames
    // for the current base frame pair into `output`.
    // - index == 0: ingests presentColor, executes pre-pass (Stages 0..9) + main-pass (Stages 10..45) at
    //   timestamp 1/(count+1), writing to output.
    // - index > 0: executes ONLY the main-pass (Stages 10..45) at timestamp (index+1)/(count+1), reusing the
    //   pinned pre-pass feature images (alphaArray, betaImageArray) from index 0.
    // Output resource is restored to `outputState`.
    bool RecordGenerated(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor,
                         D3D12_RESOURCE_STATES presentState, uint32_t index, uint32_t count, ID3D12Resource* output,
                         D3D12_RESOURCE_STATES outputState, const FrameConstants& frame,
                         Schedule schedule = Schedule::Waves);

    const ContextKey& Key() const { return _key; }
    History& GetHistory() { return _history; }
    bool EverRecorded() const { return _recorded; }
    uint32_t DescriptorCount() const { return _descriptorCount; }
    uint32_t ImageCount() const { return uint32_t(_textures.size()); }
    bool HasTimestamps() const { return _readbackData != nullptr; }
    uint64_t VramBytes() const { return _vramBytes; }
    uint32_t WaveCount(bool generates) const { return uint32_t((generates ? _wavesPreMain : _wavesPre).size()); }
    OutputRoute LastOutputRoute() const { return _route; }
    const char* OutputRouteReason() const { return _routeReason; }

    // GPU time of the most recent recorded frames whose timestamps are old enough to have landed, in ms;
    // appended to `out` (frames recorded with Schedule::PerPass to `perPass` when given) and consumed.
    // Timestamps are read without a fence, a few frames late, and implausible values (a slot the GPU had not
    // written yet) are dropped.
    void CollectGpuTimes(std::vector<double>& out, std::vector<double>* perPass = nullptr);

  private:
    struct Texture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        GraphImage shape;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool clear = false;     // zeroed on the first Record (the feature history must start at zero)
        uint32_t clearSlot = 0; // index among the cleared textures
        bool dirty = false;     // written as a UAV with no barrier on it since (a later write needs one)
    };

    struct Pass
    {
        const ProgramSet_Dx12::Program* program = nullptr;
        const GraphPass* graph = nullptr;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> tables; // per root parameter (unused for root constants)
        int outputParam = -1;                            // the root parameter of the output UAV (generate only)
    };

    // FFX's output, when generate writes it itself.
    struct DirectTarget
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // FFX's, restored afterwards
        D3D12_GPU_DESCRIPTOR_HANDLE table {};
    };

    Context_Dx12() = default;
    bool _Create(std::string& error);
    void _Transition(std::vector<D3D12_RESOURCE_BARRIER>& barriers, Texture& t, D3D12_RESOURCE_STATES next);
    void _UavBarrier(std::vector<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource);
    void _Flush(ID3D12GraphicsCommandList* list);
    void _Dispatch(ID3D12GraphicsCommandList* list, const Pass& p, const uint32_t* frameWords,
                   D3D12_GPU_DESCRIPTOR_HANDLE outputTable);
    void _ExecutePerPass(ID3D12GraphicsCommandList* list, std::vector<Pass>& passes, const FrameConstants& frame);
    void _ExecuteWaves(ID3D12GraphicsCommandList* list, const Waves& waves, const FrameConstants& pre,
                       const FrameConstants& main, DirectTarget* target);
    bool _BindDirect(ID3D12Resource* target, D3D12_RESOURCE_STATES state, DirectTarget& out);
    void _CopyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState);

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
    std::vector<D3D12_RESOURCE_BARRIER> _barriers; // pending, issued as one batch by _Flush
    ID3D12PipelineState* _boundPso = nullptr;      // last set on the list during this Record
    uint32_t _listsUsed = 1;                       // by the last Record

    Waves _wavesPre;     // a base frame that generates nothing
    Waves _wavesPreMain; // a base frame and its first generated frame, as one sequence
    Waves _wavesMain;    // every further generated frame of the same base frame

    // The direct output's UAV, written per generated frame into a ring at the end of the heap: FFX hands two
    // outputs that alternate, and each may be recreated at the same address, so nothing is cached by pointer. A
    // slot is rewritten kOutputRing generated frames later, when the list that read it has long run: SDK 2.3.0's
    // swapchain even waits for the previous interpolation list before it calls back again
    // (FrameInterpolationSwapchainDX12::Present, interpolationEvent), though the signed DLL is not that source.
    static constexpr uint32_t kOutputRing = 8;
    uint32_t _ringBase = 0;
    uint64_t _ringNext = 0;
    OutputRoute _route = OutputRoute::None;
    const char* _routeReason = "";

    // GPU timing: two timestamps per recorded frame, kTimingSlots frames deep, resolved into a persistently
    // mapped readback buffer and read kTimingLag frames later.
    static constexpr uint32_t kTimingSlots = 8;
    static constexpr uint32_t kTimingLag = 4;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> _queries;
    Microsoft::WRL::ComPtr<ID3D12Resource> _readback;
    const uint64_t* _readbackData = nullptr;
    uint64_t _timestampFrequency = 0;
    uint64_t _timedFrames = 0;                      // frames that recorded timestamps
    std::array<bool, kTimingSlots> _slotPerPass {}; // the schedule each slot's frame was recorded with
    uint64_t _collectedFrames = 0;                  // the oldest not yet read
    uint64_t _lastCollectedBegin = 0;

    void _ClearHistory(ID3D12GraphicsCommandList* list);
    void _Ingest(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor, D3D12_RESOURCE_STATES presentState);
};

} // namespace Lsfg
