// Cases for the identity glue as the renderer has it (sliced by run.py, placed after glue_fakes.h).

namespace
{
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

// Everything the ring can say about itself, to show that a note left it as it was.
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
} // namespace

int main()
{
    using namespace DlssNr;

    ID3D12Device device;
    IDXGISwapChain3 swapchain;
    int bridgeHost = 0;
    ID3D12GraphicsCommandList list { &device };

    // A ring that never recorded (a title with no upscaler): neither note counts, marks a boundary, takes a device
    // reference or logs.
    NotePresentPassIdentity(&swapchain, &device, 1920, 1080, kRingSize);
    NoteBridgeIdentity(&list, &bridgeHost, 1920, 1080, kRingSize);
    GLUE_CHECK(g_guideIdentity.tracker.Epoch() == 0);
    GLUE_CHECK(g_guideIdentity.present.Open().presents == 0 && g_guideIdentity.bridge.Open().presents == 0);
    GLUE_CHECK(device.refs == 1);

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

    // The present pass's note, after its pick: counted as today's read, accepted, and the ring untouched.
    size_t picked = g_presentGuideRing.PickForPresent(Done, NowMs());
    GLUE_CHECK(picked == slot);
    {
        const RingSnapshot before = RingSnapshot::Take();
        NotePresentPassIdentity(&swapchain, &device, 1920, 1080, picked);
        GLUE_CHECK(RingSnapshot::Take() == before);
    }
    {
        const auto& open = g_guideIdentity.present.Open();
        GLUE_CHECK(open.presents == 1 && open.read == 1);
        GLUE_CHECK(open.readReasons[static_cast<size_t>(PresentGuideIdentity::Reason::Accepted)] == 1);
        GLUE_CHECK(open.lag[1] == 1);
        GLUE_CHECK(g_guideIdentity.present.LastConsumed() == g_presentGuideRing.GetSlot(slot).sequence);
    }

    // RunPresentPass's own skip (nothing recorded since its last pass) is what the note calls skipped; the rules
    // agree: that capture was consumed.
    g_nrLastEnhancedRecordedSeq = g_presentGuideRing.NewestRecordedSequence();
    picked = g_presentGuideRing.PickForPresent(Done, NowMs());
    NotePresentPassIdentity(&swapchain, &device, 1920, 1080, picked);
    {
        const auto& open = g_guideIdentity.present.Open();
        GLUE_CHECK(open.presents == 2 && open.skipped == 1 && open.skippedWouldPass == 0);
    }

    // No pick and no skip: zero or synthesized guides today, counted as none.
    g_nrLastEnhancedRecordedSeq = 0;
    NotePresentPassIdentity(&swapchain, &device, 1920, 1080, kRingSize);
    GLUE_CHECK(g_guideIdentity.present.Open().none == 1);
    GLUE_CHECK(g_guideIdentity.tracker.Epoch() == 3);

    // A stated output base and subrect reach the stamp; the swapchain stamp is the one presenting chain.
    params.uints[NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X] = 16;
    params.uints[NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y] = 8;
    frame.RenderSubrectWidth = 1000;
    frame.RenderSubrectHeight = 500;
    slot = Capture(&params, frame, &device, 1280, 720);
    {
        const auto& stamp = g_guideIdentity.tracker.Stamped()[slot];
        GLUE_CHECK(stamp.outputBaseX == 16 && stamp.outputBaseY == 8);
        GLUE_CHECK(stamp.renderWidth == 1000 && stamp.renderHeight == 500);
        GLUE_CHECK(stamp.presentEpoch == 3 && stamp.swapchain == &swapchain);
    }

    // The bridge: switched off, or at a HookMethod it is not built for, it is not counted and takes no device
    // reference; with no presenter it is not counted; on, it is counted and gives the reference back.
    picked = g_presentGuideRing.PickForPresent(Done, NowMs());
    Config::Instance()->DlssNrEnabled.value = false;
    NoteBridgeIdentity(&list, &bridgeHost, 1920, 1080, picked);
    GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0 && device.refs == 1);
    Config::Instance()->DlssNrEnabled.value = true;
    Config::Instance()->DlssNrHookMethod.value = 1;
    NoteBridgeIdentity(&list, &bridgeHost, 1920, 1080, picked);
    GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0 && device.refs == 1);
    Config::Instance()->DlssNrHookMethod.value = 2;
    NoteBridgeIdentity(&list, nullptr, 1920, 1080, picked);
    GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 0 && device.refs == 1);
    {
        const RingSnapshot before = RingSnapshot::Take();
        NoteBridgeIdentity(&list, &bridgeHost, 1920, 1080, picked);
        GLUE_CHECK(RingSnapshot::Take() == before);
    }
    GLUE_CHECK(device.refs == 1);
    {
        const auto& open = g_guideIdentity.bridge.Open();
        GLUE_CHECK(open.presents == 1 && open.read == 1);
        GLUE_CHECK(open.readReasons[static_cast<size_t>(PresentGuideIdentity::Reason::OutputBase)] == 1);
        GLUE_CHECK(open.haveMismatch && open.mismatch.outputBaseX == 16);
    }

    // A slot without both copies is not a read today, on the bridge.
    g_presentGuideClones[picked].motion = nullptr;
    NoteBridgeIdentity(&list, &bridgeHost, 1920, 1080, picked);
    GLUE_CHECK(g_guideIdentity.bridge.Open().none == 1);

    // A list with no device: not counted, nothing leaked.
    ID3D12GraphicsCommandList orphan;
    NoteBridgeIdentity(&orphan, &bridgeHost, 1920, 1080, picked);
    GLUE_CHECK(g_guideIdentity.bridge.Open().presents == 2);

    // All of it inside one window: nothing logged.
    GLUE_CHECK(g_logged.empty());

    std::printf("PASS: identity glue as the renderer has it: %d checks -- inert on an empty ring, the stamp's reads, "
                "today's read/skip/none, the ring untouched by a note, the bridge off, nameless and on\n",
                g_glueChecks);
    return 0;
}
