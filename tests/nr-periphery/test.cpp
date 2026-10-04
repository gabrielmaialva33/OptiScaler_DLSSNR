// Peripheral compression's layout and mapping (OptiScaler/shaders/dlssnr/DlssNr_Periphery.h and the shared
// precompile/dlssnr_periphery_warp.h the shader includes), pinned against the cases they were written for.
// dlssnr/design/peripheral-compression.md.
//
// Everything below runs the production headers in float, as the shader does; only the image the colour pack
// reads is a vector here instead of a texture, and mode 2 of dlssnr.hlsl (the downsample the colour pack must
// reduce to) is transcribed, with run.py holding the transcription to the shader's text.
#include <shaders/dlssnr/DlssNr_Periphery.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace
{
int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

bool Near(double a, double b, double tolerance) { return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance; }

// The image the colour pack reads: one channel, row-major, loads already clamped by the header.
std::vector<float> g_image;
int g_imageWidth = 0;

float ImageAt(int i, int j) { return g_image[(size_t) j * (size_t) g_imageWidth + (size_t) i]; }
} // namespace

#define PW_COLOUR float
#define PW_COLOUR_ZERO 0.0f
#define PW_LOAD_COLOUR(ci, cj) ImageAt((ci), (cj))
#include <shaders/dlssnr/precompile/dlssnr_periphery_warp.h>

using namespace DlssNr::Periphery;

namespace
{
Settings On(uint32_t center = 80, uint32_t work = 90)
{
    Settings s {};
    s.enabled = true;
    s.center = center;
    s.work = work;
    return s;
}

// The uniform grid, as Dispatch computes it.
uint32_t Grid(uint32_t native, float scale) { return (uint32_t) (native * scale + 0.5f); }

// Mode 2 of dlssnr.hlsl, the downsample, transcribed: the exact box integral of the source over the destination
// pixel's footprint. run.py checks that the shader still computes these bounds this way.
float Mode2(int idx, int idy, int srcW, int srcH, int dstW, int dstH)
{
    const float x0 = ((float) idx * (float) srcW) / (float) dstW;
    const float x1 = ((float) (idx + 1) * (float) srcW) / (float) dstW;
    const float y0 = ((float) idy * (float) srcH) / (float) dstH;
    const float y1 = ((float) (idy + 1) * (float) srcH) / (float) dstH;
    const float area = (x1 - x0) * (y1 - y0);
    const int i0 = (int) std::floor(x0);
    const int i1 = (int) std::ceil(x1) - 1;
    const int j0 = (int) std::floor(y0);
    const int j1 = (int) std::ceil(y1) - 1;
    float acc = 0.0f;
    for (int j = j0; j <= j1; ++j)
    {
        const int jj = std::clamp(j, 0, srcH - 1);
        const float aY = std::max(y0, (float) j);
        const float bY = std::min(y1, (float) j + 1.0f);
        const float wy = std::max(bY - aY, 0.0f);
        for (int i = i0; i <= i1; ++i)
        {
            const int ii = std::clamp(i, 0, srcW - 1);
            const float aX = std::max(x0, (float) i);
            const float bX = std::min(x1, (float) i + 1.0f);
            acc += ImageAt(ii, jj) * (std::max(bX - aX, 0.0f) * wy);
        }
    }
    return acc / area;
}

// What the colour pack writes for packed texel (i, j): the shader's CSMain for PERIPHERY_COLOUR, minus alpha.
float PackedColour(int i, int j, const PeripheryAxis& ax, const PeripheryAxis& ay, int nativeW, int nativeH)
{
    const float x0 = PeripheryUnpack((float) i, ax);
    const float x1 = PeripheryUnpack((float) i + 1.0f, ax);
    const float y0 = PeripheryUnpack((float) j, ay);
    const float y1 = PeripheryUnpack((float) j + 1.0f, ay);
    return PeripheryAreaAverage(x0, x1, y0, y1, nativeW, nativeH);
}

void FillRandom(int w, int h, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    g_imageWidth = w;
    g_image.assign((size_t) w * (size_t) h, 0.0f);
    for (auto& v : g_image)
        v = dist(rng);
}

void FillConstant(int w, int h, float value)
{
    g_imageWidth = w;
    g_image.assign((size_t) w * (size_t) h, value);
}

// --- Extents, and the reasons the layout is refused -------------------------------------------------------

void Extents()
{
    // Off: inactive, nothing laid out, and Dispatch keeps the uniform grid.
    Layout off = Build(Settings {}, 3840, 2160, 1.0f);
    CHECK(!off.active && off.reason == kReasonOff && off.modelW == 0);

    // wilsjo2's smoke values (tests/nr_spatial_mapping_smoke.cpp at v0.8.91), reproduced.
    Layout l = Build(On(), 3840, 2160, 1.0f);
    CHECK(l.active && l.reason == kReasonActive && l.modelW == 3456 && l.modelH == 1944);
    CHECK(Near(Pack(0.0f, l.x), 0.0, 0.01));
    CHECK(Near(Pack(3840.0f, l.x), 3456.0, 0.01));
    CHECK(Near(Pack(1920.0f, l.x), 1728.0, 0.01));
    CHECK(Near(Pack(192.0f, l.x), 64.0, 0.01)); // radius 0.9 -> 0.8666667 at centre 0.8, work 0.9

    l = Build(On(), 1921, 1081, 0.85f);
    CHECK(l.active && Grid(1921, 0.85f) == 1633 && l.modelW == 1470);

    // Ours: the Crimson Desert and Cyberpunk frame at 100 and 50, and 1080p at 50 (the RTX 3060 case).
    l = Build(On(), 3440, 1440, 1.0f);
    CHECK(l.active && l.modelW == 3096 && l.modelH == 1296);
    CHECK(Near(l.PixelShare(3440, 1440), 0.81, 0.001)); // what a uniform WorkingScale of 0.9 costs
    CHECK(Near(l.x.compression[0], 0.5, 0.002) && Near(l.x.compression[1], 0.5, 0.002));
    CHECK(Near(l.y.compression[0], 0.5, 0.002) && Near(l.y.compression[1], 0.5, 0.002));

    l = Build(On(), 3440, 1440, 0.5f);
    CHECK(l.active && l.modelW == 1548 && l.modelH == 648 && Grid(3440, 0.5f) == 1720);

    l = Build(On(), 1920, 1080, 0.5f);
    CHECK(l.active && l.modelW == 864 && l.modelH == 486 && Grid(1920, 0.5f) == 960 && Grid(1080, 0.5f) == 540);

    // Refusals, each with its reason.
    CHECK(Build(On(), 3840, 2160, 2.0f).reason == kReasonSupersampling);
    CHECK(Build(On(), 3840, 2160, 1.01f).reason == kReasonSupersampling);
    CHECK(Build(On(), 1920, 1080, 0.25f).reason == kReasonTooSmall); // 432 of 1920 is under a quarter
    CHECK(Build(On(90, 90), 1920, 1080, 1.0f).reason == kReasonRange);
    CHECK(Build(On(5, 80), 1920, 1080, 1.0f).reason == kReasonRange);
    CHECK(Build(On(80, 100), 1920, 1080, 1.0f).reason == kReasonRange);
    CHECK(Build(On(80, 89), 1920, 1080, 1.0f).reason == kReasonCompression); // k = 0.45
    CHECK(Build(On(70, 84), 1920, 1080, 1.0f).reason == kReasonCompression);
    CHECK(Build(On(70, 85), 1920, 1080, 1.0f).active); // k = 0.5 exactly
    CHECK(Build(On(), 1, 1, 1.0f).reason == kReasonFrame);
    CHECK(Build(On(), 1920, 1080, std::nanf("")).reason == kReasonFrame);
    CHECK(Build(On(96, 99), 48, 48, 1.0f).reason == kReasonOneTexel);
    CHECK(Build(On(96, 99), 64, 64, 1.0f).active);
    const Layout refused[] = { Build(On(), 3840, 2160, 2.0f), Build(On(80, 89), 1920, 1080, 1.0f) };
    for (const Layout& r : refused)
        CHECK(!r.active && r.modelW == 0);

    // Every refusal says something the menu can show.
    for (const char* reason : { kReasonOff, kReasonFrame, kReasonSupersampling, kReasonRange, kReasonCompression,
                                kReasonTooSmall, kReasonOneTexel, kReasonBeforeUpscale, kReasonUseProxy,
                                kReasonNoShader, kReasonPassFailed })
        CHECK(reason != nullptr && std::strlen(reason) > 8 && std::strchr(reason, '%') == nullptr);
}

// --- The mapping: inverse, monotone, smooth at both joins, whole-texel centre at 100 -------------------

struct Case
{
    uint32_t w, h;
    float scale;
    uint32_t center, work;
};

const Case kCases[] = {
    { 3840, 2160, 1.0f, 80, 90 },  { 3440, 1440, 1.0f, 80, 90 }, { 3440, 1440, 0.5f, 80, 90 },
    { 1920, 1080, 0.5f, 80, 90 },  { 1921, 1081, 0.85f, 80, 90 }, { 1279, 719, 1.0f, 80, 90 },
    { 2001, 1001, 0.7f, 70, 85 },  { 2560, 1440, 0.75f, 50, 75 }, { 3440, 1440, 1.0f, 90, 95 },
    { 1365, 767, 0.66f, 60, 80 },  { 4095, 2047, 1.0f, 20, 60 },
};

void AxisProperties(const Axis& a, const Case& c)
{
    const PeripheryAxis s = ShaderAxis(a);
    const float n = a.nativeExtent;

    // Inverse to a hundredth of a pixel, in float, across the frame and 64 px past each edge.
    double worst = 0.0;
    for (float x = -64.0f; x <= n + 64.0f; x += 0.37f)
        worst = std::max(worst, (double) std::fabs(PeripheryUnpack(PeripheryPack(x, s), s) - x));
    CHECK(worst <= 0.01);

    // Strictly increasing over every texel centre and well past both edges.
    bool increasing = true;
    float previous = PeripheryPack(-64.5f, s);
    for (int i = -64; i <= (int) n + 64; ++i)
    {
        const float packed = PeripheryPack((float) i + 0.5f, s);
        increasing = increasing && std::isfinite(packed) && packed > previous;
        previous = packed;
    }
    CHECK(increasing);

    // Packed [0, model] is the frame [0, native]: the footprints tile the frame.
    CHECK(Near(PeripheryPack(0.0f, s), 0.0, 0.02));
    CHECK(Near(PeripheryPack(n, s), a.workExtent, 0.02));

    // No kink at either join. One-sided slopes a quarter to half a pixel either side: the same on both sides, and the
    // analytic value -- the global scale at the band's edge, k^2 times it at the frame's.
    const auto slope = [&](float x0, float x1) { return (PeripheryPack(x1, s) - PeripheryPack(x0, s)) / (x1 - x0); };
    for (int side = 0; side < 2; ++side)
    {
        const float sign = side == 0 ? -1.0f : 1.0f;
        const float bandEdge = a.bandCenter + sign * a.halfBand;
        const float frameEdge = side == 0 ? 0.0f : n;
        const double band = a.scale;
        const double edge = a.scale * a.edgeSlope[side];

        const double inBand = slope(bandEdge - sign * 0.5f, bandEdge - sign * 0.25f);
        const double outBand = slope(bandEdge + sign * 0.25f, bandEdge + sign * 0.5f);
        CHECK(Near(inBand, band, 0.03 * band) && Near(outBand, band, 0.03 * band));

        const double inFrame = slope(frameEdge - sign * 0.5f, frameEdge - sign * 0.25f);
        const double outFrame = slope(frameEdge + sign * 0.25f, frameEdge + sign * 0.5f);
        CHECK(Near(inFrame, edge, 0.03 * edge) && Near(outFrame, edge, 0.03 * edge));
        CHECK(a.compression[side] >= 0.5f - 1e-3f);
    }

    // At 100 the band is copied, not resampled: it moves by a whole number of texels.
    if (c.scale == 1.0f)
    {
        const double shift = a.workCenter - a.bandCenter;
        CHECK(Near(shift, std::round(shift), 1e-3));
        bool whole = true;
        for (float x = a.bandCenter - a.halfBand + 0.5f; x < a.bandCenter + a.halfBand; x += 1.0f)
            whole = whole && Near(PeripheryPack(x, s) - x, std::round(shift), 2e-3);
        CHECK(whole);
    }
}

void Mapping()
{
    for (const Case& c : kCases)
    {
        const Layout l = Build(On(c.center, c.work), c.w, c.h, c.scale);
        CHECK(l.active);
        if (!l.active)
        {
            std::printf("  case %ux%u @%.2f %u/%u refused: %s\n", c.w, c.h, c.scale, c.center, c.work, l.reason);
            continue;
        }
        CHECK(l.modelW % 2 == 0 && l.modelH % 2 == 0 && l.modelW < c.w && l.modelH < c.h);
        CHECK(l.modelW <= Grid(c.w, c.scale) + 2 && l.modelH <= Grid(c.h, c.scale) + 2);
        AxisProperties(l.x, c);
        AxisProperties(l.y, c);
    }
}

// --- Nothing compressed is the uniform path ---------------------------------------------------------

// An axis whose work extent is the whole frame: the shoulder is the identity and the mapping a uniform scale.
// Build refuses this layout (work 100 compresses nothing); BuildAxis is what both paths share.
Axis Uniform(uint32_t native, uint32_t model) { return BuildAxis(native, native, model, 0.8f, 0.0f, 0.0f); }

void Identity()
{
    const Axis a = Uniform(3440, 1720);
    const PeripheryAxis s = ShaderAxis(a);
    bool uniform = true;
    for (float x = -32.0f; x < 3500.0f; x += 7.25f)
        uniform = uniform && Near(PeripheryPack(x, s), x * 1720.0 / 3440.0, 2e-3);
    CHECK(uniform);

    // The guide read is GuideMatch::PointSource, the guide resample's own rule. 2293 -> 1720 is Cyberpunk's
    // render size under a half-size model at 3440; 1920 -> 2688 is Onimusha's render size under a 70 percent
    // model at 3840. A centre lands exactly on a texel edge only when the region carries at least as many
    // factors of two as twice the model does; none of these does, so they agree everywhere.
    struct Guide
    {
        uint32_t native, model, region, base;
    };
    for (const Guide& g : { Guide { 3440, 1720, 2293, 0 }, Guide { 3840, 2688, 1920, 0 }, Guide { 3440, 1720, 2293, 16 },
                            Guide { 2560, 1152, 1707, 0 } })
    {
        const PeripheryAxis u = ShaderAxis(Uniform(g.native, g.model));
        int differ = 0;
        for (uint32_t i = 0; i < g.model; ++i)
        {
            const float nativePixel = PeripheryUnpack((float) i + 0.5f, u);
            const int got = PeripheryGuideTexel(nativePixel, (float) g.native, (int) g.base, (int) g.region);
            differ += got != (int) DlssNr::GuideMatch::PointSource(i, g.model, g.base, g.region);
        }
        CHECK(differ == 0);
    }

    // Where the exact answer sits on a texel edge (guides twice the model), float may pick either side of it;
    // never further than that.
    {
        const PeripheryAxis u = ShaderAxis(Uniform(3440, 1720));
        bool adjacent = true;
        for (uint32_t i = 0; i < 1720; ++i)
        {
            const int got = PeripheryGuideTexel(PeripheryUnpack((float) i + 0.5f, u), 3440.0f, 0, 3440);
            adjacent = adjacent && std::abs(got - (int) DlssNr::GuideMatch::PointSource(i, 1720, 0, 3440)) <= 1;
        }
        CHECK(adjacent);
    }

    // The colour pack over a uniform axis is mode 2, the downsample, on a random image.
    for (const auto& [nw, nh, mw, mh] : { std::array<int, 4> { 347, 211, 173, 105 }, std::array<int, 4> { 300, 200, 150, 100 },
                                          std::array<int, 4> { 333, 177, 267, 141 } })
    {
        FillRandom(nw, nh, 7u + (unsigned) nw);
        const PeripheryAxis ax = ShaderAxis(Uniform((uint32_t) nw, (uint32_t) mw));
        const PeripheryAxis ay = ShaderAxis(Uniform((uint32_t) nh, (uint32_t) mh));
        double worst = 0.0;
        for (int j = 0; j < mh; ++j)
            for (int i = 0; i < mw; ++i)
                worst = std::max(worst, (double) std::fabs(PackedColour(i, j, ax, ay, nw, nh) - Mode2(i, j, nw, nh, mw, mh)));
        CHECK(worst < 1e-4);
    }
}

// --- The colour pack: weights sum to one, and nothing is gained or lost -----------------------------

void ColourFilter()
{
    for (const Case& c : { kCases[1], kCases[3], kCases[6], kCases[7] })
    {
        // A quarter-size frame keeps the run quick; the layout rules are the same.
        const uint32_t w = c.w / 4, h = c.h / 4;
        const Layout l = Build(On(c.center, c.work), w, h, c.scale);
        CHECK(l.active);
        if (!l.active)
            continue;
        const PeripheryAxis ax = ShaderAxis(l.x), ay = ShaderAxis(l.y);

        // A constant frame packs to the same constant everywhere: the weights of every footprint sum to one.
        FillConstant((int) w, (int) h, 0.625f);
        double worst = 0.0;
        for (uint32_t j = 0; j < l.modelH; ++j)
            for (uint32_t i = 0; i < l.modelW; ++i)
                worst = std::max(worst, std::fabs(PackedColour((int) i, (int) j, ax, ay, (int) w, (int) h) - 0.625));
        CHECK(worst < 1e-5);

        // The footprints tile the frame, so an exact area filter conserves the frame's total: each packed
        // texel's average times its footprint's area, summed, is the sum of the frame.
        FillRandom((int) w, (int) h, 11u + w);
        double frameSum = 0.0, packedSum = 0.0;
        for (float v : g_image)
            frameSum += v;
        for (uint32_t j = 0; j < l.modelH; ++j)
        {
            const double height = PeripheryUnpack((float) j + 1.0f, ay) - PeripheryUnpack((float) j, ay);
            for (uint32_t i = 0; i < l.modelW; ++i)
            {
                const double width = PeripheryUnpack((float) i + 1.0f, ax) - PeripheryUnpack((float) i, ax);
                packedSum += PackedColour((int) i, (int) j, ax, ay, (int) w, (int) h) * width * height;
            }
        }
        CHECK(Near(packedSum, frameSum, 1e-4 * frameSum));

        // At the frame's edge one packed texel covers several frame pixels; in the band, one (at 100).
        const double edgeWidth = PeripheryUnpack(1.0f, ax) - PeripheryUnpack(0.0f, ax);
        const double mid = std::floor(l.modelW * 0.5);
        const double bandWidth = PeripheryUnpack((float) mid + 1.0f, ax) - PeripheryUnpack((float) mid, ax);
        const double edgeLimit = 1.0 / (l.x.scale * l.x.edgeSlope[0]); // the slope only rises inward
        CHECK(edgeWidth <= edgeLimit * 1.001 && edgeWidth >= 0.8 * edgeLimit);
        CHECK(Near(bandWidth, 1.0 / l.x.scale, 1e-3));
        CHECK(edgeWidth <= PERIPHERY_MAX_SPAN - 1);
    }

    // The worst a valid layout asks of the loop: compression 0.5 at a quarter of the frame is 16 px a texel.
    const Layout worst = Build(On(50, 75), 1920, 1080, 0.34f);
    CHECK(worst.active);
    if (worst.active)
    {
        const PeripheryAxis ax = ShaderAxis(worst.x);
        CHECK(PeripheryUnpack(1.0f, ax) - PeripheryUnpack(0.0f, ax) < PERIPHERY_MAX_SPAN - 1);
    }
}

// --- Motion: both ends through the mapping, in frame pixels -----------------------------------------

void Motion()
{
    const Layout l = Build(On(), 3440, 1440, 1.0f);
    const PeripheryAxis s = ShaderAxis(l.x);

    // Zero stays zero, exactly, everywhere.
    bool zero = true;
    for (float x = -100.5f; x < 3540.0f; x += 13.0f)
        zero = zero && PeripheryPackMotion(x, 0.0f, s) == 0.0f;
    CHECK(zero);

    // Inside the band at 100 a vector is unchanged; at the edge it shrinks toward k^2 of itself; with an end off
    // the frame it is still finite, and still points the same way.
    CHECK(Near(PeripheryPackMotion(1720.0f, 12.5f, s), 12.5, 1e-3));
    CHECK(Near(PeripheryPackMotion(1500.0f, -40.0f, s), -40.0, 1e-3));
    const float edge = PeripheryPackMotion(4.0f, 2.0f, s);
    CHECK(edge > 0.0f && edge < 0.6f * 2.0f && Near(edge, 2.0 * 0.25, 0.1));
    const float out = PeripheryPackMotion(5.0f, -500.0f, s);
    CHECK(std::isfinite(out) && out < 0.0f && out > -500.0f);
    CHECK(std::isfinite(PeripheryPackMotion(3435.0f, 5000.0f, s)));

    // The frame-pixel scale, which is the defect in wilsjo2's v0.8.91. Cyberpunk at 3440x1440, DLSS Quality:
    // render 2293x960, low-resolution vectors, game scale 2293 x 960 (vectors in normalised units). The
    // mapping must be handed frame pixels: 2293 x (3440 / 2293) = 3440. The raw game scale left every vector
    // two thirds of its length.
    const uint32_t refX = DlssNr::GuideMatch::MotionReference(true, 2293, 3440);
    const uint32_t refY = DlssNr::GuideMatch::MotionReference(true, 960, 1440);
    CHECK(refX == 2293 && refY == 960);
    const float frameX = FrameMotionScale(2293.0f, 3440, refX);
    const float frameY = FrameMotionScale(960.0f, 1440, refY);
    CHECK(Near(frameX, 3440.0, 0.01) && Near(frameY, 1440.0, 0.01));

    const float gameVector = 0.01f; // a hundredth of the screen to the right
    const float packed = PeripheryPackMotion(1720.0f, gameVector * frameX, s);
    const float defect = PeripheryPackMotion(1720.0f, gameVector * 2293.0f, s);
    CHECK(Near(packed, 34.4, 0.01));
    CHECK(Near(defect, 22.93, 0.01) && !Near(packed, defect, 1.0));

    // Output-resolution vectors are measured against the frame already: the scale passes through.
    CHECK(Near(FrameMotionScale(1.0f, 3440, DlssNr::GuideMatch::MotionReference(false, 2293, 3440)), 1.0, 1e-6));

    // At 50 the packed raster is half the frame in the band, so is a vector there.
    const Layout half = Build(On(), 3440, 1440, 0.5f);
    CHECK(Near(PeripheryPackMotion(1720.0f, 34.4f, ShaderAxis(half.x)), 17.2, 0.01));
}

// --- Constants ----------------------------------------------------------------------------------------

void Constants()
{
    const Layout l = Build(On(), 3440, 1440, 1.0f);
    const PeripheryConstants c = MakeConstants(l);
    CHECK(sizeof(PeripheryConstants) == 256 && alignof(PeripheryConstants) == 256);
    CHECK(c.NativeWidth == 3440 && c.NativeHeight == 1440 && c.ModelWidth == 3096 && c.ModelHeight == 1296);
    CHECK(c.OutWidth == 0 && c.DepthWidth == 0 && c.MotionScaleX == 0.0f);
    CHECK(c.AxisX.bandCenter == l.x.bandCenter && c.AxisX.workCenter == l.x.workCenter &&
          c.AxisX.edgeSlopePos == l.x.edgeSlope[1] && c.AxisY.halfSpanNeg == l.y.halfSpan[0]);
    CHECK(c.AxisX.pad0 == 0.0f && c.AxisY.pad2 == 0.0f);
}
} // namespace

int main()
{
    Extents();
    Mapping();
    Identity();
    ColourFilter();
    Motion();
    Constants();

    if (g_failures != 0)
    {
        std::printf("FAIL: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }

    std::printf("PASS: %d checks -- extents and refusals, inverse, monotone, smooth joins, whole-texel centre, the "
                "uniform path (guide reads and mode 2), the area filter's weights and conservation, motion in "
                "frame pixels\n",
                g_checks);
    return 0;
}
