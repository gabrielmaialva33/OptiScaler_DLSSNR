// What the present host builds, and when, across the sizes and toggles a real swapchain goes through.
//
// Written after PCSX2 under the D3D11 bridge (2026-09-27): the bridge handed the host a 1x1 frame
// mid-session, the host rebuilt everything at 1x1 and spent the model's creation there, and the model
// ran on none of the frames that followed.

namespace
{

// One swapchain's host, fed frame by frame the way the bridge feeds it: a fresh list per frame, the
// host records onto it, the list is closed and executes, and the host is told it did.
struct Rig
{
    ID3D12Device device;
    ID3D12CommandQueue queue;
    ID3D12Resource source;
    std::vector<std::unique_ptr<ID3D12GraphicsCommandList>> frameLists;
    DlssNr::PresentHost host; // last, so it is torn down while the device that owns its lists still exists

    void Size(uint32_t width, uint32_t height) { source.desc = { width, height, DXGI_FORMAT_R8G8B8A8_UNORM }; }

    bool Frame()
    {
        frameLists.push_back(std::make_unique<ID3D12GraphicsCommandList>());
        auto* list = frameLists.back().get();
        const bool passed = host.Record(&device, list, &source, D3D12_RESOURCE_STATE_COPY_SOURCE, &queue);
        assert(passed == (host.Output() != nullptr));
        list->Close();
        host.ConfirmExecuted();
        return passed;
    }
};

void Fresh(bool enabled = true)
{
    g_log.clear();
    g_transferBuffersMade = 0;
    DlssNr::g_creations.clear();
    DlssNr::g_modelWidth = 0;
    DlssNr::g_modelHeight = 0;
    DlssNr::g_passesRun = 0;
    DlssNr::g_motionResets.clear();
    DlssNr::g_refuseModel = false;
    Config::Instance()->DlssNrEnabled.value = enabled;
    Config::Instance()->DlssNrZeroGuideReset.value = false;
    Config::Instance()->DlssNrSynthMotion.value = false;
}

bool CreatedOnEmptyListAt(size_t index, uint32_t width, uint32_t height)
{
    const auto& creation = DlssNr::g_creations.at(index);
    return creation.width == width && creation.height == height && creation.onEmptyList;
}

} // namespace

int main()
{
    unsigned cases = 0;
    const auto CASE = [&cases](const char* name)
    {
        ++cases;
        std::cout << "  " << name << "\n";
    };

    // --- a frame too small for the model ----------------------------------------------------------
    {
        Fresh();
        Rig r;
        r.Size(1, 1);
        assert(!r.Frame() && !r.Frame());
        assert(g_transferBuffersMade == 0 && r.device.lists.empty() && DlssNr::g_creations.empty());
        assert(LogCount("built") == 0 && LogCount("model creation attempted") == 0);
        CASE("a 1x1 frame builds nothing, creates no list and does not attempt the model");

        r.Size(841, 1356);
        assert(r.Frame());
        assert(g_transferBuffersMade == 2 && DlssNr::g_creations.size() == 1);
        assert(CreatedOnEmptyListAt(0, 841, 1356));
        assert(LogCount("model creation attempted") == 1);
        CASE("the first real size after it builds normally, and the model is created there, on the host's own list");
    }

    // --- the sequence PCSX2 produced ---------------------------------------------------------------
    {
        Fresh();
        Rig r;
        r.Size(841, 1356);
        assert(r.Frame() && r.Frame());
        const auto made = g_transferBuffersMade;
        const auto lists = r.device.lists.size();
        const auto passes = DlssNr::g_passesRun;

        r.Size(1, 1);
        assert(!r.Frame() && !r.Frame() && !r.Frame());
        assert(g_transferBuffersMade == made && g_transferBuffersLive == 2 && r.device.lists.size() == lists);
        assert(DlssNr::g_passesRun == passes);
        CASE("running, then 1x1: nothing is released, rebuilt or attempted for the small frames");

        r.Size(841, 1356);
        assert(r.Frame() && r.Frame());
        assert(DlssNr::g_passesRun == passes + 2);
        assert(g_transferBuffersMade == made && DlssNr::g_creations.size() == 1);
        CASE("back to the real size: the model runs on the very first frame, with no rebuild at all");
    }

    // --- an ordinary resize --------------------------------------------------------------------------
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080);
        assert(r.Frame() && r.Frame());
        assert(g_transferBuffersMade == 2 && LogCount("built") == 1);

        // The bridge only changes size inside ResizeBuffers, after its drain, so what the host throws
        // away here is idle.
        r.Size(1280, 720);
        assert(r.Frame());
        assert(g_transferBuffersMade == 4 && g_transferBuffersLive == 2 && LogCount("built") == 2);
        assert(DlssNr::g_creations.size() == 2 && CreatedOnEmptyListAt(1, 1280, 720));
        assert(r.Frame());
        CASE("a resize rebuilds at the new size at once, and the model is created again on the host's own list");

        r.Size(1920, 1080);
        assert(r.Frame());
        assert(DlssNr::g_creations.size() == 3 && CreatedOnEmptyListAt(2, 1920, 1080));
        CASE("and a resize back does the same, so every size the model runs at was created on an empty list");
    }

    // --- switched off, then resized ------------------------------------------------------------------
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080);
        assert(r.Frame());

        Config::Instance()->DlssNrEnabled.value = false;
        assert(!r.Frame());
        const auto made = g_transferBuffersMade;
        const auto lists = r.device.lists.size();
        const auto creations = DlssNr::g_creations.size();
        const auto attempts = LogCount("model creation attempted");

        r.Size(1280, 720);
        assert(!r.Frame() && !r.Frame());
        assert(g_transferBuffersMade == made && r.device.lists.size() == lists);
        assert(DlssNr::g_creations.size() == creations && LogCount("model creation attempted") == attempts);
        CASE("switched off, then resized: nothing is rebuilt and the model's creation is not spent while off");

        Config::Instance()->DlssNrEnabled.value = true;
        assert(r.Frame());
        assert(DlssNr::g_creations.size() == creations + 1 && CreatedOnEmptyListAt(creations, 1280, 720));
        CASE("switched back on at the new size: rebuilt then, with the model created on the host's own list");
    }

    // --- a host that starts with the pass off --------------------------------------------------------
    {
        Fresh(false);
        Rig r;
        r.Size(1920, 1080);
        assert(!r.Frame() && !r.Frame());
        assert(g_transferBuffersMade == 0 && r.device.lists.empty() && DlssNr::g_creations.empty());
        CASE("a host created while the pass is off stays empty");

        Config::Instance()->DlssNrEnabled.value = true;
        assert(r.Frame());
        assert(DlssNr::g_creations.size() == 1 && CreatedOnEmptyListAt(0, 1920, 1080));
        CASE("and builds, with the model on its own list, the first frame the pass is on");
    }

    // --- synthesized motion while the model cannot run -----------------------------------------------
    // Divinity, 2026-09-28: the host passed the model's reset to the estimator, and that reset stays
    // owed until the model runs. While it was refused or settling, the estimator was reset every frame,
    // never warmed, and frame generation -- which takes its field on the bridge -- built a second one.
    {
        Fresh();
        Config::Instance()->DlssNrSynthMotion.value = true;
        DlssNr::g_refuseModel = true;
        Rig r;
        r.Size(1920, 1080);
        assert(!r.Frame() && !r.Frame() && !r.Frame());
        assert((DlssNr::g_motionResets == std::vector<bool> { true, false, false }));
        CASE("the model refused: the estimator is reset once after the build, not on every frame the model declines");

        DlssNr::g_refuseModel = false;
        assert(r.Frame());
        assert(DlssNr::g_motionResets.size() == 4 && !DlssNr::g_motionResets.back());
        CASE("and when the model starts running, the estimator carries on without a reset of its own");

        r.Size(1280, 720);
        assert(r.Frame());
        assert(DlssNr::g_motionResets.size() == 5 && DlssNr::g_motionResets.back());
        CASE("a resize owes the estimator its reset again");
    }

    assert(g_transferBuffersLive == 0);
    std::cout << "present host: " << cases << " cases passed\n";
    return 0;
}
