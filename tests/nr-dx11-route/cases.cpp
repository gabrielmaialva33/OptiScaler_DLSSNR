// tests/nr-dx11-route: the D3D11 DLSS route decision (retoque/dlssnr/DlssNr_Dx11Route.h) and the production layer
// that the call site and the panel use (retoque/dlssnr/DlssNr_Dx11Upscaler.h), the latter against stubs/Config.h.
//
//   cases            run every case, exit non-zero on a failure
//   cases --texts    print each report text the panel translates, one per line, for run.py's pt-BR check

#include <dlssnr/DlssNr_Dx11Upscaler.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
int g_failures = 0;

void Require(bool condition, const char* what)
{
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool Contains(const char* text, const char* part) { return text != nullptr && std::strstr(text, part) != nullptr; }

using DlssNr::Dx11Route::Decide;
using DlssNr::Dx11Route::Inputs;
using DlssNr::Dx11Route::Kind;
using DlssNr::Dx11Route::Report;
using DlssNr::Dx11Route::ReportText;

constexpr Kind kKinds[] = { Kind::Other, Kind::NativeDlss, Kind::BridgedDlss };
constexpr Report kReports[] = { Report::NoPass, Report::NoGuides };
constexpr uint32_t kHookMethods[] = { 0, 1, 2, 3, 0xffffffffu };

void PureReport()
{
    for (Kind kind : kKinds)
        for (uint32_t method : kHookMethods)
        {
            Require(Decide(Inputs { false, kind, method }) == Report::None, "NR off: nothing to report, ever");

            const Report on = Decide(Inputs { true, kind, method });
            if (kind != Kind::NativeDlss)
                Require(on == Report::None, "dlss_12 and the routes this header does not judge report nothing");
            else if (method == 2)
                Require(on == Report::NoGuides, "native DLSS at HookMethod 2: the guides never reach the pass");
            else
                Require(on == Report::NoPass, "native DLSS at HookMethod 0, 1 or anything else: after the upscaler");
        }

    Require(ReportText(Report::None) == nullptr, "None has no text");

    for (Report report : kReports)
    {
        const char* text = ReportText(report);
        Require(text != nullptr, "every report has a text");
        Require(Contains(text, "natively on D3D11"), "the text names the reason: native D3D11 DLSS");
        Require(Contains(text, "Dx11Upscaler dlss"), "the text names the setting that put DLSS there");
        Require(Contains(text, "(dlss_12)") && Contains(text, "Dx11Upscaler=dlss_12"),
                "the text names dlss_12 as the route that carries NR");
        Require(Contains(text, "DLSS w/Dx12") && Contains(text, "under Upscalers"),
                "the text names the entry the menu's Upscalers card shows");
        Require(std::strchr(text, '\n') == nullptr && std::strchr(text, '\t') == nullptr,
                "a text is one line: it is also a log line and a pt-BR key");
    }

    Require(std::strcmp(ReportText(Report::NoPass), ReportText(Report::NoGuides)) != 0,
            "the two reports say different things");
    Require(Contains(ReportText(Report::NoGuides), "depth and motion"),
            "the HookMethod 2 text says what is missing: the depth and motion");
}

void PureOnce()
{
    {
        DlssNr::Dx11Route::LogOnce once;
        Require(!once.First(Report::None), "None is never logged");

        // A session's worth of native-DLSS evaluates: one line.
        int lines = 0;
        for (int frame = 0; frame < 3600; ++frame)
            lines += once.First(Decide(Inputs { true, Kind::NativeDlss, 1 })) ? 1 : 0;
        Require(lines == 1, "3600 evaluates on native DLSS log one line, not one per frame");

        Require(once.First(Report::NoGuides), "a different report is logged once too");

        // Routes moving back and forth (a menu switch, a feature recreated) are not told again.
        lines = 0;
        for (int i = 0; i < 100; ++i)
        {
            lines += once.First(Report::NoPass) ? 1 : 0;
            lines += once.First(Report::NoGuides) ? 1 : 0;
            lines += once.First(Report::None) ? 1 : 0;
        }
        Require(lines == 0, "a report already said is not said again when the route comes back");
    }

    {
        // The evaluate thread is the game's; one latch, several threads, one line.
        DlssNr::Dx11Route::LogOnce once;
        std::atomic<int> lines { 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back(
                [&]
                {
                    for (int i = 0; i < 1000; ++i)
                        if (once.First(Report::NoPass))
                            lines.fetch_add(1);
                });
        for (auto& thread : threads)
            thread.join();
        Require(lines.load() == 1, "racing evaluates still log one line");
    }
}

void PureSeen()
{
    using namespace std::chrono_literals;
    using DlssNr::Dx11Route::Seen;

    Require(Seen(Kind::NativeDlss, 0ms) == Kind::NativeDlss, "a sample just taken is current");
    Require(Seen(Kind::NativeDlss, DlssNr::Dx11Route::kSeenFor) == Kind::NativeDlss, "the window is inclusive");
    Require(Seen(Kind::NativeDlss, DlssNr::Dx11Route::kSeenFor + 1ms) == Kind::Other,
            "a route not seen for longer than the window is not reported");
    Require(Seen(Kind::NativeDlss, -5ms) == Kind::NativeDlss, "a sample stamped after the panel's clock is current");
    Require(Seen(Kind::NativeDlss, 24h) == Kind::Other, "an old sample is not reported");
}

// The production layer, in the order a session meets it. Its state is process-wide (inline globals), as in the DLL,
// so each part starts from the shipped defaults and counts log lines from where it began.
void ResetConfig()
{
    Config& cfg = *Config::Instance();
    cfg.DlssNrEnabled.reset();
    cfg.DlssNrHookMethod.reset();
    cfg.Dx11Upscaler.reset();
}

std::string LastLine() { return g_fakeLog.empty() ? std::string() : g_fakeLog.back(); }

void ProductionReport()
{
    namespace Detail = DlssNr::Dx11RouteDetail;
    Config& cfg = *Config::Instance();
    ResetConfig();
    const size_t base = g_fakeLog.size();
    const auto lines = [base] { return g_fakeLog.size() - base; };

    // --- NR off: nothing is stored, nothing is said.
    for (int frame = 0; frame < 100; ++frame)
        DlssNr::NoteDx11Evaluate(Upscaler::DLSS);

    Require(lines() == 0, "NR off: no line at all");
    Require(Detail::g_lastAt.load() == 0 && Detail::g_lastRoute.load() == Kind::Other,
            "NR off: the evaluate note stores nothing");
    Require(DlssNr::Dx11RouteReport() == Report::None, "NR off: no panel line");

    // --- NR on, native DLSS evaluates, HookMethod 1 (the default).
    cfg.DlssNrEnabled = true;
    Require(DlssNr::Dx11RouteReport() == Report::None, "no panel line before any D3D11 evaluate");

    for (int frame = 0; frame < 600; ++frame)
        DlssNr::NoteDx11Evaluate(Upscaler::DLSS);

    Require(lines() == 1, "600 native evaluates log one line");
    Require(LastLine() == std::string("WARN DLSS-NR: ") + ReportText(Report::NoPass),
            "the line is the after-upscaler report, at warning level");
    Require(DlssNr::Dx11RouteReport() == Report::NoPass, "the panel shows the same report");

    // HookMethod is read live by the panel; the log says the HookMethod 2 report at the next evaluate.
    cfg.DlssNrHookMethod = 2u;
    Require(DlssNr::Dx11RouteReport() == Report::NoGuides, "the panel follows HookMethod at once");
    for (int frame = 0; frame < 10; ++frame)
        DlssNr::NoteDx11Evaluate(Upscaler::DLSS);
    Require(lines() == 2 && LastLine() == std::string("WARN DLSS-NR: ") + ReportText(Report::NoGuides),
            "the HookMethod 2 report is logged once");
    cfg.DlssNrHookMethod = 1u;

    // Switched to dlss_12 in the menu: the line goes at the next evaluate, and nothing is logged.
    DlssNr::NoteDx11Evaluate(Upscaler::DLSS_on12);
    Require(DlssNr::Dx11RouteReport() == Report::None && lines() == 2, "dlss_12: no panel line, no log line");

    // Ray reconstruction and the other upscalers are not judged here.
    for (Upscaler upscaler : { Upscaler::DLSSD, Upscaler::FSR22, Upscaler::XeSS, Upscaler::FFX_on12 })
    {
        DlssNr::NoteDx11Evaluate(upscaler);
        Require(DlssNr::Dx11RouteReport() == Report::None, "routes other than native DLSS get no panel line");
    }

    // Back to native: the panel line returns, the log stays quiet.
    DlssNr::NoteDx11Evaluate(Upscaler::DLSS);
    Require(DlssNr::Dx11RouteReport() == Report::NoPass && lines() == 2,
            "back on native DLSS: panel line again, no second log line");

    // NR switched off in the menu: the line goes at once, with the record still fresh.
    cfg.DlssNrEnabled = false;
    Require(DlssNr::Dx11RouteReport() == Report::None, "NR off: no panel line, even with a fresh native record");
    cfg.DlssNrEnabled = true;

    // A game that stopped evaluating (feature released, a loading screen): the line ages out.
    Detail::g_lastAt.store(
        (std::chrono::steady_clock::now() - DlssNr::Dx11Route::kSeenFor - std::chrono::milliseconds(50))
            .time_since_epoch()
            .count());
    Require(DlssNr::Dx11RouteReport() == Report::None, "no D3D11 evaluate for longer than the window: no panel line");

    Require(lines() == 2, "two lines in the whole session, one per report");
    ResetConfig();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--texts") == 0)
    {
        for (Report report : kReports)
            std::printf("%s\n", ReportText(report));
        return 0;
    }

    PureReport();
    PureOnce();
    PureSeen();
    ProductionReport();

    if (g_failures != 0)
    {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("PASS: D3D11 route -- NR off says nothing; native D3D11 DLSS is reported once per report and shown "
                "while it runs\n");
    return 0;
}
