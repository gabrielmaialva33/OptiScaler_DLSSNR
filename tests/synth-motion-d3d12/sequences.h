// The synthesized-motion loopbacks' content and scoring, shared by this suite (harness.cpp, D3D12) and
// tests/synth-motion-vk (harness.cpp, Vulkan): the procedural frames, the sequences and the estimator's scores, so
// both estimators are held to the same pixels against the same truth. The HUD mask's own scoring stays in this
// suite's harness.cpp.
//
// Included inside each harness's anonymous namespace, after <Windows.h>, <algorithm>, <cmath>, <cstring>, <cstdint>
// and <vector>; it includes nothing itself.
#pragma once

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
    Hud, // Overlay, with the 1 px frame and the translucent panel besides (the HUD mask's own sequences)
};

bool HasOverlay(Kind kind) { return kind == Kind::Overlay || kind == Kind::Hud; }

struct Sequence
{
    const char* name;
    Kind kind;
    UINT width, height;
    int dx, dy; // content motion per frame (+x right, +y down)
    int frames;
    bool timingOnly = false;
    // The HUD mask with the UI layer's own mask and the layer ([FrameGen] SynthesizedHudLayer), or the mask alone
    // (the default, SynthesizedHudDepth only). A timing sequence runs the first half alone and the second with it.
    bool hudLayer = true;
};

bool SequenceLayer(const Sequence& s, int t) { return s.timingOnly ? t >= s.frames / 2 : s.hudLayer; }

// The moving object for Kind::Object: a rectangle of a second pattern over a static background.
constexpr int kObjX = 400, kObjY = 260, kObjW = 256, kObjH = 192;
// The estimator produces no field for FFX's five warm-up frames after a reset, so the events below
// happen well after that. Kind::Cut changes content (and its brightness distribution) at this frame.
constexpr int kCutFrame = 8;
// Kind::Abandon records this frame and throws the list away instead of executing it.
constexpr int kAbandonFrame = 8;

// The static overlay for Kind::Overlay: what a first-person HUD draws over a moving world, and what
// smeared in Generation Zero under synthesized FG (docs/dlssnr/design/synthesized-motion.md, "Static
// overlays"). Three elements, identical in every frame:
//  - a crosshair: four 2 px arms from 4 to 16 px off the centre and a 2x2 dot, each with a 1 px dark
//    outline;
//  - a HUD panel: opaque dark fill, a 2 px light border, two rows of glyphs at 2x;
//  - glyphs at 3x drawn straight over the scene with a 1 px dark outline, like an ammo counter.
// The glyphs are seven-segment-like: random subsets of eight strokes of a 5x7 cell, so they carry
// text's mix of horizontal and vertical strokes without being any font.
// Kind::Hud adds two elements the strict mask cannot mark at all (synthesized-frame-generation.md, "The HUD
// layer's own mask"):
//  - a 1 px frame: a hollow rectangle, a 1 px light line with a 1 px dark drop shadow down-right, like a HUD
//    bracket or a selection box. No pixel of it has a still 3x3;
//  - a glass panel: translucent (the scene at 45% shows through), with an opaque 1 px light border and two rows
//    of outlined glyphs at 2x. Its fill changes with the scene behind it, so it is never still.
// Every overlay pixel is also one part: stroke (the element's light or opaque marks), outline (1 px outlines
// and drop shadows) or fill (the panel's flat interior, the glass's translucent one).
enum OverlayElement : uint8_t
{
    kElementNone,
    kElementCrosshair,
    kElementPanel,
    kElementText,
    kElementFrame,
    kElementGlass,
    kElementCount,
};

enum OverlayPart : uint8_t
{
    kPartStroke,
    kPartOutline,
    kPartFill,
    kPartCount,
};

const char* const kElementNames[kElementCount] = { "none", "crosshair", "panel", "text", "frame", "glass" };
const char* const kPartNames[kPartCount] = { "stroke", "outline", "fill" };

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

constexpr Rect kFrame { 360, 120, 424, 160 };
constexpr Rect kGlass { 880, 540, 1120, 640 };
constexpr int kGlassGlyphX = 892, kGlassGlyphY = 562, kGlassGlyphs = 16;

bool FrameLine(int x, int y)
{
    return kFrame.Contains(x, y) && !Rect { kFrame.x0 + 1, kFrame.y0 + 1, kFrame.x1 - 1, kFrame.y1 - 1 }.Contains(x, y);
}

bool GlassTextBit(int x, int y) { return TextBit(x, y, kGlassGlyphX, kGlassGlyphY, kGlassGlyphs, 2, 2, 13, 900); }

// Which element covers (x, y), and which part of it, or kElementNone. With rgba, also paints it: rgba holds the
// scene's colour on entry, which the glass's fill darkens rather than replaces. extras: Kind::Hud's frame and glass.
OverlayElement OverlayAt(int x, int y, bool extras, uint8_t* rgba, uint8_t* part)
{
    auto paint = [rgba](uint8_t r, uint8_t g, uint8_t b)
    {
        if (rgba != nullptr)
        {
            rgba[0] = r;
            rgba[1] = g;
            rgba[2] = b;
            rgba[3] = 255;
        }
    };
    auto hit = [part](OverlayElement e, OverlayPart which)
    {
        if (part != nullptr)
            *part = which;
        return e;
    };

    if (kCrossBox.Contains(x, y))
    {
        for (const auto& r : kCrossStrokes)
        {
            if (r.Contains(x, y))
            {
                paint(240, 240, 240);
                return hit(kElementCrosshair, kPartStroke);
            }
        }
        for (const auto& r : kCrossStrokes)
        {
            if (r.Contains(x, y, 1))
            {
                paint(16, 16, 16);
                return hit(kElementCrosshair, kPartOutline);
            }
        }
    }

    if (kPanel.Contains(x, y))
    {
        if (!Rect { kPanel.x0 + 2, kPanel.y0 + 2, kPanel.x1 - 2, kPanel.y1 - 2 }.Contains(x, y))
        {
            paint(200, 200, 200);
            return hit(kElementPanel, kPartStroke);
        }
        if (TextBit(x, y, kPanelGlyphX, kPanelGlyphY, kPanelGlyphs, kPanelRows, 2, 12, 100))
        {
            paint(235, 215, 110);
            return hit(kElementPanel, kPartStroke);
        }
        paint(22, 24, 30);
        return hit(kElementPanel, kPartFill);
    }

    if (kTextBox.Contains(x, y))
    {
        if (FloatingTextBit(x, y))
        {
            paint(250, 250, 250);
            return hit(kElementText, kPartStroke);
        }
        for (int oy = -1; oy <= 1; ++oy)
        {
            for (int ox = -1; ox <= 1; ++ox)
            {
                if (FloatingTextBit(x + ox, y + oy))
                {
                    paint(10, 10, 10);
                    return hit(kElementText, kPartOutline);
                }
            }
        }
    }

    if (!extras)
        return kElementNone;

    if (FrameLine(x, y))
    {
        paint(230, 230, 230);
        return hit(kElementFrame, kPartStroke);
    }
    if (FrameLine(x - 1, y - 1))
    {
        paint(12, 12, 12);
        return hit(kElementFrame, kPartOutline);
    }

    if (kGlass.Contains(x, y))
    {
        if (!Rect { kGlass.x0 + 1, kGlass.y0 + 1, kGlass.x1 - 1, kGlass.y1 - 1 }.Contains(x, y))
        {
            paint(210, 210, 210);
            return hit(kElementGlass, kPartStroke);
        }
        if (GlassTextBit(x, y))
        {
            paint(255, 255, 255);
            return hit(kElementGlass, kPartStroke);
        }
        for (int oy = -1; oy <= 1; ++oy)
        {
            for (int ox = -1; ox <= 1; ++ox)
            {
                if (GlassTextBit(x + ox, y + oy))
                {
                    paint(0, 0, 0);
                    return hit(kElementGlass, kPartOutline);
                }
            }
        }
        if (rgba != nullptr)
        {
            for (int c = 0; c < 3; ++c)
                rgba[c] = (uint8_t) ((rgba[c] * 115 + 127) / 255); // the scene at 45%
        }
        return hit(kElementGlass, kPartFill);
    }
    return kElementNone;
}

// The overlay of the estimator's own sequences (Kind::Overlay), which keep the three elements they were measured on.
OverlayElement OverlayPixel(int x, int y, uint8_t* rgba) { return OverlayAt(x, y, false, rgba, nullptr); }

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
    case Kind::Hud:
        // The scene first: the glass shows it through.
        Shade(0x1234u, ix - t * s.dx, iy - t * s.dy, 1.0f, p);
        OverlayAt(ix, iy, true, p, nullptr);
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
    case Kind::Hud: // never scored: the hud_* sequences are the HUD mask's
        scores = ScoreOverlayFrame(s, t, f);
        break;
    }
    return scores;
}
