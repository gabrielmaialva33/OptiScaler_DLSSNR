// Real-GPU endpoint-error test for SynthMotion::Estimator_Dx12 (OptiScaler/shaders/synth_motion), and
// with --source nvofa for SynthMotion::NvofaEstimator_Dx12, the NVIDIA Optical Flow source behind the
// same contract.
//
// Renders synthetic sequences whose true motion is known exactly, runs the production estimator on
// a real D3D12 device (vkd3d-proton under Wine), reads the motion field back and scores it. Nothing
// below the estimator's public interface is mocked. Scoring thresholds live in run.py; this file
// only measures and writes synth-motion-report.json.
//
// Convention under test (the interface's promise): Motion() holds, per colour pixel, the
// displacement from the current frame to the previous one, in colour pixels, +x right, +y down.
// Content moving by (+dx, +dy) per frame therefore reads (-dx, -dy).
//
// The NVIDIA source's field is one frame late by construction (SynthMotionNvofa_Dx12.h): the field
// read back at frame t describes the pair confirmed at t-1. Each frame is therefore scored against
// the ground truth of frame t - lag, which is the same for a steady pan and one frame behind for the
// moving object and around an abandoned recording.
#include "pch.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

#include "SynthMotion_Dx12.h"
#include "SynthMotionNvofa_Dx12.h"
#include "SynthOverlay_Dx12.h"

using Microsoft::WRL::ComPtr;

namespace
{
FILE* g_report = nullptr;

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// A failure ends the process without unwinding GPU-owned objects: completion is unknown at that
// point, and releasing under a possibly-running GPU is exactly what the production code forbids.
[[noreturn]] void Fail(const char* what, HRESULT hr = S_OK)
{
    Log("FAIL: %s (hr 0x%08X)", what, (unsigned) hr);
    std::fflush(stdout);
    ExitProcess(1);
}

// The source under test is not present on this machine (--source nvofa without nvofapi64.dll, or a
// build whose NVOFA shaders were never generated). Not a failure: the report says so and run.py
// reports SKIP. Same no-unwinding rule as Fail.
const char* g_reportPath = "synth-motion-report.json";
[[noreturn]] void Skip(const char* source, const char* why)
{
    if (g_report != nullptr)
        std::fclose(g_report);
    g_report = nullptr;
    std::string reason = why;
    for (char& c : reason)
        c = (c == '"' || c == '\\') ? '\'' : c;
    FILE* report = nullptr;
    if (fopen_s(&report, g_reportPath, "w") == 0 && report != nullptr)
    {
        std::fprintf(report, "{\"source\": \"%s\", \"skipped\": \"%s\", \"sequences\": []}\n", source, reason.c_str());
        std::fclose(report);
    }
    Log("SYNTH-MOTION SKIP source=%s: %s", source, reason.c_str());
    std::fflush(stdout);
    ExitProcess(0);
}

void Hr(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fail(what, hr);
}

// ---------------------------------------------------------------------------------------------
// Procedural content. Integer sampling of an infinite function, so an integer shift of the
// sampling origin is an exact translation of the image: ground truth needs no interpolation.
// ---------------------------------------------------------------------------------------------
inline int FloorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
inline int FloorMod(int a, int b) { return a - FloorDiv(a, b) * b; }

inline uint32_t Hash(int32_t x, int32_t y, uint32_t seed)
{
    uint32_t h = (uint32_t) x * 0x8da6b343u ^ (uint32_t) y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

inline float Rand01(int32_t x, int32_t y, uint32_t seed) { return (Hash(x, y, seed) & 0xFFFFFF) / 16777215.0f; }

inline float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float ValueNoise(int x, int y, int cell, uint32_t seed)
{
    const int gx = FloorDiv(x, cell);
    const int gy = FloorDiv(y, cell);
    const float fx = Smooth((x - gx * cell + 0.5f) / cell);
    const float fy = Smooth((y - gy * cell + 0.5f) / cell);
    const float a = Rand01(gx, gy, seed), b = Rand01(gx + 1, gy, seed);
    const float c = Rand01(gx, gy + 1, seed), d = Rand01(gx + 1, gy + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// Multi-scale noise over a brick grid: detail at 3, 9, 27 and 81 px plus hard mortar edges, which
// gives a block matcher something to lock onto at every pyramid level.
void Shade(uint32_t seed, int x, int y, float gain, uint8_t* rgba)
{
    const float n1 = ValueNoise(x, y, 3, seed);
    const float n2 = ValueNoise(x, y, 9, seed + 1);
    const float n3 = ValueNoise(x, y, 27, seed + 2);
    const float n4 = ValueNoise(x, y, 81, seed + 3);
    const int row = FloorDiv(y, 37);
    const bool mortar = FloorMod(y, 37) < 3 || FloorMod(x + (row & 1) * 19, 53) < 3;
    const float m = mortar ? 0.35f : 1.0f;
    const float r = (0.15f + 0.35f * n1 + 0.30f * n3 + 0.20f * n4) * m * gain;
    const float g = (0.15f + 0.30f * n2 + 0.35f * n3 + 0.20f * (1.0f - n4)) * m * gain;
    const float b = (0.10f + 0.40f * n1 * n2 + 0.30f * n4 + 0.20f * n3) * m * gain;
    rgba[0] = (uint8_t) std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f);
    rgba[1] = (uint8_t) std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f);
    rgba[2] = (uint8_t) std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f);
    rgba[3] = 255;
}

// ---------------------------------------------------------------------------------------------
// Sequences.
// ---------------------------------------------------------------------------------------------
enum class Kind
{
    Pan,
    Object,
    Static,
    Cut,
    Abandon,
    Overlay,
};

struct Sequence
{
    const char* name;
    Kind kind;
    UINT width, height;
    int dx, dy; // content motion per frame (+x right, +y down)
    int frames;
    bool timingOnly = false;
};

// The moving object for Kind::Object: a rectangle of a second pattern over a static background.
constexpr int kObjX = 400, kObjY = 260, kObjW = 256, kObjH = 192;
// The estimator produces no field for FFX's five warm-up frames after a reset, so the events below
// happen well after that. Kind::Cut changes content (and its brightness distribution) at this frame.
constexpr int kCutFrame = 8;
// Kind::Abandon records this frame and throws the list away instead of executing it.
constexpr int kAbandonFrame = 8;

// The static overlay for Kind::Overlay: what a first-person HUD draws over a moving world, and what
// smeared in Generation Zero under synthesized FG (dlssnr/design/synthesized-motion.md, "Static
// overlays"). Three elements, identical in every frame:
//  - a crosshair: four 2 px arms from 4 to 16 px off the centre and a 2x2 dot, each with a 1 px dark
//    outline;
//  - a HUD panel: opaque dark fill, a 2 px light border, two rows of glyphs at 2x;
//  - glyphs at 3x drawn straight over the scene with a 1 px dark outline, like an ammo counter.
// The glyphs are seven-segment-like: random subsets of eight strokes of a 5x7 cell, so they carry
// text's mix of horizontal and vertical strokes without being any font.
enum OverlayElement : uint8_t
{
    kElementNone,
    kElementCrosshair,
    kElementPanel,
    kElementText,
};

struct Rect
{
    int x0, y0, x1, y1; // half-open
    bool Contains(int x, int y, int grow = 0) const
    {
        return x >= x0 - grow && x < x1 + grow && y >= y0 - grow && y < y1 + grow;
    }
};

constexpr Rect kCrossStrokes[] = {
    { 624, 359, 636, 361 }, { 644, 359, 656, 361 }, // horizontal arms
    { 639, 344, 641, 356 }, { 639, 364, 641, 376 }, // vertical arms
    { 639, 359, 641, 361 },                         // centre dot
};
constexpr Rect kCrossBox { 623, 343, 657, 377 };
constexpr Rect kPanel { 48, 600, 256, 676 };
constexpr int kPanelGlyphX = 56, kPanelGlyphY = 612, kPanelGlyphs = 16, kPanelRows = 2;
constexpr int kTextX = 1000, kTextY = 48, kTextGlyphs = 8;
constexpr Rect kTextBox { 999, 47, 1000 + kTextGlyphs * 20 + 1, 48 + 21 + 1 };

bool GlyphBit(uint32_t glyph, int gx, int gy)
{
    uint32_t strokes = Hash((int32_t) glyph, 7, 0x61u) & 0xFFu;
    if ((strokes & (strokes - 1)) == 0) // fewer than two strokes: make it a readable shape
        strokes |= 0x41u;
    return ((strokes & 0x01u) && gx == 0 && gy <= 3) || ((strokes & 0x02u) && gx == 0 && gy >= 3) ||
           ((strokes & 0x04u) && gx == 4 && gy <= 3) || ((strokes & 0x08u) && gx == 4 && gy >= 3) ||
           ((strokes & 0x10u) && gx == 2) || ((strokes & 0x20u) && gy == 0) || ((strokes & 0x40u) && gy == 3) ||
           ((strokes & 0x80u) && gy == 6);
}

// One line of glyphs: 5x7 cells at `scale`, `advance` px apart, `rows` rows 20 px apart at scale 2.
bool TextBit(int x, int y, int ox, int oy, int glyphs, int rows, int scale, int advance, uint32_t seed)
{
    const int rowPitch = 10 * scale;
    if (x < ox || y < oy)
        return false;
    const int col = (x - ox) / advance, row = (y - oy) / rowPitch;
    const int gx = (x - ox - col * advance) / scale, gy = (y - oy - row * rowPitch) / scale;
    if (col >= glyphs || row >= rows || gx >= 5 || gy >= 7)
        return false;
    return GlyphBit(seed + (uint32_t) (row * glyphs + col), gx, gy);
}

bool FloatingTextBit(int x, int y) { return TextBit(x, y, kTextX, kTextY, kTextGlyphs, 1, 3, 20, 500); }

// Writes the overlay's colour and returns which element covers (x, y), or kElementNone.
OverlayElement OverlayPixel(int x, int y, uint8_t* rgba)
{
    auto paint = [rgba](uint8_t r, uint8_t g, uint8_t b)
    {
        rgba[0] = r;
        rgba[1] = g;
        rgba[2] = b;
        rgba[3] = 255;
    };

    if (kCrossBox.Contains(x, y))
    {
        for (const auto& r : kCrossStrokes)
        {
            if (r.Contains(x, y))
            {
                paint(240, 240, 240);
                return kElementCrosshair;
            }
        }
        for (const auto& r : kCrossStrokes)
        {
            if (r.Contains(x, y, 1))
            {
                paint(16, 16, 16);
                return kElementCrosshair;
            }
        }
    }

    if (kPanel.Contains(x, y))
    {
        if (!Rect { kPanel.x0 + 2, kPanel.y0 + 2, kPanel.x1 - 2, kPanel.y1 - 2 }.Contains(x, y))
            paint(200, 200, 200);
        else if (TextBit(x, y, kPanelGlyphX, kPanelGlyphY, kPanelGlyphs, kPanelRows, 2, 12, 100))
            paint(235, 215, 110);
        else
            paint(22, 24, 30);
        return kElementPanel;
    }

    if (kTextBox.Contains(x, y))
    {
        if (FloatingTextBit(x, y))
        {
            paint(250, 250, 250);
            return kElementText;
        }
        for (int oy = -1; oy <= 1; ++oy)
        {
            for (int ox = -1; ox <= 1; ++ox)
            {
                if (FloatingTextBit(x + ox, y + oy))
                {
                    paint(10, 10, 10);
                    return kElementText;
                }
            }
        }
    }
    return kElementNone;
}

// The colour of one pixel of sequence s at frame t.
void ScenePixel(const Sequence& s, int t, int ix, int iy, uint8_t* p)
{
    switch (s.kind)
    {
    case Kind::Pan:
    case Kind::Abandon:
        Shade(0x1234u, ix - t * s.dx, iy - t * s.dy, 1.0f, p);
        break;
    case Kind::Static:
        Shade(0x1234u, ix, iy, 1.0f, p);
        break;
    case Kind::Cut:
        if (t < kCutFrame)
            Shade(0x1234u, ix - t * s.dx, iy - t * s.dy, 1.0f, p);
        else
            Shade(0xBEEF5u, ix - t * s.dx, iy - t * s.dy, 0.35f, p); // new content, much darker
        break;
    case Kind::Object:
    {
        const int ox = kObjX + t * s.dx, oy = kObjY + t * s.dy;
        if (ix >= ox && ix < ox + kObjW && iy >= oy && iy < oy + kObjH)
            Shade(0x77777u, ix - t * s.dx, iy - t * s.dy, 1.0f, p);
        else
            Shade(0x1234u, ix, iy, 1.0f, p);
        break;
    }
    case Kind::Overlay:
        if (OverlayPixel(ix, iy, p) == kElementNone)
            Shade(0x1234u, ix - t * s.dx, iy - t * s.dy, 1.0f, p);
        break;
    }
}

void RenderFrame(const Sequence& s, int t, uint8_t* dst, UINT rowPitch)
{
    for (UINT y = 0; y < s.height; ++y)
    {
        uint8_t* row = dst + (size_t) y * rowPitch;
        for (UINT x = 0; x < s.width; ++x)
            ScenePixel(s, t, (int) x, (int) y, row + x * 4);
    }
}

// ---------------------------------------------------------------------------------------------
// Scoring.
// ---------------------------------------------------------------------------------------------
float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0)
            bits = sign;
        else
        {
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0)
            {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    }
    else if (exp == 31)
        bits = sign | 0x7F800000u | (mant << 13);
    else
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

struct Field
{
    UINT width = 0, height = 0;
    std::vector<float> x, y; // per motion texel
};

struct Region
{
    int x0, y0, x1, y1; // colour pixels, half-open
    float gx, gy;       // ground truth, current -> previous
    const char* name;
};

struct Score
{
    const char* region;
    float gx, gy;
    size_t pixels = 0;
    double epeMean = 0.0;
    double within1 = 0.0;
    double overHalf = 0.0; // fraction with EPE > 0.5 px; against zero truth, the fraction that moves
    float medianX = 0.0f, medianY = 0.0f;
};

float Median(std::vector<float>& v)
{
    if (v.empty())
        return 0.0f;
    auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Scores the colour pixels `in` accepts against the truth (gx, gy). Vectors are read at the motion
// texel covering each colour pixel and taken to be in colour pixels, as the interface promises; a
// field at another extent is scored the same way and reported.
template <typename In>
Score ScorePixels(const Field& f, UINT colourW, UINT colourH, const char* name, float gx, float gy, In in)
{
    Score s { name, gx, gy };
    std::vector<float> xs, ys;
    double epeSum = 0.0;
    size_t good = 0, over = 0;
    for (int y = 0; y < (int) colourH; ++y)
    {
        for (int x = 0; x < (int) colourW; ++x)
        {
            if (!in(x, y))
                continue;
            const UINT mx = (UINT) ((uint64_t) x * f.width / colourW);
            const UINT my = (UINT) ((uint64_t) y * f.height / colourH);
            const float vx = f.x[(size_t) my * f.width + mx];
            const float vy = f.y[(size_t) my * f.width + mx];
            const double e = std::sqrt((double) (vx - gx) * (vx - gx) + (double) (vy - gy) * (vy - gy));
            epeSum += e;
            good += e < 1.0 ? 1 : 0;
            over += e > 0.5 ? 1 : 0;
            xs.push_back(vx);
            ys.push_back(vy);
        }
    }
    s.pixels = xs.size();
    if (s.pixels > 0)
    {
        s.epeMean = epeSum / (double) s.pixels;
        s.within1 = (double) good / (double) s.pixels;
        s.overHalf = (double) over / (double) s.pixels;
        s.medianX = Median(xs);
        s.medianY = Median(ys);
    }
    return s;
}

// Scores one region, excluding colour pixels inside any of the `exclude` rectangles.
Score ScoreRegion(const Field& f, UINT colourW, UINT colourH, const Region& r, const std::vector<Region>& exclude)
{
    return ScorePixels(f, colourW, colourH, r.name, r.gx, r.gy,
                       [&](int x, int y)
                       {
                           if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1)
                               return false;
                           for (const auto& e : exclude)
                           {
                               if (x >= e.x0 && x < e.x1 && y >= e.y0 && y < e.y1)
                                   return false;
                           }
                           return true;
                       });
}

// Kind::Overlay. Each colour pixel falls in one class:
//  - An overlay element, truth zero. It is split by whether the pair can show the pixel is static at
//    all. It cannot when the pixel's 3x3 in frame t is reproduced exactly by frame t-1 displaced by
//    the camera's motion: a flat fill, or a line along the motion. The block vector explains such a
//    pixel as well as zero does, for this estimator and for any other that sees only the pair, and
//    displacing it reads the same overlay. Those are "ambiguous"; the rest, "clear", is what is judged.
//  - The background away from the overlay, truth the pan: outside the border band, and outside a band
//    of kOverlayBand px around each element's box.
//  - That band, reported apart. Block vectors blended across an element's edge land there, and so does
//    the background uncovered from behind it; neither has a vector the pair can confirm.
constexpr int kOverlayBand = 24;

bool OverlayAmbiguous(const Sequence& s, int t, int x, int y)
{
    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            // prev = cur + mv, and the camera's mv is (-dx, -dy).
            const int cx = x + ox, cy = y + oy, px = cx - s.dx, py = cy - s.dy;
            if (cx < 0 || cy < 0 || px < 0 || py < 0 || cx >= (int) s.width || cy >= (int) s.height ||
                px >= (int) s.width || py >= (int) s.height)
                return false;
            uint8_t current[4], previous[4];
            ScenePixel(s, t, cx, cy, current);
            ScenePixel(s, t - 1, px, py, previous);
            if (std::memcmp(current, previous, 3) != 0)
                return false;
        }
    }
    return true;
}

std::vector<Score> ScoreOverlayFrame(const Sequence& s, int t, const Field& f)
{
    enum : uint8_t
    {
        kIgnore,
        kBackground,
        kBand,
        kOverlayClear,
        kOverlayAmbiguous,
    };
    const int W = (int) s.width, H = (int) s.height;
    const int m = 32 + 2 * std::max(std::abs(s.dx), std::abs(s.dy));
    const Rect boxes[] = { kCrossBox, kPanel, kTextBox };
    std::vector<uint8_t> cls((size_t) W * H, kIgnore);
    std::vector<uint8_t> element((size_t) W * H, kElementNone);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const size_t i = (size_t) y * W + x;
            uint8_t rgba[4];
            element[i] = OverlayPixel(x, y, rgba);
            if (element[i] != kElementNone)
            {
                cls[i] = OverlayAmbiguous(s, t, x, y) ? kOverlayAmbiguous : kOverlayClear;
                continue;
            }
            if (x < m || y < m || x >= W - m || y >= H - m)
                continue;
            bool band = false;
            for (const auto& box : boxes)
                band |= box.Contains(x, y, kOverlayBand);
            cls[i] = band ? kBand : kBackground;
        }
    }

    auto at = [W](int x, int y) { return (size_t) y * W + x; };
    auto clearIn = [&](uint8_t which)
    { return [&, which](int x, int y) { return cls[at(x, y)] == kOverlayClear && element[at(x, y)] == which; }; };
    const float gx = (float) -s.dx, gy = (float) -s.dy;
    // Smeared: an overlay pixel the field moves by more than half a pixel AND whose own displacement
    // reads another colour than the overlay's. That is what an interpolator displacing it along its vector
    // drags. A stroke or a fill displaced along itself by a vector blended across the element's edge reads
    // the same overlay and is not counted (2026-09-28: most of the clear pixels that still carried a
    // vector after the per-pixel choice were exactly that).
    std::vector<uint8_t> smeared((size_t) W * H, 0);
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const float vx = f.x[at(x, y)], vy = f.y[at(x, y)];
            if (element[at(x, y)] == kElementNone || std::sqrt(vx * vx + vy * vy) <= 0.5f)
                continue;
            uint8_t current[4], previous[4];
            ScenePixel(s, t, x, y, current);
            ScenePixel(s, t - 1, x + (int) std::lround(vx), y + (int) std::lround(vy), previous);
            smeared[at(x, y)] = std::memcmp(current, previous, 3) != 0 ? 1 : 0;
        }
    }
    auto elementIn = [&](uint8_t which, bool onlySmeared)
    {
        return [&, which, onlySmeared](int x, int y)
        { return element[at(x, y)] == which && (!onlySmeared || smeared[at(x, y)] != 0); };
    };
    return {
        ScorePixels(f, s.width, s.height, "overlay", 0.0f, 0.0f,
                    [&](int x, int y) { return element[at(x, y)] != kElementNone; }),
        ScorePixels(f, s.width, s.height, "overlay_smeared", 0.0f, 0.0f,
                    [&](int x, int y) { return smeared[at(x, y)] != 0; }),
        ScorePixels(f, s.width, s.height, "crosshair", 0.0f, 0.0f, elementIn(kElementCrosshair, false)),
        ScorePixels(f, s.width, s.height, "crosshair_smeared", 0.0f, 0.0f, elementIn(kElementCrosshair, true)),
        ScorePixels(f, s.width, s.height, "panel", 0.0f, 0.0f, elementIn(kElementPanel, false)),
        ScorePixels(f, s.width, s.height, "panel_smeared", 0.0f, 0.0f, elementIn(kElementPanel, true)),
        ScorePixels(f, s.width, s.height, "text", 0.0f, 0.0f, elementIn(kElementText, false)),
        ScorePixels(f, s.width, s.height, "text_smeared", 0.0f, 0.0f, elementIn(kElementText, true)),
        ScorePixels(f, s.width, s.height, "overlay_clear", 0.0f, 0.0f,
                    [&](int x, int y) { return cls[at(x, y)] == kOverlayClear; }),
        ScorePixels(f, s.width, s.height, "crosshair_clear", 0.0f, 0.0f, clearIn(kElementCrosshair)),
        ScorePixels(f, s.width, s.height, "panel_clear", 0.0f, 0.0f, clearIn(kElementPanel)),
        ScorePixels(f, s.width, s.height, "text_clear", 0.0f, 0.0f, clearIn(kElementText)),
        ScorePixels(f, s.width, s.height, "background", gx, gy,
                    [&](int x, int y) { return cls[at(x, y)] == kBackground; }),
        ScorePixels(f, s.width, s.height, "band", gx, gy, [&](int x, int y) { return cls[at(x, y)] == kBand; }),
    };
}

std::vector<Score> ScoreFrame(const Sequence& s, int t, const Field& f)
{
    std::vector<Score> scores;
    const int W = (int) s.width, H = (int) s.height;
    switch (s.kind)
    {
    case Kind::Pan:
    case Kind::Cut:
    {
        // Content enters at the border each frame, and blocks straddling it have no true match.
        const int m = 32 + 2 * std::max(std::abs(s.dx), std::abs(s.dy));
        if (s.kind == Kind::Cut && t == kCutFrame)
        {
            // No true motion across a cut. The interface: the field is zero on the GPU in the frame
            // the cut is detected, because FFX's own search writes zero vectors then.
            scores.push_back(ScoreRegion(f, s.width, s.height, { m, m, W - m, H - m, 0.0f, 0.0f, "cutzero" }, {}));
            break;
        }
        scores.push_back(ScoreRegion(f, s.width, s.height, { m, m, W - m, H - m, (float) -s.dx, (float) -s.dy, "interior" }, {}));
        break;
    }
    case Kind::Abandon:
    {
        // The frame after the abandoned one is measured against the last EXECUTED frame, two steps
        // back: an abandoned recording must not advance the estimator's history.
        const int steps = (t == kAbandonFrame + 1) ? 2 : 1;
        const int m = 32 + 2 * steps * std::max(std::abs(s.dx), std::abs(s.dy));
        scores.push_back(ScoreRegion(f, s.width, s.height,
                                     { m, m, W - m, H - m, (float) (-s.dx * steps), (float) (-s.dy * steps), "interior" }, {}));
        break;
    }
    case Kind::Static:
        scores.push_back(ScoreRegion(f, s.width, s.height, { 32, 32, W - 32, H - 32, 0.0f, 0.0f, "interior" }, {}));
        break;
    case Kind::Object:
    {
        const int ox = kObjX + t * s.dx, oy = kObjY + t * s.dy;
        const int px = kObjX + (t - 1) * s.dx, py = kObjY + (t - 1) * s.dy;
        const int inset = 24, band = 48;
        scores.push_back(ScoreRegion(f, s.width, s.height,
                                     { ox + inset, oy + inset, ox + kObjW - inset, oy + kObjH - inset, (float) -s.dx,
                                       (float) -s.dy, "object" },
                                     {}));
        const std::vector<Region> around = {
            { ox - band, oy - band, ox + kObjW + band, oy + kObjH + band, 0, 0, "" },
            { px - band, py - band, px + kObjW + band, py + kObjH + band, 0, 0, "" },
        };
        scores.push_back(ScoreRegion(f, s.width, s.height, { 32, 32, W - 32, H - 32, 0.0f, 0.0f, "background" }, around));
        break;
    }
    case Kind::Overlay:
        scores = ScoreOverlayFrame(s, t, f);
        break;
    }
    return scores;
}

// ---------------------------------------------------------------------------------------------
// Synthesized FG's HUD mask (SynthMotion::Overlay_Dx12), run on the same frames as the estimator. It is
// judged against the overlay the harness drew: a mask pixel is its depth (1.0 near, 0.0 far), as FSR-FG is
// handed it.
//  - A marked pixel is right when it is an overlay pixel, and "grown" when it is within 1 px of one: the
//    rule grows its protection by 1 px over glyph edges on purpose. Anything further out is scenery marked.
//  - Recall is per element, over all its pixels. The rule marks interface only where the scene moves on
//    every side of it within 24 px and the pixel has contrast (hud-protection.md): a large flat panel is
//    not marked, and neither is an element's 1 px dark outline, whose 3x3 core reaches the scene. Reported
//    per element; run.py judges floors on the long hud_* sequences.
//  - The depth must be exactly 0 or 1 and agree with the mask; the layer must carry the frame's own rgb
//    and the mask as alpha, byte for byte (an RGBA8 frame gives an RGBA8 layer).
// ---------------------------------------------------------------------------------------------
struct HudScore
{
    size_t marked = 0;         // depth 1
    size_t markedOverlay = 0;  // of those, overlay pixels
    size_t markedGrown = 0;    // of those, within 1 px of an overlay pixel (overlay pixels included)
    size_t overlayPixels[4] {};  // per OverlayElement
    size_t overlayMarked[4] {};  // per OverlayElement
    size_t depthNotBinary = 0;   // neither 0 nor 1
    size_t depthMaskMismatch = 0;
    size_t layerMismatch = 0;
    bool layerChecked = false;
};

HudScore ScoreHud(const Sequence& s, const std::vector<float>& depth, const std::vector<uint8_t>& mask,
                  const std::vector<uint8_t>& layer, const uint8_t* colour, UINT colourPitch)
{
    HudScore h {};
    const int W = (int) s.width, H = (int) s.height;
    std::vector<uint8_t> element((size_t) W * H, kElementNone);

    if (s.kind == Kind::Overlay)
    {
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                uint8_t rgba[4];
                element[(size_t) y * W + x] = OverlayPixel(x, y, rgba);
            }
        }
    }

    auto isOverlay = [&](int x, int y)
    { return x >= 0 && y >= 0 && x < W && y < H && element[(size_t) y * W + x] != kElementNone; };

    h.layerChecked = !layer.empty();
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const size_t i = (size_t) y * W + x;
            const float d = depth[i];
            const uint8_t m = mask[i];
            const uint8_t e = element[i];

            if (d != 0.0f && d != 1.0f)
                ++h.depthNotBinary;
            // The shader tests the float mask against 0.5; the stored byte rounds it, so 127 and 128 are both
            // consistent with either answer.
            if ((d == 1.0f && m < 127) || (d == 0.0f && m > 128))
                ++h.depthMaskMismatch;

            if (e != kElementNone)
            {
                ++h.overlayPixels[e];
                if (d == 1.0f)
                    ++h.overlayMarked[e];
            }

            if (d == 1.0f)
            {
                ++h.marked;
                if (e != kElementNone)
                    ++h.markedOverlay;

                bool grown = false;
                for (int oy = -1; oy <= 1 && !grown; ++oy)
                {
                    for (int ox = -1; ox <= 1 && !grown; ++ox)
                        grown = isOverlay(x + ox, y + oy);
                }
                if (grown)
                    ++h.markedGrown;
            }

            if (h.layerChecked)
            {
                const uint8_t* want = colour + (size_t) y * colourPitch + (size_t) x * 4;
                const uint8_t* got = layer.data() + i * 4;
                if (got[0] != want[0] || got[1] != want[1] || got[2] != want[2] || got[3] != m)
                    ++h.layerMismatch;
            }
        }
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// Device plumbing.
// ---------------------------------------------------------------------------------------------
struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestampReadback;
    uint64_t frequency = 0;

    void Init()
    {
        Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "create hardware D3D12 device");
        D3D12_COMMAND_QUEUE_DESC q {};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "create direct queue");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
           "create command list");
        Hr(list->Close(), "close initial list");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
            Fail("create fence event");

        // 0-1 around the estimator, 2-3 around the HUD mask and its layer.
        D3D12_QUERY_HEAP_DESC qh {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = 4;
        Hr(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&timestamps)), "create timestamp heap");
        timestampReadback = Buffer(32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        Hr(queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
           "create buffer");
        return r;
    }

    void Begin()
    {
        Hr(allocator->Reset(), "reset allocator");
        Hr(list->Reset(allocator.Get(), nullptr), "reset list");
    }

    // Execute and wait for completion; the harness never lets two frames overlap on the GPU.
    void ExecuteAndWait()
    {
        Hr(list->Close(), "close list");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Hr(queue->Signal(fence.Get(), ++fenceValue), "signal fence");
        if (fence->GetCompletedValue() < fenceValue)
        {
            Hr(fence->SetEventOnCompletion(fenceValue, event), "fence event");
            if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
                Fail("GPU did not complete within 10 s");
        }
        const HRESULT removed = device->GetDeviceRemovedReason();
        if (FAILED(removed))
            Fail("device removed", removed);
    }

    // A recorded list that is deliberately never executed (the abandon case).
    void CloseWithoutExecuting() { Hr(list->Close(), "close abandoned list"); }
};

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    list->ResourceBarrier(1, &b);
}

// ---------------------------------------------------------------------------------------------
// Report writing (a small JSON writer; no dependency).
// ---------------------------------------------------------------------------------------------
struct FrameResult
{
    int t;
    bool recorded;
    bool abandoned;
    bool ready;
    bool sceneCutAfterRecord;
    bool sceneCutAfterConfirm;
    double gpuMs;
    UINT fieldW, fieldH;
    int fieldFormat;
    std::vector<Score> scores;
    double hudGpuMs;
    bool hudScored;
    HudScore hud;
};

void WriteSequence(const Sequence& s, const std::vector<FrameResult>& frames, bool first)
{
    std::fprintf(g_report, "%s\n  {\"name\": \"%s\", \"width\": %u, \"height\": %u, \"dx\": %d, \"dy\": %d, \"timing_only\": %s, \"frames\": [",
                 first ? "" : ",", s.name, s.width, s.height, s.dx, s.dy, s.timingOnly ? "true" : "false");
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        std::fprintf(g_report,
                     "%s\n    {\"t\": %d, \"recorded\": %s, \"abandoned\": %s, \"ready\": %s, \"scene_cut_after_record\": %s, "
                     "\"scene_cut_after_confirm\": %s, \"gpu_ms\": %.6f, \"field_width\": %u, \"field_height\": %u, "
                     "\"field_format\": %d, \"scores\": [",
                     i ? "," : "", f.t, f.recorded ? "true" : "false", f.abandoned ? "true" : "false",
                     f.ready ? "true" : "false", f.sceneCutAfterRecord ? "true" : "false",
                     f.sceneCutAfterConfirm ? "true" : "false", f.gpuMs, f.fieldW, f.fieldH, f.fieldFormat);
        for (size_t j = 0; j < f.scores.size(); ++j)
        {
            const auto& sc = f.scores[j];
            std::fprintf(g_report,
                         "%s{\"region\": \"%s\", \"gt_x\": %.3f, \"gt_y\": %.3f, \"pixels\": %zu, \"epe_mean\": %.6f, "
                         "\"within_1px\": %.6f, \"over_half_px\": %.6f, \"median_x\": %.4f, \"median_y\": %.4f}",
                         j ? ", " : "", sc.region, sc.gx, sc.gy, sc.pixels, sc.epeMean, sc.within1, sc.overHalf, sc.medianX,
                         sc.medianY);
        }
        std::fprintf(g_report, "], \"hud_gpu_ms\": %.6f", f.hudGpuMs);
        if (f.hudScored)
        {
            const auto& h = f.hud;
            std::fprintf(g_report,
                         ", \"hud\": {\"marked\": %zu, \"marked_overlay\": %zu, \"marked_grown\": %zu, "
                         "\"overlay_pixels\": {\"crosshair\": %zu, \"panel\": %zu, \"text\": %zu}, "
                         "\"overlay_marked\": {\"crosshair\": %zu, \"panel\": %zu, \"text\": %zu}, "
                         "\"depth_not_binary\": %zu, \"depth_mask_mismatch\": %zu, \"layer_checked\": %s, "
                         "\"layer_mismatch\": %zu}",
                         h.marked, h.markedOverlay, h.markedGrown, h.overlayPixels[kElementCrosshair],
                         h.overlayPixels[kElementPanel], h.overlayPixels[kElementText], h.overlayMarked[kElementCrosshair],
                         h.overlayMarked[kElementPanel], h.overlayMarked[kElementText], h.depthNotBinary,
                         h.depthMaskMismatch, h.layerChecked ? "true" : "false", h.layerMismatch);
        }
        std::fprintf(g_report, "}");
    }
    std::fprintf(g_report, "\n  ]}");
    std::fflush(g_report);
}

// ---------------------------------------------------------------------------------------------
// The source under test, behind the calls the production guide (DlssNr::SynthMotionGuide) makes.
// ---------------------------------------------------------------------------------------------
struct Estimator
{
    SynthMotion::Estimator_Dx12* ffx = nullptr;
    SynthMotion::NvofaEstimator_Dx12* nvofa = nullptr;
    ID3D12CommandQueue* queue = nullptr; // the queue the harness executes on; the engine is fenced to it

    const char* Name() const { return nvofa != nullptr ? "nvofa" : "ffx"; }
    int Lag() const { return nvofa != nullptr ? (int) SynthMotion::NvofaEstimator_Dx12::LagFrames : 0; }
    bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* colour,
                D3D12_RESOURCE_STATES state, bool reset)
    {
        if (nvofa == nullptr)
            return ffx->Record(device, list, colour, state, reset);
        if (nvofa->Record(device, list, colour, state, reset, queue))
            return true;
        if (!nvofa->Available())
            Skip("nvofa", nvofa->UnavailableReason());
        return false;
    }
    bool Ready() const { return nvofa != nullptr ? nvofa->Ready() : ffx->Ready(); }
    bool SceneCut() const { return nvofa != nullptr ? nvofa->SceneCut() : ffx->SceneCut(); }
    ID3D12Resource* Motion() const { return nvofa != nullptr ? nvofa->Motion() : ffx->Motion(); }
    void ConfirmExecuted() { nvofa != nullptr ? nvofa->ConfirmExecuted() : ffx->ConfirmExecuted(); }
    void AbandonRecording() { nvofa != nullptr ? nvofa->AbandonRecording() : ffx->AbandonRecording(); }
    void Release() { nvofa != nullptr ? nvofa->Release() : ffx->Release(); }
};

// ---------------------------------------------------------------------------------------------
// One sequence end to end.
// ---------------------------------------------------------------------------------------------
// One texture read back after the frame: placed-footprint copy into a readback buffer sized for it.
struct TextureReadback
{
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    D3D12_RESOURCE_DESC desc {};
    UINT64 bytes = 0;

    // Records the copy of `texture`, which rests in NON_PIXEL_SHADER_RESOURCE.
    void Record(Gpu& gpu, ID3D12GraphicsCommandList* list, ID3D12Resource* texture)
    {
        const auto d = texture->GetDesc();
        if (buffer == nullptr || d.Width != desc.Width || d.Height != desc.Height || d.Format != desc.Format)
        {
            desc = d;
            gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            buffer = gpu.Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        Transition(list, texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = texture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Rows packed tightly, bytesPerPixel each.
    std::vector<uint8_t> Read(UINT bytesPerPixel)
    {
        std::vector<uint8_t> out((size_t) desc.Width * desc.Height * bytesPerPixel);
        uint8_t* data = nullptr;
        const D3D12_RANGE range { 0, (SIZE_T) bytes };
        Hr(buffer->Map(0, &range, reinterpret_cast<void**>(&data)), "map texture readback");
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(out.data() + (size_t) y * desc.Width * bytesPerPixel,
                        data + footprint.Offset + (size_t) y * footprint.Footprint.RowPitch,
                        (size_t) desc.Width * bytesPerPixel);
        const D3D12_RANGE none { 0, 0 };
        buffer->Unmap(0, &none);
        return out;
    }
};

std::vector<FrameResult> RunSequence(Gpu& gpu, Estimator& estimator, SynthMotion::Overlay_Dx12& overlay,
                                     const Sequence& s)
{
    TextureReadback depthReadback, maskReadback, layerReadback;

    // The colour source: RGBA8 UNORM, what a present-time host hands the estimator after a
    // backbuffer copy. Rewritten from an upload buffer every frame.
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC cd {};
    cd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cd.Width = s.width;
    cd.Height = s.height;
    cd.DepthOrArraySize = 1;
    cd.MipLevels = 1;
    cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> colour;
    Hr(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&colour)),
       "create colour texture");
    D3D12_RESOURCE_STATES colourState = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT up {};
    UINT64 upBytes = 0;
    gpu.device->GetCopyableFootprints(&cd, 0, 1, 0, &up, nullptr, nullptr, &upBytes);
    auto upload = gpu.Buffer(upBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* uploadPtr = nullptr;
    Hr(upload->Map(0, nullptr, reinterpret_cast<void**>(&uploadPtr)), "map upload");

    ComPtr<ID3D12Resource> readback;
    UINT64 readbackBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT rb {};
    D3D12_RESOURCE_DESC fieldDesc {};

    std::vector<FrameResult> results;
    for (int t = 0; t < s.frames; ++t)
    {
        RenderFrame(s, t, uploadPtr + up.Offset, up.Footprint.RowPitch);
        const D3D12_RESOURCE_STATES colourStateBeforeFrame = colourState;

        gpu.Begin();
        auto* list = gpu.list.Get();
        Transition(list, colour.Get(), colourState, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = colour.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = up;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(list, colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        FrameResult fr {};
        fr.t = t;
        list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        fr.recorded = estimator.Record(gpu.device.Get(), list, colour.Get(), colourState, t == 0);
        list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        // Synthesized FG's HUD mask on the same frame and list, and its layer from the same frame (the harness
        // has no separate presented frame), in the order the D3D11 bridge records them.
        list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        if (!overlay.Record(gpu.device.Get(), list, colour.Get(), colourState, t == 0))
            Fail("HUD mask Record returned false");
        const bool hudPending = overlay.Pending();
        const bool layerRecorded = hudPending && overlay.RecordLayer(list, colour.Get(), colourState);
        if (hudPending && !layerRecorded)
            Fail("HUD layer RecordLayer returned false");
        list->EndQuery(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);

        list->ResolveQueryData(gpu.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, gpu.timestampReadback.Get(), 0);
        if (!fr.recorded)
            Fail("Record returned false");

        if (hudPending && !s.timingOnly)
        {
            depthReadback.Record(gpu, list, overlay.Depth());
            maskReadback.Record(gpu, list, overlay.Mask());
            layerReadback.Record(gpu, list, overlay.Layer());
        }
        fr.ready = estimator.Ready();
        fr.sceneCutAfterRecord = estimator.SceneCut();

        ID3D12Resource* motion = estimator.Motion();
        if (motion != nullptr)
        {
            const auto desc = motion->GetDesc();
            if (readback == nullptr || desc.Width != fieldDesc.Width || desc.Height != fieldDesc.Height ||
                desc.Format != fieldDesc.Format)
            {
                fieldDesc = desc;
                gpu.device->GetCopyableFootprints(&fieldDesc, 0, 1, 0, &rb, nullptr, nullptr, &readbackBytes);
                readback = gpu.Buffer(readbackBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            Transition(list, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION ms {};
            ms.pResource = motion;
            ms.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION md {};
            md.pResource = readback.Get();
            md.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            md.PlacedFootprint = rb;
            list->CopyTextureRegion(&md, 0, 0, 0, &ms, nullptr);
            Transition(list, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        if (s.kind == Kind::Abandon && t == kAbandonFrame)
        {
            // Recorded, closed, never executed: the estimator must not count this frame.
            gpu.CloseWithoutExecuting();
            estimator.AbandonRecording();
            overlay.AbandonRecording();
            fr.abandoned = true;
            results.push_back(fr);
            // The colour texture's transitions were recorded but never ran.
            colourState = colourStateBeforeFrame;
            continue;
        }

        gpu.ExecuteAndWait();
        estimator.ConfirmExecuted();
        overlay.ConfirmExecuted();
        fr.sceneCutAfterConfirm = estimator.SceneCut();

        uint64_t* ts = nullptr;
        const D3D12_RANGE tsRange { 0, 32 };
        Hr(gpu.timestampReadback->Map(0, &tsRange, reinterpret_cast<void**>(&ts)), "map timestamps");
        fr.gpuMs = ts[1] > ts[0] ? (double) (ts[1] - ts[0]) * 1000.0 / (double) gpu.frequency : 0.0;
        fr.hudGpuMs = ts[3] > ts[2] ? (double) (ts[3] - ts[2]) * 1000.0 / (double) gpu.frequency : 0.0;
        const D3D12_RANGE none { 0, 0 };
        gpu.timestampReadback->Unmap(0, &none);

        if (hudPending && !s.timingOnly)
        {
            const auto depthBytes = depthReadback.Read(4);
            std::vector<float> depth(depthBytes.size() / 4);
            std::memcpy(depth.data(), depthBytes.data(), depthBytes.size());
            if (layerReadback.desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
                Fail("an RGBA8 frame did not give an RGBA8 UI layer");
            fr.hud = ScoreHud(s, depth, maskReadback.Read(1), layerReadback.Read(4), uploadPtr + up.Offset,
                              up.Footprint.RowPitch);
            fr.hudScored = true;
        }

        if (motion != nullptr && !s.timingOnly)
        {
            Field field;
            field.width = (UINT) fieldDesc.Width;
            field.height = fieldDesc.Height;
            field.x.resize((size_t) field.width * field.height);
            field.y.resize(field.x.size());
            uint8_t* data = nullptr;
            const D3D12_RANGE range { 0, (SIZE_T) readbackBytes };
            Hr(readback->Map(0, &range, reinterpret_cast<void**>(&data)), "map motion readback");
            for (UINT y = 0; y < field.height; ++y)
            {
                const uint8_t* row = data + rb.Offset + (size_t) y * rb.Footprint.RowPitch;
                for (UINT x = 0; x < field.width; ++x)
                {
                    float vx = 0.0f, vy = 0.0f;
                    if (fieldDesc.Format == DXGI_FORMAT_R16G16_FLOAT)
                    {
                        uint16_t h[2];
                        std::memcpy(h, row + x * 4, 4);
                        vx = HalfToFloat(h[0]);
                        vy = HalfToFloat(h[1]);
                    }
                    else if (fieldDesc.Format == DXGI_FORMAT_R32G32_FLOAT)
                    {
                        float v[2];
                        std::memcpy(v, row + x * 8, 8);
                        vx = v[0];
                        vy = v[1];
                    }
                    else
                        Fail("motion field is neither R16G16_FLOAT nor R32G32_FLOAT");
                    field.x[(size_t) y * field.width + x] = vx;
                    field.y[(size_t) y * field.width + x] = vy;
                }
            }
            readback->Unmap(0, &none);
            fr.fieldW = field.width;
            fr.fieldH = field.height;
            fr.fieldFormat = (int) fieldDesc.Format;
            // Scored against the frame the field describes. A source with no scene-cut detection
            // (nvofa) is not scored on the one pair that spans the cut: there is no true motion there.
            const int described = t - estimator.Lag();
            const bool spansCut = s.kind == Kind::Cut && described == kCutFrame && estimator.Lag() > 0;
            const bool hudOnly = std::strncmp(s.name, "hud_", 4) == 0; // the HUD mask's sequences
            if (described > 0 && !spansCut && !hudOnly)
                fr.scores = ScoreFrame(s, described, field);
        }

        std::string line;
        for (const auto& sc : fr.scores)
        {
            char buf[240];
            std::snprintf(buf, sizeof(buf), " %s: epe %.3f <1px %.1f%% >0.5px %.1f%% median (%.2f, %.2f) gt (%.0f, %.0f)",
                          sc.region, sc.epeMean, sc.within1 * 100.0, sc.overHalf * 100.0, sc.medianX, sc.medianY, sc.gx,
                          sc.gy);
            line += buf;
        }
        if (fr.hudScored)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf), " hud: marked %zu (overlay %zu, within 1 px %zu) gpu %.3f ms", fr.hud.marked,
                          fr.hud.markedOverlay, fr.hud.markedGrown, fr.hudGpuMs);
            line += buf;
        }
        Log("%-12s t=%d ready=%d cut=%d/%d gpu=%.3f ms%s", s.name, t, fr.ready ? 1 : 0, fr.sceneCutAfterRecord ? 1 : 0,
            fr.sceneCutAfterConfirm ? 1 : 0, fr.gpuMs, line.c_str());
        results.push_back(fr);
    }

    upload->Unmap(0, nullptr);
    // Everything above was waited on; the estimator's and the mask's resources are idle and may be released.
    estimator.Release();
    overlay.Release();
    return results;
}
} // namespace

int main(int argc, char** argv)
{
    bool nvofa = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            g_reportPath = argv[++i];
        else if (std::strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            nvofa = std::strcmp(argv[++i], "nvofa") == 0;
    }

    Gpu gpu;
    gpu.Init();
    Log("device up, timestamp frequency %llu Hz", (unsigned long long) gpu.frequency);

    const Sequence sequences[] = {
        { "pan_+1x", Kind::Pan, 1280, 720, 1, 0, 14 },
        { "pan_+1y", Kind::Pan, 1280, 720, 0, 1, 14 },
        { "pan_+4-4", Kind::Pan, 1280, 720, 4, -4, 14 },
        { "pan_-16x", Kind::Pan, 1280, 720, -16, 0, 14 },
        { "pan_-16y", Kind::Pan, 1280, 720, 0, -16, 14 },
        { "pan_+16+16", Kind::Pan, 1280, 720, 16, 16, 14 },
        { "pan_+48x", Kind::Pan, 1280, 720, 48, 0, 14 },
        { "pan_+48y", Kind::Pan, 1280, 720, 0, 48, 14 },
        { "overlay_+8x", Kind::Overlay, 1280, 720, 8, 0, 14 },
        { "overlay_+8+8", Kind::Overlay, 1280, 720, 8, 8, 14 },
        // The same overlay, long enough for the HUD mask's steady state (its entry streak is 8 frames, and the
        // protection builds up as the scene moves past each element). The estimator is not scored on these.
        { "hud_+8x", Kind::Overlay, 1280, 720, 8, 0, 40 },
        { "hud_+3+2", Kind::Overlay, 1280, 720, 3, 2, 40 },
        { "object", Kind::Object, 1280, 720, 8, 3, 14 },
        { "static", Kind::Static, 1280, 720, 0, 0, 14 },
        { "cut", Kind::Cut, 1280, 720, 4, 0, 18 },
        { "abandon", Kind::Abandon, 1280, 720, 4, 0, 14 },
        { "time_1080p", Kind::Pan, 1920, 1080, 4, 0, 24, true },
        { "time_3440", Kind::Pan, 3440, 1440, 4, 0, 24, true },
    };

    SynthMotion::Estimator_Dx12 ffx;
    SynthMotion::NvofaEstimator_Dx12 engine;
    SynthMotion::Overlay_Dx12 overlay;
    Estimator estimator;
    estimator.ffx = &ffx;
    estimator.nvofa = nvofa ? &engine : nullptr;
    estimator.queue = gpu.queue.Get();

    if (fopen_s(&g_report, g_reportPath, "w") != 0 || g_report == nullptr)
        Fail("open report");
    std::fprintf(g_report, "{\"source\": \"%s\", \"lag_frames\": %d, \"timestamp_frequency\": %llu, \"sequences\": [",
                 estimator.Name(), estimator.Lag(), (unsigned long long) gpu.frequency);

    int frames = 0;
    bool first = true;
    for (const auto& s : sequences)
    {
        const auto results = RunSequence(gpu, estimator, overlay, s);
        frames += (int) results.size();
        WriteSequence(s, results, first);
        first = false;
    }
    std::fprintf(g_report, "\n]}\n");
    std::fclose(g_report);
    g_report = nullptr;

    Log("SYNTH-MOTION DONE source=%s sequences=%d frames=%d", estimator.Name(),
        (int) (sizeof(sequences) / sizeof(sequences[0])), frames);
    return 0;
}
