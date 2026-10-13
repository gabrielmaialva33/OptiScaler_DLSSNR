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
    ID3D12Resource* lastOutput = nullptr; // what the host handed back on the last frame, before it was confirmed
    DlssNr::PresentHost host; // last, so it is torn down while the device that owns its lists still exists

    void Size(uint32_t width, uint32_t height, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        source.desc = { width, height, format };
    }

    bool Frame()
    {
        frameLists.push_back(std::make_unique<ID3D12GraphicsCommandList>());
        auto* list = frameLists.back().get();
        // The bridge hands its opened backbuffer over as a copy source and is owed it back in that state; what the
        // host hands back is a copy source too, because the one thing the bridge does with it is copy it.
        source.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        const bool passed = host.Record(&device, list, &source, D3D12_RESOURCE_STATE_COPY_SOURCE, &queue);
        assert(passed == (host.Output() != nullptr));
        assert(source.state == D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (host.Output() != nullptr)
            assert(host.Output()->state == D3D12_RESOURCE_STATE_COPY_SOURCE);
        lastOutput = host.Output();
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
    DlssNr::g_notedHdr.clear();
    g_hdrDispatches.clear();
    g_hdrAvailable = true;
    g_hdrSupports = true;
    Config::Instance()->DlssNrEnabled.value = enabled;
    Config::Instance()->DlssNrZeroGuideReset.value = false;
    Config::Instance()->DlssNrSynthMotion.value = false;
    Config::Instance()->DlssNrHdrPresent.value = false;
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

    // --- an HDR swapchain ------------------------------------------------------------------------------
    // Every present host says its frame is SDR (PresentFrameDefaults). An HDR10 or scRGB backbuffer is
    // not, and the model shown PQ code values or linear light as sRGB answers for a picture that is not on
    // screen; until the hosts convert, such a frame is declined (DlssNr_PresentColour.h).
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(!r.Frame() && !r.Frame());
        assert(g_transferBuffersMade == 0 && r.device.lists.empty() && DlssNr::g_creations.empty());
        assert(LogCount("no pass on this frame") == 1);
        CASE("HDR10 (ten bits, PQ): declined before anything is built, and said once");

        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        assert(r.Frame());
        assert(DlssNr::g_creations.size() == 1 && CreatedOnEmptyListAt(0, 1920, 1080));
        CASE("the same ten-bit swapchain set back to sRGB builds, creates the model on its own list, and runs");

        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(!r.Frame());
        const auto made = g_transferBuffersMade;
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        assert(r.Frame() && DlssNr::g_lastPassReset && g_transferBuffersMade == made);
        assert(r.Frame() && !DlssNr::g_lastPassReset);
        CASE("HDR mid-session keeps what was built, and the first frame back is a reset, the next is not");
    }
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
        assert(r.Frame());
        CASE("ten bits with no colour space ever set is DXGI's default, sRGB, and runs");
    }
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT);
        assert(!r.Frame());
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        assert(!r.Frame());
        assert(g_transferBuffersMade == 0 && DlssNr::g_creations.empty());
        CASE("FP16 is scRGB on a flip-model swapchain whatever it was told, and is declined");
    }
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(r.Frame());
        CASE("eight bits cannot carry HDR, so a PQ colour space noted on one does not stop the pass");
    }
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
        for (const auto space :
             { DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020,
               DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020, DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 })
        {
            r.host.NoteColourSpace(space);
            assert(!r.Frame());
        }
        for (const auto space :
             { DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 })
        {
            r.host.NoteColourSpace(space);
            assert(r.Frame());
        }
        CASE("scRGB, HLG, Rec.2020 and studio PQ are declined; the Rec.709 gamma spaces run");
    }

    // --- HDR10 and scRGB at present: the decision (docs/dlssnr/design/hdr-present-conversion.md section 4.1) --------
    {
        using DlssNr::PresentColour;
        namespace Text = DlssNr::PresentColourText;
        const auto Record = [](bool known, bool valid, bool set, DXGI_FORMAT format,
                               DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)
        {
            DlssNr::PresentColourRecord record;
            record.known = known;
            record.valid = valid;
            record.colourSpaceSet = set;
            record.format = format;
            record.colourSpace = space;
            return record;
        };
        const DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM,         DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                        DXGI_FORMAT_B8G8R8X8_UNORM,         DXGI_FORMAT_R10G10B10A2_UNORM,
                                        DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R16G16B16A16_FLOAT,
                                        DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM };
        const DXGI_COLOR_SPACE_TYPE spaces[] = {
            DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,         DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709,
            DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709,       DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,  DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020,
            DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020, DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020,
            DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 };

        // Off: PresentColourRefusal on the colour space the host always passed, the same literal, whatever the record.
        for (const auto format : formats)
            for (const auto space : spaces)
                for (const auto& record : { Record(false, false, false, DXGI_FORMAT_UNKNOWN),
                                            Record(true, true, true, format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020),
                                            Record(true, false, true, DXGI_FORMAT_R8G8B8A8_UNORM, space) })
                {
                    const auto decision = DlssNr::ResolvePresentColour(format, record, false, space);
                    const char* legacy = DlssNr::PresentColourRefusal(format, space);
                    assert(decision.reason == legacy);
                    assert(decision.profile == (legacy != nullptr ? PresentColour::Declined : PresentColour::Sdr));
                }
        CASE("HdrPresent off: every format and colour space gets PresentColourRefusal's answer, the same literal");

        const auto On = [](DXGI_FORMAT format, const DlssNr::PresentColourRecord& record)
        { return DlssNr::ResolvePresentColour(format, record, true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020); };
        const auto R10 = DXGI_FORMAT_R10G10B10A2_UNORM;
        const auto F16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

        for (const auto format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM })
        {
            assert(On(format, Record(true, true, true, format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)).profile ==
                   PresentColour::Sdr);
            assert(On(format, Record(false, false, false, DXGI_FORMAT_UNKNOWN)).profile == PresentColour::Sdr);
        }
        CASE("HdrPresent on: eight bits run as SDR whatever is recorded, or when nothing is");

        assert(On(R10, Record(true, true, true, R10, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)).profile ==
               PresentColour::Hdr10);
        assert(On(R10, Record(true, true, false, R10)).profile == PresentColour::Sdr);
        for (const auto space : { DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709,
                                  DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709 })
            assert(On(R10, Record(true, true, true, R10, space)).profile == PresentColour::Sdr);
        for (const auto space : { DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
                                  DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020 })
            assert(On(R10, Record(true, true, true, R10, space)).reason == Text::kPqVariant);
        assert(On(R10, Record(true, true, true, R10, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)).reason ==
               Text::kTenBitLinear);
        for (const auto space : { DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020, DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020,
                                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 })
            assert(On(R10, Record(true, true, true, R10, space)).reason == Text::kOtherSpace);
        CASE("HdrPresent on, ten bits: full-range PQ is HDR10, the Rec.709 gamma spaces and none set are SDR, studio or "
             "YCbCr PQ, linear, HLG and Rec.2020 gamma are declined, each with its reason");

        assert(On(F16, Record(true, true, false, F16)).profile == PresentColour::ScRgb);
        assert(On(F16, Record(true, true, true, F16, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)).profile ==
               PresentColour::ScRgb);
        for (const auto space : { DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 })
            assert(On(F16, Record(true, true, true, F16, space)).reason == Text::kFp16NotScRgb);
        CASE("HdrPresent on, FP16: scRGB set or by default is scRGB; any other colour space set is declined");

        for (const auto format : { R10, F16 })
        {
            assert(On(format, Record(false, false, false, DXGI_FORMAT_UNKNOWN)).reason == Text::kNoRecord);
            assert(On(format, Record(true, false, true, format, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)).reason ==
                   Text::kResized);
            const auto other = format == R10 ? F16 : R10;
            assert(On(format, Record(true, true, true, other, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709)).reason ==
                   Text::kOtherFormat);
        }
        assert(On(DXGI_FORMAT_R10G10B10A2_TYPELESS, Record(true, true, true, DXGI_FORMAT_R10G10B10A2_TYPELESS,
                                                           DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020))
                   .reason == Text::kTypeless);
        assert(On(DXGI_FORMAT_R16G16B16A16_TYPELESS, Record(true, true, false, DXGI_FORMAT_R16G16B16A16_TYPELESS)).reason ==
               Text::kTypeless);
        // Another format keeps the switch-off answer for the colour space the host passes.
        assert(On(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, Record(true, true, true, R10)).reason ==
               DlssNr::PresentColourRefusal(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
                                            DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
        CASE("HdrPresent on: no record, a resize to another format since the colour space was set, a record for another "
             "format, and a typeless format are declined; an unlisted format answers as with the switch off");

        const DlssNr::PresentColourDecision sdr {}, hdr10 { PresentColour::Hdr10 }, declined { PresentColour::Declined,
                                                                                                 Text::kResized };
        assert(!DlssNr::ReportsHdrStatus(DXGI_FORMAT_R8G8B8A8_UNORM, hdr10, true));
        assert(!DlssNr::ReportsHdrStatus(R10, sdr, true) && DlssNr::ReportsHdrStatus(R10, hdr10, false));
        assert(DlssNr::ReportsHdrStatus(F16, declined, true) && !DlssNr::ReportsHdrStatus(F16, declined, false));
        CASE("the menu hears of a swapchain the switch acts on, or one it declines with the switch on; never of eight bits");
    }

    // --- the D3D12 route's colour record (DlssNr_SwapchainColour.h) ---------------------------------------------------
    {
        DlssNr::SwapchainColour::Book book;
        int windowA = 0, windowB = 0;
        assert(!book.Lookup(&windowA).known && !book.Lookup(nullptr).known);
        book.Resized(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM);
        book.ColourSpaceSet(nullptr, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(!book.Lookup(&windowA).known);
        CASE("the record: nothing is known of a window never seen; a resize does not invent a record; no window, none");

        book.Created(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM);
        auto record = book.Lookup(&windowA);
        assert(record.known && record.valid && !record.colourSpaceSet && record.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        book.Resized(&windowA, DXGI_FORMAT_R16G16B16A16_FLOAT);
        record = book.Lookup(&windowA);
        assert(record.valid && !record.colourSpaceSet && record.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        CASE("the record: a creation knows the format and no colour space; with none set, the default follows a resize");

        book.ColourSpaceSet(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        book.Resized(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM);
        record = book.Lookup(&windowA);
        assert(record.valid && record.colourSpaceSet && record.colourSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        book.Resized(&windowA, DXGI_FORMAT_R8G8B8A8_UNORM);
        assert(!book.Lookup(&windowA).valid);
        book.Resized(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM);
        assert(!book.Lookup(&windowA).valid);
        book.ColourSpaceSet(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(book.Lookup(&windowA).valid);
        CASE("the record: a resize that keeps the format keeps the colour space; one that changes it leaves it unknown, "
             "even after changing back, until the next SetColorSpace1");

        book.Created(&windowB, DXGI_FORMAT_R16G16B16A16_FLOAT);
        book.Created(&windowA, DXGI_FORMAT_R10G10B10A2_UNORM);
        record = book.Lookup(&windowA);
        assert(record.valid && !record.colourSpaceSet && record.format == DXGI_FORMAT_R10G10B10A2_UNORM);
        assert(book.Lookup(&windowB).format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        CASE("the record: a new swapchain on a window forgets what the old one was told; windows are their own");
    }

    // --- HDR10 and scRGB at present: the D3D11 bridge's host (section 5.2) --------------------------------------------
    {
        Fresh();
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        assert(!r.Frame());
        assert(g_hdrPassesBuilt == 0 && g_hdrDispatches.empty() && g_transferBuffersMade == 0);
        assert(!DlssNr::g_notedHdr.empty() && DlssNr::g_notedHdr.back().profile == DlssNr::PresentColour::Hdr10 &&
               DlssNr::g_notedHdr.back().reason == nullptr && !DlssNr::g_notedHdr.back().converted);
        CASE("HdrPresent off: an HDR10 swapchain is declined as before, nothing HDR is built, and the menu hears of it");

        Config::Instance()->DlssNrHdrPresent.value = true;
        const auto built = g_hdrPassesBuilt;
        assert(r.Frame() && r.Frame());
        assert(g_hdrPassesBuilt == built + 1);
        assert(g_hdrDispatches.size() == 4);
        const auto& decode = g_hdrDispatches[2];
        const auto& encode = g_hdrDispatches[3];
        assert(decode.mode == DlssNr::PresentHdr::kModeDecode && decode.original == &r.source &&
               decode.profile == DlssNr::PresentHdr::kProfileHdr10);
        assert(encode.mode == DlssNr::PresentHdr::kModeEncode && encode.original == &r.source &&
               encode.processed == decode.out && encode.out == r.lastOutput && encode.profile == decode.profile);
        assert(DlssNr::g_lastPassColour == decode.out && DlssNr::g_lastPassFrame.ColourIsLinearHdr);
        assert(DlssNr::g_lastPassFrame.PresentWhite == DlssNr::PresentHdr::kSdrWhiteScRgb);
        assert(DlssNr::g_lastPassFrame.PresentQuantRel == DlssNr::PresentHdr::kPqQuantRel);
        assert(r.lastOutput->desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
        assert(DlssNr::g_notedHdr.back().converted && DlssNr::g_notedHdr.back().reason == nullptr);
        CASE("HdrPresent on, HDR10: decoded into the working colour, the pass told linear HDR with the SDR white and a "
             "PQ code as its unit, encoded back with the source as reference into the source's format");
    }
    {
        Fresh();
        Config::Instance()->DlssNrHdrPresent.value = true;
        Rig r;
        r.Size(2560, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT);
        const auto whole = DlssNr::GpuTiming::g_conversionsWhole;
        assert(r.Frame());
        assert(g_hdrDispatches.size() == 2 && g_hdrDispatches[0].profile == DlssNr::PresentHdr::kProfileScRgb);
        assert(DlssNr::g_lastPassFrame.ColourIsLinearHdr && DlssNr::g_lastPassFrame.PresentQuantRel == 0.0f);
        assert(DlssNr::GpuTiming::g_conversionsWhole == whole + 1);
        CASE("HdrPresent on, FP16 never told a colour space: scRGB, converted, its FP16 unit kept, timed as one sample");

        DlssNr::g_refuseModel = true;
        const auto finished = DlssNr::GpuTiming::g_conversionsFinished;
        assert(!r.Frame());
        assert(g_hdrDispatches.size() == 3 && g_hdrDispatches.back().mode == DlssNr::PresentHdr::kModeDecode);
        assert(DlssNr::GpuTiming::g_conversionsFinished == finished + 1 &&
               DlssNr::GpuTiming::g_conversionsWhole == whole + 1);
        CASE("the model refusing: decoded, never encoded, the source shown as it is, the timing sample closed unfinished "
             "on the open list");
    }
    {
        Fresh();
        Config::Instance()->DlssNrHdrPresent.value = true;
        Rig r;
        r.Size(1920, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        g_hdrAvailable = false;
        assert(!r.Frame());
        assert(g_transferBuffersMade == 0 && g_hdrDispatches.empty());
        assert(DlssNr::g_notedHdr.back().reason == DlssNr::PresentColourText::kNoShader);
        CASE("HdrPresent on without the shader in the build: declined before anything is built, and it says so");

        g_hdrAvailable = true;
        g_hdrSupports = false;
        assert(!r.Frame());
        assert(g_hdrDispatches.empty() && DlssNr::g_notedHdr.back().reason == DlssNr::PresentColourText::kNoTypedStore);
        CASE("HdrPresent on, a device without typed UAV store for the format: declined, said, nothing dispatched");

        g_hdrSupports = true;
        assert(r.Frame());
        r.Size(1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT); // a resize to FP16, and no SetColorSpace1 after it
        const auto dispatches = g_hdrDispatches.size();
        assert(!r.Frame() && !r.Frame());
        assert(g_hdrDispatches.size() == dispatches);
        assert(DlssNr::g_notedHdr.back().reason == DlssNr::PresentColourText::kResized);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        assert(r.Frame() && g_hdrDispatches.back().profile == DlssNr::PresentHdr::kProfileScRgb);
        CASE("a format change after the colour space was set is declined until it is set again, then converted");

        r.Size(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
        const auto beforeSdr = g_hdrDispatches.size();
        assert(r.Frame());
        assert(g_hdrDispatches.size() == beforeSdr && DlssNr::g_lastPassFrame.PresentWhite == 0.0f &&
               !DlssNr::g_lastPassFrame.ColourIsLinearHdr);
        CASE("back to eight bits: the plain conversions and the SDR description, exactly as before");

        r.Size(1920, 1080, DXGI_FORMAT_R16G16B16A16_FLOAT);
        r.host.NoteColourSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        assert(r.Frame());
        Config::Instance()->DlssNrHdrPresent.value = false;
        const auto beforeOff = g_hdrDispatches.size();
        assert(!r.Frame() && g_hdrDispatches.size() == beforeOff);
        CASE("switched off mid-session: the next frame is declined with the old text and nothing HDR is dispatched");
    }

    assert(g_transferBuffersLive == 0);
    std::cout << "present host: " << cases << " cases passed\n";
    return 0;
}
