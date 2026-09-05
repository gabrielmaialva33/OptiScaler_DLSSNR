#include "pch.h"

#include <set>

#include <dlssnr/DlssNr.h>

#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <dlssnr/DlssNr_Chain.h>
#include <dlssnr/DlssNr_Submission.h>
#include <dxgi1_4.h>

#include "DlssNr_Dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>

#include <mutex>
#include <algorithm>
#include <cstring>
#include "precompile/DlssNr_Shader.h"
#include "../output_scaling/OS_Dx12.h"

namespace
{
// Coverage belongs to an upscaler boundary, not to an arbitrary Dispatch call. Keep fallback
// separate from Stage 0, and do not count the normal Stage 1 post-path stand-down as another skip.
std::mutex g_coverageMutex;
DlssNr::Detail::StageCoverage g_coverage[3];
DlssNr::Detail::CoverageTotals g_coverageLogged[3];

struct CoverageScope
{
    int stage; // -1 inactive, 0 after, 1 before, 2 after-fallback
    DlssNr::Detail::CoverageSample sample;

    explicit CoverageScope(int which) : stage(which)
    {
        if (stage >= 0)
            sample.present = State::Instance().frameCount;
    }
    ~CoverageScope()
    {
        if (stage < 0)
            return;
        std::lock_guard<std::mutex> lock(g_coverageMutex);
        auto& coverage = g_coverage[stage];
        if (!coverage.Record(sample, std::chrono::steady_clock::now()))
            return;
        const auto& t = coverage.totals;
        auto& previous = g_coverageLogged[stage];
        LOG_INFO("DLSS-NR coverage stage={} calls={} model_ok={} model_failed={} applied_recorded={} "
                 "skipped={} fallback={} applied_present_ids={} skipped_present_ids={} present_id={} "
                 "extent={}x{} model={}x{} window_calls={} window_model_ok={} window_applied={} "
                 "window_skipped={} window_fallback={} coverage={} last=\"{}\" "
                 "evidence=CPU-recorded-not-GPU-complete",
                 stage == 0   ? "after"
                 : stage == 1 ? "before"
                              : "after-fallback",
                 t.calls, t.modelOk, t.modelFailed, t.applied, t.skipped, t.fallback, t.appliedPresents,
                 t.skippedPresents, sample.present, sample.width, sample.height, sample.modelWidth, sample.modelHeight,
                 t.calls - previous.calls, t.modelOk - previous.modelOk, t.applied - previous.applied,
                 t.skipped - previous.skipped, t.fallback - previous.fallback,
                 t.applied == previous.applied ? "ZERO_APPLIED_IN_WINDOW" : "APPLIED_IN_WINDOW", sample.reason);
        previous = t;
    }
};

// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1:
        return "Success";
    case 0xBAD00001:
        return "FAIL_FeatureNotSupported";
    case 0xBAD00002:
        return "FAIL_PlatformError";
    case 0xBAD00003:
        return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004:
        return "FAIL_FeatureNotFound";
    case 0xBAD00005:
        return "FAIL_InvalidParameter";
    case 0xBAD00006:
        return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007:
        return "FAIL_NotInitialized";
    case 0xBAD00008:
        return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009:
        return "FAIL_RWFlagMissing";
    case 0xBAD0000A:
        return "FAIL_MissingInput";
    case 0xBAD0000B:
        return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C:
        return "FAIL_OutOfDate";
    case 0xBAD0000D:
        return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E:
        return "FAIL_UnsupportedFormat";
    case 0xBAD0000F:
        return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010:
        return "FAIL_UnsupportedParameter";
    case 0xBAD00011:
        return "FAIL_Denied";
    case 0xBAD00012:
        return "FAIL_NotImplemented";
    default:
        return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})", result,
             NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright = result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                                      unsigned int, unsigned int, int, float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                                     ID3D12Resource*, ID3D12Resource*, unsigned int, unsigned int, unsigned int,
                                     unsigned int, int, int, float, int, float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*)(void*);
using PFN_NrSetExtras = void(__cdecl*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                       unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*)(int);
using PFN_NrProbeFloat = void(__cdecl*)(void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.

struct NrState
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to.
    void* passFeature[3] = {};
    ID3D12Resource* passPing = nullptr;
    DlssNrResolvedPassSettings passBuilt[3] {};
    bool passReset[3] { true, true, true };
    bool passFailed[3] {};
    DlssNr::Submission::Usage creation[3];
    uint64_t creationFrame[3] {};
    uint64_t measuredModelBytes = 0;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    bool meterExposureValid[4] = {};
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";
};

NrState g_nr;
std::unique_ptr<DlssNr_Dx12> g_compose;

// The before-upscale path's own surface: the copy of the game's colour that carries the model's edit
// into the upscaler. Sized to the render subrect and reallocated when that moves, which in a
// dynamic-resolution game is often -- see the design note for what that costs and what to do about it.
struct PreUpscaleState
{
    ID3D12Resource* scratch = nullptr;
    unsigned int width = 0;
    unsigned int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

PreUpscaleState g_pre;
std::mutex g_preMutex;
DlssNr::Detail::StableExtent g_preExtent;
DlssNr::Detail::CreationFrameGate g_preCreation;
ID3D12Device* g_preDevice = nullptr; // Held by the scratch; refuse a different device this session.
bool g_preAllocationFailed = false;
bool g_lastBeforeUpscale = false;
std::atomic<const char*> g_preStatus { "Waiting for a supported upscale evaluation" };

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;

// A second timer, around the model's evaluate and nothing else.
//
// The first one brackets the whole pass, which is the number the menu shows and the right one for
// "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
// full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
// encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
// resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
// work to NGX overhead.
//
// Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
// we can actually do something about.
std::unique_ptr<GpuTime_Dx12> g_ngxTime;
std::optional<double> g_lastNgxTime;
std::optional<double> g_lastGpuTime;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
void CheckCaptureTrigger()
{
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable", g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(), GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf", "Balanced", "MaxQuality", "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr || g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Tracking is sticky: lifetime evidence must begin before the first recorded NR use.
bool g_tracked = false;
bool g_legacyRecorded = false;
DlssNr::Submission::Usage g_usage;
bool g_haveTrackedUsage = false;
DlssNr::Chain::RecordingGate g_recordings;
DlssNr::Chain::Schedule g_chainSchedule;
DlssNr::Detail::StableExtent g_chainExtent;
std::atomic<unsigned int> g_activePasses { 0 };
std::atomic<const char*> g_chainStatus { "Single pass (master settings)" };

void ChainStatus(const char* reason)
{
    if (g_chainStatus.exchange(reason) != reason)
        LOG_INFO("DLSS-NR chain: {}", reason);
}

bool RetiredCapacity();

bool WantsTrackedRecording(const Config& cfg)
{
    const auto requested = cfg.GetDlssNrPassSnapshot();
    return g_tracked || (!g_legacyRecorded && (requested.Count > 1 || requested.Individual));
}

bool TrackNrRecording(ID3D12GraphicsCommandList* cmd, const Config& cfg, DlssNr::Chain::RecordingLease* lease)
{
    const auto requested = cfg.GetDlssNrPassSnapshot();
    const bool optIn = requested.Count > 1 || requested.Individual;
    if (!g_tracked && optIn && !g_legacyRecorded)
        g_tracked = true;
    if (!g_tracked)
    {
        if (optIn)
            ChainStatus(
                "Restart required: existing NR work has no submission lifetime evidence; single/master retained");
        g_legacyRecorded = true;
        return true;
    }
    if (!lease || !lease->Valid(g_recordings, reinterpret_cast<uintptr_t>(cmd)))
    {
        ChainStatus("Another NR recording scope owns the resources; new recording refused");
        return false;
    }
    if (lease->Tracked())
        return true; // Explicit pre scope borrowing its own recording.
    if (!lease->MayTrack(g_haveTrackedUsage, !g_haveTrackedUsage || DlssNr::Submission::Ready(g_usage)))
    {
        ChainStatus("Waiting for previous NR recording Reset and all GPU fences; new recording skipped (throughput may "
                    "decrease)");
        return false;
    }
    if (!RetiredCapacity())
    {
        ChainStatus("Retired resource limit reached: pending/unknown GPU recordings retained; rebuild refused until "
                    "completion or restart");
        return false;
    }
    if (!DlssNr::Submission::Track(cmd, g_usage))
    {
        ChainStatus(DlssNr::Submission::LastRefusal());
        return false;
    }
    g_haveTrackedUsage = true;
    lease->MarkTracked();
    return true;
}

bool VideoMemory(ID3D12Device* device, DXGI_QUERY_VIDEO_MEMORY_INFO& memory)
{
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter3* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    const HRESULT found = factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(found))
        return false;
    const HRESULT result = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
    adapter->Release();
    return SUCCEEDED(result);
}

// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = 32;
    bool tracked = false;
    DlssNr::Submission::Usage usage;
    OS_Dx12* scaler = nullptr;
};

std::vector<NrRetired> g_nrRetired;

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.tracked = g_tracked;
    if (g_tracked)
        r.usage = g_usage;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrScaler(OS_Dx12*& scaler)
{
    if (!scaler)
        return;
    if (!g_tracked)
    {
        delete scaler;
        scaler = nullptr;
        return;
    }
    NrRetired r;
    r.scaler = scaler;
    r.tracked = true;
    r.usage = g_usage;
    scaler = nullptr;
    g_nrRetired.push_back(r);
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (g_nrRetired[i].tracked ? !DlssNr::Submission::Ready(g_nrRetired[i].usage) : --g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (g_nrRetired[i].feature != nullptr && g_nr.release != nullptr)
            g_nr.release(g_nrRetired[i].feature);

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        delete g_nrRetired[i].scaler;
        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

bool RetiredCapacity()
{
    if (!g_tracked)
        return true;
    TickNrRetired();
    // A rebuild can retire at most 16 objects. Refuse BEFORE recording or allocation,
    // not after unsubmitted slider/DRS changes already consumed arbitrary memory.
    return DlssNr::Chain::RetirementAllowed(g_nrRetired.size());
}

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
{
    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)", (int) g_nr.output->GetDesc().Format,
             (int) needed);

    ForgetCalibration();

    ParkNrFeature(g_nr.feature);

    // The extras go with it: they were built for this raster and this tuning too.
    for (void*& f : g_nr.passFeature)
        ParkNrFeature(f);

    for (ID3D12Resource** r : { &g_nr.output, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall, &g_nr.passPing })
        ParkNrResource(*r);

    g_nr.reset = true;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
// texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureValid[slot] = exposureBound;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough
                        ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureValid[slot] && DlssNr::Exposure::ValidSample(src[0]))
        g_nr.gameExposure = src[0];

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
}

// Keep source selection testable without changing the established exposure law.
float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const auto source = cfg.DlssNrWhitePointSource.value_or_default();
    const float anchored = isHdrBuffer && source == 2
                               ? DlssNr::ExposureScan::AnchoredWhitePoint(DlssNr::ExposureScan::BestValue(),
                                                                          cfg.DlssNrScanInverted.value_or_default(),
                                                                          cfg.DlssNrScanTrim.value_or_default())
                               : 0.0f;
    return DlssNr::Exposure::WhitePoint(source, isHdrBuffer, cfg.DlssNrWhitePointScale.value_or_default(),
                                        cfg.DlssNrWhitePointTrim.value_or_default(), g_nr.gameExposure,
                                        g_nr.gamePreExposure, anchored);
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                    IID_PPV_ARGS(&res));
    return res;
}

// The before-upscale copy. A scratch surface with one extra allowance: some games leave their colour
// in RENDER_TARGET and say so through ColorResourceBarrier, and the upscaler then transitions whatever
// it is handed as colour from that state. A copy standing in for the colour has to be able to sit in
// it, and a transition into RENDER_TARGET on a surface created without the flag is invalid. Colour
// formats are checked before allocation; failure falls back without returning an incompatible copy.
ID3D12Resource* CreatePreUpscaleScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                                        unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    return res;
}

// An sRGB surface cannot be written through a UAV, and its typed twin would be read by the upscaler
// as linear -- a different picture, gamma-shifted. Such a colour is left to the after-upscale path.
bool IsSrgb(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    const D3D12_RESOURCE_DESC src = source->GetDesc();

    // More than one candidate, because the obvious typed member of a family is not always a format a
    // resource may be created with. R32_FLOAT_X8X24_TYPELESS is the case that forced this: it is the
    // right way to VIEW an R32G8X24_TYPELESS depth buffer and CreateCommittedResource refuses it, so
    // the single-candidate version could never clone a depth buffer of that family at all. Ordered
    // most faithful first; each failure is logged with its HRESULT so a new game's format does not
    // have to be guessed at again.
    DXGI_FORMAT candidates[4] = {};
    uint32_t count = 0;
    candidates[count++] = TypedGuideFormat(src.Format);

    switch (src.Format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        candidates[count++] = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R24G8_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D24_UNORM_S8_UINT;
        candidates[count++] = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D32_FLOAT;
        break;
    case DXGI_FORMAT_R16_TYPELESS:
        candidates[count++] = DXGI_FORMAT_R16_FLOAT;
        break;
    default:
        break;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (uint32_t i = 0; i < count; i++)
    {
        D3D12_RESOURCE_DESC desc = src;
        desc.Format = candidates[i];
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Let the runtime pick the alignment rather than inheriting the source's.
        //
        // A guide created with D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT (0x400, Agility SDK) reports an
        // alignment that is only legal together with that flag. The clone drops the flags, so carrying
        // the number over made every CreateCommittedResource fail with E_INVALIDARG before the format
        // was even considered -- which is why a candidate as ordinary as R16G16B16A16_FLOAT was refused.
        desc.Alignment = 0;

        ID3D12Resource* res = nullptr;
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res));

        if (SUCCEEDED(hr) && res != nullptr)
        {
            if (i != 0)
                LOG_INFO("DLSS-NR guide clone: source format {} cloned as {} (candidate {} of {})", (int) src.Format,
                         (int) candidates[i], i + 1, count);

            return res;
        }

        LOG_WARN("DLSS-NR guide clone: source format {}, candidate {} ({}) refused, hr 0x{:08X}", (int) src.Format,
                 i + 1, (int) candidates[i], (UINT) hr);
    }

    LOG_ERROR("DLSS-NR guide clone failed for every candidate. source desc: dim {} {}x{} arr {} mips {} "
              "fmt {} samples {} layout {} flags 0x{:X} alignment {}",
              (int) src.Dimension, (UINT64) src.Width, src.Height, src.DepthOrArraySize, src.MipLevels,
              (int) src.Format, src.SampleDesc.Count, (int) src.Layout, (UINT) src.Flags, (UINT64) src.Alignment);

    return nullptr;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
struct RestoreResourceState
{
    ID3D12GraphicsCommandList* cmd;
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES from, to;
    void Restore()
    {
        if (resource)
            Barrier(cmd, resource, from, to);
        resource = nullptr;
    }
    ~RestoreResourceState() { Restore(); }
};

struct ReadResourceScope
{
    RestoreResourceState restore;
    ReadResourceScope(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES arrival)
        : restore { cmd, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival }
    {
        if (resource)
            Barrier(cmd, resource, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
};

// The codec binds Texture2D<float4>, never an integer, array, MSAA or depth view.
// Keep typeless translations identical to Shader_Dx12::TranslateTypelessFormats.
bool ExposureTextureUsable(ID3D12Device* device, ID3D12Resource* resource)
{
    if (device == nullptr || resource == nullptr)
        return false;
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0 ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.MipLevels == 0 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
        return false;

    DXGI_FORMAT view = desc.Format;
    switch (view)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R32G32_TYPELESS:
        view = DXGI_FORMAT_R32G32_FLOAT;
        break;
    case DXGI_FORMAT_R16G16_TYPELESS:
        view = DXGI_FORMAT_R16G16_FLOAT;
        break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        view = DXGI_FORMAT_R32G32B32A32_FLOAT;
        break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        view = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        break;
    default:
        return false; // Unknown interpretations must not become an exposure reading.
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { view };
    return !FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
}

ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                              ID3D12Resource** clone)
{
    if (source == nullptr || !IsTypeless(source->GetDesc().Format))
        return source;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        // Dimensions only: the clone's format is whichever candidate CreateGuideClone settled on,
        // which is not always TypedGuideFormat's first choice. Comparing against that choice would
        // retire and rebuild the clone on every frame. Shape is what the DRS crash was about.
        if (have.Width != want.Width || have.Height != want.Height)
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}", (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer, uiWidth, uiHeight, bbWidth, bbHeight);
}

bool TuningMatchesFeature(const Config& cfg)
{
    return g_nr.builtPreset == cfg.DlssNrPreset.value_or_default() &&
           g_nr.builtIntensity == cfg.DlssNrIntensity.value_or_default() &&
           g_nr.builtStyle == cfg.DlssNrStyle.value_or_default() &&
           g_nr.builtLocalStructure == cfg.DlssNrLocalStructure.value_or_default() &&
           g_nr.builtLocalTone == cfg.DlssNrLocalTone.value_or_default() &&
           g_nr.builtSkinStructure == cfg.DlssNrSkinStructure.value_or_default() &&
           g_nr.builtAutoMask == cfg.DlssNrAutoMask.value_or_default();
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
}

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                               ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                               ID3D12Resource* InMotion, ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                               ID3D12Resource* OutKeep)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }
}

bool DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                           ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                           ID3D12CommandQueue* timingQueue, DlssNr::Detail::CoverageSample* coverage,
                           DlssNr::Chain::RecordingLease* recording)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();
    const auto reportSkip = [coverage](const char* why)
    {
        if (coverage != nullptr)
            coverage->reason = why;
        ReportSkipOnce(why);
    };

    if (g_nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr || motion == nullptr ||
        output == nullptr)
    {
        reportSkip(g_nr.failed ? "it already failed this session" : "a resource was missing");
        return false;
    }

    // Different configured arrival states cannot both describe the same resource. Reject before
    // the first transition; guessing would issue a barrier from a state that never existed.
    if (depth == motion || depth == colour || depth == output || motion == colour || motion == output)
    {
        reportSkip("aliased color/output/depth/motion resources cannot be normalized safely");
        return false;
    }

    g_activePasses = 0;
    std::unique_ptr<DlssNr::Chain::RecordingLease> ownedRecording;
    if (!recording && WantsTrackedRecording(cfg))
    {
        ownedRecording =
            std::make_unique<DlssNr::Chain::RecordingLease>(g_recordings, reinterpret_cast<uintptr_t>(cmdList));
        recording = ownedRecording.get();
    }
    if (!TrackNrRecording(cmdList, cfg, recording))
    {
        reportSkip(g_chainStatus.load());
        return false;
    }
    auto passSnapshot = cfg.GetDlssNrPassSnapshot();
    const bool chainEnabled = g_tracked && !cfg.DlssNrUseProxy.value_or_default();
    if (g_tracked && cfg.DlssNrUseProxy.value_or_default())
        ChainStatus("Driver proxy uses one pass and master settings; overrides are inactive");
    if (!chainEnabled)
    {
        passSnapshot.Count = 1;
        passSnapshot.Individual = false;
        const auto master = DlssNrResolvedPassSettings {
            cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
            cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrStyle.value_or_default(),     cfg.DlssNrPreset.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default()
        };
        passSnapshot.Settings.fill(master);
    }
    const auto& firstSettings = passSnapshot.Settings[0];
    const uint64_t observedFrame = State::Instance().frameCount;
    const bool chainStable = !chainEnabled || g_chainSchedule.Stable(passSnapshot, std::chrono::steady_clock::now());
    if (chainEnabled && (!chainStable || !g_chainSchedule.Evaluate(observedFrame)))
    {
        reportSkip(!chainStable ? "chain settings are settling (500 ms)"
                                : "chain already evaluated at this observed present");
        return false;
    }

    ID3D12Resource* target = output;

    // What the model is shown. On the after-upscale path it is the target itself: the pass reads the
    // upscaler's output and writes the edit back over it, so the target is moved to a readable state
    // around every read. On the before-upscale path it is the game's own colour, which arrives in
    // NON_PIXEL_SHADER_RESOURCE -- the state NGX requires of every input -- is read there, and is
    // never written or transitioned by this pass at all.
    ID3D12Resource* const source = colour;
    const bool sourceIsTarget = source == target;

    // Whether the resolve wrote the target this frame. See the header.
    bool wrote = false;

    // The state the upscaler left the output in. Every upscaler in this tree ends Evaluate by moving
    // the output to OutputResourceBarrier when the user set it (FFXFeature_Dx12.cpp:606 and the FSR2 /
    // XeSS equivalents), and leaves it in UNORDERED_ACCESS -- what its own compute wrote -- when they
    // did not. This pass then reads and writes the output as a UAV, so it normalises to that here and
    // restores the arrival state before every exit. When the config is unset the two states are equal
    // and Barrier() skips the no-op, so the default path is byte-identical.
    //
    // Unless the caller owns the output and says otherwise. The before-upscale path writes into a copy
    // of its own that never passed through an upscaler, and rests it in UNORDERED_ACCESS whatever the
    // config says about the game's output.
    const D3D12_RESOURCE_STATES outputArrival =
        frame.OutputState >= 0 ? (D3D12_RESOURCE_STATES) frame.OutputState
        : Config::Instance()->OutputResourceBarrier.has_value()
            ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
            : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    struct RestoreOutput
    {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* target;
        D3D12_RESOURCE_STATES arrival;
        bool active;
        ~RestoreOutput()
        {
            if (active)
                Barrier(cmd, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, arrival);
        }
    } restoreOutput { cmdList, target, outputArrival, true };

    Barrier(cmdList, target, outputArrival, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        reportSkip("the output texture belongs to no D3D12 device");
        return false;
    }

    const bool restoreRequired =
        cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default();
    if (restoreRequired && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        reportSkip("command-list state could not be restored, including feature creation");
        device->Release();
        return false;
    }
    ScopedNrStateEnvelope stateEnvelope(cmdList);

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;

    // Depth and motion vectors are the upscaler's inputs and so are at render resolution, while colour
    // and output are at display resolution. The model takes that as a subrect per resource rather than
    // needing them resampled, which is why nothing here rescales anything.
    // The guides are the upscaler's inputs and so are at render resolution, while colour and output
    // are at display resolution. Their sizes come from the resources rather than from the caller:
    // one less thing a call site can get wrong, and the model takes the difference as a subrect per
    // resource rather than needing anything resampled.
    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();
    unsigned int guideWidth = (unsigned int) guideDesc.Width;
    unsigned int guideHeight = guideDesc.Height;

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    // What the game rendered wins over how big the texture is.
    //
    // The comment above says the sizes come from the resources so there is one less thing a call site
    // can get wrong, and that was right about call sites and wrong about the game. A dynamic
    // resolution title allocates its depth once at the maximum it will ever need and renders into
    // the corner; the resource then describes the allocation, not the picture, and the model gets
    // handed the stale margin as though it were scene.
    //
    // Bounded by the resource because a subrect larger than the texture is a game bug that would
    // otherwise become a read off the end of it.
    if (frame.RenderSubrectWidth != 0 && frame.RenderSubrectHeight != 0)
    {
        const unsigned int subW = std::min(frame.RenderSubrectWidth, guideWidth);
        const unsigned int subH = std::min(frame.RenderSubrectHeight, guideHeight);

        if (subW != guideWidth || subH != guideHeight)
        {
            static unsigned int saidW = 0, saidH = 0;

            if (saidW != subW || saidH != subH)
            {
                saidW = subW;
                saidH = subH;
                LOG_INFO("DLSS-NR guides: the game renders {}x{} into a {}x{} texture, so the model is "
                         "told the smaller number",
                         subW, subH, guideWidth, guideHeight);
            }
        }

        guideWidth = subW;
        guideHeight = subH;
    }

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    g_nr.guideDepthInverted = frame.DepthInverted;

    // The game's own encoding, passed through. Every resource already carries a subrect saying how
    // big it is, so scaling by the resolution ratio on top of that counts it twice -- vectors come
    // out too long and the model warps its history past where the surface went.
    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        g_nr.reset = true;

        static unsigned long long resets = 0;
        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    // Logged whenever it changes, not once per session.
    //
    // A guide size change does not rebuild the feature -- the guides are handed over as subrects and
    // the output size is what the model is built for -- so a once-only line goes stale the moment the
    // player moves the quality slider, and every later line in the log is then read against numbers
    // that stopped being true. In Nioh 3 the session opened at DLAA, moved to 66% and ended at 33%,
    // and the log claimed 1920x1080 guides throughout.
    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow {
        true,  g_nr.guideDepthInverted, g_nr.guideMvScaleX, g_nr.guideMvScaleY, guideWidth, guideHeight,
        width, (unsigned int) height
    };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                 guideWidth, guideHeight, width, height);
    }

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return false;
    }

    // What the model works at. The frame and its edit stay full resolution; only the model's input and
    // answer change size, and the resolve enlarges (or minifies) the answer while compositing. Below 1
    // the model runs reduced and cheaper; above 1 it SUPERSAMPLES -- the proxy is upscaled to a larger
    // working size so the model denoises a super-native input, which the resolve then samples back down.
    // Capped at 2x: cost grows with the area and NGX acceptance above native is what this probe tests.
    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;

    if (chainEnabled && !g_chainExtent.Observe(workWidth, workHeight, std::chrono::steady_clock::now(),
                                               static_cast<uint32_t>(desc.Format)))
    {
        reportSkip("chain working resolution is settling (500 ms); no model reconstruction");
        device->Release();
        return false;
    }

    if (chainEnabled && (!g_nr.feature || g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth ||
                         g_nr.workHeight != workHeight || g_nr.passBuilt[0] != firstSettings ||
                         (g_nr.output && g_nr.output->GetDesc().Format != desc.Format)))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO budget {};
        const uint64_t surfaceBytes = uint64_t(workWidth) * workHeight * 48 + uint64_t(width) * height * 48;
        if (!VideoMemory(device, budget) ||
            !DlssNr::Chain::Admit(budget.Budget, budget.CurrentUsage, g_nr.measuredModelBytes, surfaceBytes))
        {
            g_nr.failed = true;
            g_nr.reason = "chain generation refused: insufficient or unavailable adapter headroom (retry latched)";
            ChainStatus(g_nr.reason);
            reportSkip(g_nr.reason);
            device->Release();
            return false;
        }
    }

    ReleaseSurfacesIfFormatChanged(desc.Format);

    const bool beforeUpscale = !sourceIsTarget;
    if (g_lastBeforeUpscale != beforeUpscale)
    {
        g_nr.reset = true;
        g_lastBeforeUpscale = beforeUpscale;
    }

    const bool resolutionChanged =
        g_nr.width != width || g_nr.height != height || g_nr.workWidth != workWidth || g_nr.workHeight != workHeight;

    // The model reads its tuning once, while the feature is built, so a changed setting only takes
    // effect when the feature is rebuilt. TuningMatchesFeature was written to notice that and then
    // never called, which is why every one of these controls appeared to do nothing until something
    // else -- a resolution change -- happened to force a rebuild by accident.
    const bool tuningChanged = chainEnabled ? g_nr.passBuilt[0] != firstSettings : !TuningMatchesFeature(cfg);

    if (g_nr.feature != nullptr && (resolutionChanged || tuningChanged))
    {
        // Parked rather than released: with frame generation the GPU can still be several frames
        // deep in work that references all of it.
        ParkNrFeature(g_nr.feature);

        if (resolutionChanged)
            for (void*& f : g_nr.passFeature)
                ParkNrFeature(f);
        for (auto& reset : g_nr.passReset)
            reset = true;

        // Only a resolution change invalidates the scratch textures. Tuning does not, and throwing
        // them away for it would mean a reallocation every time a slider moves.
        if (resolutionChanged)
        {
            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.outputNative);
            ParkNrResource(g_nr.passPing);
        }
    }

    if (chainEnabled)
    {
        for (unsigned i = 1; i < 3; ++i)
        {
            if (g_nr.passFeature[i] && (i >= passSnapshot.Count || g_nr.passBuilt[i] != passSnapshot.Settings[i]))
            {
                ParkNrFeature(g_nr.passFeature[i]);
                for (unsigned downstream = i; downstream < 3; ++downstream)
                    g_nr.passReset[downstream] = true;
            }
            if (resolutionChanged || g_nr.passBuilt[i] != passSnapshot.Settings[i])
                g_nr.passFailed[i] = false;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (chainEnabled && (g_nr.output == nullptr || g_nr.colorCopy == nullptr || g_nr.hdrCopy == nullptr ||
                         (reduced && g_nr.colorSmall == nullptr)))
    {
        g_nr.failed = true;
        g_nr.reason = "chain scratch initialization failed; retry latched";
        reportSkip(g_nr.reason);
        device->Release();
        return false;
    }

    // The down-leg target is native (the answer is brought back to frame size before the resolve).
    if (workScale > 1.0f && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, desc.Format, width, height);

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr && g_nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return false;
        }

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        DXGI_QUERY_VIDEO_MEMORY_INFO memoryBefore {};
        if (chainEnabled)
        {
            g_nr.creation[0] = {};
            if (!g_chainSchedule.Create(observedFrame) || !DlssNr::Submission::Track(cmdList, g_nr.creation[0]))
            {
                device->Release();
                return false;
            }
            VideoMemory(device, memoryBefore);
        }
        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                        cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) (chainEnabled ? firstSettings.Preset : cfg.DlssNrPreset.value_or_default()),
                        (chainEnabled ? firstSettings.Intensity : cfg.DlssNrIntensity.value_or_default()),
                        (int) (chainEnabled ? firstSettings.Style : cfg.DlssNrStyle.value_or_default()),
                        (chainEnabled ? firstSettings.LocalStructure : cfg.DlssNrLocalStructure.value_or_default()),
                        (chainEnabled ? firstSettings.LocalTone : cfg.DlssNrLocalTone.value_or_default()),
                        (chainEnabled ? firstSettings.SkinStructure : cfg.DlssNrSkinStructure.value_or_default()),
                        (chainEnabled ? firstSettings.AutoMask : cfg.DlssNrAutoMask.value_or_default()) ? 1 : 0,
                        // UI correction at the model's own default: with no UI layer fed to it there
                        // is nothing for it to correct.
                        1);

        if (chainEnabled)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO memoryAfter {};
            if (VideoMemory(device, memoryAfter) && memoryAfter.CurrentUsage > memoryBefore.CurrentUsage)
                g_nr.measuredModelBytes = memoryAfter.CurrentUsage - memoryBefore.CurrentUsage;
            g_nr.creationFrame[0] = observedFrame;
            g_nr.passBuilt[0] = firstSettings;
        }

        if (g_nr.feature == nullptr)
        {
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            // Cast before formatting. These are ints, and "0x{:X}" on a negative int prints
            // 0x-452FFFFF, which no one can decode back to 0xBAD00001.
            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return false;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.reset = true;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running at {}x{}, guides {}x{} (preset {}, intensity {}, style {})", width, height,
                 guideWidth, guideHeight, g_nr.builtPreset, g_nr.builtIntensity, g_nr.builtStyle);
        if (!sourceIsTarget)
        {
            g_preCreation.Created(State::Instance().frameCount);
            LOG_INFO("DLSS-NR created before the upscaler; waiting for an observed present before evaluation");
        }

        // Creating and evaluating a feature in the same command list is the dice-roll that hung the
        // GPU (every crash died on a creation frame). The creation goes through the game's own submit
        // first; the first evaluate happens next frame. One frame without the model is invisible.
        device->Release();
        return false;
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return false;
    }

    if (!sourceIsTarget && !g_preCreation.Ready(State::Instance().frameCount))
    {
        reportSkip("before the upscaler: feature creation has not crossed a present yet");
        device->Release();
        return false;
    }

    if (chainEnabled && (observedFrame <= g_nr.creationFrame[0] || !DlssNr::Submission::Completed(g_nr.creation[0])))
    {
        reportSkip("chain feature creation awaits a later present and actual submitted GPU completion");
        device->Release();
        return false;
    }

    // Build at most one extra instance per stable observed frame. Creation is not evaluation.
    if (chainEnabled && passSnapshot.Count > 1)
    {
        for (unsigned i = 1; i < passSnapshot.Count; ++i)
        {
            if (g_nr.passFailed[i])
                break;
            if (g_nr.passFeature[i])
                continue;
            DXGI_QUERY_VIDEO_MEMORY_INFO budget {};
            const uint64_t bytes = uint64_t(workWidth) * workHeight * 16;
            if (!VideoMemory(device, budget) ||
                !DlssNr::Chain::Admit(budget.Budget, budget.CurrentUsage, g_nr.measuredModelBytes, bytes))
            {
                g_nr.passFailed[i] = true;
                g_nr.passBuilt[i] = passSnapshot.Settings[i];
                ChainStatus(
                    "Additional pass refused: adapter budget/headroom unavailable or insufficient (retry latched)");
                break;
            }
            if (!g_chainSchedule.Create(observedFrame))
                break;
            if (!g_nr.passPing)
                g_nr.passPing = CreateScratch(device, desc.Format, workWidth, workHeight);
            auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");
            if (!snippet)
                snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");
            g_nr.creation[i] = {};
            if (!g_nr.passPing || !snippet || !DlssNr::Submission::Track(cmdList, g_nr.creation[i]))
            {
                g_nr.passFailed[i] = true;
                g_nr.passBuilt[i] = passSnapshot.Settings[i];
                ChainStatus("Additional pass initialization failed (retry latched); last successful answer retained");
                break;
            }
            const auto& settings = passSnapshot.Settings[i];
            SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
            g_nr.passFeature[i] =
                g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device,
                            cmdList, g_nr.capabilityParams, workWidth, workHeight, (int) settings.Preset,
                            settings.Intensity, (int) settings.Style, settings.LocalStructure, settings.LocalTone,
                            settings.SkinStructure, settings.AutoMask ? 1 : 0, 1);
            g_nr.passBuilt[i] = settings;
            g_nr.creationFrame[i] = observedFrame;
            g_nr.passReset[i] = true;
            g_nr.passFailed[i] = !g_nr.passFeature[i];
            LOG_INFO("DLSS-NR chain: create pass {} result {}; budget={} usage={} measured-model={}; awaiting "
                     "submitted completion",
                     i + 1, g_nr.passFeature[i] ? "created" : "failed", budget.Budget, budget.CurrentUsage,
                     g_nr.measuredModelBytes);
            break;
        }
    }

    // The upscaler has just written this, so it is a UAV. The model needs it readable.
    // Whether the buffer the upscaler just wrote is linear HDR or an already tone-mapped picture is not
    // something to assume: the game says so, in the flags it created its own DLSS feature with. Running
    // the colour transform over a frame that has already been through a tonemapper is pure damage, and
    // skipping it on one that has not leaves the model reading ordinary values as enormously bright.
    // Both have to agree: the caller says what the game intends, the format says what the surface can
    // actually hold. A game that claims HDR while rendering into eight bits gets its frame encoded
    // twice otherwise.
    const bool gameSaysHdr = frame.ColourIsLinearHdr;
    const bool isHdrBuffer = gameSaysHdr && FormatCanHoldLinearHdr(desc.Format);

    static bool reportedHdr = false;

    if (!reportedHdr)
    {
        reportedHdr = true;
        LOG_INFO("DLSS-NR: the game's DLSS buffer is {} so the colour transform is {}",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped", isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return false;
    }

    // What the upscaler produces is linear HDR with an open-ended range; the model was trained on
    // finished, sRGB-encoded frames. The white point is what maps one to the other, and it is a property
    // of the game's exposure rather than a number worth asking anyone to guess: measured means of 0.065,
    // 1.8 and 185 have all been seen in this one game.
    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    // Paper white, and nothing else. The frame is divided by this and encoded, and the soft knee
    // above 0.75 takes whatever is left over.
    //
    // It used to be divided by a white point measured from the frame -- around 3 in Cyberpunk -- which
    // was right for the old composition, where the encode had to be inverted and highlights therefore
    // had to survive it. Under the composition this now uses it is actively wrong twice over: the
    // model is handed a picture three times darker than it should see, and the highlight branch is
    // defeated. That branch hands back `originalLuma - proxyLuma`, the headroom the proxy could not
    // represent -- it exists precisely because the proxy is meant to clip. Normalising the highlights
    // away first leaves it nothing to give back.

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_ngxTime == nullptr)
        g_ngxTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    // Fetch the game's exposure, where the game supplies one and the user asked for it.
    //
    // This used to measure the white point off the frame as well, over a 64x64 grid of tile
    // luminances. That is gone: the pass writes the frame it was measuring, so the divisor chased its
    // own output -- one Enshrouded session walked it from 0.010 to 97.910, and toggling NR at a fixed
    // spot read 41.31 off against 0.46 on. What remains dispatches a single thread to copy the game's
    // 1x1 exposure texture into tile 0. That is a courier, not a measurement, and cannot feed back.
    // Gated on the source the menu actually writes. This read the retired WhitePointFromExposure
    // flag while consumption keyed on WhitePointSource == 1, so choosing "the game's own exposure"
    // never dispatched the meter and the white point silently fell back to the slider.
    const bool exposureSettingOn = cfg.DlssNrWhitePointSource.value_or_default() == 1;

    // Nothing held from before the option was switched off may survive switching it back on. See
    // InvalidateExposureMeter for what froze and why it read as a colour cast.
    if (DlssNr::Exposure::InvalidateOnSourceChange(g_nr.exposureSettingWasOn,
                                                   cfg.DlssNrWhitePointSource.value_or_default()))
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    ID3D12Resource* const offeredExposure = static_cast<ID3D12Resource*>(frame.ExposureTexture);
    const bool usableExposure = exposureSettingOn && ExposureTextureUsable(device, offeredExposure) &&
                                offeredExposure != source && offeredExposure != target && offeredExposure != depth &&
                                offeredExposure != motion;
    const bool holdingColor = cfg.DlssNrHoldFrame.value_or_default();
    const bool wantExposure =
        DlssNr::Exposure::MeterWanted(cfg.DlssNrWhitePointSource.value_or_default(), usableExposure, holdingColor);
    static bool reportedInvalidExposure = false;
    if (exposureSettingOn && offeredExposure != nullptr && !usableExposure && !reportedInvalidExposure)
    {
        LOG_WARN("DLSS-NR exposure: rejected an incompatible or aliased Texture2D SRV; using CPU fallback");
        reportedInvalidExposure = true;
    }
    if (usableExposure)
        reportedInvalidExposure = false;

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;

        // One pixel. Only tile (0,0) is read back, and the tile-mean branch below it in the shader is
        // dead code the dispatch simply never reaches.
        meterParams.Width = 1;
        meterParams.Height = 1;

        if (sourceIsTarget)
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        {
            ReadResourceScope exposureRead(
                cmdList, static_cast<ID3D12Resource*>(frame.ExposureTexture),
                sourceIsTarget ? static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value_or(
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            DispatchPass(cmdList, meterParams, source, nullptr, nullptr, (ID3D12Resource*) frame.ExposureTexture,
                         nullptr, g_nr.meter, nullptr);
        }

        if (sourceIsTarget)
            Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    g_nr.gamePreExposure = DlssNr::Exposure::PreExposure(frame.PreExposure);

    float whitePoint = ResolveWhitePoint(cfg, isHdrBuffer);

    // Zero-latency exposure (D3D12, source 1): when the game hands us a live exposure texture, the
    // white point is recomputed in-shader every frame from it (ExposurePreMul / exposure) instead of
    // the 3-4 frame CPU meter readback. whitePoint above still rides along in gWhitePoint as the
    // fallback the shader uses if the live sample is missing or absurd. Bound at t4 (InPrevEdit) below.
    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    if (DlssNr::Exposure::LiveWanted(cfg.DlssNrWhitePointSource.value_or_default(), isHdrBuffer, usableExposure,
                                     holdingColor))
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        const float trim = DlssNr::Exposure::Trim(cfg.DlssNrWhitePointTrim.value_or_default());
        exposurePreMul = g_nr.gamePreExposure * trim;
        if (!std::isfinite(exposurePreMul))
        {
            exposureTex = nullptr;
            useGameExposure = 0;
            exposurePreMul = 0.0f;
        }
    }

    // Frame hold. Freeze the encode's input so a live setting change re-renders the same frame. This
    // is self-contained on purpose: it copies the output aside on hold-on and copies it BACK over the
    // live output before the encode reads it while held, so the encode's own path and barriers below
    // are untouched and the default (hold off) is byte-identical. See design/frame-hold.md.
    //
    // `target` is UAV here (normalised at entry, restored by the meter block above). The held copy is
    // left in COPY_SOURCE after capture and stays there for every restore.
    {
        const bool hold = cfg.DlssNrHoldFrame.value_or_default();

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();
            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth || td.Height != g_nr.heldHeight ||
                                     td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                // Hold-on (or the output changed shape under a hold): capture THIS frame, do not
                // restore -- target already holds the frame to freeze, and the pass runs on it.
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                // Held: restore the frozen frame onto the live output before the encode reads it.
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, g_nr.heldColor);
                Barrier(cmdList, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }

            // Suspend white-point measurement while held: use the snapshot so it cannot drift and
            // confound the comparison. (No-op on the capture frame, where the snapshot IS whitePoint.)
            if (g_nr.heldActive)
                whitePoint =
                    std::isfinite(g_nr.heldWhitePoint) && g_nr.heldWhitePoint > 0.0f ? g_nr.heldWhitePoint : whitePoint;
        }
        else if (g_nr.heldActive)
        {
            // Released: let go of the frozen frame and resume live input next frame.
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
        }
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    // A frame that is already display-referred is handed over untouched: the encode becomes a copy and
    // the resolve adds the model's edit back at full scale.
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.UseGameExposure = useGameExposure;
    encodeParams.ExposurePreMul = exposurePreMul;
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    // Match only takes effect once a fit exists; until then the table is empty and the shader would
    // read a curve of zeros, so it falls back to the plain proxy.
    encodeParams.Width = width;
    encodeParams.Height = height;

    // The encode reads the source's top-left width x height. That is the whole source on the
    // after-upscale path; on the before-upscale path the source may be a dynamic-resolution game's
    // full allocation, of which the render subrect -- the target's size -- is what was drawn.
    if (sourceIsTarget)
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    {
        ReadResourceScope exposureRead(cmdList, exposureTex,
                                       sourceIsTarget
                                           ? static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value_or(
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                           : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, encodeParams, source, nullptr, nullptr, nullptr, exposureTex, g_nr.colorCopy,
                     g_nr.hdrCopy);
    }

    if (sourceIsTarget)
        Barrier(cmdList, target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // The transitions double as the wait for the encode's writes.
    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RestoreResourceState colorRead { cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    // Measure the buffer's scale from the copy the encode just kept -- untouched, so there is no path
    // (Calibration pass removed: it produced only a menu suggestion nothing consumed, at the cost
    // of a 4096-thread dispatch, a readback and an nth_element every frame.)

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RestoreResourceState hdrRead { cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    // Below full resolution the model is shown a filtered shrink of the proxy; the edit it returns is
    // enlarged during the resolve while the frame underneath stays full size and untouched.
    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            // Supersample: enlarge the proxy to the larger working size with a real upscaling filter
            // (the Output Scaling upsampler) so the model sees a clean super-native input, rather than
            // the box minifier which only makes sense going down. colorCopy is NON_PIXEL_SHADER_RESOURCE
            // from the encode (SRV-ready); colorSmall is UNORDERED_ACCESS from last frame's resolve.
            // (Re)build the supersample scalers when missing or when the NR downscaler changed (the
            // filter is baked at construction). Both use NR's own DlssNrScalingDownscaler, independent
            // of Output Scaling, so the two can run different filters at once. superDown is built here
            // and used after the model (the down-leg below).
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler)
            {
                ParkNrScaler(g_nr.superUp);
                ParkNrScaler(g_nr.superDown);
                g_nr.nrScaler = nrScaler;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (g_nr.superUp != nullptr && g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                // Wanted to supersample but the upscaler was not available -- warn once; the box path
                // below can only enlarge blockily, so the user should know the clean path is off.
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            // Sub-native (or the upsampler could not be built): box-resample the proxy to the work size.
            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr, g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    RestoreResourceState smallRead { cmdList, reduced ? g_nr.colorSmall : nullptr,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    // Read the exposure scan's candidates on the pass's own command list, once a frame.
    if (!holdingColor)
        DlssNr::ExposureScan::Tick(device, cmdList);

    ReadResourceScope depthRead(cmdList, depth,
                                sourceIsTarget ? static_cast<D3D12_RESOURCE_STATES>(cfg.DepthResourceBarrier.value_or(
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ReadResourceScope motionRead(cmdList, motion,
                                 sourceIsTarget ? static_cast<D3D12_RESOURCE_STATES>(cfg.MVResourceBarrier.value_or(
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                                                : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    RestoreResourceState depthCloneRead { cmdList, depthIn != depth ? depthIn : nullptr,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                          D3D12_RESOURCE_STATE_COPY_DEST };
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);
    RestoreResourceState motionCloneRead { cmdList, motionIn != motion ? motionIn : nullptr,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                           D3D12_RESOURCE_STATE_COPY_DEST };

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";

        // Say which guide failed and why, because the two causes need opposite answers: a guide the
        // game never supplied is not ours to fix, while a supplied one that could not be cloned is a
        // gap in TypedGuideFormat or a resource the runtime refused to create. Without this the log
        // said only that something was unreadable, which fits both.
        const auto describe = [](ID3D12Resource* src, ID3D12Resource* out) -> std::string
        {
            if (src == nullptr)
                return "not supplied by the game";

            const auto fmt = (int) src->GetDesc().Format;

            if (out != nullptr)
                return std::format("ok (format {})", fmt);

            return std::format("supplied as typeless format {}, but the typed clone could not be "
                               "created (typed format {})",
                               fmt, (int) TypedGuideFormat(src->GetDesc().Format));
        };

        LOG_ERROR("DLSS-NR unavailable: depth {} | motion vectors {}", describe(depth, depthIn),
                  describe(motion, motionIn));

        device->Release();
        return false;
    }

    // The vectors were scaled to full-frame pixels; the image the model reprojects is the working size.
    // The vectors were scaled to full-frame pixels; the image the model reprojects is the
    // working size.
    const float mvToWork = width != 0 ? (float) workWidth / (float) width : 1.0f;

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

    // The proxy path, when asked for. Same inputs, same model -- the difference is who calls it.
    //
    // Nothing falls back automatically. A silent fallback would mean never finding out the proxy
    // path was broken: the picture would look right either way, because the forwarder would be
    // quietly doing the work.
    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, g_nr.output, workWidth, workHeight, guideWidth, guideHeight,
            g_nr.guideDepthInverted, g_nr.reset, g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);

        if (coverage != nullptr)
            coverage->ModelResult(proxyResult, workWidth, workHeight);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session", proxyResult,
                      NgxResultName(proxyResult));
        }

        device->Release();
        return false;
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->Start(cmdList);

    // Preserve the single/master call and anchor. Each extra layer has independent history.
    const auto firstCpuStart =
        chainEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
    int result;
    if (!chainEnabled)
    {
        result = g_nr.evaluate(cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, depthIn, motionIn, g_nr.output,
                               workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
                               g_nr.reset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
                               (int) cfg.DlssNrStyle.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                               cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                               cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWork,
                               g_nr.guideMvScaleY * mvToWork);
    }
    else
    {
        result = g_nr.evaluate(
            cmdList, g_nr.feature, g_nr.capabilityParams, modelInput, depthIn, motionIn, g_nr.output, workWidth,
            workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0, g_nr.reset ? 1 : 0,
            (chainEnabled ? firstSettings.Intensity : cfg.DlssNrIntensity.value_or_default()),
            (int) (chainEnabled ? firstSettings.Style : cfg.DlssNrStyle.value_or_default()),
            (chainEnabled ? firstSettings.LocalStructure : cfg.DlssNrLocalStructure.value_or_default()),
            (chainEnabled ? firstSettings.LocalTone : cfg.DlssNrLocalTone.value_or_default()),
            (chainEnabled ? firstSettings.SkinStructure : cfg.DlssNrSkinStructure.value_or_default()),
            (chainEnabled ? firstSettings.AutoMask : cfg.DlssNrAutoMask.value_or_default()) ? 1 : 0,
            g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);
    }

    if (chainEnabled && (g_frames % 120 == 0 || result != 1))
        LOG_INFO("DLSS-NR chain: pass 1 result=0x{:X}, dimensions={}x{}, CPU-call-ms={:.3f} (not GPU time)",
                 (unsigned) result, workWidth, workHeight,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - firstCpuStart).count());
    DlssNr::Chain::Routing<ID3D12Resource*> chain(modelInput, g_nr.output, g_nr.passPing);
    if (result == 1)
        chain.Success();
    if (chainEnabled && result == 1)
    {
        for (unsigned i = 1; i < passSnapshot.Count; ++i)
        {
            if (!g_nr.passFeature[i] || g_nr.passFailed[i] || observedFrame <= g_nr.creationFrame[i] ||
                !DlssNr::Submission::Completed(g_nr.creation[i]))
            {
                ChainStatus(g_nr.passFailed[i]
                                ? "Additional pass failed/admission refused; last successful answer retained"
                                : "Additional pass waiting for creation and submitted GPU completion; last successful "
                                  "answer retained");
                break;
            }
            auto* input = chain.Input();
            auto* answer = chain.Output();
            Barrier(cmdList, input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const auto& settings = passSnapshot.Settings[i];
            SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
            const auto startCpu = std::chrono::steady_clock::now();
            const int extraResult =
                g_nr.evaluate(cmdList, g_nr.passFeature[i], g_nr.capabilityParams, input, depthIn, motionIn, answer,
                              workWidth, workHeight, guideWidth, guideHeight, g_nr.guideDepthInverted ? 1 : 0,
                              (g_nr.reset || g_nr.passReset[i]) ? 1 : 0, settings.Intensity, (int) settings.Style,
                              settings.LocalStructure, settings.LocalTone, settings.SkinStructure,
                              settings.AutoMask ? 1 : 0, g_nr.guideMvScaleX * mvToWork, g_nr.guideMvScaleY * mvToWork);
            const double cpuMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startCpu).count();
            Barrier(cmdList, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (extraResult != 1)
            {
                g_nr.passFailed[i] = true;
                ParkNrFeature(g_nr.passFeature[i]);
                for (unsigned downstream = i; downstream < 3; ++downstream)
                    g_nr.passReset[downstream] = true;
                LOG_INFO("DLSS-NR chain: pass {} failed result=0x{:X}, completed={}, CPU-call-ms={:.3f}; preserving "
                         "previous answer",
                         i + 1, (unsigned) extraResult, chain.completed, cpuMs);
                break;
            }
            g_nr.passReset[i] = false;
            chain.Success();
            if (g_frames % 120 == 0)
                LOG_INFO("DLSS-NR chain: pass {} evaluated at {}x{}, CPU-call-ms={:.3f} (not GPU time)", i + 1,
                         workWidth, workHeight, cpuMs);
        }
    }
    ID3D12Resource* finalOutput = chain.answer;
    g_activePasses = chain.completed;
    if (chainEnabled && chain.completed == passSnapshot.Count)
        ChainStatus("All requested passes completed; one final composition");
    if (chainEnabled && (g_frames % 120 == 0 || chain.completed != passSnapshot.Count))
    {
        static unsigned previousRequested = 0, previousCompleted = 0;
        if (g_frames % 120 == 0 || previousRequested != passSnapshot.Count || previousCompleted != chain.completed)
            LOG_INFO("DLSS-NR chain: requested={} completed={} present={} stage={} status={}", passSnapshot.Count,
                     chain.completed, observedFrame, beforeUpscale ? "before" : "after", g_chainStatus.load());
        previousRequested = passSnapshot.Count;
        previousCompleted = chain.completed;
    }

    if (coverage != nullptr)
        coverage->ModelResult(result, workWidth, workHeight);

    if (g_ngxTime != nullptr)
        g_ngxTime->End(cmdList);

    g_nr.reset = false;

    // Supersampling probe: report the model working ABOVE native so a test log tells us whether NGX even
    // accepts a super-native evaluate and what it returns. Once per working-size change, or on any error.
    if (workWidth > width || workHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})", workWidth,
                     workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    // Once, a few seconds in, so it lands after the values have been written at least once.
    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        // At INFO, because whether the model actually took a value is the only way to tell a
        // control that does nothing from one that is not being written.
        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote, (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        // The preset is the last control whose arrival has never been checked, and three of them look
        // identical in play. Either it is not landing or the presets really are alike.
        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult = g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                  (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                  cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                  cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                  cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        // Resolve takes the difference between what the model returned and what it was shown, and adds
        // that back to the frame. At strength zero the result is what the upscaler produced, exactly, and
        // anything the model left alone is untouched rather than round-tripped through the curve.
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.UseGameExposure = useGameExposure;
        resolveParams.ExposurePreMul = exposurePreMul;
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.Transfer = cfg.DlssNrTransfer.value_or_default();
        resolveParams.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        // The numbers the composition actually ran with, logged when any of them changes.
        //
        // A colour report without these cannot be read. Paper white alone decides whether the model
        // was shown a sensible picture or a blown one, and it was absent from every log in the first
        // round of reports -- one tester's "much better at 16" had to be taken on trust because
        // nothing in the file said what the value was. Debug view and compare mode are here for the
        // same reason from the other direction: both change what is on screen, and a screenshot with
        // one left on is indistinguishable from a bug.
        struct ComposeReport
        {
            bool valid;
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
        };

        static ComposeReport loggedCompose {};

        // Quantised to the precision it is printed at. Comparing raw floats logged 2376 lines in one
        // Enshrouded session, because a measured white point drifts continuously and every drift was a
        // change. A line per meaningful change is the point; a line per frame is a different problem.
        const ComposeReport composeNow { true,
                                         std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
                                         resolveParams.TransferStrength,
                                         resolveParams.ColourStrength,
                                         resolveParams.MaxRatio,
                                         resolveParams.Passthrough,
                                         resolveParams.DebugView,
                                         resolveParams.CompareMode,
                                         resolveParams.Transfer,
                                         g_nr.workWidth,
                                         g_nr.workHeight };

        if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
            loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
            loggedCompose.maxRatio != composeNow.maxRatio || loggedCompose.passthrough != composeNow.passthrough ||
            loggedCompose.debugView != composeNow.debugView || loggedCompose.compareMode != composeNow.compareMode ||
            loggedCompose.residual != composeNow.residual || loggedCompose.workW != composeNow.workW ||
            loggedCompose.workH != composeNow.workH)
        {
            loggedCompose = composeNow;
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, colour transform {}, transfer {}, model {}x{}, debug view {}, compare {}",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW, composeNow.workH,
                     composeNow.debugView, composeNow.compareMode);
        }

        Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Supersampling down-leg. Average the Nx model answer back to native with the chosen filter, so
        // the resolve composites a native answer against the native proxy 1:1 -- a real area resample,
        // not the single bilinear tap the Nx answer would otherwise get in the resolve (which aliases
        // the model's detail into noise, the "noisier above 100%" the probe showed). On success the
        // resolve reads the native proxy (colorCopy) and native answer (outputNative); on failure it
        // falls back to the Nx pair. g_nr.output is NPSR here; outputNative is UAV from last frame.
        bool superDownOk = false;
        if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
            g_nr.superDown->Dispatch(cmdList, finalOutput, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        const auto resolved =
            DlssNr::Chain::Resolve(g_nr.colorCopy, modelInput, g_nr.outputNative, finalOutput, superDownOk);
        ID3D12Resource* resolveProxy = resolved.proxy;
        ID3D12Resource* resolveAnswer = resolved.answer;

        {
            ReadResourceScope exposureRead(
                cmdList, exposureTex,
                sourceIsTarget ? static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value_or(
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            wrote = DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, g_nr.hdrCopy, motionIn,
                                 exposureTex, target, nullptr);
        }
        Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (superDownOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // On-demand capture works in this path too: the staging copy still holds the frame as the
        // upscaler produced it, and the edited frame is the output itself. The write happens a few
        // frames later, once the GPU is certainly past these copies -- this path has no fence of its
        // own.
        if (g_capture.isActive())
        {
            // The pre-upscale capture compares the original HDR frame to the edited HDR copy.
            // Preserve the legacy capture source on the default after-upscale path.
            g_capture.record(cmdList, device, sourceIsTarget ? g_nr.colorCopy : g_nr.hdrCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    hdrRead.Restore();

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        // This path records into the game's own list, so there is no queue of ours to read from.
        // A caller that knows which queue the list goes to says so; otherwise the one the upscaler was
        // invoked on serves. The bridges have to say, because they run on a queue of their own that
        // State never learns about -- a Vulkan game creates no D3D12 swapchain, so nothing ever sets
        // currentCommandQueue and the cost went unreported.
        auto* queue =
            timingQueue != nullptr ? timingQueue : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

        if (queue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime(queue); ms.has_value())
                g_lastGpuTime = ms;

            if (g_ngxTime != nullptr)
            {
                if (auto ngx = g_ngxTime->ReadGpuTime(queue); ngx.has_value())
                    g_lastNgxTime = ngx;
            }

            // The split, once every few hundred frames. What is worth reading is not the total but the
            // remainder: the model's cost is NVIDIA's to set, and everything else is ours.
            static unsigned long long lastSplitLog = 0;

            if (g_lastGpuTime.has_value() && g_lastNgxTime.has_value() && g_frames - lastSplitLog > 600)
            {
                lastSplitLog = g_frames;
                const double total = g_lastGpuTime.value();
                const double ngx = g_lastNgxTime.value();
                LOG_INFO("DLSS-NR cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms ours ({:.0f}% ours)", total, ngx,
                         total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
            }
        }
    }

    depthCloneRead.Restore();
    motionCloneRead.Restore();

    smallRead.Restore();

    // Leave the staging copy as the next frame expects to find it.
    colorRead.Restore();

    // restoreOutput hands the output back in its arrival state on every exit.
    device->Release();
    return wrote;
}

namespace DlssNr
{
void RetryAfterFailure()
{
    std::lock_guard<std::mutex> preLock(g_preMutex);
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    g_preAllocationFailed = false;
    g_preExtent.Reset();
    g_nr.failed = false;
    for (auto& failed : g_nr.passFailed)
        failed = false;
    g_nr.reason = "";
    g_nr.reset = true;
}

// What the parameter block says about this frame, read the same way whichever side of the upscaler
// the model runs on. Everything here is a property of how the game encodes its buffers, and both
// paths read the block the game filled for the upscaler.
// Diagnostic-only contract: allocation size is not evidence of the rendered subrect.
void ReportSpatialContract(NVSDK_NGX_Parameter* params, ID3D12Resource* color, ID3D12Resource* output, bool before,
                           unsigned int inputWidth, unsigned int inputHeight)
{
    struct Contract
    {
        uint64_t colorWidth = 0, outputWidth = 0;
        unsigned colorHeight = 0, outputHeight = 0, subWidth = 0, subHeight = 0, modelWidth = 0, modelHeight = 0;
        bool haveWidth = false, haveHeight = false;
        float scale = 1.0f;
        bool operator==(const Contract&) const = default;
    } now;
    if (color)
    {
        const auto desc = color->GetDesc();
        now.colorWidth = desc.Width;
        now.colorHeight = desc.Height;
    }
    if (output)
    {
        const auto desc = output->GetDesc();
        now.outputWidth = desc.Width;
        now.outputHeight = desc.Height;
    }
    now.haveWidth = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &now.subWidth) ==
                    NVSDK_NGX_Result_Success;
    now.haveHeight = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &now.subHeight) ==
                     NVSDK_NGX_Result_Success;
    now.scale = Config::Instance()->DlssNrWorkingScale.value_or_default();
    // Match Dispatch's clamping for logging only; never feed diagnostic values back to rendering.
    if (std::isfinite(now.scale))
    {
        now.scale = std::clamp(now.scale, 0.25f, 2.0f);
        now.modelWidth = static_cast<unsigned>(inputWidth * now.scale + 0.5f);
        now.modelHeight = static_cast<unsigned>(inputHeight * now.scale + 0.5f);
    }
    else
        now.scale = 0;
    static std::mutex mutex;
    static Contract previous[2];
    static bool seen[2] {};
    std::lock_guard<std::mutex> lock(mutex);
    const unsigned stage = before ? 1 : 0;
    if (seen[stage] && previous[stage] == now)
        return;
    seen[stage] = true;
    previous[stage] = now;
    LOG_INFO("DLSS-NR spatial contract stage={}: Color-allocation={}x{}, render-subrect={}x{} (width-query={}, "
             "height-query={}), "
             "game-output={}x{}, model-request={}x{}, WorkingScale={:.3f}",
             before ? "before" : "after", now.colorWidth, now.colorHeight, now.subWidth, now.subHeight,
             now.haveWidth ? "success" : "absent/failed", now.haveHeight ? "success" : "absent/failed", now.outputWidth,
             now.outputHeight, now.modelWidth, now.modelHeight, now.scale);
    if (before && now.outputWidth && now.outputHeight && now.modelWidth >= now.outputWidth &&
        now.modelHeight >= now.outputHeight)
        LOG_INFO("DLSS-NR spatial contract: no spatial reduction before the upscaler; this does not establish DLAA. "
                 "Check render-subrect query presence and WorkingScale; Color allocation alone cannot establish "
                 "rendered resolution");
}

DlssNrFrameInfo GatherFrame(NVSDK_NGX_Parameter* params)
{
    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;

    // The game telling the upscaler to forget everything it has accumulated: a cut, a teleport, a
    // load. Every upscaler in this tree reads it and this pass did not, so the model's history was
    // only ever reset by things that happened to us -- a resize, a rebuild, a recovery from failure
    // -- and never by anything that happened in the game. Across a cut the model was reprojecting
    // the previous scene onto the new one and being asked to reconcile them.
    //
    // Read the same way FFXFeature_Dx12 reads it, including leaving it alone when the parameter is
    // absent: a game that never sets it is not asking for a reset every frame.
    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    // How much of the guides is real. See DlssNrFrameInfo -- zero means the game did not say.
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    // What the game says about its own exposure. Logged, used for nothing yet.
    //
    // The white point measured from the frame turned out to be a control loop rather than a
    // measurement: the pass writes into the buffer it reads, most games adapt their exposure to the
    // finished frame, and the two chase each other -- 0.01 to 97.9 in one Enshrouded session. Any
    // statistic taken from a frame we modify has that problem.
    //
    // These do not. DLSS.Pre.Exposure is the scale the game applied before handing the buffer over,
    // and ExposureTexture is a 1x1 the game fills with the exposure it is using; both are the game's
    // own numbers, decided upstream of anything here. Whether either is close to the divisor the model
    // actually wants is unknown, which is why this only prints them.
    //
    // The auto-exposure flag decides whether the texture means anything: with it set the game is
    // telling DLSS to work exposure out for itself and may supply nothing. OptiScaler forces that flag
    // on for eighteen games, so it is logged too -- reading a value whose flag has been overridden is
    // how the debug views lied earlier tonight.
    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        DlssNr::Detail::ColorBinding<NVSDK_NGX_Parameter, ID3D12Resource> exposureBinding;
        auto* exposureTex = exposureBinding.Read(params, NVSDK_NGX_Parameter_ExposureTexture, NVSDK_NGX_Result_Success);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre ? DlssNr::Exposure::PreExposure(preExposure) : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        struct ExposureReport
        {
            bool valid;
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;
        };

        static ExposureReport logged {};
        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        // The value itself, once it has come back off the GPU. Separate from the line above because
        // that one says what the game offers and this one says what it actually reads -- and because
        // the reading arrives three frames after the offer.
        static float loggedExposure = -1.0f;

        if (g_nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - g_nr.gameExposure) > std::max(0.02f * g_nr.gameExposure, 1e-5f))
        {
            loggedExposure = g_nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        // The scan's number, on the same cadence, so one log carries both.
        //
        // This is the whole validation. In a game that hands over an exposure texture there is a
        // known-correct value; if the scan's candidate tracks it, the scan found the right buffer
        // rather than merely a moving one, and can be trusted where a game hands over nothing.
        // Comparing two numbers after the fact needs both written down, and until now the scan's
        // value existed only in a menu nobody can read while playing.
        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            static float loggedScan = -1.0f;

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                if (g_nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, g_nr.gameExposure, scanned / g_nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    return frame;
}

// Reads the game's parameter block and runs the pass on what it finds.
//
// This is the call site's job, not the pass's. A caller that has the resources in hand -- a
// reprojection stage, a frame generation path, anything that is not the upscaler seam -- calls
// RunPass directly and never touches an NGX parameter block.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue, bool preUpscaleDeclined)
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    CoverageScope coverage(Config::Instance()->DlssNrStage.value_or_default() != 1 ? 0 : preUpscaleDeclined ? 2 : -1);

    if (cmdList == nullptr || params == nullptr)
    {
        coverage.sample.reason = "no command list or parameter block";
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    // Stage 1 runs the model before the upscaler, and this is after it. The caller says whether the
    // pre-upscale scope it wrapped around this same evaluate declined it for a reason this path can
    // serve -- ray reconstruction, a colour it could not copy -- and only then does this path run
    // under stage 1. Otherwise the model would run twice a frame, on both sides of the upscaler.
    //
    // The answer travels with the evaluate rather than through a global on purpose. Jedi Survivor's
    // frame generation evaluates on its own command list, interleaved with the upscaler's, and a
    // global flag written by one evaluate was read by the other: the after-upscale pass ran, the
    // model was rebuilt at display size, and rebuilt again at render size the next frame.
    if (Config::Instance()->DlssNrStage.value_or_default() == 1 && !preUpscaleDeclined)
    {
        ReportSkipOnce("stage 1: the model runs before the upscaler, so the after-upscale pass stands down");
        return;
    }

    // Which of the game's APIs this evaluate arrived through.
    //
    // Says out loud what was previously only reasoned about: an FSR or XeSS title reaches this pass
    // transitively, because those shims call OptiScaler's own NVSDK_NGX_D3D12_EvaluateFeature and
    // this pass hangs off that. Nothing needed adding to the shims -- a call there would run the
    // model twice -- but "nothing needed adding" is a claim, and this is the line that checks it.
    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    // Without all three there is nothing to run on. This is not a failure -- some evaluates legitimately
    // carry none of it -- so it stays quiet and tries again next frame.
    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        coverage.sample.reason = "missing output, depth or motion vectors";
        ReportSkipOnce(target == nullptr  ? "the parameters carried no output texture"
                       : depth == nullptr ? "the parameters carried no depth"
                                          : "the parameters carried no motion vectors");
        return;
    }

    const DlssNrFrameInfo frame = GatherFrame(params);
    const auto contractOutput = target->GetDesc();
    ReportSpatialContract(params, GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color"), target, false,
                          static_cast<unsigned>(contractOutput.Width), contractOutput.Height);

    // The upscaler's inputs are at render resolution while colour and output are at display
    // resolution; the model takes that as a subrect per resource, which the pass reads from the
    // resources themselves.
    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        coverage.sample.reason = "output has no D3D12 device";
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    // The pass is the object, so the caller holds it. Built once, on the device the frame is on.
    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    device->Release();

    if (g_compose == nullptr)
    {
        coverage.sample.reason = "composition pass unavailable";
        ReportSkipOnce("the pass could not be created");
        return;
    }

    const auto desc = target->GetDesc();
    coverage.sample.width = static_cast<uint32_t>(desc.Width);
    coverage.sample.height = desc.Height;
    coverage.sample.applied =
        g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue, &coverage.sample);
    if (coverage.sample.applied)
        coverage.sample.reason = "composition recorded into upscaler output";
}

// ---------------------------------------------------------------------------------------------
// Before the upscaler.
// ---------------------------------------------------------------------------------------------

ScopedPreUpscale::ScopedPreUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool applies,
                                   ID3D12CommandQueue* timingQueue)
    : _cmdList(cmdList), _params(params)
{
    const Config& cfg = *Config::Instance();

    if (!cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrStage.value_or_default() != 1)
        return;

    CoverageScope coverage(1);
    if (cmdList == nullptr || params == nullptr)
    {
        coverage.sample.reason = "no command list or parameter block";
        return;
    }

    _lock = std::unique_lock<std::mutex>(g_preMutex, std::try_to_lock);
    if (!_lock.owns_lock())
    {
        coverage.sample.reason = "nested or concurrent scratch loan";
        return; // Nested or concurrent evaluate: never swap a scratch another scope is lending.
    }

    // Declining hands the evaluate to the after-upscale path; a plain return does not. The
    // distinction is whether the model could still do a sensible job of this frame from the other
    // side, and for ray reconstruction it can: the finished frame is denoised, the input is not.
    // The answer is kept on this scope, so it can only ever reach the after-upscale call for the
    // same evaluate; see EvaluateAfterUpscale for the frame-generation interleaving that made a
    // global flag wrong.
    const auto report = [&coverage](const char* why)
    {
        coverage.sample.reason = why;
        g_preStatus = why;
        ReportSkipOnce(why);
    };
    auto decline = [this, &report, &coverage](const char* why)
    {
        coverage.sample.fallback = true;
        _declined = true;
        g_preStatus = why;
        report(why);
    };

    if (!applies)
    {
        decline("before the upscaler: ray reconstruction's colour is undenoised, so the after-upscale "
                "pass serves those evaluates");
        return;
    }

    if (cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrUseProxy.value_or_default())
    {
        decline("before the upscaler: Hold frame and the driver proxy use the after-upscale path");
        return;
    }

    if (State::Instance().frameCount == 0)
    {
        decline("before the upscaler: no observed present boundary; using the after-upscale path");
        return;
    }

    // The colour, and how the block stored it. NVIDIA's block keeps typed and untyped slots apart,
    // OptiScaler's does not; the swap and its undo have to use whichever the game or the bridge used,
    // or the upscaler reads the original from the other slot and nothing changes.
    ID3D12Resource* colour = _color.Read(params, NVSDK_NGX_Parameter_Color, NVSDK_NGX_Result_Success);

    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (colour == nullptr || depth == nullptr || motion == nullptr)
    {
        report(colour == nullptr  ? "before the upscaler: the parameters carried no colour"
               : depth == nullptr ? "before the upscaler: the parameters carried no depth"
                                  : "before the upscaler: the parameters carried no motion vectors");
        return;
    }

    // What was drawn, as opposed to how big the texture is. A dynamic-resolution game renders into
    // the corner of a buffer allocated once at its largest, and the upscaler is told the drawn size
    // through the render subrect. The copy is that size: the model then works on the picture and
    // not the margin, and the upscaler reads the copy with the same subrect it would have read the
    // original with.
    const D3D12_RESOURCE_DESC colourDesc = colour->GetDesc();
    unsigned int width = (unsigned int) colourDesc.Width;
    unsigned int height = colourDesc.Height;

    {
        unsigned int subW = 0, subH = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &subW);
        params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &subH);

        if ((subW == 0) != (subH == 0))
        {
            decline("before the upscaler: incomplete render subrect dimensions");
            return;
        }
        if (subW != 0 && subH != 0)
        {
            if (subW > width || subH > height)
            {
                decline("before the upscaler: render subrect exceeds the color texture");
                return;
            }
            width = subW;
            height = subH;
        }
    }

    // The copy starts at the origin, so the game's colour has to as well. A game that offsets its
    // colour subrect would need the copy offset the same way, which nothing here does yet.
    {
        unsigned int baseX = 0, baseY = 0;
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
        params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);

        if (baseX != 0 || baseY != 0)
        {
            decline("before the upscaler: the game's colour subrect does not start at the origin, "
                    "which the copy does not follow yet; the after-upscale pass serves instead");
            return;
        }
    }

    if (colourDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || colourDesc.SampleDesc.Count != 1 ||
        colourDesc.DepthOrArraySize != 1 || (colourDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) ||
        colourDesc.Width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || IsTypeless(colourDesc.Format))
    {
        decline("before the upscaler: unsupported color texture contract (including ambiguous typeless color)");
        return;
    }

    coverage.sample.width = width;
    coverage.sample.height = height;
    const DXGI_FORMAT format = colourDesc.Format;

    if (IsSrgb(format) || width == 0 || height == 0)
    {
        decline("before the upscaler: the game's colour is an sRGB surface, which cannot be written as "
                "a UAV or read back as linear; the after-upscale pass serves instead");
        return;
    }

    ID3D12Device* device = nullptr;

    if (FAILED(colour->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        report("before the upscaler: the colour belongs to no D3D12 device");
        return;
    }

    if (g_preDevice && g_preDevice != device)
    {
        device->Release();
        // The existing NR state is also device-bound. Neither stage can safely reuse it here.
        report("before the upscaler: device changed; restart the session before enabling NR");
        return;
    }

    if (!g_preExtent.Observe(width, height, std::chrono::steady_clock::now(), static_cast<uint32_t>(format)))
    {
        device->Release();
        g_preStatus = "Waiting for a stable render resolution (500 ms); original color retained";
        report(g_preStatus.load());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_nrMutex);

        if (WantsTrackedRecording(cfg))
            _recording = std::make_unique<Chain::RecordingLease>(g_recordings, reinterpret_cast<uintptr_t>(cmdList));
        if (!TrackNrRecording(cmdList, cfg, _recording.get()))
        {
            device->Release();
            report(g_chainStatus.load());
            return;
        }

        if (g_pre.width != width || g_pre.height != height || g_pre.format != format)
        {
            // Parked, not released: the upscaler read it a frame or two ago and the GPU may still
            // be there.
            ParkNrResource(g_pre.scratch);
            g_preAllocationFailed = false;
        }

        if (g_pre.scratch == nullptr && !g_preAllocationFailed)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
                !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
                !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) ||
                !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                device->Release();
                decline("before the upscaler: color format lacks required shader, render-target or UAV support");
                return;
            }

            g_pre.scratch = CreatePreUpscaleScratch(device, format, width, height);
            g_pre.width = width;
            g_pre.height = height;
            g_pre.format = format;
            g_preAllocationFailed = g_pre.scratch == nullptr;
            if (g_pre.scratch != nullptr)
                g_preDevice = device;

            if (g_pre.scratch != nullptr)
                LOG_INFO("DLSS-NR before the upscaler: the model works on the {}x{} render-size colour "
                         "(format {}, texture {}x{}); the upscaler is handed the edited copy",
                         width, height, (int) format, (unsigned int) colourDesc.Width, colourDesc.Height);
        }
    }

    if (g_pre.scratch == nullptr)
    {
        device->Release();
        decline("before the upscaler: the working copy could not be allocated; the after-upscale pass "
                "serves instead");
        return;
    }

    DlssNrFrameInfo frame = GatherFrame(params);
    ReportSpatialContract(params, colour, GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output"), true, width,
                          height);
    if (colour == depth || colour == motion || depth == motion ||
        (frame.ExposureTexture &&
         (frame.ExposureTexture == colour || frame.ExposureTexture == depth || frame.ExposureTexture == motion)))
    {
        device->Release();
        decline("before the upscaler: aliased color, guide or exposure inputs are unsupported");
        return;
    }
    if (!ExposureTextureUsable(device, static_cast<ID3D12Resource*>(frame.ExposureTexture)))
        frame.ExposureTexture = nullptr;
    frame.OutputState = (int) D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // The colour is read the way NGX reads it, in NON_PIXEL_SHADER_RESOURCE. A game that leaves it
    // elsewhere says so through ColorResourceBarrier, and OptiScaler's upscalers move it from that
    // state before reading and back after. They will not see the game's colour this frame -- they
    // see the copy -- so the move is made here for the game's colour, and the copy is handed over
    // resting in the same state the game's colour rests in, so that their move applies to it.
    const D3D12_RESOURCE_STATES colourRest = cfg.ColorResourceBarrier.has_value()
                                                 ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
                                                 : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    const unsigned int colorStates = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE |
                                     D3D12_RESOURCE_STATE_COPY_DEST;
    if ((static_cast<unsigned int>(colourRest) & ~colorStates) != 0)
    {
        device->Release();
        decline("before the upscaler: unsupported color arrival state");
        return;
    }

    struct ReadScope
    {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES arrival;
        ReadScope(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a)
            : cmd(c), resource(r), arrival(a)
        {
            if (resource)
                Barrier(cmd, resource, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        ~ReadScope()
        {
            if (resource)
                Barrier(cmd, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        }
    };
    // Each game resource is restored even on a build/skip/failed dispatch. ReadableGuide sees the
    // NPSR state it expects; the upscaler receives its original configured arrival states afterward.
    ReadScope colorRead(cmdList, colour, colourRest);
    const auto depthRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.DepthResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto motionRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.MVResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    const auto exposureRest = static_cast<D3D12_RESOURCE_STATES>(
        cfg.ExposureResourceBarrier.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    ReadScope depthRead(cmdList, depth, depthRest);
    ReadScope motionRead(cmdList, motion, motionRest);
    ReadScope exposureRead(cmdList, static_cast<ID3D12Resource*>(frame.ExposureTexture), exposureRest);

    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);

    device->Release();

    bool wrote = false;

    if (g_compose != nullptr)
        wrote = g_compose->Dispatch(cmdList, colour, depth, motion, g_pre.scratch, frame, timingQueue, &coverage.sample,
                                    _recording.get());

    // Nothing written means nothing to swap: the copy holds stale or no picture, and the upscaler
    // gets the game's own colour, which is the frame as it would have been without the pass. This is
    // every frame until the model is built, and any frame the pass skips.
    if (!wrote)
    {
        report(coverage.sample.reason);
        return;
    }

    Barrier(cmdList, g_pre.scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, colourRest);

    _scratch = g_pre.scratch;
    _color.Swap(_scratch);
    _scratchState = (int) colourRest;
    _swapped = true;
    coverage.sample.applied = true;
    coverage.sample.reason = "composition recorded and edited color selected for upscaler";
    g_preStatus = "Before the upscaler: edited render-resolution color supplied";
}

ScopedPreUpscale::~ScopedPreUpscale()
{
    if (!_swapped)
        return;

    // The game's colour goes back before the block is reused, and the copy goes back to rest where
    // the next frame's pass expects to find it. The list is still recording: the scope closes right
    // after the evaluate it wrapped, before anything is submitted.
    _color.Restore();

    if (_scratch != nullptr)
        Barrier(_cmdList, _scratch, (D3D12_RESOURCE_STATES) _scratchState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The pass. Resources in, nothing read from anywhere the caller cannot see.

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    // Every other entry point in this file takes the lock before touching g_nr; this one was reaching
    // EnsureForwarder without it.
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    // Opt in only. See the note on DlssNrProbeD3D11: this is the one call in the pass that reaches
    // into a subsystem on the game's own device rather than reading something we already hold.
    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(g_nr.forwarder,
                                                                                                 "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    // Four bits, one per entry point: init 1, create 2, evaluate 4, release 8.
    const int bits = probe(snippet->wstring().c_str());

    // And the question NGX has an API for. Asked first because it creates nothing: if the feature
    // declines D3D11 here, that is the feature's own answer rather than our reading of a failed init.
    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        // The adapter the game is actually running on. Without it the query answers
        // AdapterUnsupported, which looks like a verdict on the hardware and is really a verdict on
        // the question -- that is what the first attempt got, on a 5080.
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            factory->EnumAdapters(0, &adapter);

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0     ? "SUPPORTED"
                              : (supported & 16) ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)  ? "AdapterUnsupported"
                              : (supported & 2)  ? "DriverVersionUnsupported"
                              : (supported & 8)  ? "OSVersionBelowMinimum"
                              : (supported & 1)  ? "CheckNotPresent"
                                                 : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})", bits,
             (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    // Four ways of asking, since the feature has already said it supports this platform.
    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        // Deliberately not "so the bridge is required". GetFeatureRequirements answers 0x0 SUPPORTED
        // with a minimum architecture this card meets, so the platform is not the obstacle and saying
        // otherwise here would be printing a conclusion the evidence does not carry.
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

// What the game offers by way of exposure, and what has been read from it. For the menu, so a user
// can see whether this game supplies one at all without having to read a log.
ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    return s;
}

std::optional<double> LastGpuTime() { return g_lastGpuTime; }
const char* BeforeUpscaleStatus() { return g_preStatus.load(); }
unsigned int ActivePassCount() { return g_activePasses.load(); }
const char* MultipassStatus() { return g_chainStatus.load(); }

void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void Shutdown()
{
    std::lock_guard<std::mutex> preLock(g_preMutex);
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    if (g_tracked)
    {
        TickNrRetired();
        const bool pendingRetired =
            std::any_of(g_nrRetired.begin(), g_nrRetired.end(), [](const NrRetired& retired)
                        { return retired.tracked && !DlssNr::Submission::Ready(retired.usage); });
        if (!DlssNr::Submission::Ready(g_usage) || pendingRetired)
        {
            LOG_INFO("DLSS-NR shutdown: retained all objects because tracked recordings are unsealed, pending or "
                     "unknown; no unsafe release");
            return;
        }
    }

    for (auto& r : g_nrRetired)
    {
        delete r.scaler;
        if (r.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();
    }

    g_nrRetired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;

    for (void*& f : g_nr.passFeature)
    {
        if (f != nullptr && g_nr.release != nullptr)
            g_nr.release(f);

        f = nullptr;
    }

    if (g_nr.passPing)
    {
        g_nr.passPing->Release();
        g_nr.passPing = nullptr;
    }

    if (g_pre.scratch != nullptr)
    {
        g_pre.scratch->Release();
        g_pre.scratch = nullptr;
    }

    if (g_nr.output != nullptr)
    {
        g_nr.output->Release();
        g_nr.output = nullptr;
    }

    if (g_nr.colorCopy != nullptr)
    {
        g_nr.colorCopy->Release();
        g_nr.colorCopy = nullptr;
    }

    if (g_nr.hdrCopy != nullptr)
    {
        g_nr.hdrCopy->Release();
        g_nr.hdrCopy = nullptr;
    }

    if (g_nr.colorSmall != nullptr)
    {
        g_nr.colorSmall->Release();
        g_nr.colorSmall = nullptr;
    }

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    if (g_nr.outputNative != nullptr)
    {
        g_nr.outputNative->Release();
        g_nr.outputNative = nullptr;
    }

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    // The slots these flags describe have just been released, so nothing may vouch for what the next
    // buffers happen to contain. gameExposure is deliberately NOT cleared here: a recreate is a
    // transition within the same scene, and dropping to the slider for a few frames would be the
    // flicker the held value exists to prevent. The user switching the option off is the case where
    // the held value has to go, and that is handled at the edge in Dispatch.
    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    g_nr.meterFrames = 0;

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }

    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    g_gpuTime.reset();
    g_ngxTime.reset();
    g_lastNgxTime.reset();
    g_lastGpuTime.reset();

    g_compose.reset();
}
} // namespace DlssNr
