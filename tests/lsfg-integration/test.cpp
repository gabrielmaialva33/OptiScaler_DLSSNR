// LSFG inside FSR-FG (study branch): the host-testable half of retoque/lsfg/ -- the DXIL PSV0 reader the root
// signatures are built from, the pass graph, the constant-buffer table, which frames LSFG takes, and the
// history schedule. The headers under test are included unmodified; nothing here touches D3D12.
//
// argv[1], optional: a folder of the user's own converted DXIL (<rid>.cso, Tmp/lsfg-study/dxil). When given,
// all 99 modules are read and held against the graph the way ProgramSet_Dx12 / Context_Dx12 hold them.

#include <lsfg/LsfgDxil.h>
#include <lsfg/LsfgGraph.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace Lsfg;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        ++g_checks;                                                                                                    \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ++g_failures;                                                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
        }                                                                                                              \
    } while (false)

// ---------------------------------------------------------------------------------------------------------
// Synthetic DXIL containers

struct Record
{
    uint32_t type, space, lower, upper, kind, flags;
};

static void PutU32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(uint8_t(v >> (8 * i)));
}

static std::vector<uint8_t> MakePsv(const std::vector<Record>& records, uint32_t recordSize, uint32_t infoSize,
                                    uint32_t nx, uint32_t ny, uint32_t nz)
{
    std::vector<uint8_t> psv;
    PutU32(psv, infoSize);
    std::vector<uint8_t> info(infoSize, 0);
    if (infoSize >= 48)
    {
        auto put = [&](size_t at, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                info[at + i] = uint8_t(v >> (8 * i));
        };
        put(36, nx);
        put(40, ny);
        put(44, nz);
    }
    psv.insert(psv.end(), info.begin(), info.end());
    PutU32(psv, uint32_t(records.size()));
    if (!records.empty())
    {
        PutU32(psv, recordSize);
        for (const auto& r : records)
        {
            PutU32(psv, r.type);
            PutU32(psv, r.space);
            PutU32(psv, r.lower);
            PutU32(psv, r.upper);
            if (recordSize >= 24)
            {
                PutU32(psv, r.kind);
                PutU32(psv, r.flags);
                for (uint32_t extra = 24; extra < recordSize; extra += 4)
                    PutU32(psv, 0);
            }
        }
    }
    return psv;
}

struct Part
{
    const char* fourcc;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> MakeContainer(const std::vector<Part>& parts)
{
    std::vector<uint8_t> b = { 'D', 'X', 'B', 'C' };
    b.resize(20, 0); // digest
    b.push_back(1);
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    const size_t sizeAt = b.size();
    PutU32(b, 0); // container size, patched below
    PutU32(b, uint32_t(parts.size()));
    const size_t tableAt = b.size();
    for (size_t i = 0; i < parts.size(); ++i)
        PutU32(b, 0);
    for (size_t i = 0; i < parts.size(); ++i)
    {
        const uint32_t offset = uint32_t(b.size());
        for (int k = 0; k < 4; ++k)
            b[tableAt + i * 4 + k] = uint8_t(offset >> (8 * k));
        b.insert(b.end(), parts[i].fourcc, parts[i].fourcc + 4);
        PutU32(b, uint32_t(parts[i].data.size()));
        b.insert(b.end(), parts[i].data.begin(), parts[i].data.end());
    }
    const uint32_t total = uint32_t(b.size());
    for (int k = 0; k < 4; ++k)
        b[sizeAt + k] = uint8_t(total >> (8 * k));
    return b;
}

// gamma0's real layout: cb, push constants, two samplers, alpha3 (7 x Texture2DArray), betaImageArray
// (7 x Texture2D at space 34 in the real module; 7 here), the output array.
static const std::vector<Record> kGamma0 = {
    { 2, 0, 0, 0, 13, 0 }, { 2, 0, 1, 1, 13, 0 }, { 1, 2, 0, 0, 14, 0 }, { 1, 3, 0, 0, 14, 0 },
    { 3, 14, 0, 6, 7, 0 }, { 3, 34, 0, 6, 2, 0 }, { 6, 25, 0, 6, 7, 0 },
};

static void TestDxil()
{
    std::string error;
    Dxil::Reflection r;

    {
        const auto c = MakeContainer({ { "DXIL", { 1, 2, 3, 4 } }, { "PSV0", MakePsv(kGamma0, 24, 52, 8, 8, 1) } });
        CHECK(Dxil::Parse(c.data(), c.size(), r, error));
        CHECK(r.bindings.size() == 7);
        CHECK(r.hasKinds && r.hasNumThreads);
        CHECK(r.numThreads[0] == 8 && r.numThreads[1] == 8 && r.numThreads[2] == 1);
        CHECK(r.bindings[0].cls == Dxil::ResClass::Cbv && r.bindings[0].lower == 0);
        CHECK(r.bindings[1].cls == Dxil::ResClass::Cbv && r.bindings[1].lower == 1);
        CHECK(r.bindings[2].cls == Dxil::ResClass::Sampler && r.bindings[2].space == 2);
        CHECK(r.bindings[4].cls == Dxil::ResClass::Srv && r.bindings[4].space == 14 && r.bindings[4].count == 7 &&
              r.bindings[4].kind == Dxil::kKindTexture2DArray);
        CHECK(r.bindings[5].kind == Dxil::kKindTexture2D);
        CHECK(r.bindings[6].cls == Dxil::ResClass::Uav && r.bindings[6].space == 25);
    }

    {
        // Part order does not matter; extra parts are ignored; record version 0 has no kinds.
        const auto c = MakeContainer({ { "SFI0", { 0, 0, 0, 0, 0, 0, 0, 0 } },
                                       { "PSV0", MakePsv(kGamma0, 16, 24, 0, 0, 0) },
                                       { "HASH", std::vector<uint8_t>(20, 7) },
                                       { "DXIL", { 9 } } });
        CHECK(Dxil::Parse(c.data(), c.size(), r, error));
        CHECK(!r.hasKinds && !r.hasNumThreads);
        CHECK(r.bindings.size() == 7 && r.bindings[4].kind == 0 && r.bindings[4].count == 7);
    }

    {
        // A longer, future record size still reads the first six fields.
        const auto c = MakeContainer({ { "DXIL", { 1 } }, { "PSV0", MakePsv(kGamma0, 32, 52, 8, 8, 1) } });
        CHECK(Dxil::Parse(c.data(), c.size(), r, error));
        CHECK(r.bindings.size() == 7 && r.bindings[6].kind == Dxil::kKindTexture2DArray);
    }

    {
        // No resources at all (beta1 has a sampler, but a module with none must parse).
        const auto c = MakeContainer({ { "DXIL", { 1 } }, { "PSV0", MakePsv({}, 24, 52, 8, 8, 1) } });
        CHECK(Dxil::Parse(c.data(), c.size(), r, error) && r.bindings.empty());
    }

    auto refuses = [&](std::vector<uint8_t> c, const char* what)
    {
        Dxil::Reflection out;
        std::string why;
        const bool ok = Dxil::Parse(c.data(), c.size(), out, why);
        if (ok)
            std::fprintf(stderr, "  accepted: %s\n", what);
        CHECK(!ok && !why.empty());
    };

    const auto good = MakeContainer({ { "DXIL", { 1, 2, 3, 4 } }, { "PSV0", MakePsv(kGamma0, 24, 52, 8, 8, 1) } });
    {
        auto c = good;
        c[0] = 'X';
        refuses(c, "bad magic");
    }
    refuses(std::vector<uint8_t>(good.begin(), good.begin() + 31), "header cut");
    refuses(MakeContainer({ { "PSV0", MakePsv(kGamma0, 24, 52, 8, 8, 1) } }), "no DXIL part (SM5 DXBC)");
    refuses(MakeContainer({ { "DXIL", { 1 } } }), "no PSV0");
    refuses(MakeContainer({ { "DXIL", { 1 } }, { "PSV0", MakePsv(kGamma0, 20, 52, 8, 8, 1) } }), "record size 20");
    refuses(
        MakeContainer({ { "DXIL", { 1 } }, { "PSV0", MakePsv({ { 3, 5, 0, 0xFFFFFFFFu, 2, 0 } }, 24, 52, 8, 8, 1) } }),
        "unbounded range");
    refuses(MakeContainer({ { "DXIL", { 1 } }, { "PSV0", MakePsv({ { 3, 5, 4, 2, 2, 0 } }, 24, 52, 8, 8, 1) } }),
            "inverted range");
    {
        auto c = good;
        c[24] = 0xFF; // container size beyond the file
        c[25] = 0xFF;
        refuses(c, "container size");
    }
    {
        auto c = good;
        c[28] = 200; // part count
        refuses(c, "part count");
    }

    // Every truncation and every single-byte corruption of a valid container is either read or refused, never
    // a read past the end (ASan watches).
    for (size_t n = 0; n < good.size(); ++n)
    {
        Dxil::Reflection out;
        std::string why;
        std::vector<uint8_t> cut(good.begin(), good.begin() + n);
        Dxil::Parse(cut.data(), cut.size(), out, why);
    }
    for (size_t i = 0; i < good.size(); ++i)
    {
        for (uint8_t v : { uint8_t(0x00), uint8_t(0xFF), uint8_t(0x80), uint8_t(0x7F) })
        {
            auto c = good;
            c[i] = v;
            Dxil::Reflection out;
            std::string why;
            Dxil::Parse(c.data(), c.size(), out, why);
        }
    }
    CHECK(!Dxil::Parse(nullptr, 0, r, error));
}

// ---------------------------------------------------------------------------------------------------------
// The graph

static const GraphPass* FindPass(const std::vector<GraphPass>& passes, uint32_t baseRid, uint32_t index, bool perf,
                                 bool fp16)
{
    const uint32_t rid = VariantRid(baseRid, perf, fp16);
    for (const auto& p : passes)
    {
        if (p.rid == rid && p.index == index)
            return &p;
    }
    return nullptr;
}

static bool Groups(const GraphPass* p, uint32_t x, uint32_t y)
{
    return p != nullptr && p->groupsX == x && p->groupsY == y;
}

static void TestGraph()
{
    Graph g;
    std::string error;

    CHECK(BuildGraph(1920, 1080, 1.0f, false, false, g, error));
    CHECK(g.pre.size() == kPrePasses && g.main.size() == kMainPasses);
    CHECK(g.flowWidth == 1920 && g.flowHeight == 1080);

    // Dispatch extents at 1920x1080, flow 1.0: tools/lsfg-study/PIPELINE_V3_1.md, section 6.
    CHECK(Groups(FindPass(g.pre, 0, 0, false, false), 30, 17));   // mipmaps
    CHECK(Groups(FindPass(g.pre, 13, 0, false, false), 2, 1));    // alpha0 level 0
    CHECK(Groups(FindPass(g.pre, 13, 6, false, false), 120, 68)); // alpha0 level 6
    CHECK(Groups(FindPass(g.pre, 15, 0, false, false), 1, 1));    // alpha2 level 0
    CHECK(Groups(FindPass(g.pre, 16, 6, false, false), 60, 34));  // alpha3 level 6
    CHECK(Groups(FindPass(g.pre, 22, 0, false, false), 60, 34));  // beta0
    CHECK(Groups(FindPass(g.pre, 26, 0, false, false), 15, 9));   // beta4
    CHECK(Groups(FindPass(g.main, 3, 0, false, false), 1, 1));    // gamma0 level 0
    CHECK(Groups(FindPass(g.main, 3, 6, false, false), 60, 34));  // gamma0 level 6
    CHECK(Groups(FindPass(g.main, 8, 0, false, false), 15, 9));   // delta0 (level 4)
    CHECK(Groups(FindPass(g.main, 17, 2, false, false), 60, 34)); // epsilon0 (level 6)
    CHECK(Groups(FindPass(g.main, 1, 0, false, false), 120, 68)); // generate

    // pc_FirstIter: only the first level of each chain, at its first and last shader.
    CHECK(FindPass(g.main, 3, 0, false, false)->special == 1 && FindPass(g.main, 7, 0, false, false)->special == 1);
    CHECK(FindPass(g.main, 4, 0, false, false)->special == 0 && FindPass(g.main, 3, 1, false, false)->special == 0);
    CHECK(FindPass(g.main, 8, 0, false, false)->special == 1 && FindPass(g.main, 21, 0, false, false)->special == 1);

    // Pools: the UAV binding and the SRV binding after it cover the same images.
    CHECK(g.pools.at(kSourceBinding) == std::vector<uint32_t> { g.source });
    CHECK(g.pools.at(kOutputBinding) == std::vector<uint32_t> { g.output });
    CHECK(g.pools.count(kOutputBinding + 1) == 0);
    for (uint32_t binding = 5; binding < kOutputBinding; binding += 2)
        CHECK(g.pools.count(binding) == 1 && g.pools.at(binding) == g.pools.at(binding + 1));
    CHECK(g.pools.at(5).size() == 7 && g.pools.at(23).size() == 6 && g.pools.at(13).size() == 7);
    CHECK(g.pools.at(35).size() == 3 && g.pools.at(45).size() == 3 && g.pools.at(25).size() == 7);
    for (uint32_t i = 0; i < 7; ++i)
    {
        const auto& m = g.images[g.pools.at(5)[i]];
        CHECK(m.format == ImageFormat::R8 && m.width == (1920u >> i) && m.height == (1080u >> i) && m.layers == 1);
        CHECK(g.images[g.pools.at(13)[i]].layers == 12); // alpha3: 6 x mul, three base frames of history
    }
    CHECK(g.images[g.source].layers == 2 && g.images[g.source].format == ImageFormat::Display);
    CHECK(g.images[g.output].format == ImageFormat::Display && g.images[g.output].width == 1920);
    CHECK(g.images[g.pools.at(33)[0]].format == ImageFormat::Rgba16F); // gamma4 results

    // Every image but the source is written by exactly one dispatch a frame, and read only after it.
    std::vector<int> writers(g.images.size(), 0);
    std::vector<int> firstWrite(g.images.size(), -1);
    int order = 0;
    bool readBeforeWrite = false;
    for (const auto* passes : { &g.pre, &g.main })
    {
        for (const auto& p : *passes)
        {
            for (auto r : p.reads)
            {
                CHECK(r < g.images.size());
                if (r != g.source && firstWrite[r] < 0)
                    readBeforeWrite = true;
            }
            for (auto w : p.writes)
            {
                CHECK(w < g.images.size());
                ++writers[w];
                if (firstWrite[w] < 0)
                    firstWrite[w] = order;
            }
            ++order;
        }
    }
    CHECK(!readBeforeWrite);
    CHECK(writers[g.source] == 0);
    for (size_t i = 0; i < writers.size(); ++i)
    {
        if (i != g.source)
            CHECK(writers[i] == 1);
        CHECK(g.images[i].width > 0 && g.images[i].height > 0);
    }

    // Variants: performance +24 and half the channels, FP16 +48; mipmaps and generate have none.
    Graph p;
    CHECK(BuildGraph(1920, 1080, 1.0f, true, false, p, error));
    CHECK(FindPass(p.pre, 0, 0, true, false) != nullptr && FindPass(p.pre, 0, 0, true, false)->rid == 0);
    CHECK(FindPass(p.main, 1, 0, true, false) != nullptr && FindPass(p.main, 1, 0, true, false)->rid == 1);
    CHECK(FindPass(p.main, 3, 0, true, false)->rid == 27 && FindPass(p.pre, 26, 0, true, false)->rid == 50);
    CHECK(p.images[p.pools.at(13)[0]].layers == 6 && p.images[p.pools.at(7)[0]].layers == 1);
    Graph h;
    CHECK(BuildGraph(1920, 1080, 1.0f, true, true, h, error));
    CHECK(FindPass(h.main, 3, 0, true, true)->rid == 75 && FindPass(h.main, 1, 0, true, true)->rid == 1);

    // Flow scale sizes the pyramids, never the source or the output.
    Graph f;
    CHECK(BuildGraph(1920, 1080, 0.5f, false, false, f, error));
    CHECK(f.flowWidth == 960 && f.flowHeight == 540);
    CHECK(Groups(FindPass(f.pre, 0, 0, false, false), 15, 9));
    CHECK(f.images[f.source].width == 1920 && f.images[f.output].width == 1920);
    CHECK(Groups(FindPass(f.main, 1, 0, false, false), 120, 68));

    // Odd sizes build; the ones lsfg-vk refuses are refused.
    CHECK(BuildGraph(257, 193, 1.0f, false, false, g, error));
    CHECK(BuildGraph(3440, 1440, 1.0f, false, false, g, error));
    CHECK(!BuildGraph(63, 1080, 1.0f, false, false, g, error));
    CHECK(!BuildGraph(200, 200, 0.25f, false, false, g, error)); // 50 px once flow-scaled
    CHECK(!BuildGraph(1920, 1080, std::nanf(""), false, false, g, error));
    CHECK(!BuildGraph(1920, 1080, 1.5f, false, false, g, error));
    CHECK(!BuildGraph(20000, 1080, 1.0f, false, false, g, error));
}

// ---------------------------------------------------------------------------------------------------------
// Constant buffers

static void TestCbuffers()
{
    // CBV count of each base shader in the converted modules (PSV0 of Tmp/lsfg-study/dxil, 2026-10-13).
    const int cbvs[kShaderCount] = { 1, 1, 1, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 1 };
    for (uint32_t base = 0; base < kShaderCount; ++base)
    {
        const auto use = CbuffersOf(base);
        CHECK(int(use.cb) + int(use.pc) == cbvs[base]);
    }
    for (uint32_t rid = 0; rid < 99; ++rid)
        CHECK(BaseShader(rid) < kShaderCount);
    CHECK(BaseShader(3) == 3 && BaseShader(27) == 3 && BaseShader(51) == 3 && BaseShader(75) == 3 &&
          BaseShader(98) == 26);

    CHECK(offsetof(FrameConstants, timestamp) == 0);
    CHECK(offsetof(FrameConstants, sourceIndex) == 4);
    CHECK(offsetof(FrameConstants, advancedColorKind) == 8);
    CHECK(offsetof(FrameConstants, hdrSupport) == 12);
    CHECK(offsetof(FrameConstants, resolutionInvScale) == 16);
    CHECK(offsetof(FrameConstants, uiThreshold) == 20);
    CHECK(kCbWords * 4 >= sizeof(FrameConstants) && kPcWords >= 2);
}

// ---------------------------------------------------------------------------------------------------------
// Which frames LSFG takes, and the history schedule

static void TestPolicy()
{
    using namespace Format;
    CHECK(ClassifySurface(Bgra8, 1920, 1080, Bgra8, 1920, 1080, kTransferSrgb, 1.0f) == Surface::Ok);
    CHECK(ClassifySurface(Rgba8Srgb, 1920, 1080, Rgba8, 1920, 1080, kTransferSrgb, 1.0f) == Surface::Ok);
    CHECK(ClassifySurface(Rgba8Typeless, 1920, 1080, Rgba8Srgb, 1920, 1080, kTransferSrgb, 1.0f) == Surface::Ok);
    CHECK(ClassifySurface(Rgb10A2, 2560, 1440, Rgb10A2, 2560, 1440, kTransferSrgb, 1.0f) == Surface::Ok);
    CHECK(ClassifySurface(Rgb10A2, 2560, 1440, Rgb10A2, 2560, 1440, kTransferPq, 1.0f) == Surface::Hdr);
    CHECK(ClassifySurface(Rgba16F, 2560, 1440, Rgba16F, 2560, 1440, kTransferScRgb, 1.0f) == Surface::Hdr);
    CHECK(ClassifySurface(Rgba16F, 2560, 1440, Rgba16F, 2560, 1440, kTransferSrgb, 1.0f) == Surface::Hdr);
    CHECK(ClassifySurface(Bgra8, 1920, 1080, Rgba8, 1920, 1080, kTransferSrgb, 1.0f) == Surface::FormatMismatch);
    CHECK(ClassifySurface(Bgra8, 1920, 1080, Bgra8, 1280, 720, kTransferSrgb, 1.0f) == Surface::SizeMismatch);
    CHECK(ClassifySurface(26 /* R11G11B10 */, 1920, 1080, 26, 1920, 1080, kTransferSrgb, 1.0f) ==
          Surface::UnsupportedFormat);
    CHECK(ClassifySurface(Bgra8, 1920, 1080, Bgra8, 1920, 1080, 7, 1.0f) == Surface::UnsupportedFormat);
    CHECK(ClassifySurface(Bgra8, 100, 100, Bgra8, 100, 100, kTransferSrgb, 0.5f) == Surface::TooSmall);
    CHECK(ClassifySurface(Bgra8, 63, 1080, Bgra8, 63, 1080, kTransferSrgb, 1.0f) == Surface::TooSmall);
    CHECK(ClassifySurface(Bgra8, 128, 128, Bgra8, 128, 128, kTransferSrgb, std::nanf("")) == Surface::Ok);

    CHECK(WorkingFormat(Bgra8Srgb) == Bgra8 && WorkingFormat(Rgba8Typeless) == Rgba8 &&
          WorkingFormat(Rgb10A2Typeless) == Rgb10A2 && WorkingFormat(Rgba16F) == 0);
    for (auto s : { Surface::Ok, Surface::UnsupportedFormat, Surface::FormatMismatch, Surface::Hdr,
                    Surface::SizeMismatch, Surface::TooSmall })
        CHECK(std::string(SurfaceText(s)).size() > 1);

    CHECK(ClampFlow(std::nanf("")) == 1.0f && ClampFlow(0.1f) == 0.25f && ClampFlow(2.0f) == 1.0f &&
          ClampFlow(0.5f) == 0.5f);

    CHECK(TimestampFor(1, 1) == 0.5f);
    CHECK(std::fabs(TimestampFor(1, 2) - 1.0f / 3.0f) < 1e-6f && TimestampFor(2, 3) == 0.5f);

    // A fresh context ingests one frame before it generates; slots alternate; the counter never resets.
    History h;
    CHECK(h.SourceSlot() == 0 && !h.Generates(false) && !h.Follows(100));
    h.Advance(!h.Follows(100), 100); // the first frame is a reset by continuity
    CHECK(h.SourceSlot() == 1 && h.sinceReset == 1 && h.Follows(101) && !h.Follows(102) && !h.Follows(100));
    CHECK(h.Generates(!h.Follows(101)) && !h.Generates(true));
    h.Advance(false, 101);
    CHECK(h.SourceSlot() == 0 && h.iteration == 2 && h.Generates(!h.Follows(102)));

    // A reset frame is ingested but generates nothing; the next one does.
    CHECK(!h.Generates(true));
    h.Advance(true, 102);
    CHECK(h.iteration == 3 && h.sinceReset == 1 && h.Generates(!h.Follows(103)));

    // A gap in the frame IDs (a frame skipped, refused or interpolated by FFX) is a reset: no generated frame.
    CHECK(!h.Follows(105) && !h.Generates(!h.Follows(105)));
    h.Advance(!h.Follows(105), 105);
    CHECK(h.sinceReset == 1 && h.Generates(!h.Follows(106)));

    // A broken stream (cancelled frame) starts the warm-up over, and the next frame cannot follow anything.
    h.Restart();
    CHECK(!h.Generates(false) && !h.Follows(106));
    h.Advance(!h.Follows(106), 106);
    CHECK(h.Generates(!h.Follows(107)) && h.iteration == 5);
}

// ---------------------------------------------------------------------------------------------------------
// The user's own converted modules, when a folder is given

static void TestRealModules(const std::filesystem::path& dir)
{
    int read = 0;
    for (uint32_t rid = 0; rid < 99; ++rid)
    {
        std::ifstream file(dir / (std::to_string(rid) + ".cso"), std::ios::binary);
        if (!file)
        {
            std::fprintf(stderr, "FAIL: %u.cso missing in the given folder\n", rid);
            ++g_failures;
            continue;
        }
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        Dxil::Reflection r;
        std::string error;
        if (!Dxil::Parse(blob.data(), blob.size(), r, error))
        {
            std::fprintf(stderr, "FAIL: %u.cso: %s\n", rid, error.c_str());
            ++g_failures;
            continue;
        }
        ++read;

        const uint32_t base = BaseShader(rid);
        const uint32_t group = (base == 0 || base == 26) ? 32 : (base <= 2 ? 16 : 8);
        CHECK(r.hasKinds && r.hasNumThreads);
        CHECK(r.numThreads[0] == group && r.numThreads[1] == group && r.numThreads[2] == 1);

        const auto use = CbuffersOf(base);
        int cbvs = 0;
        for (const auto& b : r.bindings)
            cbvs += b.cls == Dxil::ResClass::Cbv;
        CHECK(cbvs == int(use.cb) + int(use.pc));

        // Each SRV/UAV binding against the graph it will be bound in, as Context_Dx12 checks it.
        Graph g;
        CHECK(BuildGraph(1920, 1080, 1.0f, rid >= 27 && rid < 51 ? true : rid >= 75, rid >= 51, g, error));
        for (const auto& b : r.bindings)
        {
            CHECK(b.cls != Dxil::ResClass::Other);
            if (b.cls == Dxil::ResClass::Sampler)
                CHECK(b.space >= 1 && b.space <= 3 && b.count == 1);
            if (b.cls != Dxil::ResClass::Srv && b.cls != Dxil::ResClass::Uav)
                continue;
            CHECK(b.lower == 0 && b.count <= 7);
            CHECK(g.pools.count(b.space) == 1 && g.pools.at(b.space).size() == b.count);
            if (g.pools.count(b.space) == 0)
                continue;
            for (auto image : g.pools.at(b.space))
                CHECK(b.kind == Dxil::kKindTexture2DArray || g.images[image].layers == 1);
        }
    }
    std::printf("real modules: %d of 99 read from %s\n", read, dir.string().c_str());
}

int main(int argc, char** argv)
{
    TestDxil();
    TestGraph();
    TestCbuffers();
    TestPolicy();
    if (argc > 1 && argv[1][0] != '\0')
        TestRealModules(argv[1]);
    else
        std::printf("real modules: skipped (set LSFG_DXIL_DIR to a folder of converted <rid>.cso)\n");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
