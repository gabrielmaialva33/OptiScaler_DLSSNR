// Included after the runner extracts the real report types and initializers.
static int siteFailures = 0;
static void Expect(bool condition, const char* name)
{
    if (!condition)
    {
        std::cerr << "FAIL real site: " << name << '\n';
        ++siteFailures;
    }
}

static auto At(int ms) { return DlssNr::LogRate::Clock::time_point {} + std::chrono::milliseconds(ms); }

template <class Change> static void ManualChange(const char* name, int source, Change change)
{
    ComposeSite site;
    site.whitePointSource = source;
    Expect(site.Observe(At(0)), "first composition");
    const auto original = site.inputs();
    change(site);
    Expect(site.Observe(At(100)), name);
    site.inputs() = original;
    Expect(site.Observe(At(200)), "manual reversal inside one second");
    Expect(!site.Observe(At(300)), "unchanged config is silent");
}

int main()
{
    ManualChange("manual paper white", 0, [](auto& s) { s.whitePointScale = 2.0f; });
    ManualChange("source switch with same numerical white point", 0, [](auto& s) { s.whitePointSource = 1; });
    ManualChange("game exposure trim", 1, [](auto& s) { s.whitePointTrim = 2.0f; });
    ManualChange("scan trim", 2, [](auto& s) { s.scanTrim = 2.0f; });
    ManualChange("scan inversion", 2, [](auto& s) { s.scanInverted = true; });
    ManualChange("scan anchor edit", 2, [](auto& s) { s.anchors[0].white = 2; });
    ManualChange("anchor edit below serialization precision", 2,
                 [](auto& s) { s.anchors[0].white = std::nextafter(1.0f, 2.0f); });
    ManualChange("scan anchor input", 2, [](auto& s) { s.anchors[0].scan = 2; });
    ManualChange("scan anchor count", 2, [](auto& s) { s.anchors.push_back({ 2, 3 }); });
    ManualChange("scan collection toggle", 2, [](auto& s) { s.cfg.DlssNrScanExposure.value = true; });
    ManualChange("hold state", 1, [](auto& s) { s.holdFrame = true; });
    ManualChange("model dimensions", 1, [](auto& s) { s.g_nr.workWidth = 2560; });
    ManualChange("composition detail", 1, [](auto& s) { s.detail = 0.5f; });

    ComposeSite inactive;
    Expect(inactive.Observe(At(0)), "first manual source");
    inactive.whitePointTrim = 2;
    inactive.scanTrim = 2;
    inactive.scanInverted = true;
    inactive.anchors[0].white = 2;
    inactive.cfg.DlssNrScanExposure.value = true;
    Expect(!inactive.Observe(At(100)), "inactive source controls are not configuration events");
    Expect(!inactive.Observe(At(1000)), "inactive source controls do not become drift either");

    ComposeSite clamped;
    clamped.whitePointSource = 1;
    clamped.whitePointTrim = 4;
    Expect(clamped.Observe(At(0)), "first clamped trim");
    clamped.whitePointTrim = 5;
    Expect(!clamped.Observe(At(100)), "equivalent effective trims are silent");

    // Natural drift must keep the one-second window and the last emitted baseline.
    ComposeSite natural;
    natural.whitePointSource = 1;
    Expect(natural.Observe(At(0)), "first measured white point");
    for (int ms = 10; ms < 1000; ms += 10)
    {
        natural.gameExposure += 0.001f;
        Expect(!natural.Observe(At(ms)), "natural exposure drift is throttled");
    }
    Expect(natural.Observe(At(1000)), "settled accumulated drift emitted at the boundary");
    Expect(!natural.Observe(At(2000)), "same emitted drift stays silent");

    ExposureSite offered;
    Expect(offered.Observe(At(0)), "first offered exposure");
    offered.preExposure = 2.0f;
    Expect(!offered.Observe(At(100)), "offered pre-exposure drift");
    offered.autoExposureFlag = true;
    Expect(offered.Observe(At(200)), "game auto-exposure flag immediately");
    offered.exposureTex = nullptr;
    Expect(offered.Observe(At(300)), "texture availability immediately");
    offered.havePre = false;
    Expect(offered.Observe(At(400)), "pre-exposure availability immediately");

    ExposureValueSite value;
    Expect(value.Observe(At(0)), "first readback exposure");
    value.g_nr.gamePreExposure = 2;
    Expect(!value.Observe(At(100)), "readback pre-exposure drift waits");
    Expect(value.Observe(At(1000)), "pre-exposure alone changes the printed derived white point");

    ScanSite scan;
    Expect(scan.Observe(At(0)), "first scan");
    scan.scanned = 1.5f;
    Expect(!scan.Observe(At(100)), "scan value drift waits");
    scan.which = 2;
    Expect(scan.Observe(At(200)), "new candidate immediately despite same numeric value");
    scan.which = 1;
    Expect(scan.Observe(At(300)), "candidate reversal immediately");
    DlssNr::ExposureScan::enabled = true;
    Expect(scan.Observe(At(400)), "scan config change immediately");
    scan.g_nr.gameExposure = 0;
    Expect(scan.Observe(At(500)), "loss of comparison exposure immediately");
    scan.g_nr.gameExposure = 1;
    Expect(scan.Observe(At(600)), "recovery of comparison exposure immediately");
    scan.low = 0.25f;
    Expect(!scan.Observe(At(700)), "scan range drift waits");
    Expect(scan.Observe(At(1600)), "scan range drift emitted after interval");

    std::cout << (siteFailures ? "FAIL" : "PASS") << ": four real report classifications and initializers\n";
    return siteFailures ? 1 : 0;
}
