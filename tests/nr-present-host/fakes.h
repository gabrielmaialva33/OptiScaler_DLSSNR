// Host fakes for the production present host (dlssnr/DlssNr_PresentHost.cpp).
//
// The host decides what to build, when, and on which list the model is created; the pass itself
// (EvaluateAtPresent) is not what is under test here. So the pass is a fake that keeps the one rule
// the host depends on -- no model below kDlssNrMinExtent -- and records where every creation
// happened: on an empty list of the host's own, or on the frame's list behind other work.
#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
    DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
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
    DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020 = 18,
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
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
    D3D12_RESOURCE_BARRIER_TYPE_UAV = 2,
};
enum D3D12_COMMAND_LIST_TYPE
{
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0
};
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xffffffff;

enum D3D12_RESOURCE_FLAGS
{
    D3D12_RESOURCE_FLAG_NONE = 0,
};
enum D3D12_HEAP_TYPE
{
    D3D12_HEAP_TYPE_DEFAULT = 1,
};
enum D3D12_HEAP_FLAGS
{
    D3D12_HEAP_FLAG_NONE = 0,
};
struct D3D12_HEAP_PROPERTIES
{
    D3D12_HEAP_TYPE Type {};
};
enum D3D12_FEATURE
{
    D3D12_FEATURE_FORMAT_SUPPORT = 2,
};
enum D3D12_FORMAT_SUPPORT1
{
    D3D12_FORMAT_SUPPORT1_SHADER_LOAD = 0x100,
    D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW = 0x40000000,
};
enum D3D12_FORMAT_SUPPORT2
{
    D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE = 0x80,
};
struct D3D12_FEATURE_DATA_FORMAT_SUPPORT
{
    DXGI_FORMAT Format {};
    UINT Support1 {};
    UINT Support2 {};
};

enum D3D12_TEXTURE_LAYOUT
{
    D3D12_TEXTURE_LAYOUT_UNKNOWN = 0,
};
struct D3D12_RESOURCE_DESC
{
    UINT64 Width {};
    UINT Height {};
    DXGI_FORMAT Format {};
    D3D12_RESOURCE_FLAGS Flags {};
    UINT64 Alignment {};
    D3D12_TEXTURE_LAYOUT Layout {};
};

// The D3D12 rules the host's formats are held to. An sRGB format takes no unordered-access view, so no buffer that
// carries one may be made in it; CopyResource moves bytes only within one typeless group.
inline bool IsSrgb(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
inline int TypelessGroup(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 1;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 2;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 3;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 5;
    default:
        return 100 + (int) format;
    }
}

struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc {};
    // Where the resource is, as the barriers recorded so far left it. Every transition must start from here.
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    int refs = 1;
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
    void SetName(const wchar_t*) {}
    void Release()
    {
        assert(refs > 0 && "resource released twice");
        --refs;
    }
};

// What the frame's list carried, in order: copies ('c', destination then source) and transfer dispatches ('t', input
// then output). A case reads it to see what the conversion in read, and what was copied before it.
struct ListEvent
{
    char kind;
    ID3D12Resource* first;
    ID3D12Resource* second;
};
inline std::vector<ListEvent> g_listEvents;

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
    struct
    {
        ID3D12Resource* pResource {};
    } UAV;
};

struct ID3D12CommandAllocator
{
    int refs = 1;
    std::wstring name; // what the host named it, for a DRED report (DlssNr_Dred.h)
    void Release() { --refs; }
    void SetName(const wchar_t* value) { name = value; }
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
    std::wstring name; // what the host named it, for a DRED report (DlssNr_Dred.h)
    void SetName(const wchar_t* value) { name = value; }
    HRESULT closeResult = S_OK;
    unsigned uavBarriers = 0;
    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        assert(!closed && "barrier on a closed list");
        for (UINT i = 0; i < count; ++i)
        {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
            {
                assert(barriers[i].UAV.pResource != nullptr && barriers[i].UAV.pResource->state ==
                                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ++uavBarriers;
                continue;
            }
            auto* resource = barriers[i].Transition.pResource;
            assert(barriers[i].Transition.StateBefore != barriers[i].Transition.StateAfter);
            assert(resource != nullptr && resource->state == barriers[i].Transition.StateBefore &&
                   "a transition from a state the resource is not in");
            resource->state = barriers[i].Transition.StateAfter;
        }
        recorded += count;
    }
    void CopyResource(ID3D12Resource* destination, ID3D12Resource* source)
    {
        assert(!closed && "copy on a closed list");
        assert(destination != nullptr && source != nullptr && destination != source);
        assert(destination->state == D3D12_RESOURCE_STATE_COPY_DEST && "a copy into a resource not in COPY_DEST");
        assert(source->state == D3D12_RESOURCE_STATE_COPY_SOURCE && "a copy from a resource not in COPY_SOURCE");
        assert(TypelessGroup(destination->desc.Format) == TypelessGroup(source->desc.Format) &&
               "CopyResource across typeless groups");
        assert(destination->desc.Width == source->desc.Width && destination->desc.Height == source->desc.Height);
        g_listEvents.push_back({ 'c', destination, source });
        ++recorded;
    }
    HRESULT Close()
    {
        closed = true;
        return closeResult;
    }
    void Release() { --refs; }
};

// The device's format answers. Every typed format the swapchain can carry is loadable; a typed store is the hardware's
// answer for the UNORM, ten-bit and FP16 formats, never for sRGB or B8G8R8X8, and a case can take it away from more.
inline std::vector<DXGI_FORMAT> g_formatsWithoutStore;
inline unsigned g_formatQueries = 0;
inline unsigned g_committedMade = 0;

struct ID3D12Device
{
    // Every allocator and list the host creates is owned here, so a case can count them and nothing leaks.
    std::vector<std::unique_ptr<ID3D12CommandAllocator>> allocators;
    std::vector<std::unique_ptr<ID3D12GraphicsCommandList>> lists;
    std::vector<std::unique_ptr<ID3D12Resource>> resources;

    HRESULT CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT size)
    {
        assert(feature == D3D12_FEATURE_FORMAT_SUPPORT && size == sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT));
        auto* support = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT*>(data);
        ++g_formatQueries;
        const auto format = support->Format;
        const bool typeless = format == DXGI_FORMAT_R10G10B10A2_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                              format == DXGI_FORMAT_UNKNOWN;
        const bool store = !typeless && !IsSrgb(format) && format != DXGI_FORMAT_B8G8R8X8_UNORM &&
                           std::find(g_formatsWithoutStore.begin(), g_formatsWithoutStore.end(), format) ==
                               g_formatsWithoutStore.end();
        support->Support1 = (typeless ? 0u : (UINT) D3D12_FORMAT_SUPPORT1_SHADER_LOAD) |
                            (store ? (UINT) D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW : 0u);
        support->Support2 = store ? (UINT) D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE : 0u;
        return S_OK;
    }

    HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC* desc,
                                    D3D12_RESOURCE_STATES state, const void*, ID3D12Resource** out)
    {
        assert(heap != nullptr && heap->Type == D3D12_HEAP_TYPE_DEFAULT && desc != nullptr && out != nullptr);
        resources.push_back(std::make_unique<ID3D12Resource>());
        auto* resource = resources.back().get();
        resource->desc = *desc;
        resource->state = state;
        ++g_committedMade;
        *out = resource;
        return S_OK;
    }
    unsigned LiveResources() const
    {
        unsigned live = 0;
        for (const auto& resource : resources)
            live += resource->refs > 0 ? 1 : 0;
        return live;
    }

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
    bool CreateBufferResource(ID3D12Device*, ID3D12Resource* source, D3D12_RESOURCE_STATES state)
    {
        assert(_buffer == nullptr);
        // Production's buffer carries ALLOW_UNORDERED_ACCESS and gets an unordered-access view in its own format.
        assert(!IsSrgb(_format) && "a transfer buffer in an sRGB format, which takes no unordered-access view");
        _buffer = new ID3D12Resource();
        _buffer->desc = source->GetDesc();
        _buffer->desc.Format = _format;
        _buffer->state = state;
        ++g_transferBuffersLive;
        ++g_transferBuffersMade;
        return true;
    }
    bool CanRender() const { return _buffer != nullptr; }
    ID3D12Resource* Buffer() { return _buffer; }
    bool Dispatch(ID3D12GraphicsCommandList* list, ID3D12Resource* input, ID3D12Resource* output)
    {
        assert(!list->closed && input != nullptr && output != nullptr);
        assert(input->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE && "the transfer reads through an SRV");
        assert(output->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && "the transfer writes through a UAV");
        assert(!IsSrgb(output->desc.Format) && "an unordered-access view of an sRGB format");
        g_listEvents.push_back({ 't', input, output });
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
    FakeFlag DlssNrHdrPresent { false };
    FakeFlag DlssNrUseProxy { false };
    struct DlssNrGpuTimingSettings
    {
        bool Enabled = false;
        uint32_t Interval = 30;
    };
    DlssNrGpuTimingSettings GetDlssNrGpuTimingSettings() const { return {}; }
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
    bool BridgeHost = false;
    bool ExtentIsStable = false;
    bool AllowSupersampling = true;
    int OutputState = 0;
    float PresentWhite = 0.0f;
    float PresentQuantRel = 0.0f;
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
inline DlssNrFrameInfo g_lastPassFrame {}; // the whole description, on a frame it ran
inline ID3D12Resource* g_lastPassColour = nullptr;

inline bool CapturedPresentGuides(ID3D12GraphicsCommandList*, ID3D12Resource**, ID3D12Resource**, DlssNrFrameInfo*,
                                  ID3D12CommandQueue* = nullptr, const void* = nullptr, unsigned int = 0,
                                  unsigned int = 0, bool* gameGuidesHold = nullptr)
{
    if (gameGuidesHold != nullptr)
        *gameGuidesHold = false;
    return false;
}
inline void AbandonCapturedGuides() {}
// What a frame without the game's guides ran on: the last answer, for the cases that care.
inline int g_fallbackNotes = 0;
inline bool g_lastFallbackSynthesized = false;
inline void NotePresentGuideFallback(bool synthesized)
{
    ++g_fallbackNotes;
    g_lastFallbackSynthesized = synthesized;
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
    assert(colour->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && "the pass is told its colour arrives as a UAV");
    ++list->recorded;
    ++g_passesRun;
    g_lastPassReset = frame.Reset;
    g_lastPassFrame = frame;
    g_lastPassColour = colour;
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
