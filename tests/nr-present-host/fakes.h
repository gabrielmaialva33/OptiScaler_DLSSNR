// Host fakes for the production present host (dlssnr/DlssNr_PresentHost.cpp).
//
// The host decides what to build, when, and on which list the model is created; the pass itself
// (EvaluateAtPresent) is not what is under test here. So the pass is a fake that keeps the one rule
// the host depends on -- no model below kDlssNrMinExtent -- and records where every creation
// happened: on an empty list of the host's own, or on the frame's list behind other work.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using UINT = unsigned;
using UINT64 = unsigned long long;
using HRESULT = int;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr bool FAILED(HRESULT r) { return r < 0; }
#define IID_PPV_ARGS(x) x

// Every log call is kept by its format string, so a case can ask whether "built" was said.
inline std::vector<std::string> g_log;
template <typename... Args> void LogSink(const char* format, Args&&...) { g_log.emplace_back(format); }
#define LOG_ERROR(...) LogSink(__VA_ARGS__)
#define LOG_WARN(...) LogSink(__VA_ARGS__)
#define LOG_INFO(...) LogSink(__VA_ARGS__)

inline size_t LogCount(const char* needle)
{
    size_t n = 0;
    for (const auto& line : g_log)
        n += line.find(needle) != std::string::npos ? 1 : 0;
    return n;
}

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
};
// dxgicommon.h's values, for the ones DlssNr_PresentColour.h names.
enum DXGI_COLOR_SPACE_TYPE
{
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 = 0,
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 = 1,
    DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 = 2,
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 12,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 = 13,
    DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 = 14,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020 = 16,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 = 17,
    DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 = 19,
    DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 = 20,
};
enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_COMMON = 0,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 8,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 64,
    D3D12_RESOURCE_STATE_COPY_DEST = 0x400,
    D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800,
};
enum D3D12_RESOURCE_BARRIER_TYPE
{
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0
};
enum D3D12_COMMAND_LIST_TYPE
{
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0
};
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xffffffff;

struct D3D12_RESOURCE_DESC
{
    UINT64 Width {};
    UINT Height {};
    DXGI_FORMAT Format {};
};

struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc {};
    int refs = 1;
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
    void Release()
    {
        assert(refs > 0 && "resource released twice");
        --refs;
    }
};

struct D3D12_RESOURCE_BARRIER
{
    D3D12_RESOURCE_BARRIER_TYPE Type {};
    UINT Flags {};
    struct
    {
        ID3D12Resource* pResource {};
        UINT Subresource {};
        D3D12_RESOURCE_STATES StateBefore {}, StateAfter {};
    } Transition;
};

struct ID3D12CommandAllocator
{
    int refs = 1;
    void Release() { --refs; }
};

struct ID3D12CommandList
{
    // What has been recorded so far. "Empty" is what the host promises NGX when it creates the model.
    unsigned recorded = 0;
    bool closed = false;
    bool executed = false;
};

struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    int refs = 1;
    HRESULT closeResult = S_OK;
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        assert(!closed && "barrier on a closed list");
        for (UINT i = 0; i < count; ++i)
            assert(barriers[i].Transition.StateBefore != barriers[i].Transition.StateAfter);
        recorded += count;
    }
    HRESULT Close()
    {
        closed = true;
        return closeResult;
    }
    void Release() { --refs; }
};

struct ID3D12Device
{
    // Every allocator and list the host creates is owned here, so a case can count them and nothing leaks.
    std::vector<std::unique_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<std::unique_ptr<ID3D12GraphicsCommandList>> lists;

    HRESULT CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator** out)
    {
        allocators.push_back(std::make_unique<ID3D12CommandAllocator>());
        *out = allocators.back().get();
        return S_OK;
    }
    HRESULT CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator* allocator, void*,
                              ID3D12GraphicsCommandList** out)
    {
        assert(allocator != nullptr);
        lists.push_back(std::make_unique<ID3D12GraphicsCommandList>());
        *out = lists.back().get();
        return S_OK;
    }
};

struct ID3D12CommandQueue
{
    unsigned executions = 0;
    void ExecuteCommandLists(UINT count, ID3D12CommandList* const* lists)
    {
        for (UINT i = 0; i < count; ++i)
        {
            assert(lists[i]->closed && "executed a list that was never closed");
            lists[i]->executed = true;
        }
        executions += count;
    }
};

// --- the format transfer the host converts in and out with -------------------------------------

inline unsigned g_transferBuffersLive = 0;
inline unsigned g_transferBuffersMade = 0;

class FT_Dx12
{
    ID3D12Resource* _buffer = nullptr;
    DXGI_FORMAT _format;

  public:
    FT_Dx12(std::string, ID3D12Device*, DXGI_FORMAT format) : _format(format) {}
    ~FT_Dx12()
    {
        if (_buffer != nullptr)
        {
            _buffer->Release();
            delete _buffer;
            --g_transferBuffersLive;
        }
    }
    bool CreateBufferResource(ID3D12Device*, ID3D12Resource* source, D3D12_RESOURCE_STATES)
    {
        assert(_buffer == nullptr);
        _buffer = new ID3D12Resource();
        _buffer->desc = source->GetDesc();
        _buffer->desc.Format = _format;
        ++g_transferBuffersLive;
        ++g_transferBuffersMade;
        return true;
    }
    bool CanRender() const { return _buffer != nullptr; }
    ID3D12Resource* Buffer() { return _buffer; }
    bool Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource*, ID3D12Resource*)
    {
        ++list->recorded;
        return true;
    }
};

// --- config -------------------------------------------------------------------------------------

struct FakeFlag
{
    bool value = false;
    bool value_or_default() const { return value; }
};

class Config
{
  public:
    FakeFlag DlssNrEnabled { true };
    FakeFlag DlssNrZeroGuideReset { false };
    FakeFlag DlssNrSynthMotion { false };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

// --- what the pass shares with its present hosts ------------------------------------------------

constexpr unsigned int kDlssNrMinExtent = 64;

struct DlssNrFrameInfo
{
    bool Reset = false;
    bool ResetIsPolicy = false;
    bool ColourIsLinearHdr = true;
    void* ExposureTexture = nullptr;
    float PreExposure = 1.0f;
    bool PresentSource = false;
    bool ExtentIsStable = false;
    bool AllowSupersampling = true;
    int OutputState = 0;
};

namespace DlssNr
{

// Every reset the host asked of the estimator, one entry per Record.
inline std::vector<bool> g_motionResets;

// Synthesized motion (DlssNrFeature_Dx12.h). Off by default in these cases: the host must build nothing
// for it and hand the pass exactly what it did before.
class SynthMotionGuide
{
  public:
    ID3D12Resource* Record(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES,
                           bool reset, DlssNrFrameInfo&, const char*, ID3D12CommandQueue* = nullptr)
    {
        ++records;
        g_motionResets.push_back(reset);
        return nullptr;
    }
    void AfterPass(ID3D12GraphicsCommandList*) {}
    void ConfirmExecuted() {}
    void AbandonRecording() {}
    void Release() { ++releases; }
    bool LastFrameSynthesized() const { return false; }
    int records = 0;
    int releases = 0;
};

// The zero guides. Their own contract is tested by nr-zero-guides; here they only have to be there,
// at the size they were asked for.
class ZeroGuides
{
    ID3D12Resource _depth {}, _motion {};
    bool _allocated = false;

  public:
    unsigned ensures = 0;
    bool Ensure(ID3D12Device*, uint32_t width, uint32_t height)
    {
        ++ensures;
        _depth.desc = { width, height, DXGI_FORMAT_UNKNOWN };
        _motion.desc = { width, height, DXGI_FORMAT_UNKNOWN };
        _allocated = true;
        return true;
    }
    bool RecordClear(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES)
    {
        ++list->recorded;
        return true;
    }
    void ConfirmExecuted() {}
    void AbandonRecording() {}
    void Release() { _allocated = false; }
    ID3D12Resource* Depth() { return _allocated ? &_depth : nullptr; }
    ID3D12Resource* Motion() { return _allocated ? &_motion : nullptr; }
};

// Every model creation the pass performed: at what size, and whether the list it went on was empty.
struct Creation
{
    uint32_t width, height;
    bool onEmptyList;
};
inline std::vector<Creation> g_creations;
inline uint32_t g_modelWidth = 0, g_modelHeight = 0;
inline unsigned g_passesRun = 0;
inline bool g_refuseModel = false;   // NGX refusing the feature: the pass declines every frame
inline bool g_lastPassReset = false; // the Reset the pass was last handed, on a frame it ran

inline DlssNrFrameInfo PresentFrameDefaults(bool reset)
{
    DlssNrFrameInfo frame {};
    frame.PresentSource = true;
    frame.ColourIsLinearHdr = false;
    frame.ExtentIsStable = true;
    frame.OutputState = (int) D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.Reset = reset;
    return frame;
}

inline bool CapturedPresentGuides(ID3D12GraphicsCommandList*, ID3D12Resource**, ID3D12Resource**, DlssNrFrameInfo*,
                                  ID3D12CommandQueue* = nullptr)
{
    return false;
}
inline int GuideRestState(bool) { return (int) D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; }

// The pass, reduced to the decisions the host cares about: it declines when off, declines below the
// minimum size (the rule is production's, kDlssNrMinExtent), and otherwise (re)creates the model when
// the colour's size differs from the one it has, then runs.
inline bool EvaluateAtPresent(ID3D12GraphicsCommandList* list, ID3D12Resource* colour, ID3D12Resource* depth,
                              ID3D12Resource* motion, const DlssNrFrameInfo& frame, ID3D12CommandQueue* = nullptr,
                              const char** outReason = nullptr)
{
    static const char* disabled = "the pass is disabled";
    static const char* tiny = "the frame or working size is below 64 pixels; the model is not built that small";
    static const char* refused = "the model was refused";
    static const char* ran = "";

    assert(list != nullptr && colour != nullptr && depth != nullptr && motion != nullptr);
    const auto desc = colour->GetDesc();

    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        *outReason = disabled;
        return false;
    }
    if (desc.Width < kDlssNrMinExtent || desc.Height < kDlssNrMinExtent)
    {
        *outReason = tiny;
        return false;
    }
    if (g_refuseModel)
    {
        *outReason = refused;
        return false;
    }
    if (g_modelWidth != desc.Width || g_modelHeight != desc.Height)
    {
        g_creations.push_back({ (uint32_t) desc.Width, desc.Height, list->recorded == 0 });
        g_modelWidth = (uint32_t) desc.Width;
        g_modelHeight = desc.Height;
    }
    ++list->recorded;
    ++g_passesRun;
    g_lastPassReset = frame.Reset;
    *outReason = ran;
    return true;
}

namespace Identity
{
using Sink = void (*)(void* context, const char* line);
inline void ReportNgxModules(Sink, void*) {}
inline void ReportAll(Sink, void*, const char*, ID3D12Device*, ID3D12CommandQueue*) {}
} // namespace Identity

} // namespace DlssNr
