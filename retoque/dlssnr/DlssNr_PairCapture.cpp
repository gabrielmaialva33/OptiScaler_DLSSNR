#include "pch.h"

// Same-frame comparison capture: the D3D12 backend and the writer thread (docs/dlssnr/design/comparison-capture.md).
//
// The render thread records two copies on the list that ran the pass and remembers a ticket; it polls the ticket
// once a dispatch and, once the GPU is proven past the copies, hands the readbacks to the writer thread. The writer
// maps them, converts, writes the files under temporary names, renames them, and lets go of its references. The
// render thread keeps its own reference until the list that carried the copies can never run again, so nothing is
// freed under the GPU. Nothing on the render thread waits for the GPU, maps, or touches a file.

#include "DlssNr_PairCaptureDx12.h"
#include "DlssNr_PairCapture.h"
#include "DlssNr_ImageFile.h"
#include "DlssNr_Readback.h"
#include "DlssNr_Submission.h"
#include "DlssNr_NgxInfo.h"
#include "DlssNrFeature_Dx12.h"

#include <BuildInfo.h>
#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <Util.h>
#include <proxies/Dxgi_Proxy.h>

#include <dxgi1_4.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace ImageDxgi = DlssNr::ImageFile::Dxgi;
static_assert(ImageDxgi::R32G32B32A32_TYPELESS == DXGI_FORMAT_R32G32B32A32_TYPELESS);
static_assert(ImageDxgi::R32G32B32A32_FLOAT == DXGI_FORMAT_R32G32B32A32_FLOAT);
static_assert(ImageDxgi::R16G16B16A16_TYPELESS == DXGI_FORMAT_R16G16B16A16_TYPELESS);
static_assert(ImageDxgi::R16G16B16A16_FLOAT == DXGI_FORMAT_R16G16B16A16_FLOAT);
static_assert(ImageDxgi::R10G10B10A2_TYPELESS == DXGI_FORMAT_R10G10B10A2_TYPELESS);
static_assert(ImageDxgi::R10G10B10A2_UNORM == DXGI_FORMAT_R10G10B10A2_UNORM);
static_assert(ImageDxgi::R11G11B10_FLOAT == DXGI_FORMAT_R11G11B10_FLOAT);
static_assert(ImageDxgi::R8G8B8A8_TYPELESS == DXGI_FORMAT_R8G8B8A8_TYPELESS);
static_assert(ImageDxgi::R8G8B8A8_UNORM == DXGI_FORMAT_R8G8B8A8_UNORM);
static_assert(ImageDxgi::R8G8B8A8_UNORM_SRGB == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
static_assert(ImageDxgi::B8G8R8A8_UNORM == DXGI_FORMAT_B8G8R8A8_UNORM);
static_assert(ImageDxgi::B8G8R8A8_TYPELESS == DXGI_FORMAT_B8G8R8A8_TYPELESS);
static_assert(ImageDxgi::B8G8R8A8_UNORM_SRGB == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

namespace DlssNr::PairCapture
{
namespace
{
constexpr const wchar_t* kDirectoryName = L"retoque-captures";

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Everything the writer needs besides the readbacks. The record carries no path; the directory travels beside it.
struct CapturePayload
{
    Record record;
    ImageFile::Plan plan;
    std::filesystem::path directory;
    std::string exe;
    UtcStamp stamp;
    LUID luid {};
};

struct Job
{
    capture::Shot shots[2];
    CapturePayload payload;
    Completion completion = Completion::AssumedFrames;
};

class Writer
{
  public:
    // Starts the thread on first use. False, with nothing taken, when it cannot.
    bool Push(Job&& job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_)
        {
            try
            {
                // Detached and never joined: the writer and this object live for the process. Joining at static
                // destruction would run under the loader lock, and a joinable std::thread destroyed there calls
                // std::terminate. A write cut short by the process ending leaves a ".tmp", never a final name.
                std::thread([this] { Run(); }).detach();
            }
            catch (...)
            {
                return false;
            }
            started_ = true;
        }
        jobs_.push_back(std::move(job));
        wake_.notify_one();
        return true;
    }

  private:
    void Run();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool started_ = false;
};

// Never destroyed: the session is touched by the menu, the render thread and the writer, and the writer outlives
// static destruction. The same reason g_presentMotion is heap-allocated in DlssNr_Dx12.cpp.
Writer& TheWriter()
{
    static Writer* writer = new Writer();
    return *writer;
}

std::atomic<bool> g_directoryFailed { false };
std::atomic<uint32_t> g_seq { 0 };

// Set by the first request. Until then nothing of the capture exists -- not even the session object -- and the pass's
// two calls are one relaxed load each (comparison-capture.md, section 4.7).
std::atomic<bool> g_used { false };

struct Dx12Backend
{
    using Readback = capture::Shot;
    using Ticket = DlssNr::Submission::Usage;
    using Payload = CapturePayload;

    struct Recording
    {
        ID3D12GraphicsCommandList* cmd = nullptr;
        ID3D12Device* device = nullptr;
        ID3D12Resource* surface[2] = {};
        D3D12_RESOURCE_STATES state[2] = {};
    };

    bool Alloc(Recording& recording, int which, Readback& out)
    {
        const D3D12_RESOURCE_DESC desc = recording.surface[which]->GetDesc();
        return capture::AllocReadback(recording.device, desc, out);
    }

    // Before any copy is recorded, as Submission::Track requires. Where it refuses, the record says the completion
    // was assumed, and the log says why the list could not be followed.
    bool Track(Recording& recording, Ticket& ticket)
    {
        if (DlssNr::Submission::Track(recording.cmd, ticket))
            return true;
        const char* why = DlssNr::Submission::LastRefusal();
        LOG_INFO(
            "DLSS-NR capture: the pass's list cannot be followed to its completion ({}); the copies are read after "
            "{} of the pass's dispatches, an assumption the record names (completion=assumed-8-frames)",
            why != nullptr ? why : "unknown", kAssumedPolls);
        return false;
    }

    void Copy(Recording& recording, int which, Readback& readback)
    {
        capture::CopyToReadback(recording.cmd, recording.surface[which], recording.state[which], readback);
    }

    bool Completed(const Ticket& ticket) { return DlssNr::Submission::Completed(ticket); }
    bool Ready(const Ticket& ticket) { return DlssNr::Submission::Ready(ticket); }

    void Release(Readback& readback)
    {
        if (readback.readback != nullptr)
            readback.readback->Release();
        readback = {};
    }

    bool Hand(Readback (&readbacks)[2], CapturePayload&& payload, Completion completion)
    {
        Job job;
        for (int i = 0; i < 2; ++i)
        {
            job.shots[i] = readbacks[i];
            if (job.shots[i].readback != nullptr)
                job.shots[i].readback->AddRef(); // the writer's own; the session keeps its reference
        }
        job.payload = std::move(payload);
        job.completion = completion;
        if (TheWriter().Push(std::move(job)))
            return true;

        // Not taken: drop the references just added. The session still holds its own, so nothing is freed here.
        for (int i = 0; i < 2; ++i)
            if (job.shots[i].readback != nullptr)
                job.shots[i].readback->Release();
        return false;
    }

    // Completion will never be proven, so the buffers are kept for the rest of the process: forgetting the
    // references is the point (DEVELOPMENT.md rule 8's D3D12 counterpart).
    void Leak(Readback (&readbacks)[2])
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            LOG_WARN("DLSS-NR capture: the GPU never confirmed a capture's copies; its readback buffers are kept until "
                     "the process ends rather than freed under the GPU");
        }
        readbacks[0] = {};
        readbacks[1] = {};
    }
};

Session<Dx12Backend>& TheSession()
{
    static Session<Dx12Backend>* session = new Session<Dx12Backend>();
    return *session;
}

// --- The writer thread ---------------------------------------------------------------------------------------

struct Adapter
{
    std::string name = "unknown";
    std::string driver = "unknown";
};

std::string Narrow(const wchar_t* text)
{
    std::string out;
    for (; text != nullptr && *text != 0; ++text)
        out.push_back(*text >= 0x20 && *text < 0x7F ? static_cast<char>(*text) : '?');
    return out;
}

// The adapter the device was made on, by its LUID: the name and the user-mode driver version, nothing else.
// Real values, not what spoofing tells the game, which is what a bug report needs. Asked once, on this thread.
Adapter QueryAdapter(LUID luid)
{
    Adapter out;
    ScopedSkipSpoofingThread skipSpoofing {};
    const auto create = DxgiProxy::CreateDxgiFactory1_();
    if (create == nullptr)
        return out;

    IDXGIFactory1* factory = nullptr;
    {
        ScopedCreatingD3DDevice creating {};
        if (FAILED(create(__uuidof(IDXGIFactory1), &factory)) || factory == nullptr)
            return out;
    }

    IDXGIFactory4* factory4 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) && factory4 != nullptr)
    {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(factory4->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) && adapter != nullptr)
        {
            DXGI_ADAPTER_DESC1 desc {};
            if (SUCCEEDED(adapter->GetDesc1(&desc)))
                out.name = Narrow(desc.Description);

            LARGE_INTEGER umd {};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                out.driver = std::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
                                         LOWORD(umd.LowPart));
            adapter->Release();
        }
        factory4->Release();
    }
    factory->Release();
    return out;
}

const Adapter& AdapterFor(LUID luid)
{
    // Writer thread only, so no lock: one device per session in practice, and a second LUID is asked again. Never
    // destroyed, like the writer that reads it.
    static Adapter* cached = new Adapter();
    static LUID cachedLuid {};
    static bool have = false;
    if (!have || cachedLuid.LowPart != luid.LowPart || cachedLuid.HighPart != luid.HighPart)
    {
        *cached = QueryAdapter(luid);
        cachedLuid = luid;
        have = true;
    }
    return *cached;
}

const char* RouteToken(DlssNr::PeScan::Feature18Route route)
{
    switch (route)
    {
    case DlssNr::PeScan::Feature18Route::Present:
        return "loader-routes-feature-18";
    case DlssNr::PeScan::Feature18Route::Absent:
        return "loader-does-not-route-feature-18";
    default:
        return "unknown";
    }
}

std::string Version(const uint16_t v[4])
{
    if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0)
        return "unknown";
    return std::format("{}.{}.{}.{}", v[0], v[1], v[2], v[3]);
}

// The model and the NGX loader the first feature build already logged: their versions, the model's size and the
// loader's feature-18 route, never their paths. Found here, once, on the writer thread, so the render thread does no
// file search for a capture; a search that throws leaves "unknown" rather than an exception in a game thread.
struct ModelFacts
{
    std::string version = "unknown";
    std::string bytes = "unknown";
    std::string loader = "unknown";
    std::string route = "unknown";
};

const ModelFacts& ModelFactsOnce()
{
    static ModelFacts* facts = new ModelFacts(); // never destroyed, like the writer that reads it
    static bool asked = false;                   // writer thread only
    if (!asked)
    {
        asked = true;
        try
        {
            if (const auto path = NgxInfo::FindModel(); path.has_value())
            {
                const auto& ngx = NgxInfo::Describe(*path);
                facts->version = Version(ngx.modelVersion);
                facts->bytes = std::to_string(ngx.modelBytes);
                facts->loader = Version(ngx.loaderVersion);
                facts->route = RouteToken(ngx.route);
            }
        }
        catch (...)
        {
        }
    }
    return *facts;
}

// The readback as rows, from its placed footprint.
ImageFile::Rows RowsOf(const capture::Shot& shot, const void* mapped)
{
    ImageFile::Rows rows;
    rows.base = static_cast<const uint8_t*>(mapped);
    rows.offset = shot.layout.Offset;
    rows.rowPitch = shot.layout.Footprint.RowPitch;
    rows.width = shot.layout.Footprint.Width;
    rows.height = shot.layout.Footprint.Height;
    rows.bytes = shot.bytes;
    return rows;
}

std::FILE* OpenForWriting(const std::filesystem::path& path) { return _wfopen(path.c_str(), L"wb"); }

// Returns null on success, or the reason.
const char* WritePair(Job& job, void* const (&mapped)[2], std::string& stemOut)
{
    CapturePayload& p = job.payload;
    const ImageFile::Rows rows[2] = { RowsOf(job.shots[0], mapped[0]), RowsOf(job.shots[1], mapped[1]) };
    for (const auto& r : rows)
    {
        if (!r.Fits(p.plan.source))
        {
            LOG_ERROR("DLSS-NR capture: a readback is smaller than its footprint says; nothing written");
            return kReasonWrite;
        }
    }

    // The folder first: where it cannot exist, nothing else is worth computing.
    std::string why;
    if (!EnsureDirectory(p.directory, &why))
    {
        if (!g_directoryFailed.exchange(true))
            LOG_ERROR("DLSS-NR capture: the retoque-captures folder beside OptiScaler could not be created ({}); "
                      "the capture is off for this session",
                      why);
        return kReasonDirectory;
    }

    // A stem no file has yet, so nothing is overwritten even if the clock went back.
    std::error_code ec;
    std::string stem;
    uint32_t seq = 0;
    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        seq = ++g_seq;
        stem = Stem(p.stamp.compact, p.exe, seq);
        bool taken = false;
        for (const Part part : { Part::Original, Part::Processed, Part::Sidecar })
            taken = taken || std::filesystem::exists(p.directory / FileName(stem, part, p.plan.kind), ec);
        if (!taken)
            break;
    }

    const std::string names[3] = { FileName(stem, Part::Original, p.plan.kind),
                                   FileName(stem, Part::Processed, p.plan.kind),
                                   FileName(stem, Part::Sidecar, p.plan.kind) };

    // The record is complete before the first byte, so both images carry the same one.
    const Adapter& adapter = AdapterFor(p.luid);
    const ModelFacts& model = ModelFactsOnce();
    WriteRecordInputs writeRecord;
    writeRecord.completion = job.completion;
    writeRecord.modelVersion = model.version;
    writeRecord.modelBytes = model.bytes;
    writeRecord.ngxLoader = model.loader;
    writeRecord.ngxRoute = model.route;
    writeRecord.gpu = adapter.name;
    writeRecord.gpuDriver = adapter.driver;
    writeRecord.means[0] = ImageFile::MeanOf(rows[0], p.plan);
    writeRecord.means[1] = ImageFile::MeanOf(rows[1], p.plan);
    writeRecord.pfm = p.plan.kind == ImageFile::FileKind::Pfm;
    writeRecord.seq = seq;
    writeRecord.files[0] = names[0];
    writeRecord.files[1] = names[1];
    writeRecord.files[2] = names[2];
    FillWriteRecord(p.record, writeRecord);

    const std::string line = p.record.Line();
    const std::string json = p.record.Json();
    const std::filesystem::path paths[3] = { p.directory / names[0], p.directory / names[1], p.directory / names[2] };

    const auto image = [&p, &line](const ImageFile::Rows& r)
    {
        return [&r, &p, &line](std::FILE* f)
        {
            auto sink = [f](const uint8_t* data, size_t size) { return std::fwrite(data, 1, size, f) == size; };
            return ImageFile::WriteImage(sink, r, p.plan, kTextKeyword, line);
        };
    };
    std::string failedAt;
    const bool ok = WriteViaTemporary(paths[0], OpenForWriting, image(rows[0]), &failedAt) &&
                    WriteViaTemporary(paths[1], OpenForWriting, image(rows[1]), &failedAt) &&
                    WriteViaTemporary(
                        paths[2], OpenForWriting, [&json](std::FILE* f)
                        { return std::fwrite(json.data(), 1, json.size(), f) == json.size(); }, &failedAt);

    if (!ok)
    {
        // A pair or nothing: whatever of this stem was renamed into place goes again. No older pair was touched.
        for (const auto& path : paths)
            std::filesystem::remove(path, ec);
        LOG_ERROR("DLSS-NR capture: writing {} failed ({}); nothing kept", stem, failedAt);
        return kReasonWrite;
    }

    LOG_INFO("DLSS-NR capture: wrote {}, {} and {} to retoque-captures beside OptiScaler", names[0], names[1],
             names[2]);
    LOG_INFO("DLSS-NR capture provenance: {}", line);

    // The cap, now that this pair exists: the oldest pairs go so that at most kMaxPairs remain, this one among them.
    if (const size_t removed = RotateFolder(p.directory, kMaxPairs, stem); removed != 0)
        LOG_INFO("DLSS-NR capture: removed {} file(s) of the oldest pairs to keep {} pairs", removed, kMaxPairs);
    stemOut = stem;
    return nullptr;
}

void Writer::Run()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return !jobs_.empty(); });
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        // The ticket proved the copies complete before this job existed, so mapping reads finished data.
        void* mapped[2] = {};
        bool isMapped[2] = {};
        for (int i = 0; i < 2; ++i)
        {
            const D3D12_RANGE read { 0, static_cast<SIZE_T>(job.shots[i].bytes) };
            isMapped[i] = job.shots[i].readback != nullptr &&
                          SUCCEEDED(job.shots[i].readback->Map(0, &read, &mapped[i])) && mapped[i] != nullptr;
        }

        const char* failure = kReasonWrite;
        std::string stem;
        if (isMapped[0] && isMapped[1])
            failure = WritePair(job, mapped, stem);
        else
            LOG_ERROR("DLSS-NR capture: a readback could not be mapped; nothing written");

        for (int i = 0; i < 2; ++i)
        {
            if (isMapped[i])
            {
                const D3D12_RANGE none { 0, 0 }; // nothing written by the CPU
                job.shots[i].readback->Unmap(0, &none);
            }
            // The writer's reference only; the session's is let go when the list can never run again.
            if (job.shots[i].readback != nullptr)
                job.shots[i].readback->Release();
        }

        TheSession().Finished(failure == nullptr, failure, std::move(stem));
    }
}
} // namespace

void Poll()
{
    // Never asked: one relaxed load. Idle with nothing held: two more, and no clock read.
    if (!g_used.load(std::memory_order_relaxed))
        return;
    auto& session = TheSession();
    if (!session.Busy())
        return;
    Dx12Backend backend;
    session.Poll(backend, NowMs());
}

bool Wanted() { return g_used.load(std::memory_order_relaxed) && TheSession().Current() == Phase::Requested; }

void AbandonAfterError()
{
    if (!g_used.load(std::memory_order_relaxed))
        return;
    static bool said = false; // render thread, under the pass's lock
    if (!said)
    {
        said = true;
        LOG_ERROR("DLSS-NR capture: describing or recording the frame failed with an exception; that capture is "
                  "withdrawn and the frame is unchanged");
    }
    TheSession().Abandon(kReasonInternal);
}

void AtRecordSite(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* original,
                  D3D12_RESOURCE_STATES originalState, ID3D12Resource* processed, D3D12_RESOURCE_STATES processedState,
                  const FrameFacts& facts)
{
    auto& session = TheSession();
    if (session.Current() != Phase::Requested || cmdList == nullptr || device == nullptr || original == nullptr ||
        processed == nullptr)
        return;

    const D3D12_RESOURCE_DESC o = original->GetDesc();
    const D3D12_RESOURCE_DESC p = processed->GetDesc();
    const DXGI_FORMAT format = capture::TypedForCopy(p.Format);
    const ImageFile::Plan plan = ImageFile::PlanFor(static_cast<uint32_t>(format), facts.linearHdr);

    // Settings first, then the surfaces; one footprint shape for both, and a texture a copy to a buffer can take.
    RefusalInputs refusalInputs;
    refusalInputs.proxy = facts.proxy;
    refusalInputs.compare = facts.compare;
    refusalInputs.debugView = facts.debugView;
    refusalInputs.directoryFailed = g_directoryFailed.load();
    refusalInputs.formatRefusal = plan.refusal;
    refusalInputs.singleSample2d = o.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                                   p.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && o.SampleDesc.Count == 1 &&
                                   p.SampleDesc.Count == 1;
    refusalInputs.sameShape = o.Width == p.Width && o.Height == p.Height && capture::TypedForCopy(o.Format) == format;
    const char* refusal = RefusalFor(refusalInputs);

    CapturePayload payload;
    if (refusal == nullptr)
    {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        payload.stamp = FormatUtc(seconds);
        payload.plan = plan;
        payload.directory = Util::DllPath().remove_filename() / kDirectoryName;
        payload.exe = SanitizeExeStem(Util::ExePath().stem().wstring());
        payload.luid = device->GetAdapterLuid();

        FrameRecordInputs frameRecord;
        frameRecord.build = BuildInfo::ProductVersion();
        frameRecord.timeUtc = payload.stamp.iso;
        frameRecord.exe = payload.exe;
        frameRecord.route = facts.route;
        frameRecord.surface = facts.surface;
        frameRecord.hookMethod = facts.hookMethod;
        frameRecord.width = static_cast<uint32_t>(p.Width);
        frameRecord.height = p.Height;
        frameRecord.originalFormat = static_cast<uint32_t>(o.Format);
        frameRecord.processedFormat = static_cast<uint32_t>(p.Format);
        frameRecord.plan = plan;
        frameRecord.linearHdr = facts.linearHdr;
        frameRecord.held = facts.held;
        frameRecord.carried = facts.carried;
        frameRecord.whitePoint = facts.whitePoint;
        frameRecord.frame = facts.frame;
        frameRecord.present = facts.present;
        frameRecord.settings = facts.settings;
        FillFrameRecord(payload.record, frameRecord);
    }

    Dx12Backend backend;
    Dx12Backend::Recording recording;
    recording.cmd = cmdList;
    recording.device = device;
    recording.surface[0] = original;
    recording.surface[1] = processed;
    recording.state[0] = originalState;
    recording.state[1] = processedState;

    const char* why = nullptr;
    if (session.Record(backend, recording, std::move(payload), refusal, &why))
    {
        LOG_INFO("DLSS-NR capture: copies of {}x{} {} recorded on the pass's list ({}, {})", p.Width, p.Height,
                 ImageFile::FormatName(static_cast<uint32_t>(format)), facts.route, ImageFile::FileKindName(plan.kind));
    }
    else if (why != nullptr)
    {
        LOG_INFO("DLSS-NR capture: not taken: {}", why);
    }
}

void Shutdown()
{
    if (!g_used.load(std::memory_order_acquire))
        return;
    Dx12Backend backend;
    TheSession().Shutdown(backend);
}
} // namespace DlssNr::PairCapture

namespace DlssNr
{
bool RequestPairCapture(const char** why)
{
    if (why != nullptr)
        *why = nullptr;
    // The driver proxy returns from the pass before the frame is written, so no frame could ever take the request.
    const char* refusal = PairCapture::kReasonProxy;
    if (!Config::Instance()->DlssNrUseProxy.value_or_default())
    {
        PairCapture::g_used.store(true, std::memory_order_release);
        auto& session = PairCapture::TheSession();
        if (session.Request(PairCapture::NowMs()))
        {
            LOG_INFO("DLSS-NR capture: requested");
            return true;
        }
        refusal = session.GaveUp() ? PairCapture::kReasonNoCompletion : PairCapture::kReasonBusy;
    }
    if (why != nullptr)
        *why = refusal;
    return false;
}

PairCaptureStatus GetPairCaptureStatus()
{
    PairCaptureStatus status;
    status.directoryFailed = PairCapture::g_directoryFailed.load();
    if (!PairCapture::g_used.load(std::memory_order_acquire))
        return status; // never asked: idle, nothing to report, and nothing allocated to say so

    auto& session = PairCapture::TheSession();
    session.ExpireIfStale(PairCapture::NowMs());

    status.phase = session.Current();
    status.gaveUp = session.GaveUp();
    const auto outcome = session.LastOutcome();
    status.ok = outcome.ok;
    status.reason = outcome.reason;
    status.lastStem = outcome.stem;
    return status;
}
} // namespace DlssNr
