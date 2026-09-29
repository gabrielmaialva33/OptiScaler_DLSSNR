// The UI layer's own mask (OptiScaler/shaders/synth_motion/precompile/static_overlay_layer.h), the decisions the
// detect shader's SYNTH_OVERLAY_LAYER permutation runs, over synthetic luma sequences on the CPU. The shader's
// structure is reproduced here and nothing else: one 8x8 tile per thread group, the group's tile tests on last
// frame's tile map (four sides, perpendicular orientations around, the camera), then the per-pixel state, then this
// frame's tile map, stored as the shader stores it (shares in 8 bits). Every decision is the header's.
//
// Design: dlssnr/design/synthesized-frame-generation.md, "The HUD layer's own mask: recall and a margin". The GPU
// harness (tests/synth-motion-d3d12) runs the shader itself on the hud_* sequences; these are the fixtures it has
// no room for: still scenes, a swaying silhouette, a straight edge along the motion, rain.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using std::abs;
using std::max;
using std::min;

static float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }

namespace
{
#ifndef LAYER_W
#define LAYER_W 256
#define LAYER_H 192
#endif
constexpr int W = LAYER_W;
constexpr int H = LAYER_H;
constexpr int TILE = 8;
constexpr int TW = W / TILE;
constexpr int TH = H / TILE;

// Last frame's tile map (counts, as the shader stores them) and grown distances, which the header's macros read.
std::vector<float> g_tileMoving(TW* TH), g_tileTextured(TW* TH), g_tilePixels(TW* TH, 64.0f);
std::vector<int> g_tileBits(TW* TH);
std::vector<float> g_distPrev(W* H);
bool g_distValid = false;

int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float TileMoving(int qx, int qy) { return g_tileMoving[ClampI(qy, 0, TH - 1) * TW + ClampI(qx, 0, TW - 1)]; }
float TileTextured(int qx, int qy) { return g_tileTextured[ClampI(qy, 0, TH - 1) * TW + ClampI(qx, 0, TW - 1)]; }
int TileBits(int qx, int qy) { return g_tileBits[ClampI(qy, 0, TH - 1) * TW + ClampI(qx, 0, TW - 1)]; }
float TilePixels(int qx, int qy) { return g_tilePixels[ClampI(qy, 0, TH - 1) * TW + ClampI(qx, 0, TW - 1)]; }
float DistPrev(int qx, int qy)
{
    if (!g_distValid || qx < 0 || qy < 0 || qx >= W || qy >= H)
        return 255.0f;
    return g_distPrev[qy * W + qx];
}
} // namespace

#define UL_FN inline
#define UL_OUT(T) T&
#define UL_UNROLL
#define UL_TILE_MOVED(qx, qy) TileMoving(qx, qy)
#define UL_TILE_TEXTURED(qx, qy) TileTextured(qx, qy)
#define UL_TILE_BITS(qx, qy) TileBits(qx, qy)
#define UL_TILE_PIXELS(qx, qy) TilePixels(qx, qy)
#define UL_DIST_PREV(qx, qy) DistPrev(qx, qy)
#include <shaders/synth_motion/precompile/static_overlay_layer.h>

namespace
{
// What the emulation may leave out, to prove a fixture reproduces the case a test is for.
struct Variant
{
    int sidesMin = 0;             // 0: the shipped UiLayerParams::sidesMin
    bool orientation = true;      // two perpendicular orientations around
    bool camera = true;           // the camera gate
    bool staticNeighbours = true; // orientation against steady neighbours only
};

struct Frame
{
    std::vector<float> luma = std::vector<float>(W * H);
    void Set(int x, int y, float v) { luma[y * W + x] = v; }
};

// The shader, frame by frame.
class LayerSim
{
  public:
    explicit LayerSim(Variant v = {}) : _v(v), _P(UiLayerShipped(H)) {}

    const std::vector<float>& Core() const { return _core; }

    void Step(const Frame& f)
    {
        const bool valid = _hasPrev;
        g_distValid = valid;
        g_distPrev = _dist;

        std::vector<float> still(W * H), anchor(W * H), hold(W * H), dist(W * H);
        std::vector<float> moving(TW * TH), textured(TW * TH);
        std::vector<int> bits(TW * TH);
        _core.assign(W * H, 0.0f);

        for (int ty = 0; ty < TH; ++ty)
        {
            for (int tx = 0; tx < TW; ++tx)
            {
                bool seedable = false;
                if (valid)
                {
                    int sides = 0;
                    const int dirs[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (const auto& d : dirs)
                        sides += UiLayerSideMoving(tx, ty, d[0], d[1], 1, 1, TW, TH, _P) ? 1 : 0;
                    int around = 0;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            around |= UiLayerTileBits(tx + dx, ty + dy, TW, TH);
                    float camTextured = 0.0f, camMoving = 0.0f;
                    for (int i = 0; i < UL_CAMERA_SAMPLES; ++i)
                    {
                        float t = 0.0f, m = 0.0f;
                        UiLayerCameraSample(i, TW, TH, _P, t, m);
                        camTextured += t;
                        camMoving += m;
                    }
                    seedable = sides >= (_v.sidesMin > 0 ? _v.sidesMin : _P.sidesMin) &&
                               (!_v.orientation || UiLayerPerpendicular(around)) &&
                               (!_v.camera || UiLayerCameraMoving(camMoving, camTextured, _P));
                }

                int nMoved = 0, nTextured = 0, orBits = 0;
                for (int y = ty * TILE; y < (ty + 1) * TILE; ++y)
                {
                    for (int x = tx * TILE; x < (tx + 1) * TILE; ++x)
                    {
                        auto L = [&](int qx, int qy)
                        { return f.luma[ClampI(qy, 0, H - 1) * W + ClampI(qx, 0, W - 1)]; };
                        const float c = L(x, y);
                        const float own = valid ? abs(c - _prevLuma[y * W + x]) : 0.0f;
                        const float l = L(x - 1, y), r = L(x + 1, y), u = L(x, y - 1), d = L(x, y + 1);
                        const float contrast = max(max(abs(c - l), abs(c - r)), max(abs(c - u), abs(c - d)));
                        const float prevStill = valid ? _still[y * W + x] : 0.0f;
                        const float prevHold = valid ? _hold[y * W + x] : 0.0f;
                        const float prevAnchor = valid ? _anchor[y * W + x] : 0.0f;

                        float s = 0.0f, a = 0.0f, h = 0.0f, g = 0.0f;
                        UiLayerPixel(x, y, valid, seedable, c, contrast, prevStill, prevHold, prevAnchor, _P, s, a, h,
                                     g);
                        still[y * W + x] = s;
                        anchor[y * W + x] = std::round(a * 255.0f) / 255.0f; // the state's 8-bit channel
                        hold[y * W + x] = h;
                        dist[y * W + x] = g;
                        _core[y * W + x] = UiLayerGrown(g, _P) ? 1.0f : 0.0f;

                        // The orientation of its static structure, with the neighbours that are steady too.
                        const int offsets[8][2] = { { -1, 0 },  { 1, 0 },  { 0, -1 }, { 0, 1 },
                                                    { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
                        float n[8];
                        int steadyMask = 0;
                        for (int k = 0; k < 8; ++k)
                        {
                            const int qx = x + offsets[k][0], qy = y + offsets[k][1];
                            const bool in = qx >= 0 && qy >= 0 && qx < W && qy < H;
                            n[k] = L(qx, qy);
                            const bool steady =
                                in && valid && UiLayerSteady(true, n[k], _still[qy * W + qx], _anchor[qy * W + qx], _P);
                            steadyMask |= (steady || !_v.staticNeighbours) ? (1 << k) : 0;
                        }
                        const int o = UiLayerOrientation(c, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], steadyMask,
                                                         _P.detailMin);
                        float m = 0.0f, t = 0.0f;
                        int b = 0;
                        UiLayerTileContribution(valid, own, contrast, s, o, _P, m, t, b);
                        nMoved += m > 0.5f ? 1 : 0;
                        nTextured += t > 0.5f ? 1 : 0;
                        orBits |= b;
                    }
                }
                // Counts, as the shader's RGBA8 texel stores them (exactly).
                moving[ty * TW + tx] = (float) nMoved;
                textured[ty * TW + tx] = (float) nTextured;
                bits[ty * TW + tx] = orBits;
            }
        }

        _still = still;
        _anchor = anchor;
        _hold = hold;
        _dist = dist;
        g_tileMoving = moving;
        g_tileTextured = textured;
        g_tileBits = bits;
        _prevLuma = f.luma;
        _hasPrev = true;
    }

  private:
    Variant _v;
    UiLayerParams _P;
    bool _hasPrev = false;
    std::vector<float> _prevLuma = std::vector<float>(W * H);
    std::vector<float> _still = std::vector<float>(W * H), _anchor = std::vector<float>(W * H),
                       _hold = std::vector<float>(W * H), _dist = std::vector<float>(W * H, 255.0f);
    std::vector<float> _core = std::vector<float>(W * H);
};

// Each new simulation starts from an empty tile map.
void ResetMaps()
{
    std::fill(g_tileMoving.begin(), g_tileMoving.end(), 0.0f);
    std::fill(g_tileTextured.begin(), g_tileTextured.end(), 0.0f);
    std::fill(g_tileBits.begin(), g_tileBits.end(), 0);
    g_distValid = false;
}

// Grainy scenery with strong local contrast, as tests/nr-uimask-rule uses. sx, sy: a scroll.
float Scenery(int x, int y, int sx, int sy = 0)
{
    uint32_t h = static_cast<uint32_t>(x + sx) * 73856093u ^ static_cast<uint32_t>(y + sy) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return 0.35f + 0.4f * static_cast<float>(h & 1023) / 1023.0f;
}

// Which fixture pixels are interface, for recall and precision.
using Truth = std::vector<uint8_t>;

// A glyph: a bright 3 px stroke frame of w x h with a 1 px dark outline around it.
void DrawGlyph(Frame& f, Truth* truth, int x0, int y0, int w, int h)
{
    for (int y = y0 - 1; y < y0 + h + 1; ++y)
        for (int x = x0 - 1; x < x0 + w + 1; ++x)
        {
            const bool inside = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
            const bool hole = x >= x0 + 3 && x < x0 + w - 3 && y >= y0 + 3 && y < y0 + h - 3;
            if (hole)
                continue;
            f.Set(x, y, inside ? 0.9f : 0.05f);
            if (truth)
                (*truth)[y * W + x] = 1;
        }
}

// A 1 px hollow frame of w x h, a light line with a dark drop shadow down-right: no pixel of it has a still 3x3.
void DrawThinFrame(Frame& f, Truth* truth, int x0, int y0, int w, int h)
{
    auto line = [&](int x, int y)
    {
        return x >= x0 && x < x0 + w && y >= y0 && y < y0 + h &&
               (x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + h - 1);
    };
    for (int y = y0; y <= y0 + h; ++y)
        for (int x = x0; x <= x0 + w; ++x)
        {
            if (line(x, y))
                f.Set(x, y, 0.92f);
            else if (line(x - 1, y - 1))
                f.Set(x, y, 0.04f);
            else
                continue;
            if (truth)
                (*truth)[y * W + x] = 1;
        }
}

struct Count
{
    int truth = 0, hit = 0, outside = 0;
};

// Core pixels on the truth, and core pixels further than 1 px from it.
Count Score(const std::vector<float>& core, const Truth& truth)
{
    Count c;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const bool t = truth[y * W + x] != 0;
            const bool m = core[y * W + x] > 0.5f;
            c.truth += t ? 1 : 0;
            c.hit += (t && m) ? 1 : 0;
            if (m && !t)
            {
                bool near = false;
                for (int dy = -1; dy <= 1 && !near; ++dy)
                    for (int dx = -1; dx <= 1 && !near; ++dx)
                    {
                        const int qx = x + dx, qy = y + dy;
                        near = qx >= 0 && qy >= 0 && qx < W && qy < H && truth[qy * W + qx] != 0;
                    }
                c.outside += near ? 0 : 1;
            }
        }
    return c;
}

int CoreTotal(const std::vector<float>& core)
{
    int n = 0;
    for (float v : core)
        n += v > 0.5f ? 1 : 0;
    return n;
}

// A glyph and a 1 px frame over a scene panning 3 px a frame: nothing before the still count, then every pixel of
// both, outlines and shadows included, and nothing of the scene.
void HudOverPan()
{
    ResetMaps();
    Truth truth(W * H, 0);
    const auto make = [&](int t, Truth* tr)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.Set(x, y, Scenery(x, y, 3 * t));
        DrawGlyph(f, tr, 60, 60, 12, 18);
        DrawThinFrame(f, tr, 140, 90, 48, 28);
        return f;
    };
    make(0, &truth);

    LayerSim sim;
    int firstCore = -1;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t, nullptr));
        if (firstCore < 0 && CoreTotal(sim.Core()) > 0)
            firstCore = t;
    }
    const Count c = Score(sim.Core(), truth);
    std::printf("layer, HUD over a pan: first core at t=%d, %d of %d interface pixels, %d scenery pixels\n", firstCore,
                c.hit, c.truth, c.outside);
    assert(firstCore >= (int) UiLayerShipped(H).stillFrames && "nothing may seed before the still count");
    assert(c.hit >= c.truth * 98 / 100 && "the glyph and the 1 px frame, outlines and shadows, must be in the core");
    assert(c.outside == 0 && "nothing of the panning scene may be in the core");
}

// A protected glyph that vanishes into the pan: its pixels leave the core on that very frame.
void DropOnChange()
{
    ResetMaps();
    const auto make = [](int t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.Set(x, y, Scenery(x, y, 3 * t));
        if (t < 30)
            DrawGlyph(f, nullptr, 60, 60, 12, 18);
        return f;
    };
    LayerSim sim;
    for (int t = 0; t < 30; ++t)
        sim.Step(make(t));
    const int before = CoreTotal(sim.Core());
    sim.Step(make(30));
    const int after = CoreTotal(sim.Core());
    std::printf("layer, drop on change: %d core pixels before the glyph vanished, %d after\n", before, after);
    assert(before > 0 && after == 0 && "a vanished glyph leaves the core at once");
}

// Still sand beside a dark coat swaying +-3 px, the camera still. Nothing; without the camera gate and with one
// moving side enough, the sand beside the coat is seeded, which is the case the fixture is for.
int SwayingCoat(Variant v)
{
    ResetMaps();
    constexpr int cx = 110, cw = 26, cy0 = 20, cy1 = 170;
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
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t));
        total += CoreTotal(sim.Core());
    }
    return total;
}

// Sand between two swaying legs under a swaying coat: motion on three sides, the camera still.
int ConcaveGap(Variant v)
{
    ResetMaps();
    const auto make = [](int t)
    {
        Frame f;
        const int sway = (t % 4 < 2) ? 2 : -2;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const bool top = y >= 20 && y < 60 && x >= 60 + sway && x < 190 + sway;
                const bool legL = y >= 60 && y < 180 && x >= 60 + sway && x < 80 + sway;
                const bool legR = y >= 60 && y < 180 && x >= 170 + sway && x < 190 + sway;
                f.Set(x, y, (top || legL || legR) ? 0.1f : Scenery(x, y, 0));
            }
        return f;
    };
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t));
        total += CoreTotal(sim.Core());
    }
    return total;
}

// A straight scenery edge running along the motion: a flat 2 px stripe, 150 px long, across a scene panning 3 px a
// frame, moving with it. Its middle stays still on screen (the aperture problem) while the scene around moves. With
// one orientation only it seeds nothing; without the orientation test it would.
int EdgeAlongMotion(Variant v)
{
    ResetMaps();
    const auto make = [](int t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const int u = x - 3 * t; // the scene's own coordinate: the stripe pans with it
                const bool stripe = y >= 95 && y < 97 && u >= -100 && u < 50;
                f.Set(x, y, stripe ? 0.95f : Scenery(x, y, -3 * t));
            }
        return f;
    };
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t));
        total += CoreTotal(sim.Core());
    }
    return total;
}

// A textured patch moving (4, 2) over a still scene: the camera is still. Nothing; with one side enough and no
// camera gate, the still scene beside the patch is seeded ("why not one side moving" in the design).
int MovingPatch(Variant v)
{
    ResetMaps();
    const auto make = [](int t)
    {
        Frame f;
        const int ox = 20 + 4 * t, oy = 40 + 2 * t;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const bool patch = x >= ox && x < ox + 64 && y >= oy && y < oy + 48;
                f.Set(x, y, patch ? Scenery(x - ox + 1000, y - oy + 500, 0) : Scenery(x, y, 0));
            }
        return f;
    };
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t));
        total += CoreTotal(sim.Core());
    }
    return total;
}

// Sparse rain over a still scene with a still, outlined object in it: streaks over about a tenth of the frame each
// frame. The camera is still, so nothing.
int SparseRain(Variant v)
{
    ResetMaps();
    const auto make = [](int t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                uint32_t h = static_cast<uint32_t>(x / 2) * 2654435761u ^
                             static_cast<uint32_t>((y + 7 * t) / 6) * 40503u ^ static_cast<uint32_t>(t) * 97u;
                h ^= h >> 15;
                const bool streak = (h % 10) == 0;
                f.Set(x, y, streak ? 0.98f : Scenery(x, y, 0));
            }
        DrawGlyph(f, nullptr, 120, 80, 16, 20);
        return f;
    };
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 40; ++t)
    {
        sim.Step(make(t));
        total += CoreTotal(sim.Core());
    }
    return total;
}

// tests/synth-motion-d3d12's texture (harness.cpp, Shade: value noise at 3, 9, 27 and 81 px over a brick grid), as
// the shader sees it: 8-bit colour, then luma. At 1 px a frame its mortar lines run along the pan and stay still on
// screen for many frames, the aperture case; the first GPU run seeded them (2026-09-29) through an orientation test
// that gave a straight edge both diagonals.
namespace harness
{
int FloorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
int FloorMod(int a, int b) { return a - FloorDiv(a, b) * b; }
uint32_t Hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t) x * 0x8da6b343u ^ (uint32_t) y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
float Rand01(int32_t x, int32_t y, uint32_t seed) { return (Hash(x, y, seed) & 0xFFFFFF) / 16777215.0f; }
float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }
float ValueNoise(int x, int y, int cell, uint32_t seed)
{
    const int gx = FloorDiv(x, cell), gy = FloorDiv(y, cell);
    const float fx = Smooth((x - gx * cell + 0.5f) / cell), fy = Smooth((y - gy * cell + 0.5f) / cell);
    const float a = Rand01(gx, gy, seed), b = Rand01(gx + 1, gy, seed);
    const float c = Rand01(gx, gy + 1, seed), d = Rand01(gx + 1, gy + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}
float Luma(int x, int y, float gain)
{
    const uint32_t seed = 0x1234u;
    const float n1 = ValueNoise(x, y, 3, seed), n2 = ValueNoise(x, y, 9, seed + 1);
    const float n3 = ValueNoise(x, y, 27, seed + 2), n4 = ValueNoise(x, y, 81, seed + 3);
    const int row = FloorDiv(y, 37);
    const bool mortar = FloorMod(y, 37) < 3 || FloorMod(x + (row & 1) * 19, 53) < 3;
    const float m = mortar ? 0.35f : 1.0f;
    const float rgb[3] = { (0.15f + 0.35f * n1 + 0.30f * n3 + 0.20f * n4) * m * gain,
                           (0.15f + 0.30f * n2 + 0.35f * n3 + 0.20f * (1.0f - n4)) * m * gain,
                           (0.10f + 0.40f * n1 * n2 + 0.30f * n4 + 0.20f * n3) * m * gain };
    float q[3];
    for (int i = 0; i < 3; ++i)
        q[i] = std::round(std::clamp(rgb[i] * 255.0f + 0.5f, 0.0f, 255.0f) - 0.5f) / 255.0f;
    return saturate(q[0] * 0.2126f + q[1] * 0.7152f + q[2] * 0.0722f);
}
} // namespace harness

// The harness's texture panning (dx, dy) a frame, at gain (the cut's darker content is 0.35), seen from (ox, oy) of
// it: nothing.
int HarnessPan(int dx, int dy, float gain, Variant v = {}, int ox = 0, int oy = 0)
{
    ResetMaps();
    LayerSim sim(v);
    int total = 0;
    for (int t = 0; t < 24; ++t)
    {
        Frame f;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                f.Set(x, y, harness::Luma(ox + x - t * dx, oy + y - t * dy, gain));
        sim.Step(f);
        total += CoreTotal(sim.Core());
    }
    return total;
}

void Units()
{
    // The band's weight: 1 up to margin / 2, then down to 0 at margin + 1.
    assert(UiLayerBandWeight(0, 0) == 1.0f && UiLayerBandWeight(1, 0) == 0.0f);
    for (int d = 0; d <= 8; ++d)
        assert(UiLayerBandWeight(d, 16) == 1.0f);
    assert(std::fabs(UiLayerBandWeight(9, 16) - 8.0f / 9.0f) < 1e-6f);
    assert(std::fabs(UiLayerBandWeight(16, 16) - 1.0f / 9.0f) < 1e-6f);
    assert(UiLayerBandWeight(17, 16) == 0.0f);
    for (int m = 0; m <= UL_MAX_MARGIN; ++m)
        for (int d = 0; d < UL_MAX_MARGIN + 2; ++d)
            assert(UiLayerBandWeight(d + 1, m) <= UiLayerBandWeight(d, m));

    // Orientation: a line differs across it and not along it; a 1 px line counts; flat is nothing.
    const float bg = 0.3f, fg = 0.9f;
    assert(UiLayerOrientation(fg, fg, fg, bg, bg, bg, bg, bg, bg, 255, 0.15f) == UL_HORIZONTAL);
    assert(UiLayerOrientation(fg, bg, bg, fg, fg, bg, bg, bg, bg, 255, 0.15f) == UL_VERTICAL);
    assert(UiLayerOrientation(fg, bg, bg, bg, bg, fg, bg, bg, fg, 255, 0.15f) == UL_DIAGONAL_DOWN);
    assert(UiLayerOrientation(fg, bg, bg, bg, bg, bg, fg, fg, bg, 255, 0.15f) == UL_DIAGONAL_UP);
    assert(UiLayerOrientation(bg, bg, bg, bg, bg, bg, bg, bg, bg, 255, 0.15f) == 0);
    // A 1 px light line with a shadow below and moving scene above: horizontal, from the shadow alone. Its end pixel,
    // with the scene beside it, has no direction to run along. Without a steady neighbour there is no structure.
    const int allButUp = 255 & ~(UL_N_UP | UL_N_UP_LEFT | UL_N_UP_RIGHT);
    assert(UiLayerOrientation(fg, fg, fg, bg, 0.05f, bg, bg, 0.05f, 0.05f, allButUp, 0.15f) == UL_HORIZONTAL);
    assert(UiLayerOrientation(fg, fg, bg, bg, 0.05f, bg, bg, 0.05f, 0.05f, allButUp & ~(UL_N_RIGHT | UL_N_DOWN_RIGHT),
                              0.15f) != UL_HORIZONTAL);
    assert(UiLayerOrientation(fg, bg, bg, bg, bg, bg, bg, bg, bg, 0, 0.15f) == 0);
    assert(UiLayerPerpendicular(UL_HORIZONTAL | UL_VERTICAL) &&
           UiLayerPerpendicular(UL_DIAGONAL_DOWN | UL_DIAGONAL_UP));
    assert(!UiLayerPerpendicular(UL_HORIZONTAL) && !UiLayerPerpendicular(UL_HORIZONTAL | UL_DIAGONAL_UP));

    // The side reach scales with the height: a sixth of it, 8 to 32 tiles.
    assert(UiLayerShipped(720).sideTiles == 15 && UiLayerShipped(1440).sideTiles == 30);
    assert(UiLayerShipped(192).sideTiles == 8 && UiLayerShipped(2160).sideTiles == 32);
    std::printf("layer, units: band weight, orientation, side reach\n");
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Units();
    HudOverPan();
    DropOnChange();

    Variant loose;
    loose.sidesMin = 1;
    loose.camera = false;
    Variant noOrientation;
    noOrientation.orientation = false;

    const int coat = SwayingCoat({}), coatLoose = SwayingCoat(loose);
    std::printf("layer, swaying coat on still sand: %d core pixels; one side and no camera gate: %d\n", coat,
                coatLoose);
    assert(coatLoose > 0 && "the fixture must seed the sand when one side is enough, or it tests nothing");
    assert(coat == 0 && "still sand beside a moving silhouette is not interface");

    const int gap = ConcaveGap({}), gapLoose = ConcaveGap(loose);
    std::printf("layer, concave gap: %d core pixels; one side and no camera gate: %d\n", gap, gapLoose);
    assert(gapLoose > 0 && gap == 0);

    const int edge = EdgeAlongMotion({}), edgeLoose = EdgeAlongMotion(noOrientation);
    std::printf("layer, straight edge along the motion: %d core pixels; without the orientation test: %d\n", edge,
                edgeLoose);
    assert(edgeLoose > 0 && "the fixture must seed the stripe without the orientation test, or it tests nothing");
    assert(edge == 0 && "a straight scenery edge along the motion is not interface");

    const int patch = MovingPatch({}), patchLoose = MovingPatch(loose);
    std::printf("layer, moving patch on a still scene: %d core pixels; one side and no camera gate: %d\n", patch,
                patchLoose);
    assert(patchLoose > 0 && patch == 0);

    const int rain = SparseRain({});
    std::printf("layer, sparse rain on a still scene: %d core pixels\n", rain);
    assert(rain == 0 && "a still scene under rain is not a moving camera");

    const int pans[][3] = { { 1, 0, 100 }, { 0, 1, 100 }, { 4, 0, 100 }, { 4, 0, 35 }, { 3, 2, 100 } };
    for (const auto& pan : pans)
    {
        const int held = HarnessPan(pan[0], pan[1], pan[2] / 100.0f);
        const int loose = HarnessPan(pan[0], pan[1], pan[2] / 100.0f, noOrientation);
        std::printf("layer, the GPU harness's texture panning (%d, %d) at gain %.2f: %d core pixels; without the "
                    "orientation test: %d\n",
                    pan[0], pan[1], pan[2] / 100.0f, held, loose);
        assert(held == 0 && "a panning scene has no interface in it");
    }

    // The darker content where the first GPU run seeded it: smooth brick staying within stillEps of its anchor while
    // a mortar joint slides up beside it. Nothing; with the orientation taken against moving neighbours too, seeds.
    Variant rawNeighbours;
    rawNeighbours.staticNeighbours = false;
    const int dark = HarnessPan(4, 0, 0.35f, {}, 1152, 288),
              darkRaw = HarnessPan(4, 0, 0.35f, rawNeighbours, 1152, 288);
    std::printf("layer, the harness's darker texture panning (4, 0), from (1152, 288): %d core pixels; orientation "
                "against moving neighbours too: %d\n",
                dark, darkRaw);
    assert(darkRaw > 0 && "the fixture must seed when a moving neighbour gives a steady pixel its orientation");
    assert(dark == 0 && "an edge between a steady pixel and a moving one is not static structure");

    std::printf("fg-synth-policy layer: PASS\n");
    return 0;
}
