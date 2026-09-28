// The static-overlay mask's per-pixel rule (OptiScaler/shaders/synth_motion/precompile/static_overlay_rule.h),
// the exact code the shader runs, over synthetic frame sequences. The shader's own loads become array
// lookups with the same clamping; everything else is the production header.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

using std::abs;
using std::max;
using std::min;

static float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

namespace
{
constexpr int W = 192;
constexpr int H = 128;

std::vector<float> g_cur(W * H), g_prev(W * H), g_prot(W * H), g_streak(W * H);

int Clamp(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }
int Index(int x, int y) { return Clamp(y, H - 1) * W + Clamp(x, W - 1); }
float Luma(int x, int y) { return g_cur[Index(x, y)]; }
float Change(int x, int y) { return abs(g_cur[Index(x, y)] - g_prev[Index(x, y)]); }
float Prot(int x, int y) { return g_prot[Index(x, y)]; }
float Streak(int x, int y) { return g_streak[Index(x, y)]; }
} // namespace

#define UM_FN inline
#define UM_OUT(T) T&
#define UM_UNROLL
#define UM_LUMA(x, y) Luma(x, y)
#define UM_CHANGE(x, y) Change(x, y)
#define UM_PROT(x, y) Prot(x, y)
#define UM_STREAK(x, y) Streak(x, y)
#include <shaders/synth_motion/precompile/static_overlay_rule.h>

namespace
{
// The shipped thresholds (DlssNr_UiMask_Dx12.cpp).
UiMaskParams Shipped()
{
    UiMaskParams p {};
    p.staticEps = 0.008f;
    p.coreEps = 0.012f;
    p.motionTau = 0.02f;
    p.detailMin = 0.15f;
    p.decay = 0.985f;
    p.dropTau = 0.2f;
    p.streakMin = 8.0f;
    p.supportMin = 3.0f;
    p.sidesMin = 4.0f;
    return p;
}

// The rule as it shipped first (hud-protection.md, "First slice"), for the regression the new gates fix:
// no core, the mean of two rings instead of sides, detail 0.08, no streak, no support.
UiMaskParams FirstSlice()
{
    UiMaskParams p = Shipped();
    p.coreEps = 10.0f;    // no core
    p.detailMin = 0.08f;
    p.streakMin = 0.0f;
    p.supportMin = 0.0f;
    p.sidesMin = 1.0f;    // any one side
    return p;
}

// Grainy scenery with strong local contrast, like PS2 sand: a hash, not a smooth pattern, so neighbouring
// pixels differ by more than the detail threshold in many places. sx is a scroll offset.
float Scenery(int x, int y, int sx)
{
    uint32_t h = static_cast<uint32_t>(x + sx) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return 0.35f + 0.4f * static_cast<float>(h & 1023) / 1023.0f;
}

struct Frame
{
    std::vector<float> luma = std::vector<float>(W * H);
    void Set(int x, int y, float v) { luma[y * W + x] = v; }
};

// A HUD glyph: a bright fill with a dark 2 px outline, like God Hand's counter digits. (x0, y0) is the
// outline's top-left; the fill is (w-4) x (h-4).
void DrawGlyph(Frame& f, int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
        {
            const bool outline = x < x0 + 2 || x >= x0 + w - 2 || y < y0 + 2 || y >= y0 + h - 2;
            f.Set(x, y, outline ? 0.08f : 0.85f);
        }
}

struct Result
{
    std::vector<float> mask = std::vector<float>(W * H);
};

// Runs the rule over a sequence exactly as the shader does frame by frame: state from last frame in,
// state for next frame out, never read and written in the same pass.
template <typename MakeFrame> Result Run(int frames, const UiMaskParams& params, MakeFrame make)
{
    std::fill(g_prot.begin(), g_prot.end(), 0.0f);
    std::fill(g_streak.begin(), g_streak.end(), 0.0f);
    Result r;
    std::vector<float> prot(W * H), streak(W * H);
    for (int t = 0; t < frames; ++t)
    {
        const Frame f = make(t);
        g_cur = f.luma;
        const bool valid = t > 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                float p = 0.0f, s = 0.0f, m = 0.0f;
                UiMaskPixel(x, y, valid, params, p, s, m);
                prot[y * W + x] = p;
                streak[y * W + x] = s;
                r.mask[y * W + x] = m;
            }
        g_prot = prot;
        g_streak = streak;
        g_prev = g_cur;
    }
    return r;
}

int CountMasked(const Result& r, int x0, int y0, int x1, int y1, bool (*keep)(int, int) = nullptr)
{
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (r.mask[y * W + x] > 0.5f && (keep == nullptr || keep(x, y)))
                ++n;
    return n;
}

// A HUD glyph over a scene panning 3 px per frame: protected after the streak, and nothing else is.
void GlyphOverPan()
{
    constexpr int gx = 60, gy = 40, gw = 12, gh = 18;
    const auto make = [](int t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.Set(x, y, Scenery(x, y, 3 * t));
        DrawGlyph(f, gx, gy, gw, gh);
        return f;
    };

    const Result early = Run(6, Shipped(), make);
    assert(CountMasked(early, 0, 0, W, H) == 0 && "nothing may be protected before the streak");

    const Result r = Run(14, Shipped(), make);
    const int glyph = CountMasked(r, gx, gy, gx + gw, gy + gh);
    const int outside = CountMasked(r, 0, 0, W, H) - CountMasked(r, gx - 1, gy - 1, gx + gw + 1, gy + gh + 1);
    std::printf("glyph over pan: %d protected glyph pixels, %d outside\n", glyph, outside);
    assert(glyph >= 20 && "the glyph's inner edges must be protected");
    assert(outside == 0 && "nothing outside the glyph (plus its 1 px growth) may be protected");
}

// Still sand beside a dark coat that sways +-3 px every frame (an idle animation): the first slice put a
// rim of protection around the coat (the golden outline seen in PCSX2); the shipped rule protects nothing.
void SilhouetteOnStillScenery(const UiMaskParams& params, int* rim)
{
    constexpr int cx = 80, cw = 26, cy0 = 20, cy1 = 110;
    const auto make = [](int t)
    {
        Frame f;
        const int sway = (t % 4 < 2) ? 3 : -3;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const bool coat = y >= cy0 && y < cy1 && x >= cx + sway && x < cx + cw + sway;
                f.Set(x, y, coat ? 0.1f + 0.05f * static_cast<float>((x + y) & 1) : Scenery(x, y, 0));
            }
        return f;
    };

    const Result r = Run(40, params, make);
    // Everything that is sand in every frame: outside the coat's widest reach.
    *rim = CountMasked(r, 0, 0, W, H,
                       [](int x, int y) { return !(y >= cy0 && y < cy1 && x >= cx - 3 && x < cx + cw + 3); });
}

// Sand between two swaying legs under a swaying coat: moving on three sides. Still not interface.
void ConcaveGap()
{
    const auto make = [](int t)
    {
        Frame f;
        const int sway = (t % 4 < 2) ? 2 : -2;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const bool top = y >= 20 && y < 60 && x >= 60 + sway && x < 130 + sway;
                const bool legL = y >= 60 && y < 120 && x >= 60 + sway && x < 80 + sway;
                const bool legR = y >= 60 && y < 120 && x >= 110 + sway && x < 130 + sway;
                f.Set(x, y, (top || legL || legR) ? 0.1f : Scenery(x, y, 0));
            }
        return f;
    };

    const Result r = Run(40, Shipped(), make);
    const int gap = CountMasked(r, 84, 62, 106, 120);
    std::printf("concave gap: %d protected\n", gap);
    assert(gap == 0 && "sand between moving legs is not interface");
}

// A protected glyph that vanishes into the panning scene: every pixel whose own change exceeds dropTau
// loses its protection on that very frame. (A pixel that changed less keeps it, harmlessly: protection
// shows the current raw pixel, never an old one.)
void DropOnChange()
{
    constexpr int gx = 60, gy = 40, gw = 12, gh = 18;
    const auto make = [](int t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.Set(x, y, Scenery(x, y, 3 * t));
        if (t < 14)
            DrawGlyph(f, gx, gy, gw, gh);
        return f;
    };

    const Result r = Run(15, Shipped(), make);
    const Frame before = make(13);
    const Frame after = make(14);
    int changed = 0, kept = 0;
    for (int y = gy; y < gy + gh; ++y)
        for (int x = gx; x < gx + gw; ++x)
            if (abs(after.luma[y * W + x] - before.luma[y * W + x]) > Shipped().dropTau)
            {
                ++changed;
                kept += r.mask[y * W + x] > 0.0f ? 1 : 0;
            }
    std::printf("drop on change: %d glyph pixels changed past dropTau, %d still protected\n", changed, kept);
    assert(changed > 0 && kept == 0 && "a pixel that changed past dropTau is not protected");
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    GlyphOverPan();

    int rimFirst = 0, rimShipped = 0;
    SilhouetteOnStillScenery(FirstSlice(), &rimFirst);
    SilhouetteOnStillScenery(Shipped(), &rimShipped);
    std::printf("silhouette on still scenery: first slice %d protected sand pixels, shipped %d\n", rimFirst,
                rimShipped);
    assert(rimFirst > 0 && "the fixture must reproduce the first slice's rim, or it tests nothing");
    assert(rimShipped == 0 && "no still scenery beside a moving silhouette may be protected");

    ConcaveGap();
    DropOnChange();

    std::printf("nr-uimask-rule: PASS\n");
    return 0;
}
