// What DlssNr_Dx12.cpp's Transfer 2 section stands on, faked: resource states that every barrier must
// chain through, scratch allocation, the composition pass's DispatchPass, the guide resample, submission
// tracking, the NGX proxy's getters and the process-exit flag. run.py puts the production slices after this.
#pragma once

#include "ngx_fakes.h"

#include <dlssnr/DlssNr_Chain.h>
#include <dlssnr/DlssNr_Enlarge.h>
#include <shaders/dlssnr/DlssNr_Common.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <vector>

// The log lines are checked as formats: std::format refuses a pattern that does not fit its arguments at
// compile time, which is the mistake a fmt call in production would only make at run time.
std::vector<std::string> g_log;
#define LOG_INFO(...) g_log.push_back(std::format(__VA_ARGS__))
#define LOG_ERROR(...) g_log.push_back("ERROR " + std::format(__VA_ARGS__))

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R32_FLOAT = 41,
};

enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_COMMON = 0,
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
};

// Every resource's state, as the barriers recorded so far leave it. A barrier from a state the resource is not
// in is the device removal native D3D12 answers with, so it fails here.
std::map<const ID3D12Resource*, D3D12_RESOURCE_STATES> g_states;
unsigned g_barriers = 0;

D3D12_RESOURCE_STATES StateOf(const ID3D12Resource* r)
{
    const auto found = g_states.find(r);
    assert(found != g_states.end());
    return found->second;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    assert(cmd != nullptr && res != nullptr);
    if (from == to)
        return;
    if (StateOf(res) != from)
    {
        std::fprintf(stderr, "barrier on %s from 0x%X, but it is in 0x%X\n", res->name.c_str(), (unsigned) from,
                     (unsigned) StateOf(res));
        assert(false);
    }
    g_states[res] = to;
    ++g_barriers;
}

// Scratch textures, created in UNORDERED_ACCESS as the production CreateScratch creates them.
std::vector<std::unique_ptr<ID3D12Resource>> g_scratch;
int g_failScratchAt = -1;

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height)
{
    assert(device != nullptr && width != 0 && height != 0);
    if (static_cast<int>(g_scratch.size()) == g_failScratchAt)
    {
        g_failScratchAt = -1;
        return nullptr;
    }
    auto r = std::make_unique<ID3D12Resource>();
    r->format = format;
    r->width = width;
    r->height = height;
    r->name = std::format("scratch{}({}x{} fmt {})", g_scratch.size(), width, height, (int) format);
    g_states[r.get()] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    g_scratch.push_back(std::move(r));
    return g_scratch.back().get();
}

struct State
{
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};

// The driver core's getters: null while the core is not initialised, as NVNGXProxy's are.
struct NVNGXProxy
{
    inline static bool inited = true;
    inline static uint64_t shutdowns = 0;
    static bool IsDx12Inited() { return inited; }
    static uint64_t Dx12Shutdowns() { return shutdowns; }
    static DlssNr::PrivateSr::AllocateParameters D3D12_AllocateParameters() { return &Ngx::Allocate; }
    static DlssNr::PrivateSr::DestroyParameters D3D12_DestroyParameters() { return inited ? &Ngx::Destroy : nullptr; }
    static DlssNr::PrivateSr::CreateFeature D3D12_CreateFeature() { return inited ? &Ngx::Create : nullptr; }
    static DlssNr::PrivateSr::EvaluateFeature D3D12_EvaluateFeature() { return inited ? &Ngx::Evaluate : nullptr; }
    static DlssNr::PrivateSr::ReleaseFeature D3D12_ReleaseFeature() { return inited ? &Ngx::Release : nullptr; }
};

namespace DlssNr::Submission
{
struct Usage
{
    bool tracked = false;
};
inline bool trackOk = true, ready = true, completed = true;
inline unsigned tracks = 0;
inline bool Track(ID3D12GraphicsCommandList* list, Usage& usage)
{
    assert(list != nullptr);
    ++tracks;
    usage.tracked = trackOk;
    return trackOk;
}
inline bool Ready(const Usage&) { return ready; }
inline bool Completed(const Usage& usage) { return usage.tracked && completed; }
} // namespace DlssNr::Submission

bool g_tracked = false;
DlssNr::Submission::Usage g_usage;

struct OS_Dx12
{
};
struct DlssNr_Stabilizer_Dx12
{
};
struct DlssNr_UiMask_Dx12
{
};

// The guide resample: reads its inputs readable and writes its outputs as UAVs, and says what it was asked.
struct DlssNr_GuideMatch_Dx12
{
    inline static bool available = true;
    inline static bool fail = false;
    inline static unsigned dispatches = 0;
    inline static unsigned lastOutWidth = 0, lastOutHeight = 0, lastDepthWidth = 0, lastMotionWidth = 0;
    static bool Available() { return available; }
    DlssNr_GuideMatch_Dx12(std::string, ID3D12Device*) {}
    bool IsInit() const { return true; }
    bool Dispatch(ID3D12GraphicsCommandList* cmd, ID3D12Resource* depth, ID3D12Resource* motion, uint32_t, uint32_t,
                  uint32_t depthWidth, uint32_t, uint32_t, uint32_t, uint32_t motionWidth, uint32_t,
                  ID3D12Resource* outDepth, ID3D12Resource* outMotion, uint32_t outWidth, uint32_t outHeight)
    {
        assert(cmd != nullptr);
        assert(StateOf(depth) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(StateOf(motion) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(StateOf(outDepth) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(StateOf(outMotion) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(outDepth->format == DXGI_FORMAT_R32_FLOAT && outMotion->format == DXGI_FORMAT_R32G32_FLOAT);
        assert(outDepth->width == outWidth && outMotion->height == outHeight);
        if (fail)
            return false;
        ++dispatches;
        lastOutWidth = outWidth;
        lastOutHeight = outHeight;
        lastDepthWidth = depthWidth;
        lastMotionWidth = motionWidth;
        return true;
    }
};

// Model cadence's pass belongs to another feature (tests/nr-cadence); here it is only parked and deleted.
struct DlssNr_Cadence_Dx12
{
};

struct DlssNr_DetailStats_Dx12
{
};

struct NrState
{
    void (*release)(void*) = nullptr;
    DlssNr_GuideMatch_Dx12* guideMatch = nullptr;
    bool failed = false;
    bool peripheryFailed = false; // peripheral compression's latch, which Retry clears too (tests/nr-periphery)
    bool passFailed[3] {};
    const char* reason = "";
    bool reset = false;
};
NrState g_nr;

// The composition pass, as far as Transfer 2 uses it: one recorded dispatch per call, its source and model
// readable and its target a UAV.
struct Recorded
{
    DlssNrConstants constants;
    ID3D12Resource* source;
    ID3D12Resource* model;
    ID3D12Resource* target;
};
struct DlssNr_Dx12
{
    std::vector<Recorded> recorded;
    bool fail = false;
    bool DispatchPass(ID3D12GraphicsCommandList* cmd, const DlssNrConstants& constants, ID3D12Resource* source,
                      ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* motion, ID3D12Resource* previous,
                      ID3D12Resource* target, ID3D12Resource* keep)
    {
        assert(cmd != nullptr && source != nullptr && target != nullptr);
        assert(original == nullptr && motion == nullptr && previous == nullptr && keep == nullptr);
        assert(StateOf(source) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(model == nullptr || StateOf(model) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(StateOf(target) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (fail)
            return false;
        recorded.push_back({ constants, source, model, target });
        return true;
    }
};

// What RetryAfterFailure also touches.
std::mutex g_preMutex;
std::mutex g_nrMutex;
bool g_preAllocationFailed = false;
struct ExtentGate
{
    unsigned resets = 0;
    void Reset() { ++resets; }
};
ExtentGate g_preExtent, g_postExtent;
