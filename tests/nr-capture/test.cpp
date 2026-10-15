// tests/nr-capture: the comparison capture's pure half, compiled from the production headers.
//
// The assertions here cover what C++ can check against itself: the state machine over a fake backend, the menu
// button's table, names, the cap, the UTC stamp, the provenance record and the write-then-rename. The image files
// are checked from the other side: this writes them, with the source surfaces they were made from, and run.py
// decodes them with Python's zlib and struct and recomputes every expected value independently.

#include <dlssnr/DlssNr_PairCapture.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DlssNr;
using namespace DlssNr::PairCapture;
namespace fs = std::filesystem;
namespace IF = DlssNr::ImageFile;

static int g_cases = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

// --- A fake backend that counts ---------------------------------------------------------------------------------

struct FakeReadback
{
    int id = 0; // 0: none
};

struct FakeTicket
{
    int list = 0;
};

struct FakePayload
{
    std::string tag;
};

struct FakeBackend
{
    using Readback = FakeReadback;
    using Ticket = FakeTicket;
    using Payload = FakePayload;
    struct Recording
    {
        int list = 1;
    };

    int nextId = 1;
    int allocs = 0;
    int failAllocAt = -1; // fail the n-th Alloc (1-based)
    bool trackable = true;
    int tracks = 0;
    int copies = 0;
    int copiesThisRecording = 0; // Track must come before any copy of the recording it tracks
    std::set<int> live;          // ids allocated and not released or leaked
    std::set<int> leaked;        // ids handed to Leak
    std::set<int> writer;        // ids the writer holds a reference to
    std::set<int> released;      // ids released by the session
    std::set<int> completedLists;
    std::set<int> readyLists;
    int hands = 0;
    bool handFails = false;
    bool copyThrows = false; // an exception part way through a recording, as the record site can meet one
    std::string lastPayload;
    Completion lastCompletion = Completion::AssumedFrames;
    // Anything a real backend could block in. The template must never call one; nothing here can be called to wait.
    int blockingWaits = 0;

    bool Alloc(Recording&, int which, Readback& out)
    {
        if (which == 0)
            copiesThisRecording = 0;
        ++allocs;
        if (allocs == failAllocAt)
            return false;
        out.id = nextId++;
        live.insert(out.id);
        return true;
    }
    bool Track(Recording& r, Ticket& t)
    {
        CHECK(copiesThisRecording == 0); // Submission::Track: before recording any use
        ++tracks;
        if (!trackable)
            return false;
        t.list = r.list;
        return true;
    }
    void Copy(Recording&, int, Readback& rb)
    {
        CHECK(rb.id != 0);
        if (copyThrows)
            throw std::runtime_error("copy");
        ++copies;
        ++copiesThisRecording;
    }
    bool Completed(const Ticket& t) { return completedLists.count(t.list) != 0; }
    bool Ready(const Ticket& t) { return readyLists.count(t.list) != 0; }
    void Release(Readback& rb)
    {
        if (rb.id != 0)
        {
            CHECK(live.erase(rb.id) == 1);
            released.insert(rb.id);
        }
        rb = {};
    }
    bool Hand(Readback (&rb)[2], Payload&& p, Completion c)
    {
        if (handFails)
            return false;
        ++hands;
        writer.insert(rb[0].id);
        writer.insert(rb[1].id);
        lastPayload = p.tag;
        lastCompletion = c;
        return true;
    }
    void Leak(Readback (&rb)[2])
    {
        for (auto& r : rb)
        {
            if (r.id != 0)
            {
                CHECK(live.erase(r.id) == 1);
                leaked.insert(r.id);
            }
            r = {};
        }
    }
};

static void StateMachine()
{
    // Idle: Poll does nothing, a record site does nothing.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        FakeBackend::Recording rec;
        CHECK(!s.Busy());
        s.Poll(b, 0);
        CHECK(!s.Record(b, rec, FakePayload { "x" }, nullptr));
        CHECK(b.allocs == 0 && b.tracks == 0 && b.copies == 0);
        ++g_cases;
    }

    // Tracked: request, record, nothing handed before the ticket completes, one hand when it does, a second request
    // ignored while under way, the session's references released only when the list can never run again.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        FakeBackend::Recording rec;
        CHECK(s.Request(0));
        CHECK(!s.Request(0)); // one at a time
        CHECK(s.Current() == Phase::Requested);
        CHECK(s.Record(b, rec, FakePayload { "first" }, nullptr));
        CHECK(s.Current() == Phase::Pending);
        CHECK(b.allocs == 2 && b.tracks == 1 && b.copies == 2 && b.live.size() == 2);
        CHECK(!s.Request(0)); // still one at a time
        for (int i = 0; i < 100; ++i)
            s.Poll(b, 10);
        CHECK(b.hands == 0 && s.Current() == Phase::Pending); // nothing mapped before completion
        b.completedLists.insert(1);
        s.Poll(b, 10);
        CHECK(b.hands == 1 && b.lastPayload == "first" && b.lastCompletion == Completion::Submission);
        CHECK(s.Current() == Phase::Writing);
        s.Poll(b, 10);
        CHECK(b.hands == 1);                                 // exactly one write
        CHECK(b.released.empty() && s.RetainedCount() == 1); // completed is not ready
        b.readyLists.insert(1);
        s.Poll(b, 10);
        CHECK(b.released.size() == 2 && b.live.empty() && s.RetainedCount() == 0);
        CHECK(!s.Request(0)); // the writer has not finished
        s.Finished(true, nullptr, "stem-1");
        CHECK(s.Current() == Phase::Idle && s.LastOutcome().ok && s.LastOutcome().stem == "stem-1");
        CHECK(s.Request(0));
        CHECK(b.blockingWaits == 0);
        ++g_cases;
    }

    // A tracked recording that never completes: kept, never released, given up on after kGiveUpPolls.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        FakeBackend::Recording rec;
        CHECK(s.Request(0) && s.Record(b, rec, FakePayload { "never" }, nullptr));
        for (uint64_t i = 0; i + 1 < kGiveUpPolls; ++i)
            s.Poll(b, 0);
        CHECK(s.Current() == Phase::Pending && b.leaked.empty());
        s.Poll(b, 0);
        CHECK(s.Current() == Phase::Idle && b.hands == 0);
        CHECK(b.leaked.size() == 2 && b.released.empty() && b.live.empty());
        CHECK(s.LastOutcome().reason == kReasonNoCompletion);
        for (int i = 0; i < 100; ++i)
            s.Poll(b, 0);
        CHECK(b.released.empty()); // never released later either
        // And the capture is off for the session: a route that never completes cannot leak a pair per press.
        CHECK(s.GaveUp() && !s.Request(0) && s.Current() == Phase::Idle);
        ++g_cases;
    }

    // Untracked: read after kAssumedPolls dispatches and labelled so; our reference let go after the retirement count.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        b.trackable = false;
        FakeBackend::Recording rec;
        CHECK(s.Request(0) && s.Record(b, rec, FakePayload { "assumed" }, nullptr));
        for (uint64_t i = 0; i + 1 < kAssumedPolls; ++i)
            s.Poll(b, 0);
        CHECK(b.hands == 0);
        s.Poll(b, 0);
        CHECK(b.hands == 1 && b.lastCompletion == Completion::AssumedFrames);
        s.Finished(true, nullptr, "s");
        for (uint64_t i = kAssumedPolls; i + 1 < kUntrackedReleasePolls; ++i)
            s.Poll(b, 0);
        CHECK(b.released.empty());
        s.Poll(b, 0);
        CHECK(b.released.size() == 2 && !s.Busy());
        CHECK(std::string(CompletionName(Completion::AssumedFrames)) == "assumed-8-frames");
        ++g_cases;
    }

    // A refusal settles the reason and allocates nothing; a request nothing took expires.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        FakeBackend::Recording rec;
        CHECK(s.Request(0));
        const char* why = nullptr;
        CHECK(!s.Record(b, rec, FakePayload {}, kReasonCompare, &why));
        CHECK(why == kReasonCompare);
        CHECK(s.Current() == Phase::Idle && b.allocs == 0 && s.LastOutcome().reason == kReasonCompare);
        CHECK(!s.Record(b, rec, FakePayload {}, nullptr, &why) && why == nullptr); // not requested: not this call's
        CHECK(s.Request(100));
        CHECK(!s.Request(100 + kRequestExpiryMs - 10)); // an ignored press does not move the expiry
        s.ExpireIfStale(100 + kRequestExpiryMs - 1);
        CHECK(s.Current() == Phase::Requested);
        s.Poll(b, 100 + kRequestExpiryMs);
        CHECK(s.Current() == Phase::Idle && s.LastOutcome().reason == kReasonExpired);
        ++g_cases;
    }

    // An allocation that fails releases what was made (never recorded into) and records nothing.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        b.failAllocAt = 2;
        FakeBackend::Recording rec;
        CHECK(s.Request(0));
        CHECK(!s.Record(b, rec, FakePayload {}, nullptr));
        CHECK(b.tracks == 0 && b.copies == 0 && b.live.empty() && b.released.size() == 1);
        CHECK(s.LastOutcome().reason == kReasonAllocation && s.Current() == Phase::Idle);
        ++g_cases;
    }

    // Retained readbacks are bounded: past kMaxRetained a capture is refused rather than holding more.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        for (size_t i = 0; i < kMaxRetained; ++i)
        {
            FakeBackend::Recording rec;
            rec.list = static_cast<int>(10 + i);
            CHECK(s.Request(0) && s.Record(b, rec, FakePayload {}, nullptr));
            b.completedLists.insert(rec.list);
            s.Poll(b, 0);
            s.Finished(true, nullptr, "s");
        }
        CHECK(s.RetainedCount() == kMaxRetained);
        FakeBackend::Recording rec;
        rec.list = 99;
        CHECK(s.Request(0));
        CHECK(!s.Record(b, rec, FakePayload {}, nullptr));
        CHECK(s.LastOutcome().reason == kReasonRetained);
        b.readyLists.insert(10);
        s.Poll(b, 0);
        CHECK(s.RetainedCount() == kMaxRetained - 1);
        ++g_cases;
    }

    // A hand-off the writer cannot take: back to idle with a reason, readbacks kept until proven done.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        b.handFails = true;
        FakeBackend::Recording rec;
        CHECK(s.Request(0) && s.Record(b, rec, FakePayload {}, nullptr));
        b.completedLists.insert(1);
        s.Poll(b, 0);
        CHECK(s.Current() == Phase::Idle && s.LastOutcome().reason == kReasonHandOff && s.RetainedCount() == 1);
        CHECK(b.released.empty());
        ++g_cases;
    }

    // Shutdown frees only what is proven done: a retained pair whose list is ready is released, one whose list is
    // complete but not sealed is kept, and so is a capture still pending.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        for (const int list : { 1, 3 })
        {
            FakeBackend::Recording handed;
            handed.list = list;
            CHECK(s.Request(0) && s.Record(b, handed, FakePayload {}, nullptr));
            b.completedLists.insert(list);
            s.Poll(b, 0);
            s.Finished(true, nullptr, "s");
        }
        FakeBackend::Recording pending;
        pending.list = 2;
        CHECK(s.Request(0) && s.Record(b, pending, FakePayload {}, nullptr));
        b.readyLists.insert(1);
        // Not polled since list 1 became ready: Shutdown is what lets it go.
        s.Shutdown(b);
        CHECK(b.released.size() == 2 && b.leaked.size() == 4 && b.live.empty());
        CHECK(b.released.count(1) == 1 && b.released.count(2) == 1); // list 1's pair, the first allocated
        ++g_cases;
    }

    // An exception part way through the record site: the request it took goes back to idle with the reason, and what
    // was allocated -- copies may already be recorded into it -- is never released. A request still waiting is
    // withdrawn the same way; a recording that completed, and an idle session, are untouched.
    {
        Session<FakeBackend> s;
        FakeBackend b;
        FakeBackend::Recording rec;
        s.Abandon(kReasonInternal);
        CHECK(s.Current() == Phase::Idle && s.LastOutcome().reason == nullptr); // idle: nothing to withdraw

        CHECK(s.Request(0));
        b.copyThrows = true;
        bool threw = false;
        try
        {
            s.Record(b, rec, FakePayload { "t" }, nullptr);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        CHECK(threw && s.Current() == Phase::Recording && b.live.size() == 2);
        s.Abandon(kReasonInternal);
        CHECK(s.Current() == Phase::Idle && !s.LastOutcome().ok && s.LastOutcome().reason == kReasonInternal);
        CHECK(b.released.empty() && b.live.size() == 2 && s.RetainedCount() == 0);

        CHECK(s.Request(0));
        s.Abandon(kReasonInternal);
        CHECK(s.Current() == Phase::Idle);

        b.copyThrows = false;
        CHECK(s.Request(0) && s.Record(b, rec, FakePayload { "u" }, nullptr));
        s.Abandon(kReasonInternal);
        CHECK(s.Current() == Phase::Pending);
        ++g_cases;
    }
}

// --- The menu button ---------------------------------------------------------------------------------------------

static void Button()
{
    ButtonInputs in;
    in.enabled = true;
    in.d3d12Ran = true;
    auto state = ButtonFor(in);
    CHECK(state.shown && state.enabled && state.reason == nullptr);

    ButtonInputs off = in;
    off.enabled = false;
    CHECK(!ButtonFor(off).shown); // NR off: no frame to capture

    ButtonInputs none = in;
    none.d3d12Ran = false;
    CHECK(!ButtonFor(none).shown); // no D3D12 route ran: no frame to capture

    ButtonInputs vk = none;
    vk.nativeVulkan = true;
    state = ButtonFor(vk);
    CHECK(state.shown && !state.enabled && state.reason == kReasonVulkan);

    const struct
    {
        void (*set)(ButtonInputs&);
        const char* reason;
    } rows[] = {
        { [](ButtonInputs& i) { i.proxy = true; }, kReasonProxy },
        { [](ButtonInputs& i) { i.compare = 1; }, kReasonCompare },
        { [](ButtonInputs& i) { i.compare = 2; }, kReasonCompare },
        { [](ButtonInputs& i) { i.debugView = 3; }, kReasonDebugView },
        { [](ButtonInputs& i) { i.phase = Phase::Requested; }, kWaitingWords },
        { [](ButtonInputs& i) { i.phase = Phase::Recording; }, kWaitingWords },
        { [](ButtonInputs& i) { i.phase = Phase::Pending; }, kReadingWords },
        { [](ButtonInputs& i) { i.phase = Phase::Writing; }, kWritingWords },
        { [](ButtonInputs& i) { i.directoryFailed = true; }, kReasonDirectory },
        { [](ButtonInputs& i) { i.gaveUp = true; }, kReasonNoCompletion },
    };
    for (const auto& row : rows)
    {
        ButtonInputs i = in;
        row.set(i);
        state = ButtonFor(i);
        CHECK(state.shown && !state.enabled && state.reason == row.reason);
    }

    // Rows are checked top to bottom and the first that applies wins: every condition set at once, then cleared one
    // row at a time, shows each row's reason in the table's order.
    {
        ButtonInputs i = in;
        i.proxy = true;
        i.compare = 1;
        i.debugView = 2;
        i.phase = Phase::Pending;
        i.directoryFailed = true;
        i.gaveUp = true;
        CHECK(ButtonFor(i).reason == kReasonProxy);
        i.proxy = false;
        CHECK(ButtonFor(i).reason == kReasonCompare);
        i.compare = 0;
        CHECK(ButtonFor(i).reason == kReasonDebugView);
        i.debugView = 0;
        CHECK(ButtonFor(i).reason == kReadingWords);
        i.phase = Phase::Idle;
        CHECK(ButtonFor(i).reason == kReasonDirectory);
        i.directoryFailed = false;
        CHECK(ButtonFor(i).reason == kReasonNoCompletion);
        i.gaveUp = false;
        CHECK(ButtonFor(i).enabled);

        ButtonInputs v = in;
        v.proxy = true;
        v.nativeVulkan = true;
        CHECK(ButtonFor(v).reason == kReasonVulkan); // native Vulkan before every D3D12 row
        v.enabled = false;
        CHECK(!ButtonFor(v).shown); // NR off before everything
    }

    // The last outcome's line: a refusal for a setting is not shown once the setting is off; anything about the
    // attempt itself stays.
    {
        ButtonInputs now = in;
        CHECK(!ReasonStillApplies(kReasonCompare, now) && !ReasonStillApplies(kReasonDebugView, now) &&
              !ReasonStillApplies(kReasonProxy, now));
        now.compare = 2;
        now.debugView = 1;
        now.proxy = true;
        CHECK(ReasonStillApplies(kReasonCompare, now) && ReasonStillApplies(kReasonDebugView, now) &&
              ReasonStillApplies(kReasonProxy, now));
        for (const char* attempt :
             { kReasonExpired, kReasonWrite, kReasonRetained, kReasonAllocation, kReasonHandOff, kReasonShape,
               kReasonSurface, kReasonInternal, kReasonDirectory, IF::kRefusalFormat })
            CHECK(ReasonStillApplies(attempt, in));
    }
    ++g_cases;
}

// --- The record site's refusal -----------------------------------------------------------------------------------

static void Refusals()
{
    CHECK(RefusalFor(RefusalInputs {}) == nullptr);

    // Each alone gives its own reason.
    {
        RefusalInputs i;
        i.proxy = true;
        CHECK(RefusalFor(i) == kReasonProxy);
        i = {};
        i.compare = 2;
        CHECK(RefusalFor(i) == kReasonCompare);
        i = {};
        i.debugView = 4;
        CHECK(RefusalFor(i) == kReasonDebugView);
        i = {};
        i.directoryFailed = true;
        CHECK(RefusalFor(i) == kReasonDirectory);
        i = {};
        i.formatRefusal = IF::kRefusalFormat;
        CHECK(RefusalFor(i) == IF::kRefusalFormat);
        i = {};
        i.singleSample2d = false;
        CHECK(RefusalFor(i) == kReasonSurface);
        i = {};
        i.sameShape = false;
        CHECK(RefusalFor(i) == kReasonShape);
    }

    // All at once, cleared in order: settings before surfaces, so the reason shown is the one to act on first.
    {
        RefusalInputs i;
        i.proxy = true;
        i.compare = 1;
        i.debugView = 1;
        i.directoryFailed = true;
        i.formatRefusal = IF::kRefusalFormat;
        i.singleSample2d = false;
        i.sameShape = false;
        CHECK(RefusalFor(i) == kReasonProxy);
        i.proxy = false;
        CHECK(RefusalFor(i) == kReasonCompare);
        i.compare = 0;
        CHECK(RefusalFor(i) == kReasonDebugView);
        i.debugView = 0;
        CHECK(RefusalFor(i) == kReasonDirectory);
        i.directoryFailed = false;
        CHECK(RefusalFor(i) == IF::kRefusalFormat);
        i.formatRefusal = nullptr;
        CHECK(RefusalFor(i) == kReasonSurface);
        i.singleSample2d = true;
        CHECK(RefusalFor(i) == kReasonShape);
        i.sameShape = true;
        CHECK(RefusalFor(i) == nullptr);
    }

    // The format refusal is the plan's: a format outside the table is declined; a linear FP16 frame is not (it is a
    // PFM), nor a display-referred one (an 8-bit PNG, quantised).
    {
        RefusalInputs i;
        i.formatRefusal = IF::PlanFor(85 /* B5G6R5 */, false).refusal;
        CHECK(RefusalFor(i) == IF::kRefusalFormat);
        i.formatRefusal = IF::PlanFor(IF::Dxgi::R16G16B16A16_FLOAT, true).refusal;
        CHECK(RefusalFor(i) == nullptr);
        i.formatRefusal = IF::PlanFor(IF::Dxgi::R16G16B16A16_FLOAT, false).refusal;
        CHECK(RefusalFor(i) == nullptr);
    }
    ++g_cases;
}

// --- Names, the cap and the clock ------------------------------------------------------------------------------

static void Names(const fs::path& out)
{
    const auto epoch = FormatUtc(0);
    CHECK(std::string(epoch.iso) == "1970-01-01T00:00:00Z" && std::string(epoch.compact) == "19700101-000000");
    const auto leap = FormatUtc(1709210096); // 2024-02-29 12:34:56 UTC
    CHECK(std::string(leap.iso) == "2024-02-29T12:34:56Z" && std::string(leap.compact) == "20240229-123456");
    const auto today = FormatUtc(1760358896); // 2025-10-13 12:34:56 UTC
    CHECK(std::string(today.iso) == "2025-10-13T12:34:56Z");
    const auto before = FormatUtc(-1);
    CHECK(std::string(before.iso) == "1969-12-31T23:59:59Z");
    const auto far = FormatUtc(4102444800); // 2100-01-01, not a leap year
    CHECK(std::string(far.iso) == "2100-01-01T00:00:00Z");

    CHECK(SanitizeExeStem(L"CrimsonDesert") == "CrimsonDesert");
    CHECK(SanitizeExeStem(L"My Game (x64)") == "My_Game__x64_");
    CHECK(SanitizeExeStem(L"") == "game");
    CHECK(SanitizeExeStem(L"...") == "game");
    CHECK(SanitizeExeStem(L"\u00e9t\u00e9") == "_t_");
    CHECK(SanitizeExeStem(std::wstring(80, L'a')).size() == 40);
    // Whatever arrives, only the last component is a name.
    CHECK(SanitizeExeStem(std::wstring(L"Z:") + L"\\opt\\games\\Witcher3") == "Witcher3");

    const std::string stem = Stem("20261013-123456", "CrimsonDesert", 7);
    CHECK(stem == "20261013-123456-CrimsonDesert-007");
    CHECK(FileName(stem, Part::Original, IF::FileKind::Png8) == stem + ".original.png");
    CHECK(FileName(stem, Part::Processed, IF::FileKind::Pfm) == stem + ".processed.pfm");
    CHECK(FileName(stem, Part::Sidecar, IF::FileKind::Png16) == stem + ".json");

    std::string parsed;
    CHECK(ParseName(stem + ".original.png", &parsed) && parsed == stem);
    CHECK(ParseName(stem + ".processed.pfm.tmp", &parsed) && parsed == stem);
    CHECK(ParseName("20261013-123456-a.b-c_d-1234.json", &parsed) && parsed == "20261013-123456-a.b-c_d-1234");
    for (const char* foreign :
         { "notes.txt", "20261013-123456-Game-007.png", "20261013-123456-Game-07.json", "2026101-123456-Game-007.json",
           "20261013_123456-Game-007.json", "20261013-123456--007.json", "20261013-123456-Game-007.original.png.bak",
           "20261013-123456-.hidden-007.json", "20261013-123456-Ga me-007.json", "x20261013-123456-Game-007.json",
           "20261013-123456-Game-0a7.json" })
        CHECK(!ParseName(foreign));

    // The cap over a list: 70 stems, every part of each, a stray .tmp and foreign files.
    std::vector<std::string> names = { "readme.txt", "20261013-000000-Game.json", "screenshot.png" };
    std::vector<std::string> stems;
    for (int i = 0; i < 70; ++i)
    {
        char compact[32];
        std::snprintf(compact, sizeof(compact), "202610%02d-%06d", 1 + i / 30, i);
        stems.push_back(Stem(compact, "Game", 1));
        names.push_back(stems.back() + ".original.png");
        names.push_back(stems.back() + ".processed.png");
        names.push_back(stems.back() + ".json");
    }
    names.push_back(stems[3] + ".processed.png.tmp");
    std::shuffle(names.begin(), names.end(), std::mt19937(7));
    auto victims = RotationVictims(names, kMaxPairs);
    // 70 existing, room for the new one: the 7 oldest go, every file of each, the stray .tmp of one included.
    CHECK(victims.size() == 7 * 3 + 1);
    for (const auto& v : victims)
    {
        std::string s;
        CHECK(ParseName(v, &s));
        CHECK(std::find(stems.begin(), stems.begin() + 7, s) != stems.begin() + 7);
    }
    CHECK(RotationVictims({ "readme.txt", stems[0] + ".json" }, kMaxPairs).empty());
    CHECK(RotationVictims({ stems[0] + ".json", stems[1] + ".json" }, 1).size() == 2);

    // The same in a real directory, deleting what it says: foreign files survive.
    const fs::path dir = out / "rotation";
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const auto& n : names)
        std::ofstream(dir / n) << "x";
    std::vector<std::string> listed;
    for (const auto& e : fs::directory_iterator(dir))
        listed.push_back(e.path().filename().string());
    for (const auto& v : RotationVictims(listed, kMaxPairs))
        fs::remove(dir / v);
    std::set<std::string> left;
    for (const auto& e : fs::directory_iterator(dir))
    {
        std::string s;
        if (ParseName(e.path().filename().string(), &s))
            left.insert(s);
    }
    CHECK(left.size() == kMaxPairs - 1);
    CHECK(fs::exists(dir / "readme.txt") && fs::exists(dir / "screenshot.png") &&
          fs::exists(dir / "20261013-000000-Game.json"));

    // After the write: the pair just written is protected, counted as the one more, and never a victim, even when its
    // name sorts first because the clock went back.
    {
        const std::vector<std::string> four = { stems[0] + ".json", stems[1] + ".json", stems[2] + ".json",
                                                stems[3] + ".json", stems[3] + ".original.png" };
        auto after = RotationVictims(four, 2, stems[3]);
        CHECK((after == std::vector<std::string> { stems[0] + ".json", stems[1] + ".json" }));
        after = RotationVictims(four, 2, stems[0]); // written under an older name: kept, the next oldest go
        CHECK((after == std::vector<std::string> { stems[1] + ".json", stems[2] + ".json" }));
        CHECK(RotationVictims(four, kMaxPairs, stems[0]).empty());
        CHECK(RotationVictims({ stems[0] + ".json" }, 1, stems[0]).empty());
    }
    ++g_cases;
}

static std::FILE* OpenPortable(const fs::path& path) { return std::fopen(path.string().c_str(), "wb"); }

static void WriteThenRename(const fs::path& out)
{
    const fs::path dir = out / "rename";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path final = dir / "20261013-123456-Game-001.json";
    fs::path temporary = final;
    temporary += ".tmp";

    bool sawTemporaryOnly = false;
    CHECK(WriteViaTemporary(final, OpenPortable,
                            [&](std::FILE* f)
                            {
                                sawTemporaryOnly = fs::exists(temporary) && !fs::exists(final);
                                return std::fwrite("{}", 1, 2, f) == 2;
                            }));
    CHECK(sawTemporaryOnly && fs::exists(final) && !fs::exists(temporary) && fs::file_size(final) == 2);

    const fs::path failed = dir / "20261013-123456-Game-002.json";
    fs::path failedTemporary = failed;
    failedTemporary += ".tmp";
    CHECK(!WriteViaTemporary(failed, OpenPortable, [](std::FILE*) { return false; }));
    CHECK(!fs::exists(failed) && !fs::exists(failedTemporary));

    CHECK(!WriteViaTemporary(dir / "missing" / "x.json", OpenPortable, [](std::FILE*) { return true; }));
    ++g_cases;
}

// --- The folder: created, refused, read-only ----------------------------------------------------------------------

static size_t CountEntries(const fs::path& dir)
{
    return static_cast<size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

static void Folders(const fs::path& out)
{
    const fs::path base = out / "folders";
    std::error_code ec;
    for (const char* sub : { "readonly", "rotation-ro" })
        fs::permissions(base / sub, fs::perms::owner_all, fs::perm_options::replace, ec);
    fs::remove_all(base);
    fs::create_directories(base);

    // Created with its parents; there already is fine.
    std::string why;
    CHECK(EnsureDirectory(base / "a" / "b", &why) && fs::is_directory(base / "a" / "b") && why.empty());
    CHECK(EnsureDirectory(base / "a" / "b", &why));

    // A file where the folder should be: refused with the reason, the file untouched.
    std::ofstream(base / "file") << "x";
    CHECK(!EnsureDirectory(base / "file", &why) && !why.empty() && fs::is_regular_file(base / "file"));
    why.clear();
    CHECK(!EnsureDirectory(base / "file" / "sub", &why) && !why.empty());

    // The step that failed, in the system's words and never with the path.
    why.clear();
    CHECK(!WriteViaTemporary(
        base / "missing" / "20261015-123456-Game-001.json", OpenPortable, [](std::FILE*) { return true; }, &why));
    CHECK(why.rfind("open: ", 0) == 0 && why.find(base.string()) == std::string::npos);
    why.clear();
    CHECK(!WriteViaTemporary(
        base / "20261015-123456-Game-002.json", OpenPortable,
        [](std::FILE*)
        {
            errno = ENOSPC;
            return false;
        },
        &why));
    CHECK(why == "write: " + std::generic_category().message(ENOSPC));
    CHECK(!fs::exists(base / "20261015-123456-Game-002.json") &&
          !fs::exists(base / "20261015-123456-Game-002.json.tmp"));
    // A rename that cannot land (a folder holds the final name): reported, the temporary gone, the folder untouched.
    fs::create_directories(base / "20261015-123456-Game-003.json" / "inside");
    why.clear();
    CHECK(!WriteViaTemporary(
        base / "20261015-123456-Game-003.json", OpenPortable,
        [](std::FILE* f) { return std::fwrite("{}", 1, 2, f) == 2; }, &why));
    CHECK(why.rfind("rename: ", 0) == 0 && !fs::exists(base / "20261015-123456-Game-003.json.tmp") &&
          fs::is_directory(base / "20261015-123456-Game-003.json" / "inside"));

    // A writer that throws (an allocation inside the image writer) is a failed write like any other: the file is
    // closed, the temporary removed, and nothing escapes to the writer thread.
    why.clear();
    CHECK(!WriteViaTemporary(
        base / "20261015-123456-Game-004.json", OpenPortable,
        [](std::FILE* f) -> bool
        {
            std::fwrite("{", 1, 1, f);
            throw std::runtime_error("writer");
        },
        &why));
    CHECK(why == "write: the writer threw" && !fs::exists(base / "20261015-123456-Game-004.json") &&
          !fs::exists(base / "20261015-123456-Game-004.json.tmp"));

    // Rotation after a write, in a real folder: the oldest pairs go, the one just written stays even under an older
    // name, the count is of files actually removed, and nothing foreign is touched -- a name that is not ASCII
    // included, however much it looks like ours.
    const fs::path rot = base / "rotation";
    fs::create_directories(rot);
    const char* stamps[] = { "20261015-000001", "20261015-000002", "20261015-000003", "20261015-000004" };
    std::vector<std::string> made;
    for (const char* stamp : stamps)
    {
        const std::string stem = Stem(stamp, "Game", 1);
        made.push_back(stem);
        for (const Part part : { Part::Original, Part::Processed, Part::Sidecar })
            std::ofstream(rot / FileName(stem, part, IF::FileKind::Png8)) << "x";
    }
    std::ofstream(rot / "notes.txt") << "x";
    std::ofstream(rot / "20261015-000000-G\xc3\xa4me-001.json") << "x";
    CHECK(RotateFolder(rot, 2, made[0]) == 6); // made[1] and made[2], three files each
    CHECK(fs::exists(rot / (made[0] + ".json")) && fs::exists(rot / (made[3] + ".json")) &&
          !fs::exists(rot / (made[1] + ".json")) && !fs::exists(rot / (made[2] + ".original.png")));
    CHECK(fs::exists(rot / "notes.txt") && fs::exists(rot / "20261015-000000-G\xc3\xa4me-001.json"));
    CHECK(RotateFolder(rot, 2, made[0]) == 0);
    CHECK(RotateFolder(base / "no-such-folder", 2, made[0]) == 0);

    // A read-only game directory and a read-only capture folder. The permission bits do not bind a superuser, so these
    // run only where a probe file cannot be created, and say when they did not.
    const fs::path ro = base / "readonly";
    fs::create_directories(ro);
    const fs::path rotRo = base / "rotation-ro";
    fs::create_directories(rotRo);
    for (const std::string& stem : made)
        std::ofstream(rotRo / (stem + ".json")) << "x";
    fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    fs::permissions(rotRo, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
    std::FILE* probe = std::fopen((ro / "probe").string().c_str(), "wb");
    if (probe != nullptr)
    {
        std::fclose(probe);
        fs::remove(ro / "probe", ec);
        std::printf("nr-capture: SKIP read-only folders: permission bits are not enforced for this user\n");
    }
    else
    {
        why.clear();
        CHECK(!EnsureDirectory(ro / "retoque-captures", &why) && !why.empty() &&
              why.find(ro.string()) == std::string::npos);
        why.clear();
        CHECK(!WriteViaTemporary(
            ro / "20261015-123456-Game-001.json", OpenPortable, [](std::FILE*) { return true; }, &why));
        CHECK(why.rfind("open: ", 0) == 0);
        CHECK(CountEntries(ro) == 0); // nothing left behind, not even a temporary
        CHECK(RotateFolder(rotRo, 1, made[3]) == 0 && CountEntries(rotRo) == made.size()); // nothing removed or counted
    }
    fs::permissions(ro, fs::perms::owner_all, fs::perm_options::replace);
    fs::permissions(rotRo, fs::perms::owner_all, fs::perm_options::replace);
    ++g_cases;
}

// --- Provenance --------------------------------------------------------------------------------------------------

static void Provenance(const fs::path& out)
{
    Record r;
    r.Set("record", "retoque-comparison-capture");
    r.Set("quote", "a \"quoted\" value");
    r.Set("backslash", "C:\\games");
    r.Set("semicolon", "a;b;c");
    r.Set("utf8", "\xc3\xa9t\xc3\xa9");
    r.Set("control", std::string("tab\there\nnewline") + '\x01');
    r.Set("record", "set twice keeps one entry");
    CHECK(r.Items().size() == 6);
    CHECK(*r.Find("record") == "set twice keeps one entry");
    CHECK(*r.Find("semicolon") == "a,b,c");
    CHECK(*r.Find("utf8") == "??t??");
    CHECK(IF::PngTextValid(r.Line()));

    // What the production builder feeds from outside: the executable's name and the adapter's. Inputs that carry a
    // home directory, a user profile and a tailnet host must come out without them.
    const std::wstring home = std::wstring(L"/ho") + L"me/someone/.local/share/Steam/Game";
    const std::wstring profile = std::wstring(L"C:\\Us") + L"ers\\someone\\Desktop\\Game";
    const std::wstring wineHome = std::wstring(L"Z:\\ho") + L"me\\someone\\Games\\Game";
    const std::wstring tailnet = std::wstring(L"\\\\desk.tail1234.") + L"ts.net\\share\\Game";
    for (const auto& input : { home, profile, wineHome, tailnet })
        CHECK(SanitizeExeStem(input) == "Game");
    r.Set("exe", SanitizeExeStem(profile));
    r.Set("gpu", "NVIDIA GeForce RTX 4090");
    r.Set("model", "file-bytes=123;mtime=456;version=310.8.2.0");

    std::ofstream(out / "provenance.txt", std::ios::binary) << r.Line();
    std::ofstream(out / "provenance.json", std::ios::binary) << r.Json();

    // The same record in a PNG's tEXt chunk, through the production writer.
    std::vector<uint8_t> surface(256 * 2, 0);
    IF::Rows rows { surface.data(), 0, 256, 2, 2, surface.size() };
    const IF::Plan plan = IF::PlanFor(IF::Dxgi::R8G8B8A8_UNORM, false);
    std::FILE* f = std::fopen((out / "provenance.png").string().c_str(), "wb");
    CHECK(f != nullptr);
    auto sink = [f](const uint8_t* d, size_t n) { return std::fwrite(d, 1, n, f) == n; };
    CHECK(IF::WriteImage(sink, rows, plan, kTextKeyword, r.Line()));
    std::fclose(f);

    CHECK(IF::PngKeywordValid(kTextKeyword));
    CHECK(!IF::PngKeywordValid("") && !IF::PngKeywordValid(" lead") && !IF::PngKeywordValid("trail ") &&
          !IF::PngKeywordValid("two  spaces") && !IF::PngKeywordValid(std::string(80, 'k')));
    CHECK(!IF::PngTextValid("bad\nline") && !IF::PngTextValid("\xc3\xa9"));
    ++g_cases;
}

// The record the capture writes, exactly: every key, in order, with the text each value takes. The brief's fields --
// build, route, the preset and working scale (inside settings, the timing contract's text), the resolution, the time
// and the executable -- are here by name, and a key lost from either builder fails this.
static void RecordExact(const fs::path& out)
{
    FrameRecordInputs frame;
    frame.build = "0.9.0 38e0d621 2026-10-15 12:00:00";
    frame.timeUtc = FormatUtc(1792067696).iso; // 2026-10-15 12:34:56 UTC
    frame.exe = SanitizeExeStem(L"CrimsonDesert");
    frame.route = "d3d12-present";
    frame.surface = "present-final";
    frame.hookMethod = 2;
    frame.width = 3440;
    frame.height = 1440;
    frame.originalFormat = IF::Dxgi::R16G16B16A16_FLOAT;
    frame.processedFormat = IF::Dxgi::R16G16B16A16_FLOAT;
    frame.plan = IF::PlanFor(IF::Dxgi::R16G16B16A16_FLOAT, false);
    frame.held = true;
    frame.whitePoint = 1.25f;
    frame.frame = 1234;
    frame.present = 5678;
    frame.settings = "stage=present render=2293x960 compose=3440x1440 model=1720x720 scale=0.5 passes=1 "
                     "pass1=[preset=0,style=0]";

    WriteRecordInputs write;
    write.completion = Completion::Submission;
    write.modelVersion = "310.8.2.0";
    write.modelBytes = "123456789";
    write.ngxLoader = "32.0.16.1691";
    write.ngxRoute = "loader-routes-feature-18";
    write.gpu = "NVIDIA GeForce RTX 4090";
    write.gpuDriver = "32.0.15.7700";
    write.means[0].Add(0.25f);
    write.means[0].Add(0.75f);
    write.means[1].Add(0.5f);
    write.means[1].Add(1.0f);
    write.seq = 7;
    const std::string stem = Stem(FormatUtc(1792067696).compact, frame.exe, write.seq);
    write.files[0] = FileName(stem, Part::Original, frame.plan.kind);
    write.files[1] = FileName(stem, Part::Processed, frame.plan.kind);
    write.files[2] = FileName(stem, Part::Sidecar, frame.plan.kind);

    Record r;
    FillFrameRecord(r, frame);
    FillWriteRecord(r, write);
    const std::string expected =
        "record=retoque-comparison-capture;record_version=1;build=0.9.0 38e0d621 2026-10-15 12:00:00;"
        "time_utc=2026-10-15T12:34:56Z;exe=CrimsonDesert;route=d3d12-present;surface=present-final;hook_method=2;"
        "original=the frame the pass read (hdrCopy);processed=the frame the pass wrote;width=3440;height=1440;"
        "original_format=R16G16B16A16_FLOAT;processed_format=R16G16B16A16_FLOAT;file_kind=png8;quantised_from=fp16;"
        "linear_hdr=0;held=1;carried=0;white_point=1.25;frame=1234;present=5678;"
        "settings=stage=present render=2293x960 compose=3440x1440 model=1720x720 scale=0.5 passes=1 "
        "pass1=[preset=0,style=0];"
        "completion=submission-fence;model_version=310.8.2.0;model_bytes=123456789;ngx_loader=32.0.16.1691;"
        "ngx_route=loader-routes-feature-18;gpu=NVIDIA GeForce RTX 4090;gpu_driver=32.0.15.7700;mean_original=0.5;"
        "mean_processed=0.75;seq=7;original_file=20261015-123456-CrimsonDesert-007.original.png;"
        "processed_file=20261015-123456-CrimsonDesert-007.processed.png;sidecar=20261015-123456-CrimsonDesert-007.json";
    if (r.Line() != expected)
        std::fprintf(stderr, "record line:\n%s\n", r.Line().c_str());
    CHECK(r.Line() == expected);
    CHECK(IF::PngTextValid(r.Line()));
    std::ofstream(out / "record_exact.txt", std::ios::binary) << r.Line();
    std::ofstream(out / "record_exact.json", std::ios::binary) << r.Json();

    // A linear HDR frame: a PFM, no quantisation key, and the count of values that are not finite.
    FrameRecordInputs hdr = frame;
    hdr.route = "after-upscale";
    hdr.surface = "upscaler-output";
    hdr.linearHdr = true;
    hdr.plan = IF::PlanFor(IF::Dxgi::R16G16B16A16_FLOAT, true);
    WriteRecordInputs hdrWrite = write;
    hdrWrite.completion = Completion::AssumedFrames;
    hdrWrite.pfm = true;
    hdrWrite.means[1].Add(std::numeric_limits<float>::infinity());
    Record h;
    FillFrameRecord(h, hdr);
    FillWriteRecord(h, hdrWrite);
    CHECK(h.Find("quantised_from") == nullptr && *h.Find("file_kind") == "pfm" && *h.Find("linear_hdr") == "1");
    CHECK(*h.Find("completion") == "assumed-8-frames" && *h.Find("nonfinite_original") == "0" &&
          *h.Find("nonfinite_processed") == "1" && *h.Find("mean_processed") == "0.75");
    std::vector<std::string> keys;
    for (const auto& item : h.Items())
        keys.push_back(item.first);
    const std::vector<std::string> expectedKeys = { "record",
                                                    "record_version",
                                                    "build",
                                                    "time_utc",
                                                    "exe",
                                                    "route",
                                                    "surface",
                                                    "hook_method",
                                                    "original",
                                                    "processed",
                                                    "width",
                                                    "height",
                                                    "original_format",
                                                    "processed_format",
                                                    "file_kind",
                                                    "linear_hdr",
                                                    "held",
                                                    "carried",
                                                    "white_point",
                                                    "frame",
                                                    "present",
                                                    "settings",
                                                    "completion",
                                                    "model_version",
                                                    "model_bytes",
                                                    "ngx_loader",
                                                    "ngx_route",
                                                    "gpu",
                                                    "gpu_driver",
                                                    "mean_original",
                                                    "mean_processed",
                                                    "nonfinite_original",
                                                    "nonfinite_processed",
                                                    "seq",
                                                    "original_file",
                                                    "processed_file",
                                                    "sidecar" };
    CHECK(keys == expectedKeys);

    // Numbers are written as std::format("{:.6g}") wrote them before the builders existed, digit for digit, NaN and
    // infinities included.
    {
        std::mt19937_64 rng(2026);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::uniform_int_distribution<int> exponent(-40, 40);
        std::vector<double> values = { 0.0,
                                       -0.0,
                                       1.0,
                                       1.25,
                                       0.45,
                                       1e-5,
                                       123456.0,
                                       1234567.0,
                                       1e300,
                                       5e-324,
                                       std::numeric_limits<double>::infinity(),
                                       -std::numeric_limits<double>::infinity(),
                                       std::numeric_limits<double>::quiet_NaN(),
                                       static_cast<double>(1.25f),
                                       static_cast<double>(0.1f) };
        for (int i = 0; i < 200000; ++i)
            values.push_back(std::ldexp(unit(rng), exponent(rng)) * (i % 2 == 0 ? 1.0 : -1.0));
        for (const double v : values)
        {
            if (FormatNumber(v) != std::format("{:.6g}", v))
                std::fprintf(stderr, "FormatNumber(%a) = %s\n", v, FormatNumber(v).c_str());
            CHECK(FormatNumber(v) == std::format("{:.6g}", v));
        }
    }

    // The CRT is the game's: a locale with a decimal comma, which a game may set, must not reach the record.
    {
        const char* comma = nullptr;
        for (const char* name : { "pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8" })
        {
            if (std::setlocale(LC_ALL, name) != nullptr)
            {
                comma = name;
                break;
            }
        }
        if (comma == nullptr)
        {
            std::printf("nr-capture: SKIP decimal-comma locale: none is installed\n");
        }
        else
        {
            char probe[16];
            std::snprintf(probe, sizeof(probe), "%.2f", 1.25);
            CHECK(std::string(probe) == "1,25"); // the locale is in force, or the check below proves nothing
            Record again;
            FillFrameRecord(again, frame);
            FillWriteRecord(again, write);
            CHECK(again.Line() == expected && FormatNumber(0.45) == "0.45");
            std::setlocale(LC_ALL, "C");
        }
    }
    ++g_cases;
}

// --- Conversions and image files ---------------------------------------------------------------------------------

static void Conversions(const fs::path& out)
{
    // Every half, every small float, as float32 bits, for run.py to hold against Python's own decoding.
    {
        std::ofstream half(out / "half.bin", std::ios::binary);
        for (uint32_t h = 0; h < 65536; ++h)
        {
            const float v = IF::HalfToFloat(static_cast<uint16_t>(h));
            half.write(reinterpret_cast<const char*>(&v), 4);
        }
        std::ofstream small11(out / "small11.bin", std::ios::binary);
        for (uint32_t v = 0; v < 2048; ++v)
        {
            const float x = IF::SmallFloatToFloat(v, 6);
            small11.write(reinterpret_cast<const char*>(&x), 4);
        }
        std::ofstream small10(out / "small10.bin", std::ios::binary);
        for (uint32_t v = 0; v < 1024; ++v)
        {
            const float x = IF::SmallFloatToFloat(v, 5);
            small10.write(reinterpret_cast<const char*>(&x), 4);
        }
    }

    // Ten bits to sixteen: lossless (the top ten bits give the code back), ends exact, strictly increasing.
    for (uint32_t k = 0; k < 1024; ++k)
    {
        CHECK((IF::Widen10(k) >> 6) == k);
        if (k > 0)
            CHECK(IF::Widen10(k) > IF::Widen10(k - 1));
    }
    CHECK(IF::Widen10(0) == 0 && IF::Widen10(1023) == 65535);

    // Quantising: NaN, negatives and negative zero to 0; one and above, infinity included, to 255; round half up.
    CHECK(IF::Quantise8(std::nanf("")) == 0 && IF::Quantise8(-std::nanf("")) == 0);
    CHECK(IF::Quantise8(-0.0f) == 0 && IF::Quantise8(-1.0f) == 0 && IF::Quantise8(-INFINITY) == 0);
    CHECK(IF::Quantise8(1.0f) == 255 && IF::Quantise8(7.5f) == 255 && IF::Quantise8(INFINITY) == 255);
    CHECK(IF::Quantise8(0.5f) == 128 && IF::Quantise8(0.0f) == 0 && IF::Quantise8(1.0f / 255.0f) == 1);
    CHECK(IF::Quantise8(1e-30f) == 0);
    for (int i = 1; i < 1000; ++i)
        CHECK(IF::Quantise8(i / 1000.0f) >= IF::Quantise8((i - 1) / 1000.0f));

    // The plan table (comparison-capture.md, section 4.3).
    using namespace IF::Dxgi;
    for (uint32_t f : { R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB, R8G8B8A8_TYPELESS, B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB,
                        B8G8R8A8_TYPELESS })
    {
        const auto p = IF::PlanFor(f, false);
        CHECK(p.kind == IF::FileKind::Png8 && p.srgb && p.refusal == nullptr && p.quantisedFrom == nullptr);
    }
    CHECK(IF::PlanFor(R10G10B10A2_UNORM, false).kind == IF::FileKind::Png16 &&
          !IF::PlanFor(R10G10B10A2_UNORM, false).srgb);
    for (uint32_t f : { R16G16B16A16_FLOAT, R16G16B16A16_TYPELESS, R32G32B32A32_FLOAT, R11G11B10_FLOAT })
    {
        CHECK(IF::PlanFor(f, true).kind == IF::FileKind::Pfm && IF::PlanFor(f, true).refusal == nullptr);
        const auto display = IF::PlanFor(f, false);
        CHECK(display.kind == IF::FileKind::Png8 && display.srgb && display.quantisedFrom != nullptr);
    }
    for (uint32_t f : { 0u, 6u /* R32G32B32_FLOAT */, 11u /* R16G16B16A16_UNORM */, 85u /* B5G6R5 */ })
    {
        const auto p = IF::PlanFor(f, false);
        CHECK(p.kind == IF::FileKind::None && p.refusal == IF::kRefusalFormat);
    }
    CHECK(IF::FormatName(R16G16B16A16_FLOAT) == "R16G16B16A16_FLOAT" && IF::FormatName(999) == "dxgi-999");

    // Rows that do not fit their buffer are refused, not read past.
    std::vector<uint8_t> small(256 * 3 - 1);
    IF::Rows tooShort { small.data(), 0, 256, 64, 3, small.size() };
    CHECK(!tooShort.Fits(IF::Source::Rgba8));
    IF::Rows exact { small.data(), 0, 256, 64, 2, small.size() };
    CHECK(exact.Fits(IF::Source::Rgba8));
    IF::Rows pitchTooSmall { small.data(), 0, 128, 64, 2, small.size() };
    CHECK(!pitchTooSmall.Fits(IF::Source::Rgba8));

    // The mean counts what is not finite instead of summing it.
    IF::Mean mean;
    mean.Add(1.0f);
    mean.Add(3.0f);
    mean.Add(INFINITY);
    mean.Add(std::nanf(""));
    CHECK(mean.Value() == 2.0 && mean.nonFinite == 2 && mean.count == 2);
    ++g_cases;
}

struct Case
{
    std::string name;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    bool linearHdr;
};

static uint16_t FloatToHalfBits(float f)
{
    // Only for building fixtures from ordinary values; the specials are written as bits directly.
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0)
        return static_cast<uint16_t>(sign);
    if (exponent >= 31)
        return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

// Writes the source surface the way a readback holds it -- rows on a 256-byte pitch from a 512-byte-aligned offset,
// the padding filled with bytes no file may contain -- then the file the production writer makes from it.
static void ImageCase(const fs::path& out, std::ofstream& manifest, const Case& c, std::mt19937& rng)
{
    const IF::Source source = IF::SourceFor(c.format);
    const uint32_t bpp = IF::BytesPerPixel(source);
    const uint32_t rowBytes = c.width * bpp;
    const uint32_t pitch = (rowBytes + 255u) & ~255u;
    const uint32_t offset = 512; // a placed footprint's offset is 512-aligned; nonzero here to prove it is honoured
    std::vector<uint8_t> buffer(offset + static_cast<size_t>(pitch) * c.height, 0xEE);

    std::uniform_int_distribution<uint32_t> byte(0, 255);
    for (uint32_t y = 0; y < c.height; ++y)
    {
        uint8_t* row = buffer.data() + offset + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < c.width; ++x)
        {
            uint8_t* px = row + x * bpp;
            switch (source)
            {
            case IF::Source::Rgba8:
            case IF::Source::Bgra8:
                for (int i = 0; i < 4; ++i)
                    px[i] = static_cast<uint8_t>(byte(rng));
                break;
            case IF::Source::Rgb10a2:
            {
                // Every code of every channel across the row.
                const uint32_t r = x & 1023u, g = (1023u - x) & 1023u, b = (x * 7u + y) & 1023u, a = x & 3u;
                const uint32_t v = r | (g << 10) | (b << 20) | (a << 30);
                std::memcpy(px, &v, 4);
                break;
            }
            case IF::Source::Rg11b10f:
            {
                const uint32_t r = x & 2047u, g = (2047u - x) & 2047u, b = (x + y * 13u) & 1023u;
                const uint32_t v = r | (g << 11) | (b << 22);
                std::memcpy(px, &v, 4);
                break;
            }
            case IF::Source::Rgba16f:
            {
                static const uint16_t specials[] = { 0x0000, 0x8000, 0x0001, 0x03FF, 0x0400, 0x3C00, 0xBC00,
                                                     0x7BFF, 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0xFE01, 0x3800,
                                                     0x3BFF, 0x3C01, 0x4000, 0x1400, 0x2E66 };
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t pick = byte(rng);
                    const uint16_t h = pick < 64 ? specials[pick % (sizeof(specials) / 2)]
                                                 : FloatToHalfBits(static_cast<float>(byte(rng)) / 200.0f - 0.1f);
                    std::memcpy(px + 2 * i, &h, 2);
                }
                break;
            }
            case IF::Source::Rgba32f:
            {
                static const uint32_t specials[] = { 0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u,
                                                     0x7FA00001u, 0x00000001u, 0x3F800000u, 0x3F7FFFFFu, 0x41200000u };
                for (int i = 0; i < 4; ++i)
                {
                    const uint32_t pick = byte(rng);
                    uint32_t bits;
                    if (pick < 64)
                        bits = specials[pick % (sizeof(specials) / 4)];
                    else
                    {
                        const float v = static_cast<float>(byte(rng)) / 230.0f - 0.05f;
                        std::memcpy(&bits, &v, 4);
                    }
                    std::memcpy(px + 4 * i, &bits, 4);
                }
                break;
            }
            default:
                CHECK(false);
            }
        }
    }

    const IF::Plan plan = IF::PlanFor(c.format, c.linearHdr);
    CHECK(plan.refusal == nullptr);
    const IF::Rows rows { buffer.data(), offset, pitch, c.width, c.height, buffer.size() };
    CHECK(rows.Fits(plan.source));

    const std::string fileName = c.name + "." + IF::Extension(plan.kind);
    std::FILE* f = std::fopen((out / fileName).string().c_str(), "wb");
    CHECK(f != nullptr);
    size_t written = 0;
    auto sink = [f, &written](const uint8_t* d, size_t n)
    {
        written += n;
        return std::fwrite(d, 1, n, f) == n;
    };
    const std::string text = "record=fixture;case=" + c.name;
    CHECK(IF::WriteImage(sink, rows, plan, kTextKeyword, text));
    CHECK(std::fclose(f) == 0);

    std::ofstream(out / (c.name + ".src"), std::ios::binary)
        .write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    const IF::Mean mean = IF::MeanOf(rows, plan);
    char line[512];
    std::snprintf(line, sizeof(line), "%s %s %u %u %u %u %u %s %d %.17g %llu\n", c.name.c_str(), fileName.c_str(),
                  c.format, c.width, c.height, pitch, offset, IF::FileKindName(plan.kind), plan.srgb ? 1 : 0,
                  mean.Value(), static_cast<unsigned long long>(mean.nonFinite));
    manifest << line;
}

static void Images(const fs::path& out)
{
    using namespace IF::Dxgi;
    std::mt19937 rng(20261013);
    std::ofstream manifest(out / "images.txt");
    const Case cases[] = {
        { "rgba8_1x1", R8G8B8A8_UNORM, 1, 1, false },
        { "rgba8_63x7", R8G8B8A8_UNORM, 63, 7, false },
        { "rgba8_64x3", R8G8B8A8_UNORM_SRGB, 64, 3, false },
        { "rgba8_65x4", R8G8B8A8_TYPELESS, 65, 4, false },
        { "bgra8_17x9", B8G8R8A8_UNORM, 17, 9, false },
        // Filtered rows of 1 + 28 * 3 = 85 bytes: 771 rows are exactly one full stored block, 1542 exactly two, 772
        // one row into a second.
        { "rgba8_block_exact", R8G8B8A8_UNORM, 28, 771, false },
        { "rgba8_block_two", B8G8R8A8_UNORM, 28, 1542, false },
        { "rgba8_block_over", R8G8B8A8_UNORM, 28, 772, false },
        { "rgba8_300x200", R8G8B8A8_UNORM, 300, 200, false },
        { "rgb10a2_1024x3", R10G10B10A2_UNORM, 1024, 3, false },
        { "rgb10a2_odd", R10G10B10A2_TYPELESS, 33, 5, false },
        { "fp16_display", R16G16B16A16_FLOAT, 37, 6, false },
        { "fp32_display", R32G32B32A32_FLOAT, 11, 3, false },
        { "r11g11b10_display", R11G11B10_FLOAT, 2048, 2, false },
        { "fp16_linear", R16G16B16A16_FLOAT, 37, 6, true },
        { "fp16_linear_typeless", R16G16B16A16_TYPELESS, 5, 1, true },
        { "fp32_linear", R32G32B32A32_FLOAT, 11, 3, true },
        { "r11g11b10_linear", R11G11B10_FLOAT, 2048, 2, true },
    };
    for (const auto& c : cases)
        ImageCase(out, manifest, c, rng);
    ++g_cases;
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const fs::path out = argv[1];
    fs::create_directories(out);

    StateMachine();
    Button();
    Refusals();
    Names(out);
    WriteThenRename(out);
    Folders(out);
    Provenance(out);
    RecordExact(out);
    Conversions(out);
    Images(out);

    std::printf("nr-capture: %d C++ case groups passed\n", g_cases);
    return 0;
}
