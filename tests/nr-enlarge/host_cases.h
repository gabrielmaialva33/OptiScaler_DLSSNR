// Transfer 2 inside the pass: production's PlanEnlarge, EnlargeEdit, the retirement list and RetryAfterFailure,
// driven one Dispatch at a time the way DlssNr_Dx12::Dispatch drives them (run.py holds Dispatch to that
// shape). Everything below the NGX boundary is ngx_fakes.h; everything D3D12 is host_fakes.h.

// Named one by one: a using-directive would put DlssNr::Enlarge::State beside the process's State.
using DlssNr::Enlarge::Decide;
using DlssNr::Enlarge::Decision;
using DlssNr::Enlarge::GuideSource;
using DlssNr::Enlarge::Inputs;
using DlssNr::Enlarge::kTransferDlss;
using DlssNr::Enlarge::kTransferMatched;
using DlssNr::Enlarge::MotionScale;
using DlssNr::Enlarge::VulkanTransfer;
using DlssNr::Enlarge::Why;

namespace
{
int cases = 0;

ID3D12Device device { 1 }, otherDevice { 2 };
ID3D12GraphicsCommandList list { 7 };
ID3D12Resource proxy, answer, matchedDepth, matchedMotion, gameDepth, gameMotion;
DlssNr_Dx12 pass;
uint64_t present = 100;
uint64_t frameIndex = 0;

struct Spec
{
    uint32_t configured = kTransferDlss;
    unsigned width = 3440, height = 1440, workWidth = 1720, workHeight = 720;
    bool beforeUpscale = false;
    bool realGuides = true;
    bool depthInverted = true;
    bool matched = true; // the model's own guide match ran
    unsigned depthWidth = 2293, depthHeight = 960, motionWidth = 2293, motionHeight = 960;
    uint32_t debugView = 0;
    bool hold = false;
    bool reset = false;
    bool passthrough = false;
    bool advancePresent = true; // false: a second Dispatch inside the same present
    ID3D12Device* dev = &device;
};

struct Outcome
{
    Decision plan;
    uint32_t sent;
    ID3D12Resource* enlarged;
};

void Readable(ID3D12Resource& r, const char* name)
{
    r.name = name;
    g_states[&r] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

// The inputs arrive readable and must leave readable; every scratch texture rests in UNORDERED_ACCESS.
void Balanced()
{
    for (ID3D12Resource* r : { &proxy, &answer, &matchedDepth, &matchedMotion, &gameDepth, &gameMotion })
        assert(StateOf(r) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (const auto& r : g_scratch)
        assert(StateOf(r.get()) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// One Dispatch's worth of Transfer 2, in Dispatch's order: retire what is done, plan, then -- after the model
// and the chain -- enlarge, send the resolve what ran, and move the enlarged output back once it is read.
Outcome Run(const Spec& s)
{
    TickNrRetired();
    if (s.advancePresent)
        ++present;
    ++frameIndex;

    DlssNrFrameInfo info;
    info.DepthInverted = s.depthInverted;
    info.AllowSupersampling = s.realGuides;

    const bool usable =
        DlssNr::Enlarge::Guides(false, DlssNr_GuideMatch_Dx12::Available(), s.workWidth, s.workHeight, 0, 0,
                                s.depthWidth, s.depthHeight, 0, 0, s.motionWidth, s.motionHeight) != GuideSource::None;
    const auto plan = PlanEnlarge(s.configured, s.debugView, info, s.dev, s.beforeUpscale, usable, s.width, s.height,
                                  s.workWidth, s.workHeight, present, frameIndex);

    uint32_t sent = plan.transfer;
    ID3D12Resource* enlarged = nullptr;

    if (plan.keep)
    {
        EnlargeFrame ef;
        ef.proxy = &proxy;
        ef.answer = &answer;
        ef.matchedDepth = s.matched ? &matchedDepth : nullptr;
        ef.matchedMotion = s.matched ? &matchedMotion : nullptr;
        ef.gameDepth = &gameDepth;
        ef.gameMotion = &gameMotion;
        ef.depthWidth = s.depthWidth;
        ef.depthHeight = s.depthHeight;
        ef.motionWidth = s.motionWidth;
        ef.motionHeight = s.motionHeight;
        ef.workWidth = s.workWidth;
        ef.workHeight = s.workHeight;
        ef.width = s.width;
        ef.height = s.height;
        ef.depthInverted = s.depthInverted;
        ef.mvScaleX = MotionScale(2293.0f, s.workWidth, 2293, s.hold);
        ef.mvScaleY = MotionScale(960.0f, s.workHeight, 960, s.hold);
        ef.passthrough = s.passthrough;
        ef.reset = s.reset;
        ef.hold = s.hold;
        ef.present = present;
        ef.frame = frameIndex;

        enlarged = EnlargeEdit(pass, &list, s.dev, plan, ef);

        if (enlarged == nullptr && plan.evaluate)
            sent = kTransferMatched;
    }

    if (enlarged != nullptr)
    {
        // The resolve reads it here; then it goes back to UNORDERED_ACCESS, as Dispatch does.
        assert(StateOf(enlarged) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(&list, enlarged, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    Balanced();
    return { plan, sent, enlarged };
}

// What DLSS was handed at evaluate time, checked as the call sees it.
void CheckEvaluateStates(const Ngx::Table& t)
{
    for (const char* input : { NVSDK_NGX_Parameter_Color, NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors,
                               NVSDK_NGX_Parameter_ExposureTexture })
        assert(StateOf(t.At<ID3D12Resource*>(input)) == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    assert(StateOf(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Output)) == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// Starts a scenario from nothing: no bundle, nothing parked, a fresh driver.
void Fresh()
{
    NVNGXProxy::inited = true;
    State::Instance().isShuttingDown = false;
    DlssNr::Submission::trackOk = DlssNr::Submission::ready = DlssNr::Submission::completed = true;
    Ngx::driver.onRelease = nullptr;
    DlssNr::RetryAfterFailure();
    for (int i = 0; i < 40; ++i)
        TickNrRetired();
    assert(g_enlarge.live == nullptr && g_nrRetired.empty());
    Ngx::Clear();
    Ngx::driver.onEvaluate = &CheckEvaluateStates;
    pass.recorded.clear();
    pass.fail = false;
    g_tracked = false;
    DlssNr_GuideMatch_Dx12::available = true;
    DlssNr_GuideMatch_Dx12::fail = false;
    DlssNr_GuideMatch_Dx12::dispatches = 0;
}

int ResetOfLastEvaluate() { return Ngx::driver.LastTable().At<int>(NVSDK_NGX_Parameter_Reset); }

void OtherTransfersAreInert()
{
    Fresh();
    const auto scratch = g_scratch.size();
    for (uint32_t configured : { 0u, 1u, 3u, 4u })
    {
        for (int i = 0; i < 4; ++i)
        {
            Spec s;
            s.configured = configured;
            const auto o = Run(s);
            assert(o.sent == configured && o.plan.why == Why::NotSelected && o.enlarged == nullptr);
        }
    }
    assert(Ngx::driver.calls.empty() && g_scratch.size() == scratch && pass.recorded.empty());
    assert(g_enlarge.live == nullptr && g_nrRetired.empty());
    assert(DlssNr::EnlargeStatus().why == Why::NotSelected);
    ++cases;
}

void Lifecycle()
{
    Fresh();
    const auto scratch = g_scratch.size();

    // The first frame builds it -- three textures and one SuperSampling feature on this frame's list -- and
    // sends the resolve matched residual. Nothing is evaluated on the list it was created on.
    auto o = Run(Spec {});
    assert(o.plan.why == Why::WarmingUp && o.plan.build && o.sent == 1 && o.enlarged == nullptr);
    assert(Ngx::driver.Count("create") == 1 && Ngx::driver.Count("evaluate") == 0 && pass.recorded.empty());
    assert(Ngx::driver.calls.back().list == &list);
    assert(g_scratch.size() == scratch + 3 && g_enlarge.live != nullptr);
    const auto& t = Ngx::driver.LastTable();
    assert(t.At<unsigned>(NVSDK_NGX_Parameter_Width) == 1720u && t.At<unsigned>(NVSDK_NGX_Parameter_OutWidth) == 3440u);
    assert(g_enlarge.live->carrier->format == DXGI_FORMAT_R16G16B16A16_FLOAT && g_enlarge.live->carrier->width == 1720);
    assert(g_enlarge.live->enlarged->format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
           g_enlarge.live->enlarged->width == 3440);
    assert(g_enlarge.live->exposure->format == DXGI_FORMAT_R32_FLOAT && g_enlarge.live->exposure->width == 1);
    assert(DlssNr::EnlargeStatus().transfer == 1 && DlssNr::EnlargeStatus().why == Why::WarmingUp);
    ++cases;

    // A second Dispatch inside the same present waits: it might be the same command list.
    Spec same;
    same.advancePresent = false;
    o = Run(same);
    assert(o.plan.why == Why::WarmingUp && !o.plan.build && o.sent == 1 && Ngx::driver.Count("evaluate") == 0);
    ++cases;

    // The next present runs it: the carrier (mode 5, the working size, proxy and answer in), the unit exposure
    // (mode 6, one texel), then DLSS with a fresh history. The resolve gets 2 and DLSS's output.
    o = Run(Spec {});
    assert(o.plan.why == Why::Running && o.sent == 2 && o.enlarged == g_enlarge.live->enlarged);
    assert(pass.recorded.size() == 2);
    const auto& carrier = pass.recorded[0];
    assert(carrier.constants.Mode == DlssNrMode_Carrier && carrier.constants.Width == 1720 &&
           carrier.constants.Height == 720 && carrier.constants.Passthrough == 0);
    assert(carrier.source == &proxy && carrier.model == &answer && carrier.target == g_enlarge.live->carrier);
    const auto& unit = pass.recorded[1];
    assert(unit.constants.Mode == DlssNrMode_UnitTexel && unit.constants.Width == 1 && unit.constants.Height == 1);
    assert(unit.target == g_enlarge.live->exposure);
    assert(Ngx::driver.Count("evaluate") == 1 && ResetOfLastEvaluate() == 1);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Depth) == &matchedDepth);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_MotionVectors) == &matchedMotion);
    assert(t.At<float>(NVSDK_NGX_Parameter_MV_Scale_X) == 1720.0f &&
           t.At<float>(NVSDK_NGX_Parameter_MV_Scale_Y) == 720.0f);
    assert(DlssNr::EnlargeStatus().transfer == 2 && DlssNr::EnlargeStatus().why == Why::Running);
    ++cases;

    // The frame after keeps its history; the frame time is a measured one, bounded.
    o = Run(Spec {});
    assert(o.sent == 2 && ResetOfLastEvaluate() == 0);
    const float ms = t.At<float>(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec);
    assert(std::isfinite(ms) && ms >= 1.0f && ms <= 100.0f);

    // Passthrough reaches the carrier.
    Spec passthrough;
    passthrough.passthrough = true;
    o = Run(passthrough);
    assert(pass.recorded[pass.recorded.size() - 2].constants.Passthrough == 1 && ResetOfLastEvaluate() == 0);
    ++cases;

    // The model's reset resets it.
    Spec reset;
    reset.reset = true;
    o = Run(reset);
    assert(o.sent == 2 && ResetOfLastEvaluate() == 1);
    o = Run(Spec {});
    assert(ResetOfLastEvaluate() == 0);
    ++cases;

    // An NR frame without it (here the model-output debug view) breaks the run: kept, idle, and reset after.
    Spec view;
    view.debugView = 2;
    const auto evaluates = Ngx::driver.Count("evaluate");
    o = Run(view);
    assert(o.plan.why == Why::ModelView && o.plan.keep && o.sent == 1 && Ngx::driver.Count("evaluate") == evaluates);
    o = Run(Spec {});
    assert(o.sent == 2 && ResetOfLastEvaluate() == 1);
    ++cases;

    // Frame hold: the edge resets it, and while held the motion scale is 0.
    Spec held;
    held.hold = true;
    o = Run(held);
    assert(ResetOfLastEvaluate() == 1 && t.At<float>(NVSDK_NGX_Parameter_MV_Scale_X) == 0.0f &&
           t.At<float>(NVSDK_NGX_Parameter_MV_Scale_Y) == 0.0f);
    o = Run(held);
    assert(ResetOfLastEvaluate() == 0);
    o = Run(Spec {});
    assert(ResetOfLastEvaluate() == 1 && t.At<float>(NVSDK_NGX_Parameter_MV_Scale_X) == 1720.0f);
    ++cases;

    // Idle reasons keep it without building or running: before the upscaler, zero guides.
    for (int which = 0; which < 2; ++which)
    {
        Spec idle;
        idle.beforeUpscale = which == 0;
        idle.realGuides = which == 0;
        const auto creates = Ngx::driver.Count("create");
        o = Run(idle);
        assert(o.plan.keep && !o.plan.build && !o.plan.evaluate && o.sent == 1 && g_enlarge.live != nullptr);
        assert(Ngx::driver.Count("create") == creates);
    }
    ++cases;

    // At the frame's size it is let go, and nothing is built while it stays there.
    Spec full;
    full.workWidth = 3440;
    full.workHeight = 1440;
    o = Run(full);
    assert(o.plan.why == Why::FullSize && !o.plan.keep && g_enlarge.live == nullptr && g_nrRetired.size() == 1);
    o = Run(full);
    assert(Ngx::driver.Count("create") == 1);
    ++cases;

    // Untracked, it goes 32 evaluates later: the handle, then its table, then its textures.
    const auto* handle = Ngx::driver.handles.back().get();
    for (int i = 0; i < 31 && Ngx::driver.Count("release") == 0; ++i)
        TickNrRetired();
    assert(Ngx::driver.Count("release") == 1 && Ngx::driver.Count("destroy") == 1 && g_nrRetired.empty());
    const auto& calls = Ngx::driver.calls;
    assert(calls[calls.size() - 2].what == "release" && calls[calls.size() - 2].handle == handle);
    assert(calls.back().what == "destroy");
    for (size_t i = scratch; i < scratch + 3; ++i)
        assert(g_scratch[i]->releases == 1);
    ++cases;
}

void RebuiltForAnotherShape()
{
    Fresh();
    Run(Spec {});
    Run(Spec {});
    assert(Ngx::driver.Count("evaluate") == 1);

    // Another working size, another depth direction, another device: each retires the bundle on the spot and
    // builds the next one, which waits for a later present again.
    struct Change
    {
        void (*apply)(Spec&);
    };
    const Change changes[] = {
        { [](Spec& s) { s.workWidth = 1290, s.workHeight = 540; } },
        { [](Spec& s) { s.workWidth = 1290, s.workHeight = 540, s.depthInverted = false; } },
        { [](Spec& s) { s.workWidth = 1290, s.workHeight = 540, s.depthInverted = false, s.dev = &otherDevice; } },
        { [](Spec& s)
          {
              s.workWidth = 1290, s.workHeight = 540, s.depthInverted = false, s.dev = &otherDevice, s.width = 2560,
              s.height = 1080;
          } }
    };
    unsigned creates = 1;
    for (const auto& c : changes)
    {
        Spec s;
        c.apply(s);
        const auto parked = g_nrRetired.size();
        auto o = Run(s);
        assert(o.plan.why == Why::WarmingUp && o.plan.build && o.sent == 1);
        assert(g_nrRetired.size() == parked + 1 && Ngx::driver.Count("create") == ++creates);
        assert(g_enlarge.live->feature.Built().width == s.workWidth);
        o = Run(s);
        assert(o.sent == 2);
    }
    ++cases;
}

void GuideCases()
{
    // The game's own pair when it already is the working size at the origin (DLSS Performance at half scale).
    Fresh();
    Spec game;
    game.matched = false;
    game.depthWidth = game.motionWidth = 1720;
    game.depthHeight = game.motionHeight = 720;
    Run(game);
    auto o = Run(game);
    const auto& t = Ngx::driver.LastTable();
    assert(o.sent == 2 && t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Depth) == &gameDepth);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_MotionVectors) == &gameMotion);
    assert(DlssNr_GuideMatch_Dx12::dispatches == 0);
    ++cases;

    // Smaller than the model (Ultra Performance under a half-size model): resampled into the bundle's own pair.
    Spec small;
    small.matched = false;
    small.depthWidth = small.motionWidth = 1147;
    small.depthHeight = small.motionHeight = 480;
    o = Run(small);
    assert(o.sent == 2 && DlssNr_GuideMatch_Dx12::dispatches == 1);
    assert(DlssNr_GuideMatch_Dx12::lastOutWidth == 1720 && DlssNr_GuideMatch_Dx12::lastDepthWidth == 1147);
    auto* depth = t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Depth);
    auto* motion = t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_MotionVectors);
    assert(depth == g_enlarge.live->depth && motion == g_enlarge.live->motion);
    assert(depth->format == DXGI_FORMAT_R32_FLOAT && motion->format == DXGI_FORMAT_R32G32_FLOAT);
    ++cases;

    // A build with no resample shader cannot do that: the plan says so, nothing runs, and it is not a failure.
    DlssNr_GuideMatch_Dx12::available = false;
    const auto evaluates = Ngx::driver.Count("evaluate");
    o = Run(small);
    assert(o.plan.why == Why::GuidesUnmatched && o.sent == 1 && Ngx::driver.Count("evaluate") == evaluates);
    assert(!g_enlarge.failed && g_enlarge.live != nullptr);
    DlssNr_GuideMatch_Dx12::available = true;
    o = Run(small);
    assert(o.sent == 2 && ResetOfLastEvaluate() == 1);
    ++cases;

    // A resample that fails at run time falls back on that frame and is then held on the bundle: the plan says
    // so instead of planning a run that falls back on every frame, until the bundle is rebuilt.
    DlssNr_GuideMatch_Dx12::fail = true;
    o = Run(small);
    assert(o.plan.evaluate && o.sent == 1 && DlssNr::EnlargeStatus().why == Why::GuidesUnmatched);
    for (int i = 0; i < 3; ++i)
    {
        o = Run(small);
        assert(o.plan.why == Why::GuidesUnmatched && !o.plan.evaluate && o.sent == 1 && g_enlarge.live != nullptr);
    }
    DlssNr_GuideMatch_Dx12::fail = false;
    o = Run(small);
    assert(o.plan.why == Why::GuidesUnmatched);
    DlssNr::RetryEnlargement();
    Run(small);
    o = Run(small);
    assert(o.sent == 2 && !g_enlarge.live->guidesFailed);
    ++cases;
}

void FailureHoldsUntilRetry()
{
    Fresh();
    Run(Spec {});
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_FAIL_InvalidParameter;

    // Planned as 2, refused by DLSS: this frame falls back to 1, and so does every frame after until Retry.
    auto o = Run(Spec {});
    assert(o.plan.evaluate && o.sent == 1 && o.enlarged == nullptr);
    assert(g_enlarge.failed && g_enlarge.live == nullptr && g_nrRetired.size() == 1);
    assert(DlssNr::EnlargeStatus().why == Why::Failed && DlssNr::EnlargeStatus().transfer == 1);
    assert(std::any_of(g_log.begin(), g_log.end(), [](const std::string& line)
                       { return line.find("ERROR") == 0 && line.find("FAIL_InvalidParameter") != std::string::npos; }));
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_Success;
    const auto creates = Ngx::driver.Count("create");
    for (int i = 0; i < 3; ++i)
    {
        o = Run(Spec {});
        assert(o.plan.why == Why::Failed && !o.plan.keep && o.sent == 1);
    }
    assert(Ngx::driver.Count("create") == creates);
    ++cases;

    // Retry clears it; the next frame builds again.
    DlssNr::RetryAfterFailure();
    assert(!g_enlarge.failed && !g_nr.failed && g_nr.reset);
    o = Run(Spec {});
    assert(o.plan.build && Ngx::driver.Count("create") == creates + 1);
    o = Run(Spec {});
    assert(o.sent == 2);
    ++cases;

    // A create refused (no nvngx_dlss.dll), or a scratch allocation refused: failed, with nothing left half-built.
    // A core that is not up is waited for instead, with nothing allocated (CoreShutDownByTheGame has the shutdown).
    for (int which = 0; which < 3; ++which)
    {
        Fresh();
        if (which == 0)
            Ngx::driver.createResult = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        if (which == 1)
            NVNGXProxy::inited = false;
        if (which == 2)
            g_failScratchAt = static_cast<int>(g_scratch.size()) + 1;
        o = Run(Spec {});
        if (which == 1)
        {
            assert(o.sent == 1 && !g_enlarge.failed && g_enlarge.live == nullptr);
            assert(DlssNr::EnlargeStatus().why == Why::WarmingUp && Ngx::driver.Count("allocate") == 0);
            continue;
        }
        assert(o.sent == 1 && g_enlarge.failed && g_enlarge.live == nullptr);
        assert(Ngx::driver.Count("evaluate") == 0 && DlssNr::EnlargeStatus().why == Why::Failed);
        assert(Ngx::driver.Count("create") == (which == 0 ? 1u : 0u));
        assert(Ngx::driver.Count("destroy") == Ngx::driver.Count("allocate"));
    }
    ++cases;
}

// Releasing an NGX feature can re-enter OptiScaler's hooks, and those can park something. The list must be whole
// by then: the release runs after the loop, and what it parks waits its turn.
ID3D12Resource reentrant;
void ParkFromRelease(const NVSDK_NGX_Handle*)
{
    for (const auto& retired : g_nrRetired)
        assert(retired.enlarger == nullptr || retired.enlarger->feature.Live());
    ID3D12Resource* r = &reentrant;
    ParkNrResource(r);
}

void ReleasedOutsideTheList()
{
    Fresh();
    Run(Spec {});
    DlssNr::RetryAfterFailure(); // parks the live bundle
    Ngx::driver.onRelease = &ParkFromRelease;
    for (int i = 0; i < 40 && Ngx::driver.Count("release") == 0; ++i)
        TickNrRetired();
    assert(Ngx::driver.Count("release") == 1 && g_nrRetired.size() == 1 && g_nrRetired[0].resource == &reentrant);
    Ngx::driver.onRelease = nullptr;
    for (int i = 0; i < 40; ++i)
        TickNrRetired();
    assert(g_nrRetired.empty() && reentrant.releases == 1);
    ++cases;
}

void TrackedSubmissions()
{
    Fresh();
    g_tracked = true;
    DlssNr::Submission::completed = false;
    const auto tracks = DlssNr::Submission::tracks;

    // The creation's recording is tracked, and a later present is not enough while it has not completed.
    auto o = Run(Spec {});
    assert(o.plan.build && DlssNr::Submission::tracks == tracks + 1 && g_enlarge.live->tracked);
    o = Run(Spec {});
    assert(o.plan.why == Why::WarmingUp && o.sent == 1);
    DlssNr::Submission::completed = true;
    o = Run(Spec {});
    assert(o.sent == 2);
    ++cases;

    // Parked under tracking it goes when its recordings are ready, not on a count.
    DlssNr::Submission::ready = false;
    DlssNr::RetryAfterFailure();
    for (int i = 0; i < 40; ++i)
        TickNrRetired();
    assert(g_nrRetired.size() == 1 && Ngx::driver.Count("release") == 0);
    DlssNr::Submission::ready = true;
    TickNrRetired();
    assert(g_nrRetired.empty() && Ngx::driver.Count("release") == 1);
    ++cases;

    // At the cap on parked objects nothing is built, and that is not a failure.
    std::vector<ID3D12Resource> fill(32);
    for (auto& r : fill)
    {
        ID3D12Resource* p = &r;
        DlssNr::Submission::ready = false;
        ParkNrResource(p);
    }
    const auto creates = Ngx::driver.Count("create");
    o = Run(Spec {});
    assert(o.plan.build && Ngx::driver.Count("create") == creates && !g_enlarge.failed && g_enlarge.live == nullptr);
    DlssNr::Submission::ready = true;
    o = Run(Spec {});
    assert(Ngx::driver.Count("create") == creates + 1);
    g_tracked = false;
    ++cases;

    // A recording the tracker refuses is not a creation either, and allocates nothing to retire.
    Fresh();
    g_tracked = true;
    DlssNr::Submission::trackOk = false;
    const auto scratch = g_scratch.size();
    o = Run(Spec {});
    assert(Ngx::driver.Count("create") == 0 && !g_enlarge.failed && g_enlarge.live == nullptr);
    assert(g_scratch.size() == scratch && g_nrRetired.empty());
    g_tracked = false;
    ++cases;
}

void ProcessExit()
{
    // No NGX call while the process exits; the textures still go.
    Fresh();
    Run(Spec {});
    auto* carrier = g_enlarge.live->carrier;
    DlssNr::RetryAfterFailure();
    State::Instance().isShuttingDown = true;
    for (int i = 0; i < 40; ++i)
        TickNrRetired();
    assert(g_nrRetired.empty() && Ngx::driver.Count("release") == 0 && Ngx::driver.Count("destroy") == 0);
    assert(carrier->releases == 1);
    State::Instance().isShuttingDown = false;
    ++cases;
}

// The list the bundle joined still frees everything else it held, model features through the model's own
// release, and the bundle alongside them.
unsigned modelReleases = 0;
void CountModelRelease(void*) { ++modelReleases; }

void SharedRetirementList()
{
    Fresh();
    Run(Spec {});
    g_nr.release = &CountModelRelease;
    int model = 0;
    void* feature = &model;
    auto* scaler = new OS_Dx12();
    auto* stabilizer = new DlssNr_Stabilizer_Dx12();
    auto* uiMask = new DlssNr_UiMask_Dx12();
    ParkNrFeature(feature);
    ParkNrScaler(scaler);
    ParkNrStabilizer(stabilizer);
    ParkNrUiMask(uiMask);
    DlssNr::RetryAfterFailure();
    assert(feature == nullptr && scaler == nullptr && stabilizer == nullptr && uiMask == nullptr);
    assert(g_nrRetired.size() == 5);
    for (int i = 0; i < 32; ++i)
        TickNrRetired();
    assert(g_nrRetired.empty() && modelReleases == 1 && Ngx::driver.Count("release") == 1);
    g_nr.release = nullptr;
    ++cases;
}

// The game shuts the NGX core down (applying settings, say) and every feature goes with it. The bundle is let go
// without a call into the core that follows; while the core is down nothing is built and nothing fails; once
// it is back the next frame builds again.
void CoreShutDownByTheGame()
{
    Fresh();
    Run(Spec {});
    auto o = Run(Spec {});
    assert(o.sent == 2);
    const auto evaluates = Ngx::driver.Count("evaluate");

    ++NVNGXProxy::shutdowns;
    NVNGXProxy::inited = false;
    o = Run(Spec {});
    assert(o.plan.why == Why::WarmingUp && o.plan.build && o.sent == 1 && g_enlarge.live == nullptr);
    assert(!g_enlarge.failed && DlssNr::EnlargeStatus().why == Why::WarmingUp);
    assert(Ngx::driver.Count("evaluate") == evaluates && Ngx::driver.Count("create") == 1);
    for (int i = 0; i < 40; ++i)
        TickNrRetired();
    assert(g_nrRetired.empty() && Ngx::driver.Count("release") == 0 && Ngx::driver.Count("destroy") == 0);

    NVNGXProxy::inited = true;
    o = Run(Spec {});
    assert(o.plan.build && Ngx::driver.Count("create") == 2);
    o = Run(Spec {});
    assert(o.sent == 2 && ResetOfLastEvaluate() == 1);

    // The same shutdown with the core straight back up (Init again before the next frame): still dropped.
    ++NVNGXProxy::shutdowns;
    o = Run(Spec {});
    assert(o.plan.build && Ngx::driver.Count("create") == 3 && Ngx::driver.Count("release") == 0);
    ++cases;
}

// The enlargement's own Retry rebuilds its DLSS and touches nothing of the model's.
void RetryIsNarrow()
{
    Fresh();
    Run(Spec {});
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    Run(Spec {});
    assert(g_enlarge.failed);
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_Success;
    g_nr.reset = false;
    const auto preResets = g_preExtent.resets, postResets = g_postExtent.resets;
    DlssNr::RetryEnlargement();
    assert(!g_enlarge.failed && !g_nr.reset && g_preExtent.resets == preResets && g_postExtent.resets == postResets);
    auto o = Run(Spec {});
    assert(o.plan.build);
    o = Run(Spec {});
    assert(o.sent == 2);

    // The last case that builds: retire and release the bundle, so none is left live when the process ends.
    Fresh();
    ++cases;
}

void VulkanNeverBuilds()
{
    // The D3D12 plan is never asked on native Vulkan, which maps 2 to 1 itself; the rule still refuses it.
    Inputs in;
    in.configured = kTransferDlss;
    in.vulkan = true;
    in.width = 3440;
    in.height = 1440;
    in.workWidth = 1720;
    in.workHeight = 720;
    const auto d = Decide(in);
    assert(d.transfer == 1 && !d.keep && !d.build && !d.evaluate && VulkanTransfer(2) == 1);
    ++cases;
}
} // namespace

int main()
{
    Readable(proxy, "proxy");
    Readable(answer, "answer");
    Readable(matchedDepth, "matched depth");
    Readable(matchedMotion, "matched motion");
    Readable(gameDepth, "game depth");
    Readable(gameMotion, "game motion");
    g_states[&reentrant] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    assert(DlssNr::EnlargeStatus().why == Why::NotYet);
    OtherTransfersAreInert();
    Lifecycle();
    RebuiltForAnotherShape();
    GuideCases();
    FailureHoldsUntilRetry();
    ReleasedOutsideTheList();
    TrackedSubmissions();
    ProcessExit();
    SharedRetirementList();
    CoreShutDownByTheGame();
    RetryIsNarrow();
    VulkanNeverBuilds();
    std::printf("PASS: %d Transfer 2 host cases on production's plan, enlarge, retirement and retry: build on one "
                "frame and run from a later one, inert for 0/1/3, guides, resets, hold, failure until Retry, release "
                "after the list settles, tracked submissions, the cap, process exit; %u barriers chained\n",
                cases, g_barriers);
    return 0;
}
