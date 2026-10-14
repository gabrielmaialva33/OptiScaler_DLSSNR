#include "pch.h"

#include "Lsfg.h"

#include <lsfg/LsfgFrameGen_Dx12.h>
#include <lsfg/LsfgShaderSource.h>

#include <State.h>
#include <Util.h>

#include <ffx_api.h>
#include <ffx_framegeneration.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace Lsfg
{
namespace
{
// A retired context is freed this many FG presents after it was parked, unless FFX's swapchain is released
// first. FFX keeps at most two interpolation outputs in flight, far inside this; it is NR's figure.
constexpr uint64_t kParkPresents = 32;
constexpr double kSummaryIntervalMs = 10000.0;

struct Settings
{
    float flow = 1.0f;
    bool performance = false;
    bool fp16 = false; // no key: FP16 variants buy nothing measurable on D3D12 yet (harness README)
    std::wstring shaderDir;

    // What compiled shaders depend on (not the flow scale, which only sizes the images).
    bool SamePrograms(const Settings& o) const
    {
        return performance == o.performance && fp16 == o.fp16 && shaderDir == o.shaderDir;
    }
};

Settings ReadSettings()
{
    auto config = Config::Instance();
    Settings s;
    s.flow = ClampFlow(config->LsfgFlowScale.value_or_default());
    s.performance = config->LsfgPerformanceMode.value_or_default();
    s.shaderDir = config->LsfgShaderDir.value_or(std::wstring());
    return s;
}

// What the callback last saw: the frame FFX hands over and what LSFG would be built for.
struct Observed
{
    bool valid = false;
    ContextKey key;
    uint32_t frameFormat = 0;
    uint32_t transfer = 0;
    Surface verdict = Surface::Ok;
};

struct Parked
{
    std::unique_ptr<Context_Dx12> context;
    uint64_t parkedAt = 0;
};

struct Stats
{
    uint64_t generated = 0; // frames LSFG generated
    uint64_t held = 0;      // LSFG frames without a generated frame: warm-up or reset
    uint64_t notReady = 0;  // LSFG frames whose context was missing or being rebuilt
    uint64_t cancelled = 0; // LSFG frames FSR-FG had already cancelled (numGeneratedFrames 0 on entry)
    uint64_t refused = 0;   // LSFG frames refused (format, HDR, size)
    uint64_t ffx = 0;       // frames FFX interpolated while LSFG was enabled
    std::vector<double> gpuMs;
    std::vector<double> gpuMsPerPass; // OPTISCALER_LSFG_AB: the frames recorded with the harness's schedule
    double windowStartMs = 0.0;
};

// Study A/B, environment only (no ini key): OPTISCALER_LSFG_AB=1 records every other frame with the harness's
// schedule (a barrier around every dispatch, the copy-out) and splits the GPU timing summary in two, so both
// are measured at the same GPU clocks. A game that keeps the GPU half idle runs it at a fraction of its boost
// clock, which moves from run to run far more than any change here. The images are the same bytes either way
// (tools/lsfg-study/ctxbench).
bool AbTiming()
{
    static const bool ab = []
    {
        char value[8] {};
        return GetEnvironmentVariableA("OPTISCALER_LSFG_AB", value, sizeof(value)) > 0 && value[0] == '1';
    }();
    return ab;
}

// What owns D3D12 objects, and the lock. Never destroyed: at process exit a static destructor would release them under
// the loader lock, possibly after the driver detached, and a detached build worker may still want the lock.
struct Owned
{
    std::mutex mutex;
    std::unique_ptr<Context_Dx12> context;     // ready, serves context->Key()
    std::shared_ptr<ProgramSet_Dx12> programs; // compiled shaders for g_programsSettings on programs->Device()
    std::vector<Parked> parked;
};

Owned& O()
{
    static Owned* owned = new Owned();
    return *owned;
}

// Per FSR-FG frame slot (frameID % BUFFER_COUNT): EncodeLatch(frameID) when LSFG takes that frame, 0 when FFX does.
std::atomic<uint64_t> g_latch[BUFFER_COUNT] {};
uint64_t EncodeLatch(uint64_t frameId) { return (frameId << 1) | 1; }

Settings g_programsSettings;
uint64_t g_presents = 0; // FFX callbacks seen while LSFG is enabled
uint64_t g_epoch = 0;    // FFX swapchain releases: a build started before one is stale

bool g_building = false;
ContextKey g_buildKey;
bool g_hasFailedKey = false; // a context build failed for g_failedKey: not retried for that frame
ContextKey g_failedKey;
std::string g_hardRefusal; // non-empty: no LSFG this session (shaders, device)

Observed g_observed;
bool g_announced = false;
bool g_lastDecision = false;
std::string g_lastFallback;
bool g_firstFrameLogged = false;
Surface g_lastRefusal = Surface::Ok;
Stats g_stats;
OutputRoute g_lastRoute = OutputRoute::None;
bool g_abPerPass = false; // the schedule of the next frame under OPTISCALER_LSFG_AB

// FfxApiResourceState is a bit set; COMMON and PRESENT are both D3D12's COMMON (0).
D3D12_RESOURCE_STATES ToD3D12(uint32_t ffxState)
{
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    if (ffxState & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        state |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (ffxState & FFX_API_RESOURCE_STATE_COMPUTE_READ)
        state |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_PIXEL_READ)
        state |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_COPY_SRC)
        state |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (ffxState & FFX_API_RESOURCE_STATE_COPY_DEST)
        state |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (ffxState & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT)
        state |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (ffxState & FFX_API_RESOURCE_STATE_RENDER_TARGET)
        state |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    return state;
}

const char* ModeText(bool performance) { return performance ? "performance" : "quality"; }

const char* TransferText(uint32_t t)
{
    switch (t)
    {
    case kTransferSrgb:
        return "SRGB";
    case kTransferPq:
        return "PQ";
    case kTransferScRgb:
        return "scRGB";
    }
    return "unknown";
}

std::string KeyText(const ContextKey& k)
{
    return std::format("{}x{} format {} flow {:.2f} {} {}", k.width, k.height, (UINT) k.format, k.flow,
                       ModeText(k.performance), k.fp16 ? "fp16" : "fp32");
}

// Parks a retired context, or frees it now when the GPU never saw it.
void Park(std::unique_ptr<Context_Dx12> context)
{
    if (context == nullptr)
        return;
    if (!context->EverRecorded())
        return; // never recorded: nothing in flight can reference it
    O().parked.push_back({ std::move(context), g_presents });
}

void SweepParked()
{
    std::erase_if(O().parked, [](const Parked& p) { return g_presents - p.parkedAt >= kParkPresents; });
}

// Reads the frame FFX hands over: device, sizes, formats, and whether LSFG can take it.
bool Observe(const ffxDispatchDescFrameGeneration* params, const Settings& settings, Observed& out,
             ComPtr<ID3D12Device>& device)
{
    auto list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto present = static_cast<ID3D12Resource*>(params->presentColor.resource);
    auto output = static_cast<ID3D12Resource*>(params->outputs[0].resource);

    if (list == nullptr || present == nullptr || output == nullptr || FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    const auto pd = present->GetDesc();
    const auto od = output->GetDesc();

    out = {};
    out.valid = true;
    out.frameFormat = (uint32_t) pd.Format;
    out.transfer = params->backbufferTransferFunction;

    const bool plain2d = pd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && pd.SampleDesc.Count == 1 &&
                         od.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && od.SampleDesc.Count == 1 &&
                         pd.Width <= 16384 && pd.Height <= 16384;

    out.verdict = plain2d ? ClassifySurface((uint32_t) pd.Format, (uint32_t) pd.Width, pd.Height, (uint32_t) od.Format,
                                            (uint32_t) od.Width, od.Height, out.transfer, settings.flow)
                          : Surface::UnsupportedFormat;

    out.key.device = device.Get();
    out.key.width = (uint32_t) pd.Width;
    out.key.height = pd.Height;
    out.key.format = (DXGI_FORMAT) WorkingFormat((uint32_t) pd.Format);
    out.key.flow = settings.flow;
    out.key.performance = settings.performance;
    out.key.fp16 = settings.fp16;
    return true;
}

void BuildWorker(ContextKey key, ComPtr<ID3D12Device> device, Settings settings, uint64_t epoch)
{
    // Ours: the resource heuristics watching the device skip what is made here.
    ScopedInternalResourceCreation internalResources {};

    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<ProgramSet_Dx12> programs;
    {
        std::lock_guard lock(O().mutex);
        if (O().programs != nullptr && O().programs->Device() == device.Get() &&
            g_programsSettings.SamePrograms(settings))
            programs = O().programs;
    }

    std::string error;
    bool shaderFailure = false;
    bool compiled = false;

    if (programs == nullptr)
    {
        auto source = MakeDirectoryShaderSource(settings.shaderDir);
        LOG_INFO("LSFG: compiling the {} shaders ({}) from {}", kShaderCount - 1, ModeText(settings.performance),
                 source->Describe());
        programs = ProgramSet_Dx12::Build(device.Get(), settings.performance, settings.fp16, *source, error);
        shaderFailure = programs == nullptr;
        compiled = !shaderFailure;
    }

    std::unique_ptr<Context_Dx12> context;
    if (programs != nullptr)
        context = Context_Dx12::Build(programs, key, error);

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

    std::lock_guard lock(O().mutex);
    g_building = false;

    if (epoch != g_epoch)
    {
        // FFX's swapchain was released while this was built: keep nothing, so a released device is not held.
        // Never recorded, so freed here; a cached program set it reused stays alive through the parked contexts.
        LOG_INFO("LSFG: context for {} finished after FFX's swapchain was released and is dropped", KeyText(key));
        return;
    }

    if (shaderFailure)
    {
        g_hardRefusal = error;
        LOG_ERROR("LSFG: refused for this session, FFX interpolates: {}", error);
        return;
    }

    if (compiled)
    {
        O().programs = programs;
        g_programsSettings = settings;
    }

    if (context == nullptr)
    {
        g_hasFailedKey = true;
        g_failedKey = key;
        LOG_ERROR("LSFG: no context for {}: {}; FFX interpolates", KeyText(key), error);
        return;
    }

    if (!g_observed.valid || g_observed.key != key)
    {
        // The frame changed while this was built (resize, swapchain release): never recorded, so freed here.
        LOG_INFO("LSFG: context for {} is stale on arrival and is dropped", KeyText(key));
        return;
    }

    LOG_INFO("LSFG: context {}: {} images, {} descriptors, {:.1f} MiB, {} barrier waves per generated frame ({} "
             "without one), GPU timestamps {}, built in {:.0f} ms{}",
             KeyText(key), context->ImageCount(), context->DescriptorCount(),
             double(context->VramBytes()) / (1024.0 * 1024.0), context->WaveCount(true), context->WaveCount(false),
             context->HasTimestamps() ? "on" : "off", ms, compiled ? " (shaders compiled too)" : "");

    Park(std::move(O().context));
    O().context = std::move(context);
}

// Under O().mutex.
void MaybeStartBuild(const Observed& s, const ComPtr<ID3D12Device>& device, const Settings& settings)
{
    if (!g_hardRefusal.empty() || !s.valid || s.verdict != Surface::Ok || g_building)
        return;
    if (O().context != nullptr && O().context->Key() == s.key)
        return;
    if (g_hasFailedKey && g_failedKey == s.key)
        return;

    g_building = true;
    g_buildKey = s.key;

    LOG_INFO("LSFG: building the pass graph for {} on a worker thread; FFX interpolates until it is ready",
             KeyText(s.key));

    try
    {
        std::thread(BuildWorker, s.key, device, settings, g_epoch).detach();
    }
    catch (const std::exception& e)
    {
        // Inside FFX's Present: nothing may escape into its DLL.
        g_building = false;
        g_hardRefusal = std::string("could not start the build thread: ") + e.what();
        LOG_ERROR("LSFG: {}", g_hardRefusal);
    }
}

// Under O().mutex: why FFX interpolates the next frame, or empty when LSFG does.
std::string FallbackReason()
{
    if (!g_hardRefusal.empty())
        return "refused: " + g_hardRefusal;
    if (!g_observed.valid)
        return "waiting for the first frame";
    if (g_observed.verdict != Surface::Ok)
        return SurfaceText(g_observed.verdict);
    if (g_hasFailedKey && g_failedKey == g_observed.key)
        return "the context could not be built for " + KeyText(g_observed.key);
    if (O().context == nullptr || O().context->Key() != g_observed.key)
        return g_building ? "building the pass graph for " + KeyText(g_buildKey) : "no context yet";
    return {};
}

// Under O().mutex.
void LogSummary()
{
    const double now = Util::MillisecondsNow();
    if (g_stats.windowStartMs == 0.0)
    {
        g_stats.windowStartMs = now;
        return;
    }
    if (now - g_stats.windowStartMs < kSummaryIntervalMs)
        return;

    auto percentiles = [](std::vector<double>& v)
    {
        std::sort(v.begin(), v.end());
        const double p50 = v[v.size() / 2];
        const double p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
        return std::format("GPU p50 {:.3f} ms p99 {:.3f} ms over {} frames", p50, p99, v.size());
    };

    std::string gpu = "no GPU timings";
    if (!g_stats.gpuMs.empty())
        gpu = percentiles(g_stats.gpuMs);
    if (!g_stats.gpuMsPerPass.empty())
        gpu += "; A/B, the harness's schedule: " + percentiles(g_stats.gpuMsPerPass);

    std::string device = "no context";
    if (O().context != nullptr)
    {
        const HRESULT removed = O().context->Key().device->GetDeviceRemovedReason();
        device = removed == S_OK ? std::string("device ok") : std::format("DEVICE REMOVED ({:X})", (UINT) removed);
    }

    LOG_INFO("LSFG: last {:.0f} s: {} generated, {} held (warm-up/reset), {} not ready, {} cancelled, {} refused, "
             "{} by FFX; {}; {}",
             (now - g_stats.windowStartMs) / 1000.0, g_stats.generated, g_stats.held, g_stats.notReady,
             g_stats.cancelled, g_stats.refused, g_stats.ffx, gpu, device);

    g_stats = {};
    g_stats.windowStartMs = now;
}

void LogFirstFrame(const ffxDispatchDescFrameGeneration* params, uint32_t asked)
{
    if (g_firstFrameLogged)
        return;
    g_firstFrameLogged = true;

    auto list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto present = static_cast<ID3D12Resource*>(params->presentColor.resource);
    auto output = static_cast<ID3D12Resource*>(params->outputs[0].resource);
    const auto pd = present->GetDesc();
    const auto od = output->GetDesc();

    // OPTI_INTEGRATION.md risk 4: these come from AMD's signed DLL, read here from SDK 2.3.0 source. Say what
    // the running one hands over.
    LOG_INFO("LSFG: first frame from FFX: presentColor {}x{} format {} state {:X}, outputs[0] {}x{} format {} "
             "state {:X}, {} generated frame(s) asked, {} list, transfer {}, frameID {}",
             pd.Width, pd.Height, (UINT) pd.Format, params->presentColor.state, od.Width, od.Height, (UINT) od.Format,
             params->outputs[0].state, asked,
             list->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "COMPUTE (async)" : "DIRECT",
             TransferText(params->backbufferTransferFunction), params->frameID);
}
} // namespace

bool BeginFrame(int fIndex, uint64_t frameId)
{
    if (fIndex < 0 || fIndex >= BUFFER_COUNT)
        return false;

    if (!Config::Instance()->LsfgEnabled.value_or_default())
    {
        g_latch[fIndex].store(0, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard lock(O().mutex);

    if (!g_announced)
    {
        g_announced = true;
        const auto s = ReadSettings();
        LOG_INFO("LSFG: [LSFG] Enabled: LSFG 3.1 replaces FSR-FG's interpolation once its shaders are built "
                 "(ShaderDir {}, flow {:.2f}, {}); FFX keeps the swapchain, pacing and UI; x2",
                 s.shaderDir.empty() ? std::string("unset") : wstring_to_string(s.shaderDir), s.flow,
                 ModeText(s.performance));
        if (AbTiming())
            LOG_INFO("LSFG: OPTISCALER_LSFG_AB=1: every other frame uses the harness's schedule (a barrier around "
                     "every dispatch, the copy-out), timed apart in the summary");
    }

    SweepParked();

    const std::string reason = FallbackReason();
    const bool decision = reason.empty();
    g_latch[fIndex].store(decision ? EncodeLatch(frameId) : 0, std::memory_order_relaxed);

    if (decision && !g_lastDecision)
    {
        LOG_INFO("LSFG: takes over FSR-FG's interpolation: {}, x2", KeyText(O().context->Key()));
        g_lastFallback.clear();
    }
    else if (!decision && reason != g_lastFallback)
    {
        LOG_INFO("LSFG: FFX interpolates: {}", reason);
        g_lastFallback = reason;
    }

    g_lastDecision = decision;
    return decision;
}

bool TakesFrame(int fIndex, ffxDispatchDescFrameGeneration* params)
{
    if (params == nullptr)
        return false;

    // Only the frame the latch was set for, and only once: a callback for another frame ID (FFX configured with
    // frame 0 by Activate or Deactivate, a slot reused four frames later) never inherits it.
    uint64_t expected = EncodeLatch(params->frameID);
    if (fIndex >= 0 && fIndex < BUFFER_COUNT &&
        g_latch[fIndex].compare_exchange_strong(expected, 0, std::memory_order_relaxed))
        return true;

    if (!Config::Instance()->LsfgEnabled.value_or_default())
        return false;

    // A frame FFX interpolates: look at it, and start LSFG's build for it.
    std::lock_guard lock(O().mutex);
    ++g_presents;
    ++g_stats.ffx;

    const auto settings = ReadSettings();
    Observed s;
    ComPtr<ID3D12Device> device;
    if (Observe(params, settings, s, device))
    {
        g_observed = s;
        if (s.verdict != Surface::Ok && s.verdict != g_lastRefusal)
        {
            LOG_WARN("LSFG: refuses this frame ({}x{} format {}, transfer {}): {}; FFX interpolates", s.key.width,
                     s.key.height, s.frameFormat, TransferText(s.transfer), SurfaceText(s.verdict));
        }
        g_lastRefusal = s.verdict;
        MaybeStartBuild(s, device, settings);
    }

    LogSummary();
    return false;
}

uint32_t FfxDispatch(ffxDispatchDescFrameGeneration* params)
{
    if (params == nullptr)
        return FFX_API_RETURN_OK;

    std::lock_guard lock(O().mutex);
    ++g_presents;

    const uint32_t asked = params->numGeneratedFrames;
    params->numGeneratedFrames = 0; // until a generated frame is recorded

    // FSR-FG cancelled this frame before the hand-off (FG off, swapchain change, pause): the stream breaks.
    if (asked == 0 || !Config::Instance()->LsfgEnabled.value_or_default())
    {
        if (O().context != nullptr)
            O().context->GetHistory().Restart();
        ++g_stats.cancelled;
        LogSummary();
        return FFX_API_RETURN_OK;
    }

    const auto settings = ReadSettings();
    Observed s;
    ComPtr<ID3D12Device> device;
    if (!Observe(params, settings, s, device))
    {
        ++g_stats.refused;
        LogSummary();
        return FFX_API_RETURN_OK;
    }

    g_observed = s;

    if (s.verdict != Surface::Ok)
    {
        // The next BeginFrame sees the verdict and hands frames back to FFX.
        if (s.verdict != g_lastRefusal)
            LOG_WARN("LSFG: refuses this frame ({}x{} format {}, transfer {}): {}; FFX interpolates from the next one",
                     s.key.width, s.key.height, s.frameFormat, TransferText(s.transfer), SurfaceText(s.verdict));
        g_lastRefusal = s.verdict;
        ++g_stats.refused;
        LogSummary();
        return FFX_API_RETURN_OK;
    }
    g_lastRefusal = Surface::Ok;

    if (O().context == nullptr || O().context->Key() != s.key)
    {
        if (O().context != nullptr)
        {
            LOG_INFO("LSFG: the frame changed from {} to {}; rebuilding, FFX interpolates meanwhile",
                     KeyText(O().context->Key()), KeyText(s.key));
            Park(std::move(O().context));
        }
        MaybeStartBuild(s, device, settings);
        ++g_stats.notReady;
        LogSummary();
        return FFX_API_RETURN_OK;
    }

    LogFirstFrame(params, asked);

    auto list = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
    auto& history = O().context->GetHistory();
    // A frame that does not follow the last one ingested breaks the stream as a reset does: FFX resets on the
    // same frame-ID gap, and LSFG would otherwise blend a stale source slot and feature history.
    const bool reset = params->reset || !history.Follows(params->frameID);

    FrameIo io;
    io.presentColor = static_cast<ID3D12Resource*>(params->presentColor.resource);
    io.presentState = ToD3D12(params->presentColor.state);
    io.generatedFrames = asked;
    io.generate = history.Generates(reset) ? std::min<uint32_t>(asked, uint32_t(io.outputs.size())) : 0;

    for (uint32_t k = 0; k < io.generate; ++k)
    {
        io.outputs[k] = static_cast<ID3D12Resource*>(params->outputs[k].resource);
        io.outputStates[k] = ToD3D12(params->outputs[k].state);
        if (io.outputs[k] == nullptr || io.outputs[k] == io.presentColor)
        {
            io.generate = k;
            break;
        }
    }

    FrameConstants frame;
    frame.timestamp = 0.0f;
    frame.sourceIndex = history.iteration;
    frame.advancedColorKind = 0; // SDR only for now: the HDR mapping is OPTI_INTEGRATION.md section 6
    frame.hdrSupport = 0;
    frame.resolutionInvScale = 1.0f / s.key.flow;
    frame.uiThreshold = 0.5f;

    Schedule schedule = Schedule::Waves;
    if (AbTiming())
    {
        schedule = g_abPerPass ? Schedule::PerPass : Schedule::Waves;
        g_abPerPass = !g_abPerPass;
    }

    O().context->Record(list, io, frame, schedule);
    history.Advance(reset, params->frameID);

    // Where generated frames go, said once and again whenever it changes (a new FFX output, a rebuilt context).
    const OutputRoute route = O().context->LastOutputRoute();
    if (route != OutputRoute::None && route != g_lastRoute)
    {
        if (route == OutputRoute::Direct)
            LOG_INFO("LSFG: generate writes FFX's output itself (no copy-out); the presented frame is still copied in, "
                     "because the shaders sample it as one slice of LSFG's two-slice source array");
        else
            LOG_INFO("LSFG: FFX's output cannot be written by generate ({}); generated frames are copied into it",
                     O().context->OutputRouteReason());
        g_lastRoute = route;
    }

    params->numGeneratedFrames = io.generate;
    if (io.generate > 0)
        ++g_stats.generated;
    else
        ++g_stats.held;

    O().context->CollectGpuTimes(g_stats.gpuMs, AbTiming() ? &g_stats.gpuMsPerPass : nullptr);
    LogSummary();
    return FFX_API_RETURN_OK;
}

void OnSwapchainReleased()
{
    std::lock_guard lock(O().mutex);

    const bool any = O().context != nullptr || O().programs != nullptr;

    // Parked, not freed: FFX's DestroyContext only Release()s its swapchain, and the drain (its interpolation
    // fence) happens in the destructor, which runs only if that was the last reference. A game that makes a new
    // swapchain while holding the old one keeps it alive (IFGFeature_Dx12.cpp, "creating new swapchain without
    // releasing old one"), with an interpolation list possibly still in flight. The program set is dropped from
    // the cache; the parked contexts keep it alive while they need it, then the device can go.
    ++g_epoch;
    Park(std::move(O().context));
    O().programs.reset();

    g_observed = {};
    g_hasFailedKey = false;
    g_lastDecision = false;
    g_lastFallback.clear();
    g_lastRefusal = Surface::Ok;
    for (auto& latch : g_latch)
        latch.store(0, std::memory_order_relaxed);

    if (any)
        LOG_INFO("LSFG: FFX's swapchain was released; LSFG's context is parked ({} parked)", O().parked.size());
}

} // namespace Lsfg
