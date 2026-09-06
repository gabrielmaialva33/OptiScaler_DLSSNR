#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include "OptiScaler/dlssnr/DlssNr_Exposure.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "OptiScaler/dlssnr/DlssNr_PreUpscale.h"
#include "OptiScaler/dlssnr/DlssNr_Coverage.h"
#include "OptiScaler/dlssnr/DlssNr_Chain.h"
#define FMT_HEADER_ONLY
#include "external/spdlog/include/spdlog/fmt/bundled/format.h"

// Strict host simulation of the boundary around the real ScopedPreUpscale implementation.
// Vulkan/D3D12/NGX execution is intentionally not simulated as a performance or GPU-safety claim.
inline std::vector<std::string> infoLogs;
#define LOG_INFO(...) infoLogs.push_back(fmt::format(__VA_ARGS__))
#define LOG_DEBUG(...) ((void)0)
#define FAILED(x) ((x) < 0)
#define IID_PPV_ARGS(x) (x)
using D3D12_RESOURCE_STATES = int;
using DXGI_FORMAT = int;
constexpr int NVSDK_NGX_Result_Success = 1;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 1;
constexpr int D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 2;
constexpr int D3D12_RESOURCE_STATE_RENDER_TARGET = 4;
constexpr int D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 8;
constexpr int D3D12_RESOURCE_STATE_COPY_SOURCE = 16;
constexpr int D3D12_RESOURCE_STATE_COPY_DEST = 32;
constexpr int D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 1;
constexpr int D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET = 2;
constexpr int D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE = 4;
constexpr int D3D12_RESOURCE_DIMENSION_TEXTURE2D = 2;
constexpr int D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384;
constexpr int D3D12_HEAP_TYPE_DEFAULT = 0, D3D12_HEAP_FLAG_NONE = 0, D3D12_TEXTURE_LAYOUT_UNKNOWN = 0;
constexpr int D3D12_FEATURE_FORMAT_SUPPORT = 0;
constexpr int D3D12_FORMAT_SUPPORT1_SHADER_LOAD = 1, D3D12_FORMAT_SUPPORT1_RENDER_TARGET = 2;
constexpr int D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE = 1;
constexpr int DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 10, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 11;
constexpr int DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 12;
constexpr int DXGI_FORMAT_R32_TYPELESS = 200;
constexpr int DXGI_FORMAT_R32_FLOAT = 201;
constexpr int DXGI_FORMAT_R32G32_TYPELESS = 202;
constexpr int DXGI_FORMAT_R32G32_FLOAT = 203;
constexpr int DXGI_FORMAT_R16G16_TYPELESS = 204;
constexpr int DXGI_FORMAT_R16G16_FLOAT = 205;
constexpr int DXGI_FORMAT_R32G32B32A32_TYPELESS = 206;
constexpr int DXGI_FORMAT_R32G32B32A32_FLOAT = 207;
constexpr int DXGI_FORMAT_R16G16B16A16_TYPELESS = 208;
constexpr int DXGI_FORMAT_R16G16B16A16_FLOAT = 209;
constexpr int DXGI_FORMAT_R16_FLOAT = 210;
constexpr int DXGI_FORMAT_R8_UNORM = 211;
constexpr int DXGI_FORMAT_R16_UNORM = 212;
constexpr int DXGI_FORMAT_R8G8_UNORM = 213;
constexpr int DXGI_FORMAT_R16G16_UNORM = 214;
constexpr int DXGI_FORMAT_R8G8B8A8_UNORM = 215;
constexpr int DXGI_FORMAT_R16G16B16A16_UNORM = 216;
constexpr auto NVSDK_NGX_Parameter_Color = "Color";
constexpr auto NVSDK_NGX_Parameter_Depth = "Depth";
constexpr auto NVSDK_NGX_Parameter_MotionVectors = "Motion";
constexpr auto NVSDK_NGX_Parameter_Output = "Output";
constexpr auto NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width = "Width";
constexpr auto NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height = "Height";
constexpr auto NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X = "X";
constexpr auto NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y = "Y";
struct D3D12_RESOURCE_DESC
{
    int Dimension = 2;
    uint64_t Width = 1280;
    unsigned Height = 720, DepthOrArraySize = 1, MipLevels = 1;
    DXGI_FORMAT Format = 1;
    struct { unsigned Count = 1; } SampleDesc;
    int Layout = 0, Flags = 0;
};
struct D3D12_HEAP_PROPERTIES { int Type; };
struct D3D12_FEATURE_DATA_FORMAT_SUPPORT { int Format; int Support1 = 0, Support2 = 0; };
struct ID3D12Device;
struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    ID3D12Device* device;
    int state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_DESC GetDesc() { return desc; }
    int GetDevice(ID3D12Device** out) { *out = device; return 0; }
};
struct ID3D12Device
{
    bool allocationFails = false, formatSupported = true;
    unsigned allocations = 0;
    std::vector<std::unique_ptr<ID3D12Resource>> owned;
    void Release() {}
    int CheckFeatureSupport(int, D3D12_FEATURE_DATA_FORMAT_SUPPORT* s, size_t)
    {
        s->Support1 = formatSupported ? 3 : 0;
        s->Support2 = formatSupported ? 1 : 0;
        return 0;
    }
    int CreateCommittedResource(D3D12_HEAP_PROPERTIES*, int, D3D12_RESOURCE_DESC* desc, int state,
                                void*, ID3D12Resource** out)
    {
        ++allocations;
        if (allocationFails) { *out = nullptr; return -1; }
        owned.push_back(std::make_unique<ID3D12Resource>(ID3D12Resource{*desc, this, state}));
        *out = owned.back().get();
        return 0;
    }
};
struct ID3D12GraphicsCommandList
{
    unsigned barriers = 0;
    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src)
    {
        assert(dst->state == 32 && src->state == 16);
        assert(dst->desc.Width == src->desc.Width && dst->desc.Height == src->desc.Height);
    }
};
struct ID3D12CommandQueue {};
void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, int from, int to)
{
    if (from == to) return;
    assert(r->state == from && "barrier did not match tracked resource state");
    if (to == D3D12_RESOURCE_STATE_RENDER_TARGET)
        assert(r->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    r->state = to;
    ++cmd->barriers;
}
bool IsTypeless(int format) { return format == 99; }
int TypedGuideFormat(int) { return 1; }
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    if (device->allocationFails) return nullptr;
    auto desc = source->desc; desc.Format = 1;
    device->owned.push_back(std::make_unique<ID3D12Resource>(ID3D12Resource{desc, device, 32}));
    return device->owned.back().get();
}

struct NVSDK_NGX_Parameter
{
    std::unordered_map<std::string, ID3D12Resource*> typed;
    std::unordered_map<std::string, void*> untyped;
    std::unordered_map<std::string, unsigned> numbers;
    unsigned sets = 0, gets = 0;
    int Get(const char* k, ID3D12Resource** out)
    { ++gets; if (!typed.count(k)) return 0; *out = typed[k]; return 1; }
    int Get(const char* k, void** out)
    { ++gets; if (!untyped.count(k)) return 0; *out = untyped[k]; return 1; }
    int Get(const char* k, unsigned* out)
    { ++gets; if (!numbers.count(k)) return 0; *out = numbers[k]; return 1; }
    void Set(const char* k, ID3D12Resource* r) { ++sets; typed[k] = r; }
    void Set(const char* k, void* r) { ++sets; untyped[k] = r; }
};
template<typename T> struct Option : std::optional<T>
{
    T value_or_default() const { return this->value_or(T{}); }
    Option& operator=(T v) { this->emplace(v); return *this; }
};
struct Config
{
    Option<float> DlssNrWorkingScale, DlssNrRRWorkingScale;
    Option<bool> DlssNrEnabled, DlssNrHoldFrame, DlssNrUseProxy, DlssNrApplyAfterRR;
    Option<unsigned> DlssNrStage, DlssNrRRPasses;
    Option<int> ColorResourceBarrier, DepthResourceBarrier, MVResourceBarrier, ExposureResourceBarrier;
    DlssNrPassSnapshot GetDlssNrPassSnapshot() const { return {}; }
    static Config* Instance() { static Config cfg; return &cfg; }
};
struct DlssNrFrameInfo { int OutputState = -1; void* ExposureTexture = nullptr; bool AfterRayReconstruction = false; };
struct DlssNr_Dx12
{
    static inline bool writes = true;
    static inline bool modelCalled = true;
    static inline int modelResult = 1;
    static inline unsigned calls = 0;
    DlssNr_Dx12(const char*, ID3D12Device*) {}
    bool Dispatch(ID3D12GraphicsCommandList*, ID3D12Resource* color, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue*, DlssNr::Detail::CoverageSample* coverage = nullptr, DlssNr::Chain::RecordingLease* = nullptr)
    {
        ++calls;
        if (coverage && modelCalled) coverage->ModelResult(modelResult, 1280, 720);
        if (color != output)
        {
            assert(color->state == 1 && depth->state == 1 && motion->state == 1 && output->state == 2);
            if (frame.ExposureTexture) assert(static_cast<ID3D12Resource*>(frame.ExposureTexture)->state == 1);
        }
        return writes;
    }
};
struct PreUpscaleState { ID3D12Resource* scratch = nullptr; unsigned width = 0, height = 0; int format = 0; } g_pre;
std::mutex g_preMutex, g_nrMutex;
DlssNr::Detail::StableExtent g_preExtent;
ID3D12Device* g_preDevice = nullptr;
bool g_preAllocationFailed = false;
bool trackingAccepted = true;
unsigned trackingCalls = 0;
std::atomic<const char*> g_chainStatus { "test submission tracking refused" };
DlssNr::Chain::RecordingGate g_recordings;
bool trackingOptIn = false;
bool WantsTrackedRecording(const DlssNrPassSnapshot&) { return trackingOptIn; }
bool TrackNrRecording(ID3D12GraphicsCommandList*, const DlssNrPassSnapshot&, DlssNr::Chain::RecordingLease*)
{
    ++trackingCalls;
    return trackingAccepted;
}
std::atomic<const char*> g_preStatus{ "" };
std::unique_ptr<DlssNr_Dx12> g_compose;
void ParkNrResource(ID3D12Resource*& r) { r = nullptr; } // Device fixture owns retired resources.
void ReportSkipOnce(const char*) {}
ID3D12Resource* GetResource(NVSDK_NGX_Parameter* p, const char* name, const char*)
{ ID3D12Resource* r = nullptr; p->Get(name, &r); return r; }
using ApiUpscalerInput = int;
const char* ApiUpscalerInputName(int) { return "test"; }
struct State
{
    int currentInputApiName = 0;
    uint64_t frameCount = 100;
    static State& Instance() { static State s; return s; }
};
namespace DlssNr
{
DlssNrFrameInfo GatherFrame(NVSDK_NGX_Parameter* p)
{
    DlssNrFrameInfo f;
    p->Get("Exposure", &f.ExposureTexture);
    return f;
}
}

namespace DlssNr::GpuTiming { inline void SetEnabled(bool) {} }
