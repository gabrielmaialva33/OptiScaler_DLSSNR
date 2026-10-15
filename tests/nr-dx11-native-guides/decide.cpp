// The native D3D11 guide transport's decisions (retoque/dlssnr/DlssNr_Dx11Guides.h) on the production header: who is
// carried and who is not, in what order the reasons are given, the slot rotation against a simulated GPU, and the
// window and its line.

#include <dlssnr/DlssNr_Dx11Guides.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace DlssNr::Dx11Guides;

namespace
{
int g_checks = 0;
#define CHECK(x)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(x))                                                                                                      \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                                 \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

Texture Tex(uint32_t w, uint32_t h, uint32_t format)
{
    Texture t;
    t.present = true;
    t.texture2D = true;
    t.width = w;
    t.height = h;
    t.mipLevels = 1;
    t.arraySize = 1;
    t.sampleCount = 1;
    t.format = format;
    return t;
}

// Shaped like Sons Of The Forest's evaluate: native D3D11 DLSS Quality, 2294x960 render to 3440x1440, HookMethod 2, the
// bridge's present host built and presenting at the output's size. Its logs do not record the guides' formats or
// binds; these are plausible ones (a typeless 32-bit depth that is not a depth-stencil, half-float motion).
Inputs Good()
{
    Inputs in;
    in.nrEnabled = true;
    in.hookMethod = 2;
    in.nativeDlss = true;
    in.presentHost = true;
    in.evaluated = true;
    in.immediateContext = true;
    in.depth = Tex(2294, 960, kR32Typeless);
    in.motion = Tex(2294, 960, 34); // R16G16_FLOAT
    in.output = Tex(3440, 1440, 10);
    in.presentedWidth = 3440;
    in.presentedHeight = 1440;
    return in;
}

void Gates()
{
    CHECK(Decide(Good()) == Reason::Carried);

    // Not wanted: each gate alone, and nothing else is looked at (a broken evaluate on another route is not reported).
    {
        Inputs in = Good();
        in.nrEnabled = false;
        CHECK(Decide(in) == Reason::NotWanted);
        in.evaluated = false;
        in.depth = {};
        CHECK(Decide(in) == Reason::NotWanted);
    }
    for (uint32_t method : { 0u, 1u, 3u, 0xFFFFFFFFu })
    {
        Inputs in = Good();
        in.hookMethod = method;
        CHECK(Decide(in) == Reason::NotWanted);
    }
    {
        Inputs in = Good();
        in.nativeDlss = false;
        CHECK(Decide(in) == Reason::NotWanted);
        in = Good();
        in.presentHost = false;
        CHECK(Decide(in) == Reason::NotWanted);
    }

    // Each refusal alone, with the reason it gives.
    {
        Inputs in = Good();
        in.evaluated = false;
        CHECK(Decide(in) == Reason::NotEvaluated);

        in = Good();
        in.immediateContext = false;
        CHECK(Decide(in) == Reason::Deferred);

        in = Good();
        in.depth = {};
        CHECK(Decide(in) == Reason::NoDepth);
        in.depth.present = true; // a resource that is not a 2D texture
        CHECK(Decide(in) == Reason::NoDepth);

        in = Good();
        in.motion = {};
        CHECK(Decide(in) == Reason::NoMotion);
        in.motion.present = true;
        CHECK(Decide(in) == Reason::NoMotion);

        for (int variant = 0; variant < 6; ++variant)
        {
            in = Good();
            Texture& t = variant % 2 == 0 ? in.depth : in.motion;
            if (variant / 2 == 0)
                t.sampleCount = 4;
            else if (variant / 2 == 1)
                t.arraySize = 2;
            else
                t.mipLevels = 0;
            CHECK(Decide(in) == Reason::Shape);
        }

        // Mips are allowed: the copy takes the whole texture and the ring keeps mip 0.
        in = Good();
        in.depth.mipLevels = 5;
        CHECK(Decide(in) == Reason::Carried);

        in = Good();
        in.output = {};
        CHECK(Decide(in) == Reason::NoOutput);
        in.output.present = true;
        CHECK(Decide(in) == Reason::NoOutput);
        in.output = Tex(0, 1440, 10);
        CHECK(Decide(in) == Reason::NoOutput);

        // Another view of the game: Palworld's evaluates at 1691x1356 and 3396x1356 beside the main 3440x1440 one.
        in = Good();
        in.output = Tex(1691, 1356, 10);
        CHECK(Decide(in) == Reason::NotPresented);
        in.output = Tex(3440, 1356, 10);
        CHECK(Decide(in) == Reason::NotPresented);

        // Before the bridge's first present, the size is not known: the identity's rule 2 decides at the present.
        in.presentedWidth = 0;
        in.presentedHeight = 0;
        CHECK(Decide(in) == Reason::Carried);
        in.presentedWidth = 3440; // half a size is no size
        CHECK(Decide(in) == Reason::Carried);
    }

    // First failing, in order: mending one at a time walks the list to Carried.
    {
        Inputs in = Good();
        in.evaluated = false;
        in.immediateContext = false;
        in.depth = {};
        in.motion = {};
        in.output = {};
        in.presentedWidth = 1;
        in.presentedHeight = 1;
        CHECK(Decide(in) == Reason::NotEvaluated);
        in.evaluated = true;
        CHECK(Decide(in) == Reason::Deferred);
        in.immediateContext = true;
        CHECK(Decide(in) == Reason::NoDepth);
        in.depth = Tex(100, 100, 40);
        in.depth.sampleCount = 2;
        CHECK(Decide(in) == Reason::NoMotion);
        in.motion = Tex(100, 100, 34);
        CHECK(Decide(in) == Reason::Shape);
        in.depth.sampleCount = 1;
        CHECK(Decide(in) == Reason::NoOutput);
        in.output = Tex(200, 200, 10);
        CHECK(Decide(in) == Reason::NotPresented);
        in.presentedWidth = 200;
        in.presentedHeight = 200;
        CHECK(Decide(in) == Reason::Carried);
    }

    // Exhaustive over the gates: NotWanted exactly when one of them is off.
    for (int mask = 0; mask < 16; ++mask)
    {
        Inputs in = Good();
        in.nrEnabled = (mask & 1) != 0;
        in.hookMethod = (mask & 2) != 0 ? 2 : 1;
        in.nativeDlss = (mask & 4) != 0;
        in.presentHost = (mask & 8) != 0;
        CHECK((Decide(in) == Reason::NotWanted) == (mask != 15));
    }

    for (size_t r = 0; r < kReasons; ++r)
        CHECK(std::string(ReasonName(static_cast<Reason>(r))) != "?");
}

// The rotation as CarryLocked runs it, against a GPU that finishes lists late and out of step: a set is written by
// D3D11 only when SlotFree says the list that last read it has completed, and the sequence moves only on a carry.
// Held: no set is ever written while a list that reads it is pending, and a set in flight is refused, not waited for.
void Rotation()
{
    std::mt19937_64 rng(11);
    std::array<uint64_t, kSlots> lastRead {};
    uint64_t fenceValue = 0;
    uint64_t completed = 0;
    uint64_t sequence = 0;
    std::vector<uint64_t> pending; // signalled values not yet completed, in order
    int carried = 0, refused = 0;

    for (int frame = 0; frame < 100000; ++frame)
    {
        // The GPU completes some of what was submitted, in order.
        while (!pending.empty() && rng() % 3 != 0)
        {
            completed = pending.front();
            pending.erase(pending.begin());
        }

        const size_t slot = static_cast<size_t>(sequence % kSlots);
        if (!SlotFree(lastRead[slot], completed))
        {
            ++refused;
            // A refused set must really be in flight: its reader is still pending.
            CHECK(lastRead[slot] > completed);
            continue;
        }

        // Every list that read this set has completed.
        CHECK(lastRead[slot] <= completed);

        lastRead[slot] = ++fenceValue;
        pending.push_back(fenceValue);
        ++sequence;
        ++carried;
    }

    CHECK(carried > 10000 && refused > 1000);
    CHECK(SlotFree(0, 0) && SlotFree(5, 5) && SlotFree(5, 9) && !SlotFree(6, 5));
}

// Which copy carries the depth, and the one refusal it adds.
void DepthRoutes()
{
    const auto depth = [](uint32_t format, bool depthStencil)
    {
        Texture t = Tex(1280, 720, format);
        t.depthStencil = depthStencil;
        return t;
    };

    // Not a depth-stencil: upstream's copy path, over either handle kind.
    for (bool nt : { false, true })
    {
        CHECK(DepthCopy(depth(kR32Typeless, false), nt) == DepthRoute::Upstream);
        CHECK(DepthCopy(depth(41 /* R32_FLOAT */, false), nt) == DepthRoute::Upstream);
    }

    // A depth-stencil without a stencil plane: the transport's own copy, over either handle kind.
    for (bool nt : { false, true })
    {
        for (uint32_t format : { kR32Typeless, kD32Float, kR16Typeless, kD16Unorm })
            CHECK(DepthCopy(depth(format, true), nt) == DepthRoute::OwnCopy);
    }

    // With a stencil plane: upstream's DepthTransfer over legacy handles for the two typeless formats; otherwise no
    // copy here is safe.
    CHECK(DepthCopy(depth(kR24G8Typeless, true), false) == DepthRoute::Upstream);
    CHECK(DepthCopy(depth(kR32G8X24Typeless, true), false) == DepthRoute::Upstream);
    CHECK(DepthCopy(depth(kR24G8Typeless, true), true) == DepthRoute::Refuse);
    CHECK(DepthCopy(depth(kR32G8X24Typeless, true), true) == DepthRoute::Refuse);
    CHECK(DepthCopy(depth(kD24UnormS8Uint, true), false) == DepthRoute::Refuse);
    CHECK(DepthCopy(depth(kD32FloatS8X24Uint, true), false) == DepthRoute::Refuse);

    // The own copy is typeless where the source is typed: a typed depth-stencil format cannot be a shader resource.
    CHECK(OwnCopyFormat(kD32Float) == kR32Typeless && OwnCopyFormat(kD16Unorm) == kR16Typeless);
    CHECK(OwnCopyFormat(kR32Typeless) == kR32Typeless && OwnCopyFormat(kR16Typeless) == kR16Typeless);

    // In Decide, after the shape and before the output.
    Inputs in = Good();
    in.depth = depth(kR24G8Typeless, true);
    in.ntHandles = true;
    CHECK(Decide(in) == Reason::DepthStencil);
    in.ntHandles = false;
    CHECK(Decide(in) == Reason::Carried);
    in.ntHandles = true;
    in.depth.sampleCount = 4;
    CHECK(Decide(in) == Reason::Shape);
    in.depth.sampleCount = 1;
    in.output = {};
    CHECK(Decide(in) == Reason::DepthStencil);

    // A depth-stencil without a stencil plane is carried (by the own copy), over either handle kind.
    for (bool nt : { false, true })
    {
        in = Good();
        in.depth = depth(kR32Typeless, true);
        in.ntHandles = nt;
        CHECK(Decide(in) == Reason::Carried);
    }
}

// Upstream's prepare releases a stale cache whole: only when nothing the transport submitted is still in flight.
void CacheAndHost()
{
    CHECK(CacheFree(7, 7, 0, 5));  // current: whatever is in flight, the prepare touches only the free set
    CHECK(CacheFree(0, 1, 0, 0));  // the first prepare: nothing submitted yet
    CHECK(CacheFree(6, 7, 5, 5));  // stale, everything complete
    CHECK(CacheFree(6, 7, 9, 5));  // a completed value past the last signal (a fence shared by nothing else) is fine
    CHECK(!CacheFree(6, 7, 4, 5)); // stale, a list still reading: refused, the sets live on
    CHECK(!CacheFree(6, 7, 0, 1));

    // No host: NR on, HookMethod 2, native DLSS and nothing hosting the pass. Every other gate off is just NotWanted.
    Inputs in = Good();
    in.presentHost = false;
    CHECK(NoHost(in) && Decide(in) == Reason::NotWanted);
    for (int mask = 0; mask < 16; ++mask)
    {
        in = Good();
        in.nrEnabled = (mask & 1) != 0;
        in.hookMethod = (mask & 2) != 0 ? 2 : 1;
        in.nativeDlss = (mask & 4) != 0;
        in.presentHost = (mask & 8) != 0;
        CHECK(NoHost(in) == (mask == 7));
    }
    CHECK(std::string(kNoHostText).find("DLSS-NR native D3D11 guides: not carried:") == 0);

    // The depth conversion runs a shader on the game's context only for the two typeless formats with a stencil
    // plane, over legacy handles, bound as a depth-stencil or not.
    const auto depth = [](uint32_t format, bool depthStencil)
    {
        Texture t = Tex(1280, 720, format);
        t.depthStencil = depthStencil;
        return t;
    };
    for (bool ds : { false, true })
    {
        CHECK(RunsShader(depth(kR24G8Typeless, ds), false) && RunsShader(depth(kR32G8X24Typeless, ds), false));
        CHECK(!RunsShader(depth(kR24G8Typeless, ds), true) && !RunsShader(depth(kR32G8X24Typeless, ds), true));
    }
    for (uint32_t format : { kR32Typeless, kD32Float, kR16Typeless, kD16Unorm, 41u })
    {
        for (bool ds : { false, true })
        {
            for (bool nt : { false, true })
                CHECK(!RunsShader(depth(format, ds), nt));
        }
    }
    CHECK(!RunsShader(depth(kD24UnormS8Uint, true), false) && !RunsShader(depth(kD32FloatS8X24Uint, true), false));
}

void WindowAndLine()
{
    // The line, from counts set by hand.
    {
        Counts c;
        c.evaluates = 120;
        c.reasons[static_cast<size_t>(Reason::Carried)] = 117;
        c.reasons[static_cast<size_t>(Reason::SlotInFlight)] = 2;
        c.reasons[static_cast<size_t>(Reason::NotPresented)] = 1;
        c.haveCarried = true;
        c.depth = Tex(2294, 960, kR32Typeless);
        c.motion = Tex(2294, 960, 34);
        c.output = Tex(3440, 1440, 10);
        const std::string expected =
            "DLSS-NR native D3D11 guides: 120 evaluates in 2.0 s | 117 carried to the present pass, 3 not (not the "
            "presented size 1, slot in flight 2) | depth format 39 2294x960, motion format 34 2294x960, output "
            "3440x1440";
        const std::string line = FormatLine(c, std::chrono::milliseconds(2049));
        if (line != expected)
            std::fprintf(stderr, "got:      %s\nexpected: %s\n", line.c_str(), expected.c_str());
        CHECK(line == expected);

        // Nothing carried: the last refused evaluate's guides, so a refusal that depends on them says which.
        Counts none;
        none.evaluates = 60;
        none.reasons[static_cast<size_t>(Reason::AlreadyShared)] = 60;
        none.lastDepth = Tex(2294, 960, kR32Typeless);
        none.lastMotion = Tex(2294, 960, 34);
        none.lastOutput = Tex(3440, 1440, 10);
        CHECK(FormatLine(none, std::chrono::milliseconds(30000)) ==
              "DLSS-NR native D3D11 guides: 60 evaluates in 30.0 s | 0 carried to the present pass, 60 not (already "
              "shared 60) | last refused: depth format 39 2294x960, motion format 34 2294x960, output 3440x1440");

        Counts all;
        all.evaluates = 5;
        all.reasons[static_cast<size_t>(Reason::Carried)] = 5;
        CHECK(FormatLine(all, std::chrono::milliseconds(2000)) ==
              "DLSS-NR native D3D11 guides: 5 evaluates in 2.0 s | 5 carried to the present pass, 0 not");
    }

    // The rate, and NotWanted never counted.
    {
        Window w;
        Clock::time_point t {};
        std::string line;
        const Inputs in = Good();
        using std::chrono::milliseconds;

        for (int i = 0; i < 1000; ++i)
            CHECK(!w.Note(Reason::NotWanted, in, t + milliseconds(i * 100), &line));
        CHECK(w.Open().evaluates == 0);

        CHECK(!w.Note(Reason::Carried, in, t, &line));
        CHECK(!w.Note(Reason::Carried, in, t + milliseconds(1999), &line));
        CHECK(w.Note(Reason::Carried, in, t + milliseconds(2000), &line));
        CHECK(line.find(": 3 evaluates in 2.0 s | 3 carried") != std::string::npos);
        CHECK(line.find("| depth format 39 2294x960") != std::string::npos);

        // Steady: held to 30 s, then the whole interval.
        int held = 0;
        for (int ms = 2016; ms < 32000; ms += 16)
            held += w.Note(Reason::Carried, in, t + milliseconds(ms), &line) ? 0 : 1;
        CHECK(held == (32000 - 2016) / 16);
        CHECK(w.Note(Reason::Carried, in, t + milliseconds(32000), &line));
        CHECK(line.find(": 1875 evaluates in 30.0 s") != std::string::npos);

        // A refusal appears: written at 2 s.
        CHECK(!w.Note(Reason::SlotInFlight, in, t + milliseconds(33000), &line));
        CHECK(w.Note(Reason::Carried, in, t + milliseconds(34000), &line));
        CHECK(line.find("1 not (slot in flight 1)") != std::string::npos);

        // An hour of a steady mix: a line every 30 s.
        int lines = 0;
        for (int ms = 34016; ms < 34000 + 3600 * 1000; ms += 16)
            lines += w.Note(ms % 1600 == 0 ? Reason::SlotInFlight : Reason::Carried, in, t + milliseconds(ms), &line)
                         ? 1
                         : 0;
        CHECK(lines >= 119 && lines <= 121);
    }

    // A window with no carry shows the last refused evaluate's guides, not a carried one's from an earlier window.
    {
        Window w;
        Clock::time_point t {};
        std::string line;
        Inputs in = Good();
        CHECK(!w.Note(Reason::Carried, in, t, &line));
        CHECK(w.Note(Reason::Carried, in, t + std::chrono::seconds(2), &line));
        CHECK(line.find("| depth format 39 2294x960") != std::string::npos);

        in.depth = Tex(1, 1, 1);
        CHECK(!w.Note(Reason::Transport, in, t + std::chrono::seconds(3), &line));
        in.depth = Tex(7, 5, 2);
        CHECK(w.Note(Reason::Transport, in, t + std::chrono::seconds(4), &line));
        CHECK(line.find("2 not (transport 2) | last refused: depth format 2 7x5, motion format 34 2294x960, output "
                        "3440x1440") != std::string::npos);
        CHECK(line.find("| depth format") == std::string::npos);
    }
}
} // namespace

int main()
{
    Gates();
    Rotation();
    DepthRoutes();
    CacheAndHost();
    WindowAndLine();
    std::printf("PASS: native D3D11 guides: %d checks -- inert off every gate, every refusal alone and in order, "
                "another view of the game left out, the rotation never writing a set a pending list reads, which copy "
                "carries the depth, a stale cache never released under a list, the missing host, the window and its "
                "line\n",
                g_checks);
    return 0;
}
