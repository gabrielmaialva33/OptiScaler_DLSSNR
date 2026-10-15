// Cases for the identity glue as the renderer has it (sliced by run.py, placed after glue_fakes.h).

namespace
{
using DlssNr::PresentGuideIdentity::Outcome;
using DlssNr::PresentGuideIdentity::Reason;
using DlssNr::PresentGuides::kRingSize;

int g_glueChecks = 0;
#define GLUE_CHECK(x)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_glueChecks;                                                                                                \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "glue: CHECK failed at line %d: %s\n", __LINE__, #x);                                 \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

// Everything the ring can say about itself, to show that asking left it as it was.
struct RingSnapshot
{
    DlssNr::PresentGuides::Slot slots[kRingSize];
    size_t lastRead = 0;
    uint64_t newest = 0;

    static RingSnapshot Take()
    {
        RingSnapshot s;
        for (size_t i = 0; i < kRingSize; ++i)
            s.slots[i] = DlssNr::g_presentGuideRing.GetSlot(i);
        s.lastRead = DlssNr::g_presentGuideRing.LastReadIndex();
        s.newest = DlssNr::g_presentGuideRing.NewestRecordedSequence();
        return s;
    }

    bool operator==(const RingSnapshot& o) const
    {
        for (size_t i = 0; i < kRingSize; ++i)
        {
            const auto &a = slots[i], &b = o.slots[i];
            if (a.sequence != b.sequence || a.capturedAtMs != b.capturedAtMs || a.width != b.width ||
                a.height != b.height || a.depthInverted != b.depthInverted || a.recorded != b.recorded ||
                a.writeToken != b.writeToken || a.readToken != b.readToken)
                return false;
        }
        return lastRead == o.lastRead && newest == o.newest;
    }
};

bool g_done[kRingSize] {};
bool Never(void*) { return false; }
bool Done(void* token) { return *static_cast<bool*>(token); }

// What CaptureTemporal does at the end of a capture, in its order: stamp, then commit.
size_t Capture(NVSDK_NGX_Parameter* params, const DlssNrFrameInfo& frame, ID3D12Device* device, uint32_t depthWidth,
               uint32_t depthHeight)
{
    const size_t slot = DlssNr::g_presentGuideRing.AcquireForRecord(Never);
    GLUE_CHECK(slot < kRingSize);
    static ID3D12Resource copy;
    DlssNr::g_presentGuideClones[slot] = { &copy, &copy };
    DlssNr::StampPresentGuide(slot, params, frame, device, depthWidth, depthHeight);
    g_done[slot] = true;
    DlssNr::g_presentGuideRing.CommitRecord(slot, depthWidth, depthHeight, false, DlssNr::NowMs(), &g_done[slot],
                                            &DlssNr::g_presentGuideClones[slot]);
    return slot;
}

// One D3D12 present as RunPresentPass makes it: pick, ask, read what the answer allows, count, note the source.
struct PresentResult
{
    DlssNr::PresentGuideIdentity::Decision decision;
    size_t picked = kRingSize;
    bool read = false;
    bool hold = false; // the game's guides hold the history after this present
};

PresentResult PresentPass(ID3D12Device* device, IDXGISwapChain3* swapchain, uint32_t width, uint32_t height,
                          bool boundary, bool routeStarts = false)
{
    PresentResult r;
    const void* presenter = DlssNr::PresentGuidePresenter(swapchain);
    r.picked = DlssNr::g_presentGuideRing.PickForPresent(Done, DlssNr::NowMs());
    const RingSnapshot before = RingSnapshot::Take();
    const auto guides = DlssNr::AskPresentPassGuides(device, presenter, width, height, r.picked, boundary);
    GLUE_CHECK(RingSnapshot::Take() == before); // asking never touches the ring
    r.decision = guides.decision;
    r.read = guides.decision.slot < kRingSize;
    DlssNr::CountPresentPassGuides(guides, r.picked, DlssNr::PresentGuideIdentity::OutcomeOf(guides.decision, r.read));
    r.hold = DlssNr::NotePresentPassGuideSource(r.read, routeStarts);
    DlssNr::g_fakeNowMs += 16;
    return r;
}
} // namespace

int main()
{
    using namespace DlssNr;

    ID3D12Device device;
    HWND__ gameWindow {}, toolWindow {};
    IDXGISwapChain3 proxy { &gameWindow };   // the game's Streamline proxy, at the boundary
    IDXGISwapChain3 wrapped { &gameWindow }; // the swapchain beneath it, at the wrapped entry
    IDXGISwapChain3 tool { &toolWindow };
    IDXGISwapChain3 windowless;
    int bridgeHost = 0;
    ID3D12GraphicsCommandList list { &device };

    // Who presents: the window, so the proxy and the swapchain beneath it are one presenter; the swapchain itself
    // when it has no window.
    GLUE_CHECK(PresentGuidePresenter(&proxy) == &gameWindow && PresentGuidePresenter(&wrapped) == &gameWindow);
    GLUE_CHECK(PresentGuidePresenter(&tool) == &toolWindow);
    GLUE_CHECK(PresentGuidePresenter(&windowless) == &windowless);
    GLUE_CHECK(PresentGuidePresenter(nullptr) == nullptr);

    // A ring that never recorded (a title with no upscaler): nothing is asked, no boundary is marked, the bridge's list
    // is not even asked for its device, nothing is logged and no reset is asked for, however long it runs; the status
    // still counts the frame, and the source history settles on zero guides, so the day captures start is a change.
    {
        for (int i = 0; i < 20; ++i)
        {
            const auto ask = AskPresentPassGuides(&device, &gameWindow, 1920, 1080, kRingSize, true);
            GLUE_CHECK(!ask.asked && ask.decision.slot == kRingSize && ask.decision.reason == Reason::NoCapture);
            CountPresentPassGuides(ask, kRingSize, Outcome::None);
            GLUE_CHECK(!NotePresentPassGuideSource(false, i == 0));
            const auto bridgeAsk = AskBridgeGuides(&list, &bridgeHost, 1920, 1080, kRingSize);
            GLUE_CHECK(!bridgeAsk.asked);
            GLUE_CHECK(!CountBridgeGuides(bridgeAsk, kRingSize, Outcome::None, &bridgeHost));
        }
        GLUE_CHECK(g_guideIdentity.tracker.Epoch() == 0);
        GLUE_CHECK(g_guideIdentity.present.Open().presents == 0 && g_guideIdentity.bridge.Open().presents == 0);
        GLUE_CHECK(device.refs == 1 && list.getDeviceCalls == 0 && !g_resetOnReturn && g_logged.empty());
        GLUE_CHECK(g_guideIdentity.presentSource.Established() == 0 && g_guideIdentity.bridgeSource.Established() == 0);
    }

    // The stamp: the output's extent and base under the evaluate's own names; with no subrect stated, the depth
    // extent is the render extent.
    ID3D12Resource output { { 1920, 1080 } };
    NVSDK_NGX_Parameter params;
    params.output = &output;
    DlssNrFrameInfo frame {};
    size_t slot = Capture(&params, frame, &device, 1280, 720);
    {
        const auto& stamp = g_guideIdentity.tracker.Stamped()[slot];
        GLUE_CHECK(stamp.device == &device && stamp.outputWidth == 1920 && stamp.outputHeight == 1080);
        GLUE_CHECK(stamp.outputBaseX == 0 && stamp.outputBaseY == 0);
        GLUE_CHECK(stamp.renderWidth == 1280 && stamp.renderHeight == 720);
        GLUE_CHECK(stamp.presentEpoch == 0 && stamp.evalsSincePresent == 1 && stamp.swapchain == nullptr);
        GLUE_CHECK(g_resourceNames.size() == 2 && g_resourceNames[0] == "Output" &&
                   g_resourceNames[1] == "DLSSD.Output");
    }

    // The D3D12 present at the game's own Present: accepted, the slot it reads is the pick, and rule 5's record
    // moves to it. The next capture is stamped with the window, which the swapchain beneath the proxy shares.
    {
        const auto r = PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(r.decision.reason == Reason::Accepted && r.decision.slot == slot && r.read);
        GLUE_CHECK(g_guideIdentity.present.LastConsumed() == g_presentGuideRing.GetSlot(slot).sequence);
        const auto& open = g_guideIdentity.present.Open();
        GLUE_CHECK(open.presents == 1 && open.used == 1 && open.enforced == 1 && open.lag[1] == 1);
        GLUE_CHECK(device.refs == 1);
    }
    slot = Capture(&params, frame, &device, 1280, 720);
    GLUE_CHECK(g_guideIdentity.tracker.Stamped()[slot].swapchain == &gameWindow);
    {
        const auto r = PresentPass(&device, &wrapped, 1920, 1080, false);
        GLUE_CHECK(r.decision.reason == Reason::Accepted && r.read);
        GLUE_CHECK(g_guideIdentity.present.Open().enforced == 1); // the wrapped entry does not enforce rule 6
    }

    // Offered again (a present with no capture of its own that got past the pass's skip): consumed, refused, not
    // read, and the record does not move.
    {
        const uint64_t consumed = g_guideIdentity.present.LastConsumed();
        const auto r = PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(r.picked < kRingSize && r.decision.reason == Reason::Consumed && !r.read);
        GLUE_CHECK(g_guideIdentity.present.LastConsumed() == consumed);
        GLUE_CHECK(g_guideIdentity.present.Open().refused[static_cast<size_t>(Reason::Consumed)] == 1);
    }

    // The pass's own skip is counted as a skip, and as a skip the rules would not have passed (consumed).
    {
        const size_t picked = g_presentGuideRing.PickForPresent(Done, NowMs());
        const auto ask = AskPresentPassGuides(&device, &gameWindow, 1920, 1080, picked, true);
        CountPresentPassGuides(ask, picked, Outcome::Skipped);
        const auto& open = g_guideIdentity.present.Open();
        GLUE_CHECK(open.skipped == 1 && open.skippedWouldPass == 0);
    }

    // Rule 6: a capture two boundaries behind is refused at the game's own Present and used, marked, elsewhere.
    {
        slot = Capture(&params, frame, &device, 1280, 720);
        // A present with nothing new in between: the capture is now one behind; one more makes it two.
        g_done[slot] = false;
        PresentPass(&device, &proxy, 1920, 1080, true);
        g_done[slot] = true;
        const auto atBoundary = PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(atBoundary.decision.reason == Reason::Lag && !atBoundary.read);
        const auto below = PresentPass(&device, &wrapped, 1920, 1080, false);
        GLUE_CHECK(below.decision.reason == Reason::Accepted && below.decision.lagBeyond && below.read);
        GLUE_CHECK(g_guideIdentity.present.Open().lagBeyond == 1);
    }

    // A refusal for another extent: the slot is not read, and the window keeps the mismatch for the line.
    {
        slot = Capture(&params, frame, &device, 1280, 720);
        const auto r = PresentPass(&device, &proxy, 2560, 1440, true);
        GLUE_CHECK(r.decision.reason == Reason::Extent && !r.read);
        const auto& open = g_guideIdentity.present.Open();
        GLUE_CHECK(open.haveMismatch && open.mismatchWidth == 2560 && open.mismatch.outputWidth == 1920);
    }

    // A second window presenting on the device: its captures carry no stamp, and the game's present, seeing two,
    // refuses (rule 3). The proxy and the wrapped swapchain never count as two.
    {
        PresentPass(&device, &tool, 640, 480, false);
        slot = Capture(&params, frame, &device, 1280, 720);
        GLUE_CHECK(g_guideIdentity.tracker.Stamped()[slot].swapchain == nullptr);
        const auto r = PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(r.decision.reason == Reason::Swapchain && !r.read);
        g_fakeNowMs += PresentGuideIdentity::kPresenterLiveMs + 1;
        slot = Capture(&params, frame, &device, 1280, 720);
        GLUE_CHECK(PresentPass(&device, &proxy, 1920, 1080, true).decision.reason == Reason::Accepted);
        slot = Capture(&params, frame, &device, 1280, 720);
        GLUE_CHECK(PresentPass(&device, &wrapped, 1920, 1080, false).decision.reason == Reason::Accepted);
    }

    // The source. The empty ring settled it on zero guides, and nothing above held the game's for four frames in a
    // row: five frames of them now are the change from zero to the game's, which resets once and says so. Then three
    // refused frames in a row fall back without a reset, the game's guides still holding the history; the fourth asks
    // for one, once; going back resets once more.
    {
        const auto restarts = []
        {
            size_t n = 0;
            for (const auto& line : g_logged)
                n += line.find("the model's history restarts") != std::string::npos ? 1 : 0;
            return n;
        };
        GLUE_CHECK(g_guideIdentity.presentSource.Established() == 0);
        // One refused frame first: it is the established side, so whatever run of the game's guides was building
        // above starts over, and the count below is exact.
        Capture(&params, frame, &device, 1280, 720);
        GLUE_CHECK(!PresentPass(&device, &proxy, 2560, 1440, true).hold);
        g_resetOnReturn = false;
        g_logged.clear();
        for (int i = 0; i < 5; ++i)
        {
            Capture(&params, frame, &device, 1280, 720);
            const auto r = PresentPass(&device, &proxy, 1920, 1080, true);
            GLUE_CHECK(r.read && r.hold == (i >= 3));
        }
        GLUE_CHECK(g_guideIdentity.presentSource.Established() == 1 && g_resetOnReturn && restarts() == 1);
        GLUE_CHECK(g_logged.back().find("D3D12 present: the model's history restarts on the game's depth and motion, "
                                        "held for 4 frames (1 so far)") != std::string::npos);
        g_resetOnReturn = false;
        g_logged.clear();
        for (int i = 0; i < 3; ++i)
        {
            Capture(&params, frame, &device, 1280, 720);
            const auto r = PresentPass(&device, &proxy, 2560, 1440, true);
            GLUE_CHECK(r.decision.reason == Reason::Extent && r.hold); // a miss: ZeroGuideReset waits too
            GLUE_CHECK(!g_resetOnReturn);
        }
        Capture(&params, frame, &device, 1280, 720);
        GLUE_CHECK(!PresentPass(&device, &proxy, 2560, 1440, true).hold);
        GLUE_CHECK(g_resetOnReturn && restarts() == 1);
        GLUE_CHECK(g_logged.back().find("D3D12 present: the model's history restarts on zero or synthesized guides, "
                                        "held for 4 frames (2 so far)") != std::string::npos);
        g_resetOnReturn = false;
        for (int i = 0; i < 3; ++i)
        {
            Capture(&params, frame, &device, 1280, 720);
            GLUE_CHECK(PresentPass(&device, &proxy, 1920, 1080, true).read);
            GLUE_CHECK(!g_resetOnReturn);
        }
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(g_resetOnReturn && restarts() == 2);
        GLUE_CHECK(g_logged.back().find("(3 so far)") != std::string::npos);
        g_resetOnReturn = false;

        // A route that starts again (Auto back from the upscaled route) resets on its own; the count starts over.
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 2560, 1440, true, true);
        for (int i = 0; i < 2; ++i)
        {
            Capture(&params, frame, &device, 1280, 720);
            PresentPass(&device, &proxy, 2560, 1440, true);
        }
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 2560, 1440, true);
        GLUE_CHECK(!g_resetOnReturn); // four in a row since the route started: that is the start, not a change
        GLUE_CHECK(g_guideIdentity.presentSource.Established() == 0);
    }

    // The status: published once a second, read without the lock, gone two seconds after the last present.
    {
        g_fakeNowMs += 5000;
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 1920, 1080, true);
        PresentGuideIdentity::Status status;
        GLUE_CHECK(RecentGuideStatus(&status));
        GLUE_CHECK(status.presents >= 1 && status.used >= 1);
        GLUE_CHECK(RecentGuideSource(status) == PresentGuideIdentity::Source::Game);

        // A second with only refusals, on synthesized motion.
        g_fakeNowMs += 1000;
        for (int i = 0; i < 3; ++i)
        {
            Capture(&params, frame, &device, 1280, 720);
            PresentPass(&device, &proxy, 2560, 1440, true);
            NotePresentGuideFallback(true);
        }
        g_fakeNowMs += 1000;
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 2560, 1440, true);
        GLUE_CHECK(RecentGuideStatus(&status));
        GLUE_CHECK(status.used == 0 && status.refused >= 3 && status.top == Reason::Extent);
        GLUE_CHECK(RecentGuideSource(status) == PresentGuideIdentity::Source::Synthesized);
        GLUE_CHECK(PresentGuidesLine().find("Guides: synthesized motion; the game's refused on") == 0);

        // Waiting is what the D3D12 pass did under RequireDlss, not what the setting says: the bridge never waits, and
        // its fallback note puts the source back.
        NotePresentGuideWaiting();
        GLUE_CHECK(RecentGuideSource(status) == PresentGuideIdentity::Source::Waiting);
        Config::Instance()->DlssNrRequireDlss.value = true;
        NotePresentGuideFallback(false);
        GLUE_CHECK(RecentGuideSource(status) == PresentGuideIdentity::Source::Zero);
        Config::Instance()->DlssNrRequireDlss.value = false;

        // The pass off: no line.
        Config::Instance()->DlssNrEnabled.value = false;
        GLUE_CHECK(PresentGuidesLine().empty());
        Config::Instance()->DlssNrEnabled.value = true;

        g_fakeNowMs += 2001;
        GLUE_CHECK(!RecentGuideStatus(&status) && PresentGuidesLine().empty());
    }

    // The log line, once its window is due: what step 1 did, under the new prefix.
    {
        g_logged.clear();
        g_fakeNowMs += 3000;
        Capture(&params, frame, &device, 1280, 720);
        PresentPass(&device, &proxy, 1920, 1080, true);
        GLUE_CHECK(g_logged.size() == 1);
        GLUE_CHECK(g_logged[0].find("DLSS-NR present guides, identity applied, D3D12 present: ") == 0);
    }

    // The bridge: a refusal is not read and is counted; with the pass off, or at a HookMethod it is not built for, or
    // with no presenter, the answer still holds but nothing is logged; a list with no device is refused on rule 1; the
    // device reference is given back.
    {
        params.uints[NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X] = 16;
        params.uints[NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y] = 8;
        frame.RenderSubrectWidth = 1000;
        frame.RenderSubrectHeight = 500;
        slot = Capture(&params, frame, &device, 1280, 720);
        {
            const auto& stamp = g_guideIdentity.tracker.Stamped()[slot];
            GLUE_CHECK(stamp.outputBaseX == 16 && stamp.outputBaseY == 8);
            GLUE_CHECK(stamp.renderWidth == 1000 && stamp.renderHeight == 500);
        }

        size_t picked = g_presentGuideRing.PickForPresent(Done, NowMs());
        const auto bridge = [&](ID3D12GraphicsCommandList* l, const void* presenter)
        {
            const RingSnapshot before = RingSnapshot::Take();
            const auto ask = AskBridgeGuides(l, presenter, 1920, 1080, picked);
            GLUE_CHECK(RingSnapshot::Take() == before);
            const bool read = ask.decision.slot < kRingSize;
            CountBridgeGuides(ask, picked, PresentGuideIdentity::OutcomeOf(ask.decision, read), presenter);
            return ask;
        };

        Config::Instance()->DlssNrEnabled.value = false;
        GLUE_CHECK(bridge(&list, &bridgeHost).decision.reason == Reason::OutputBase);
        GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0 && device.refs == 1);
        Config::Instance()->DlssNrEnabled.value = true;
        Config::Instance()->DlssNrHookMethod.value = 1;
        GLUE_CHECK(bridge(&list, &bridgeHost).decision.slot == kRingSize);
        GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0);
        Config::Instance()->DlssNrHookMethod.value = 2;
        GLUE_CHECK(bridge(&list, nullptr).decision.slot == kRingSize);
        GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0);
        GLUE_CHECK(bridge(&list, &bridgeHost).decision.reason == Reason::OutputBase);
        GLUE_CHECK(device.refs == 1);
        {
            const auto& open = g_guideIdentity.bridge.Open();
            GLUE_CHECK(open.presents == 1 && open.used == 0 && open.enforced == 0);
            GLUE_CHECK(open.refused[static_cast<size_t>(Reason::OutputBase)] == 1);
            GLUE_CHECK(open.haveMismatch && open.mismatch.outputBaseX == 16);
        }

        ID3D12GraphicsCommandList orphan;
        GLUE_CHECK(bridge(&orphan, &bridgeHost).decision.reason == Reason::Device);
        GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 2);

        // A capture the rules accept: the bridge's answer is the pick, and rule 6 is never enforced there. The game's
        // window stops presenting first, or the bridge would share the device with it and rule 3 would refuse.
        params.uints.clear();
        frame = {};
        g_fakeNowMs += PresentGuideIdentity::kPresenterLiveMs + 1;
        slot = Capture(&params, frame, &device, 1280, 720);
        picked = g_presentGuideRing.PickForPresent(Done, NowMs());
        const auto ask = bridge(&list, &bridgeHost);
        GLUE_CHECK(ask.decision.reason == Reason::Accepted && ask.decision.slot == picked && !ask.present.enforceLag);
        GLUE_CHECK(g_guideIdentity.bridge.LastConsumed() == g_presentGuideRing.GetSlot(picked).sequence);
        GLUE_CHECK(device.refs == 1);

        // A host built at HookMethod=2 keeps running after it changes, unlogged: its uses still move rule 5's record,
        // so the same capture offered again is consumed rather than read on every present.
        Config::Instance()->DlssNrHookMethod.value = 1;
        const uint32_t loggedBefore = g_guideIdentity.bridge.Open().presents;
        slot = Capture(&params, frame, &device, 1280, 720);
        picked = g_presentGuideRing.PickForPresent(Done, NowMs());
        GLUE_CHECK(bridge(&list, &bridgeHost).decision.reason == Reason::Accepted);
        GLUE_CHECK(g_guideIdentity.bridge.LastConsumed() == g_presentGuideRing.GetSlot(picked).sequence);
        GLUE_CHECK(bridge(&list, &bridgeHost).decision.reason == Reason::Consumed);
        GLUE_CHECK(g_guideIdentity.bridge.Open().presents == loggedBefore);
        Config::Instance()->DlssNrHookMethod.value = 2;
    }

    std::printf("PASS: identity glue as the renderer has it: %d checks -- the window as presenter, inert on an empty "
                "ring, the stamp's reads, the D3D12 present's use, consumed, skip, lag at and below the game's "
                "Present, extent, a second window, the source-change reset and the route's start, the status and its "
                "line, the log line, the bridge off, nameless, deviceless and on\n",
                g_glueChecks);
    return 0;
}
