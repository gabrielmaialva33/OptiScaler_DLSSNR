// Model cadence's per-pixel rules (retoque/shaders/dlssnr/precompile/dlssnr_cadence_rule.h), the exact code
// the shader runs, over synthetic frame sequences. The shader's texture loads become array lookups with the same
// clamping; the five dispatches become loops in the order DlssNr_Cadence_Dx12 records them. Everything else is
// the production header.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <dlssnr/DlssNr_Cadence.h>

using std::abs;
using std::cos;
using std::exp2;
using std::floor;
using std::isfinite;
using std::max;
using std::min;
using std::pow;
using std::sin;

struct float2
{
    float x = 0.0f, y = 0.0f;
    float2() = default;
    float2(float a, float b) : x(a), y(b) {}
};
struct float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float3() = default;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float2 operator+(float2 a, float2 b) { return { a.x + b.x, a.y + b.y }; }
inline float2 operator-(float2 a, float2 b) { return { a.x - b.x, a.y - b.y }; }
inline float2 operator*(float2 a, float s) { return { a.x * s, a.y * s }; }
inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator*(float s, float3 a) { return a * s; }
inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float frac(float v) { return v - std::floor(v); }
inline float smoothstep(float a, float b, float v)
{
    const float t = saturate((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

namespace
{
template <class T> struct Image
{
    int w = 0, h = 0;
    std::vector<T> px;
    void Resize(int width, int height, T value = T()) { w = width, h = height, px.assign(size_t(w) * h, value); }
    T At(int x, int y) const { return px[size_t(clamp(y, 0, h - 1)) * w + clamp(x, 0, w - 1)]; }
    T& Ref(int x, int y) { return px[size_t(y) * w + x]; }
};

// What the shader's t-registers hold.
Image<float> g_depthNow, g_depthThen, g_ui;
Image<float3> g_proxyNow, g_proxyThen, g_residual, g_low, g_fade;
Image<float2> g_motion, g_chain;
} // namespace

#define CR_FN inline
#define CR_UNROLL
#define CR_LOOP
#define CR_DEPTH_NOW(ax_, ay_) g_depthNow.At(ax_, ay_)
#define CR_DEPTH_THEN(ax_, ay_) g_depthThen.At(ax_, ay_)
#define CR_PROXY_NOW(ax_, ay_) g_proxyNow.At(ax_, ay_)
#define CR_PROXY_THEN(ax_, ay_) g_proxyThen.At(ax_, ay_)
#define CR_MOTION(ax_, ay_) g_motion.At(ax_, ay_)
#define CR_CHAIN_PREV(ax_, ay_) g_chain.At(ax_, ay_)
#define CR_RESIDUAL(ax_, ay_) g_residual.At(ax_, ay_)
#define CR_RESIDUAL_LOW(ax_, ay_) g_low.At(ax_, ay_)
#define CR_UI(ax_, ay_) g_ui.At(ax_, ay_)
#define CR_FADE(ax_, ay_) g_fade.At(ax_, ay_)
#include <shaders/dlssnr/precompile/dlssnr_cadence_rule.h>

namespace
{
constexpr int W = 96;
constexpr int H = 64;
int g_cases = 0;

// The shipped numbers (DlssNr::Cadence), the working size equal to the frame's.
CadenceParams Shipped()
{
    CadenceParams P {};
    P.width = W;
    P.height = H;
    P.lowWidth = int(DlssNr::Cadence::LowExtent(W));
    P.lowHeight = int(DlssNr::Cadence::LowExtent(H));
    P.lowBlock = int(DlssNr::Cadence::kLowBlock);
    P.haveUi = 0;
    P.depthTol = DlssNr::Cadence::kDepthTolerance;
    P.colourTol = DlssNr::Cadence::kColourTolerance;
    P.scale = DlssNr::Cadence::TapScale(W, W);
    P.depthStepX = 1.0f;
    P.depthStepY = 1.0f;
    return P;
}

// The dispatches of DlssNr_Cadence_Dx12, in its order, over the globals above.
struct Machine
{
    CadenceParams P = Shipped();

    // A model frame: the edit, the snapshots, then the coarse edit from the stored edit.
    void Record(const Image<float3>& answer)
    {
        g_residual.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_residual.Ref(x, y) = CadenceResidual(answer.At(x, y), g_proxyNow.At(x, y));
        g_proxyThen = g_proxyNow;
        g_depthThen = g_depthNow;
        g_low.Resize(P.lowWidth, P.lowHeight);
        for (int y = 0; y < P.lowHeight; ++y)
            for (int x = 0; x < P.lowWidth; ++x)
                g_low.Ref(x, y) = CadenceCoarse(x, y, P);
    }

    // A carried frame's first dispatch: one link of the chain, into the other buffer, then swapped in.
    void Step(bool chainStart)
    {
        Image<float2> next;
        next.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                next.Ref(x, y) = CadenceChainStep(x, y, chainStart, P);
        g_chain = next;
    }

    // Its second: the answer, reading the chain just made.
    Image<float3> Synthesize() const
    {
        Image<float3> out;
        out.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                out.Ref(x, y) = CadenceSynthesize(x, y, g_chain.At(x, y), P);
        return out;
    }

    // The model frame after carried ones: the last link, then the rule for the model.
    Image<float2> ModelMotion() const
    {
        Image<float2> out;
        out.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                out.Ref(x, y) = CadenceModelMotion(x, y, CadenceChainStep(x, y, false, P), P);
        return out;
    }

    // A smoothed model frame (mode 5), after ModelMotion and before Record: the same last link, the fade stored, the
    // shown answer returned.
    Image<float3> Fade(const Image<float3>& answer, float weight)
    {
        Image<float3> shown, fade;
        shown.Resize(W, H);
        fade.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float3 f = CadenceFade(x, y, CadenceChainStep(x, y, false, P), answer.At(x, y), P);
                fade.Ref(x, y) = f;
                shown.Ref(x, y) = CadenceFadeShown(answer.At(x, y), f, weight);
            }
        g_fade = fade;
        return shown;
    }

    // A carried frame of the walk (mode 3 with a weight).
    Image<float3> SynthesizeFaded(float weight) const
    {
        Image<float3> out;
        out.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                out.Ref(x, y) = CadenceSynthesizeFaded(x, y, g_chain.At(x, y), weight, P);
        return out;
    }
};

// A smooth texture on the infinite plane, so content that enters the picture is new content.
float3 Texture(float x, float y)
{
    return { 0.45f + 0.20f * std::sin(0.31f * x + 0.17f * y) + 0.12f * std::sin(0.07f * x - 0.23f * y + 1.3f),
             0.40f + 0.18f * std::sin(0.21f * x - 0.13f * y + 0.5f) + 0.10f * std::cos(0.11f * x + 0.29f * y),
             0.35f + 0.15f * std::cos(0.19f * x + 0.27f * y + 2.0f) + 0.10f * std::sin(0.05f * x + 0.09f * y) };
}

const float3 kRed { 0.80f, 0.10f, 0.10f };

// Only the occluder's own colour: the texture never reaches it (its red stays under 0.78), so the stand-in model is
// smooth across the background and a sub-pixel pan measures interpolation, not a step in the model.
bool Reddish(float3 c) { return c.x > 0.79f && c.y < 0.11f && c.z < 0.11f; }

// A stand-in model: a light sharpening plus a small lift, and a strong lift on the red occluder, so an edit that
// lands on the wrong surface is unmistakable. Translation-invariant, which is what makes a carried pan exact.
Image<float3> Model(const Image<float3>& proxy)
{
    Image<float3> answer;
    answer.Resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float3 c = proxy.At(x, y);
            const float3 mean =
                (proxy.At(x - 1, y) + proxy.At(x + 1, y) + proxy.At(x, y - 1) + proxy.At(x, y + 1)) * 0.25f;
            const float lift = Reddish(c) ? 0.15f : 0.02f;
            answer.Ref(x, y) = c + (c - mean) * 0.5f + float3(lift, lift, lift);
        }
    return answer;
}

// The world at frame k: a textured plane moving by v per frame at depth bgDepth.
void Pan(float vx, float vy, int k, float bgDepth = 0.5f)
{
    g_proxyNow.Resize(W, H);
    g_depthNow.Resize(W, H, bgDepth);
    g_motion.Resize(W, H, float2(-vx, -vy)); // this frame -> the last: where the content was
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            g_proxyNow.Ref(x, y) = Texture(x + 0.5f - k * vx, y + 0.5f - k * vy);
}

float MaxAbs(float3 a, float3 b) { return max(max(abs(a.x - b.x), abs(a.y - b.y)), abs(a.z - b.z)); }

float DistanceToEdge(float x, float y) { return min(min(x, W - x), min(y, H - y)); }

bool Finite01(float3 c)
{
    return isfinite(c.x) && isfinite(c.y) && isfinite(c.z) && c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x <= 1.0f &&
           c.y <= 1.0f && c.z <= 1.0f;
}

void Rules_DepthMismatch()
{
    assert(CadenceDepthMismatch(0.5f, 0.5f) == 0.0f);
    // A wall at 2 m against 6 m in a conventional 0..1 buffer (near 0.1, far 1000): 3.4 % apart as plain values,
    // under any usable tolerance, and 67 % apart in 1 - d, which is where the distance ratio lives.
    auto conventional = [](float z) { return (1000.0f / (1000.0f - 0.1f)) * (1.0f - 0.1f / z); };
    const float a = conventional(2.0f), b = conventional(6.0f);
    assert(abs(a - b) / max(a, b) < DlssNr::Cadence::kDepthTolerance);
    assert(CadenceDepthMismatch(a, b) > 2.0f * DlssNr::Cadence::kDepthTolerance);
    // The same pair in a reversed or linear buffer is told apart by the plain ratio.
    assert(CadenceDepthMismatch(0.1f / 2.0f, 0.1f / 6.0f) > 2.0f * DlssNr::Cadence::kDepthTolerance);
    assert(CadenceDepthMismatch(std::numeric_limits<float>::quiet_NaN(), 0.5f) >= 1e9f);
    ++g_cases;
}

void Rules_ColourGate()
{
    const float tol = DlssNr::Cadence::kColourTolerance;
    const float3 c { 0.40f, 0.30f, 0.20f };
    assert(CadenceColourGate(c, c, tol) == 1.0f);
    // A lighting change keeps the hue: 10 % brighter passes whole, 30 % brighter passes in part.
    assert(CadenceColourGate(c * 1.1f, c, tol) == 1.0f);
    const float brighter = CadenceColourGate(c * 1.3f, c, tol);
    assert(brighter > 0.0f && brighter < 1.0f);
    // Another surface does not.
    assert(CadenceColourGate(kRed, float3(0.7f, 0.7f, 0.7f), tol) == 0.0f);
    ++g_cases;
}

void Rules_CatmullRomIsExactAtTexelsAndNeverRings()
{
    Machine m;
    g_residual.Resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            g_residual.Ref(x, y) = x < W / 2 ? float3(-0.2f, 0.0f, 0.1f) : float3(0.3f, 0.05f, -0.1f); // a step
    for (int x = 0; x < W; ++x)
    {
        const float3 at = CadenceResidualCatmullRom(x + 0.5f, 20.5f, m.P);
        assert(MaxAbs(at, g_residual.At(x, 20)) == 0.0f);
    }
    for (float x = W / 2 - 3.0f; x < W / 2 + 3.0f; x += 0.125f)
    {
        const float3 at = CadenceResidualCatmullRom(x, 20.3f, m.P);
        assert(at.x >= -0.2f && at.x <= 0.3f && at.z >= -0.1f && at.z <= 0.1f); // no overshoot beside the step
    }
    assert(
        MaxAbs(CadenceResidual(float3(std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.5f), float3(0.5f, 0.5f, 0.5f)),
               float3(0, 0, 0)) == 0.0f);
    ++g_cases;
}

void StillScene_CarriesTheModelAnswerExactly()
{
    Machine m;
    Pan(0.0f, 0.0f, 0);
    const auto answer = Model(g_proxyNow);
    m.Record(answer);
    float worst = 0.0f;
    for (int k = 1; k <= 3; ++k)
    {
        Pan(0.0f, 0.0f, k);
        m.Step(k == 1);
        const auto out = m.Synthesize();
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float3 expected = answer.At(x, y);
                worst = max(worst, MaxAbs(out.At(x, y), expected));
                assert(Finite01(out.At(x, y)) || MaxAbs(out.At(x, y), expected) < 1e-6f);
            }
    }
    // Every pixel, borders included: proxy + (answer - proxy) in float, nothing else.
    assert(worst < 1e-6f);

    // The model frame after: the chain is zero everywhere and holds.
    Pan(0.0f, 0.0f, 4);
    const auto motion = m.ModelMotion();
    for (const auto& v : motion.px)
        assert(v.x == 0.0f && v.y == 0.0f);
    ++g_cases;
}

struct PanError
{
    float exactMax = 0.0f; // where the carry has everything it needs
    float exactMean = 0.0f;
    float inPlaceMean = 0.0f; // the same pixels with the edit reused where it was, unmoved
    float frameMean = 0.0f;   // the whole frame, entering edge included
    int exactCount = 0;
};

PanError CarriedPan(float vx, float vy, int frames)
{
    Machine m;
    Pan(vx, vy, 0);
    m.Record(Model(g_proxyNow));
    const auto residual0 = g_residual;
    PanError e;
    double exactSum = 0.0, inPlaceSum = 0.0, frameSum = 0.0;
    int frameCount = 0;
    for (int k = 1; k <= frames; ++k)
    {
        Pan(vx, vy, k);
        m.Step(k == 1);
        const auto out = m.Synthesize();
        const auto truth = Model(g_proxyNow);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float3 o = out.At(x, y);
                assert(Finite01(o));
                const float err = MaxAbs(o, truth.At(x, y));
                frameSum += err;
                ++frameCount;

                // q, and the conditions under which nothing but interpolation separates the carry from the truth:
                // the stand-in model reads one pixel around, the taps three, the edge ramp sixteen.
                const float px = x + 0.5f, py = y + 0.5f;
                const float qx = px - k * vx, qy = py - k * vy;
                if (DistanceToEdge(qx, qy) < 16.0f || DistanceToEdge(px, py) < 4.0f)
                    continue;
                e.exactMax = max(e.exactMax, err);
                exactSum += err;
                inPlaceSum += MaxAbs(g_proxyNow.At(x, y) + residual0.At(x, y), truth.At(x, y));
                ++e.exactCount;
            }
    }
    e.exactMean = float(exactSum / max(e.exactCount, 1));
    e.inPlaceMean = float(inPlaceSum / max(e.exactCount, 1));
    e.frameMean = float(frameSum / max(frameCount, 1));
    return e;
}

void IntegerPan_CarriesExactlyAwayFromTheEnteringEdge()
{
    const auto e = CarriedPan(2.0f, 1.0f, 3);
    assert(e.exactCount > W * H / 2); // a third of three frames at least
    assert(e.exactMax < 1e-5f);
    assert(e.inPlaceMean > 50.0f * max(e.exactMean, 1e-6f)); // the motion is what makes it right
    assert(e.frameMean < 0.01f);
    ++g_cases;
}

void SubPixelPan_CarriesWithinInterpolationError()
{
    const auto e = CarriedPan(0.5f, 0.25f, 3);
    assert(e.exactCount > W * H / 2);
    assert(e.exactMax < 0.005f); // measured 0.0016: Catmull-Rom over a smooth edit
    assert(e.exactMean < 2e-4f); // measured 2.4e-5
    assert(e.inPlaceMean > 10.0f * e.exactMean);
    assert(e.frameMean < 0.01f);
    ++g_cases;
}

void LeavingThePicture_HandsTheModelAnOffPictureVector()
{
    Machine m;
    Pan(2.0f, 0.0f, 0);
    m.Record(Model(g_proxyNow));
    for (int k = 1; k <= 3; ++k)
    {
        Pan(2.0f, 0.0f, k);
        m.Step(k == 1);
    }
    Pan(2.0f, 0.0f, 4);
    const auto motion = m.ModelMotion();
    const float2 none { 4.0f * W, 0.0f };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float2 v = motion.At(x, y);
            if (x + 0.5f - 8.0f < 0.0f) // four frames of two pixels, back past the left edge
                assert(v.x == none.x && v.y == none.y);
            else if (x >= 2 && x < W - 2)
                assert(abs(v.x + 8.0f) < 1e-4f && v.y == 0.0f); // the summed chain, frame 4 back to frame 0
        }
    ++g_cases;
}

// A red occluder at depth 0.9 moving left 4 px a frame over a background at 0.2 panning right 1 px a frame.
constexpr int kOccX = 50, kOccY = 20, kOccSize = 14;

bool Covered(int x, int y, int k)
{
    const int left = kOccX - 4 * k;
    return x >= left && x < left + kOccSize && y >= kOccY && y < kOccY + kOccSize;
}

void Occluded(int k)
{
    Pan(1.0f, 0.0f, k, 0.2f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (Covered(x, y, k))
            {
                g_proxyNow.Ref(x, y) = kRed;
                g_depthNow.Ref(x, y) = 0.9f;
                g_motion.Ref(x, y) = float2(4.0f, 0.0f);
            }
}

void Occluder_RestartsTheChainAndFillsFromTheSameSurface()
{
    Machine m;
    Occluded(0);
    m.Record(Model(g_proxyNow));
    int uncovered = 0, restarted = 0, carriedBackground = 0;
    for (int k = 1; k <= 3; ++k)
    {
        Occluded(k);
        m.Step(k == 1);
        const auto out = m.Synthesize();
        const auto truth = Model(g_proxyNow);
        for (int y = kOccY + 1; y < kOccY + kOccSize - 1; ++y)
            for (int x = 0; x < W; ++x)
            {
                if (Covered(x, y, k) || !Covered(x, y, k - 1))
                    continue;
                // Background the occluder left this frame. Its chain was inherited from the occluder's, which
                // ends on the occluder in the model frame: from the second carried frame on, it restarts from
                // this frame's own vector.
                ++uncovered;
                const float2 chain = g_chain.At(x, y);
                if (k >= 2 && chain.x == -1.0f && chain.y == 0.0f)
                    ++restarted;
                // Never the occluder's strong edit: the fill comes from background around q, or nothing.
                const float3 o = out.At(x, y);
                const float3 c = g_proxyNow.At(x, y);
                assert(Finite01(o));
                assert(MaxAbs(o, c) < 0.08f);
                assert(MaxAbs(o, truth.At(x, y)) < 0.12f);
            }
        // Background the occluder never reaches keeps the summed chain.
        for (int y = 2; y < H - 2; ++y)
            for (int x = 10; x < 30; ++x)
            {
                const float2 chain = g_chain.At(x, y);
                if (chain.x == -float(k) && chain.y == 0.0f)
                    ++carriedBackground;
            }
    }
    assert(uncovered > 0);
    const int uncoveredAfterFirst = 2 * 4 * (kOccSize - 2); // frames 2 and 3, four columns each
    assert(restarted == uncoveredAfterFirst);
    assert(carriedBackground == 3 * (H - 4) * 20);
    ++g_cases;
}

void Occluder_UncoveredPixelsGetNoModelHistory()
{
    Machine m;
    Occluded(0);
    m.Record(Model(g_proxyNow));
    for (int k = 1; k <= 2; ++k)
    {
        Occluded(k);
        m.Step(k == 1);
    }
    Occluded(3);
    const auto motion = m.ModelMotion();
    for (int y = kOccY + 1; y < kOccY + kOccSize - 1; ++y)
        for (int x = 0; x < W; ++x)
            if (!Covered(x, y, 3) && Covered(x, y, 0) && !Covered(x, y, 2))
                assert(motion.At(x, y).x == 4.0f * W); // uncovered since the model looked: off the picture
    // Background that never was under it is handed its summed chain.
    assert(abs(motion.At(10, 5).x + 3.0f) < 1e-4f && motion.At(10, 5).y == 0.0f);
    ++g_cases;
}

void NoMatch_NoFill()
{
    // The model frame saw a wall at 0.9; a hole opens on a surface at 0.1 it never saw anywhere.
    Machine m;
    Pan(0.0f, 0.0f, 0, 0.9f);
    m.Record(Model(g_proxyNow));
    Pan(0.0f, 0.0f, 1, 0.9f);
    const int hx = 40, hy = 25, hs = 10;
    for (int y = hy; y < hy + hs; ++y)
        for (int x = hx; x < hx + hs; ++x)
        {
            g_depthNow.Ref(x, y) = 0.1f;
            g_proxyNow.Ref(x, y) = float3(0.2f, 0.25f, 0.6f);
        }
    m.Step(true);
    const auto out = m.Synthesize();
    for (int y = hy; y < hy + hs; ++y)
        for (int x = hx; x < hx + hs; ++x)
        {
            // The coarse edit there is not zero -- a fill from it would have been an edit from the wall.
            const float3 low = CadenceLowAt(x + 0.5f, y + 0.5f, m.P);
            assert(abs(low.x) > 0.005f);
            const float3 o = out.At(x, y);
            const float3 c = g_proxyNow.At(x, y);
            assert(o.x == c.x && o.y == c.y && o.z == c.z); // the frame as the game drew it, exactly
        }
    ++g_cases;
}

void UiProtection_KeepsTheFrameUnderTheInterface()
{
    Machine m;
    Pan(1.0f, 0.0f, 0);
    m.Record(Model(g_proxyNow));
    Pan(1.0f, 0.0f, 1);
    m.Step(true);
    g_ui.Resize(W, H, 0.0f);
    for (int x = 10; x < 30; ++x)
        g_ui.Ref(x, 10) = 1.0f;
    g_ui.Ref(40, 10) = 0.5f;
    const auto plain = m.Synthesize();
    m.P.haveUi = 1;
    const auto out = m.Synthesize();
    for (int x = 10; x < 30; ++x)
    {
        const float3 c = g_proxyNow.At(x, 10);
        assert(out.At(x, 10).x == c.x && out.At(x, 10).y == c.y && out.At(x, 10).z == c.z);
    }
    const float3 c = g_proxyNow.At(40, 10);
    const float3 half = c + (plain.At(40, 10) - c) * 0.5f;
    assert(MaxAbs(out.At(40, 10), half) < 1e-6f);
    assert(MaxAbs(out.At(60, 30), plain.At(60, 30)) == 0.0f);
    ++g_cases;
}

void ReversibleReplace_HighlightCarriedReachesClamp()
{
    // Highlights in sRGB-encoded proxy space (e.g. NeutwoEncode(light) near 1.0):
    // If last frame's proxy was 0.97 and the model answered 0.99 (residual = +0.02),
    // and this frame's proxy is already a highlight at 0.99, CadenceSynthesize carries
    // the edit and computes o = proxy_now + add = 0.99 + 0.02 = 1.01.
    // In production, CadenceSynthesize clamps the output to max(1.0, proxy_now) = 1.0.
    // When this value reaches the resolve, NeutwoDecode(1.0) diverges towards infinity (~707x),
    // which proves why ReversibleMode 2 and 4 must refuse cadence under HDR.
    CadenceParams P = Shipped();
    g_proxyNow.Resize(W, H, float3(0.99f, 0.99f, 0.99f));
    g_proxyThen.Resize(W, H, float3(0.97f, 0.97f, 0.97f));
    g_depthNow.Resize(W, H, 0.5f);
    g_depthThen.Resize(W, H, 0.5f);
    g_chain.Resize(W, H, float2(0.0f, 0.0f));             // still camera
    g_residual.Resize(W, H, float3(0.02f, 0.02f, 0.02f)); // answer (0.99) - proxy_then (0.97)
    g_low.Resize(P.lowWidth, P.lowHeight, float3(0.02f, 0.02f, 0.02f));
    g_ui.Resize(W, H, 0.0f);

    const float3 out = CadenceSynthesize(W / 2, H / 2, float2(0.0f, 0.0f), P);

    // Verifies that production CadenceSynthesize clamped the output at exactly 1.0
    assert(out.x == 1.0f && out.y == 1.0f && out.z == 1.0f);
    ++g_cases;
}

// --- Smoothing (docs/dlssnr/design/cadence-smoothing.md) -----------------------------------------------------------

// The model re-decides its light a little every time it runs: the stand-in, lifted by `drift` everywhere.
Image<float3> Drifted(const Image<float3>& proxy, float drift)
{
    Image<float3> answer = Model(proxy);
    for (auto& c : answer.px)
        c = c + float3(drift, drift, drift);
    return answer;
}

float MeanLuma(const Image<float3>& image)
{
    double sum = 0.0;
    for (const auto& c : image.px)
        sum += CadenceLuma(c);
    return float(sum / double(image.px.size()));
}

bool Same(float3 a, float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

void FadedCarry_AtWeightZeroIsTheCarryExactly()
{
    // An occluder sequence -- moved edit, fill, restarted chains, the edge -- with the fade full of NaN: at weight 0
    // it must not be read, and every pixel is CadenceSynthesize's to the bit.
    Machine m;
    Occluded(0);
    m.Record(Model(g_proxyNow));
    g_fade.Resize(W, H, float3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
    for (int k = 1; k <= 3; ++k)
    {
        Occluded(k);
        m.Step(k == 1);
        const auto plain = m.Synthesize();
        const auto faded = m.SynthesizeFaded(0.0f);
        for (size_t i = 0; i < plain.px.size(); ++i)
            assert(Same(plain.px[i], faded.px[i]));
        // And a fade that is not finite is dropped at any weight, not carried into the frame.
        const auto poisoned = m.SynthesizeFaded(0.5f);
        for (size_t i = 0; i < plain.px.size(); ++i)
            assert(Same(plain.px[i], poisoned.px[i]));
    }
    ++g_cases;
}

void Fade_WalksTheNewAnswerInInEqualSteps()
{
    // A still view at cadence 3. The model frame at k = 3 answers 0.03 brighter than the one at k = 0 did. Without
    // smoothing the light steps 0, 0, 0.03, 0, 0; with it, 0.01 on each of the model frame and the two carried after.
    const float drift = 0.03f;
    Machine m;
    Pan(0.0f, 0.0f, 0);
    const auto a0 = Drifted(g_proxyNow, 0.0f);
    m.Record(a0);
    std::vector<float> light { MeanLuma(a0) };
    for (int k = 1; k <= 2; ++k)
    {
        Pan(0.0f, 0.0f, k);
        m.Step(k == 1);
        light.push_back(MeanLuma(m.Synthesize()));
    }

    Pan(0.0f, 0.0f, 3);
    const auto a1 = Drifted(g_proxyNow, drift);
    m.ModelMotion();
    const auto shown3 = m.Fade(a1, DlssNr::Cadence::FadeWeight(0, 3));
    m.Record(a1);
    light.push_back(MeanLuma(shown3));
    for (size_t i = 0; i < a1.px.size(); ++i)
        assert(MaxAbs(shown3.px[i], a0.px[i] + float3(drift, drift, drift) * (1.0f / 3.0f)) < 1e-6f);

    for (int k = 4; k <= 5; ++k)
    {
        Pan(0.0f, 0.0f, k);
        m.Step(k == 4);
        const float weight = DlssNr::Cadence::FadeWeight(uint32_t(k - 3), 3);
        const auto out = m.SynthesizeFaded(weight);
        const float share = 1.0f - weight; // of the new answer
        for (size_t i = 0; i < out.px.size(); ++i)
            assert(MaxAbs(out.px[i], a0.px[i] + float3(drift, drift, drift) * share) < 1e-6f);
        light.push_back(MeanLuma(out));
    }

    // The light: still across the carried frames, then three equal steps of a third of the drift.
    assert(abs(light[1] - light[0]) < 1e-6f && abs(light[2] - light[1]) < 1e-6f);
    for (int k = 3; k <= 5; ++k)
        assert(abs((light[k] - light[k - 1]) - drift / 3.0f) < 1e-5f);
    ++g_cases;
}

void Fade_IsMovedWithTheSurface()
{
    // An integer pan. Old and new answers both belong to the surface, so every frame of the walk away from the
    // entering edge is a straight line between the two stand-in models of the content under the pixel now.
    const float drift = 0.04f;
    Machine m;
    Pan(2.0f, 1.0f, 0);
    m.Record(Drifted(g_proxyNow, 0.0f));
    for (int k = 1; k <= 2; ++k)
    {
        Pan(2.0f, 1.0f, k);
        m.Step(k == 1);
    }
    Pan(2.0f, 1.0f, 3);
    m.ModelMotion();
    const auto shown3 = m.Fade(Drifted(g_proxyNow, drift), DlssNr::Cadence::FadeWeight(0, 3));
    m.Record(Drifted(g_proxyNow, drift));

    int checked = 0;
    for (int k = 3; k <= 5; ++k)
    {
        Image<float3> out = shown3;
        if (k > 3)
        {
            Pan(2.0f, 1.0f, k);
            m.Step(k == 4);
            out = m.SynthesizeFaded(DlssNr::Cadence::FadeWeight(uint32_t(k - 3), 3));
        }
        const float oldShare = DlssNr::Cadence::FadeWeight(uint32_t(k - 3), 3);
        const auto before = Drifted(g_proxyNow, 0.0f);
        const auto after = Drifted(g_proxyNow, drift);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                // Back to frame 0 through both chains, and the stand-in's one-pixel reach, away from every edge.
                const float px = x + 0.5f, py = y + 0.5f;
                if (DistanceToEdge(px - k * 2.0f, py - k * 1.0f) < 16.0f || DistanceToEdge(px, py) < 4.0f)
                    continue;
                const float3 expected = after.At(x, y) * (1.0f - oldShare) + before.At(x, y) * oldShare;
                assert(MaxAbs(out.At(x, y), expected) < 1e-5f);
                ++checked;
            }
    }
    assert(checked > W * H / 2); // three frames of the interior
    ++g_cases;
}

void Fade_TakesTheNewAnswerAtOnceWhereTheCarryHadNoSurface()
{
    // NoMatch_NoFill's hole, still open on the model frame: the carried answer there was the plain frame, a guess, so
    // the new answer is shown at once and nothing of the guess is walked from. Elsewhere the walk runs.
    Machine m;
    Pan(0.0f, 0.0f, 0, 0.9f);
    m.Record(Drifted(g_proxyNow, 0.0f));
    const int hx = 40, hy = 25, hs = 10;
    auto openHole = []()
    {
        for (int y = hy; y < hy + hs; ++y)
            for (int x = hx; x < hx + hs; ++x)
            {
                g_depthNow.Ref(x, y) = 0.1f;
                g_proxyNow.Ref(x, y) = float3(0.2f, 0.25f, 0.6f);
            }
    };
    Pan(0.0f, 0.0f, 1, 0.9f);
    openHole();
    m.Step(true);
    Pan(0.0f, 0.0f, 2, 0.9f);
    openHole();
    m.ModelMotion();
    const auto answer = Drifted(g_proxyNow, 0.05f);
    const auto shown = m.Fade(answer, DlssNr::Cadence::FadeWeight(0, 2));
    for (int y = hy + 2; y < hy + hs - 2; ++y)
        for (int x = hx + 2; x < hx + hs - 2; ++x)
        {
            assert(Same(g_fade.At(x, y), float3(0.0f, 0.0f, 0.0f)));
            assert(Same(shown.At(x, y), answer.At(x, y)));
        }
    // On the wall the walk runs: half of the 0.05 lift is still to come.
    assert(abs(shown.At(10, 10).x - (answer.At(10, 10).x - 0.025f)) < 1e-5f);
    ++g_cases;
}

void Fade_LeavesTheInterfaceToTheModel()
{
    Machine m;
    Pan(0.0f, 0.0f, 0);
    m.Record(Drifted(g_proxyNow, 0.0f));
    Pan(0.0f, 0.0f, 1);
    m.Step(true);
    Pan(0.0f, 0.0f, 2);
    g_ui.Resize(W, H, 0.0f);
    for (int x = 10; x < 30; ++x)
        g_ui.Ref(x, 10) = 1.0f;
    m.P.haveUi = 1;
    m.ModelMotion();
    const auto answer = Drifted(g_proxyNow, 0.05f);
    const auto shown = m.Fade(answer, 0.5f);
    for (int x = 10; x < 30; ++x)
        assert(Same(shown.At(x, 10), answer.At(x, 10)));
    assert(!Same(shown.At(40, 30), answer.At(40, 30)));
    ++g_cases;
}

void Limiter_Rule()
{
    const float step = DlssNr::Cadence::LimitStep(1.0f / 60.0f);
    const float tol = DlssNr::Cadence::kLightInputStops;
    const float maxGain = DlssNr::Cadence::kLightMaxGainStops;

    // Nothing to hold to: the frame passes.
    CadenceLimit r = CadenceLimitLight(0.3f, 1.0f, 0.0f, 1.0f, 0, step, tol, maxGain);
    assert(r.shown == 0.3f && r.gainStops == 0.0f);
    // A change inside the step passes untouched.
    r = CadenceLimitLight(0.01f, 1.0f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(r.shown == 0.01f && r.gainStops == 0.0f);
    // A half-stop re-grade on a still frame is held to the step, either way.
    r = CadenceLimitLight(0.5f, 1.0f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(abs(r.shown - step) < 1e-7f && abs(r.gainStops - (step - 0.5f)) < 1e-7f);
    r = CadenceLimitLight(-0.5f, 1.0f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(abs(r.shown + step) < 1e-7f && abs(r.gainStops - (0.5f - step)) < 1e-7f);
    // The frame's own light moved: a cut, a flash, another room. The edit's light follows at once.
    r = CadenceLimitLight(0.5f, 1.0f + tol * 1.01f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(r.shown == 0.5f && r.gainStops == 0.0f);
    r = CadenceLimitLight(0.5f, 1.0f - tol * 0.99f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(r.gainStops < 0.0f);
    // Three stops at once on a still frame: the gain stops at one, and the light shown is what that gives.
    r = CadenceLimitLight(3.0f, 1.0f, 0.0f, 1.0f, 1, step, tol, maxGain);
    assert(r.gainStops == -maxGain && r.shown == 3.0f - maxGain);
    // Not finite anywhere: pass, nothing held.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (int i = 0; i < 4; ++i)
    {
        r = CadenceLimitLight(i == 0 ? nan : 0.5f, i == 1 ? nan : 1.0f, i == 2 ? nan : 0.0f, i == 3 ? nan : 1.0f, 1,
                              step, tol, maxGain);
        assert(r.gainStops == 0.0f);
    }
    ++g_cases;
}

void Limiter_LightAndGainAgree()
{
    // The light the limiter measures, and the gain it applies, in the composition's space: sRGB decoded, or the values
    // themselves on a frame the game tone mapped. No gain is the answer to the bit.
    const float3 c { 0.55f, 0.40f, 0.30f };
    for (int passthrough = 0; passthrough <= 1; ++passthrough)
    {
        assert(Same(CadenceApplyGain(c, 0.0f, passthrough), c));
        const float before = CadenceLight(c, passthrough);
        const float after = CadenceLight(CadenceApplyGain(c, 0.25f, passthrough), passthrough);
        assert(abs(std::log2(after / before) - 0.25f) < 1e-4f);
    }
    assert(Same(CadenceApplyGain(c, -0.5f, 1), c * exp2(-0.5f)));
    assert(abs(CadenceLight(float3(1.0f, 1.0f, 1.0f), 0) - 1.0f) < 1e-6f);
    assert(abs(CadenceLight(float3(0.5f, 0.5f, 0.5f), 0) - 0.2140411f) < 1e-5f);
    assert(CadenceLight(float3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f), 1) == 0.0f);
    ++g_cases;
}

// The walk and the limiter together, as Show runs them: a re-grade of the whole picture arriving on a model frame of a
// still view. The fade spreads it over three frames; the limiter then holds the edit's light to 1.5 stops a second,
// and once the frame's own light jumps it lets go at once.
void Limiter_HoldsAReGradeToTheRate()
{
    const float step = DlssNr::Cadence::LimitStep(1.0f / 60.0f);
    Machine m;
    Pan(0.0f, 0.0f, 0);
    m.Record(Drifted(g_proxyNow, 0.0f));
    auto sums = [](const Image<float3>& shown)
    {
        double s = 0.0, p = 0.0;
        for (size_t i = 0; i < shown.px.size(); ++i)
        {
            s += CadenceLight(shown.px[i], 0);
            p += CadenceLight(g_proxyNow.px[i], 0);
        }
        return std::pair<float, float>(float(std::log2(s / p)), float(std::log2(p)));
    };

    float prevShown = 0.0f, prevInput = 0.0f;
    int valid = 0;
    std::vector<float> shownLight;
    auto limitAndApply = [&](const Image<float3>& shown)
    {
        const auto [raw, input] = sums(shown);
        const CadenceLimit r =
            CadenceLimitLight(raw, input, prevShown, prevInput, valid, step, DlssNr::Cadence::kLightInputStops,
                              DlssNr::Cadence::kLightMaxGainStops);
        Image<float3> out = shown;
        for (auto& c : out.px)
            c = CadenceApplyGain(c, r.gainStops, 0);
        // What the gain gives is what the state says is shown.
        assert(abs(sums(out).first - r.shown) < 2e-3f);
        prevShown = r.shown;
        prevInput = input;
        valid = 1;
        shownLight.push_back(r.shown);
        return r;
    };

    for (int k = 1; k <= 2; ++k)
    {
        Pan(0.0f, 0.0f, k);
        m.Step(k == 1);
        limitAndApply(m.Synthesize());
    }
    // The re-grade: 0.15 brighter in encoded values, about half a stop of light.
    Pan(0.0f, 0.0f, 3);
    m.ModelMotion();
    const auto answer = Drifted(g_proxyNow, 0.15f);
    limitAndApply(m.Fade(answer, DlssNr::Cadence::FadeWeight(0, 3)));
    m.Record(answer);
    int frames = 0;
    for (int k = 4; k <= 40; ++k)
    {
        Pan(0.0f, 0.0f, k);
        m.Step(k == 4);
        const uint32_t age = uint32_t((k - 3) % 3);
        const auto r = limitAndApply(m.SynthesizeFaded(k <= 5 ? DlssNr::Cadence::FadeWeight(age, 3) : 0.0f));
        if (r.gainStops != 0.0f)
            ++frames;
    }
    // Every step of the shown light within the rate, and the whole re-grade through in about a third of a second.
    for (size_t i = 1; i < shownLight.size(); ++i)
        assert(shownLight[i] - shownLight[i - 1] <= step + 1e-5f);
    const float regrade = sums(answer).first - shownLight[1];
    assert(regrade > 0.3f);
    assert(frames >= int(regrade / step) - 4 && frames <= int(regrade / step) + 2);
    assert(abs(shownLight.back() - sums(answer).first) < 1e-4f);

    // The frame's own light jumps (a light switched on in the room): the limit lets go on that frame.
    for (auto& c : g_proxyNow.px)
        c = c * 1.6f;
    const auto [raw, input] = sums(Drifted(g_proxyNow, 0.0f));
    const CadenceLimit r = CadenceLimitLight(raw, input, prevShown, prevInput, 1, step,
                                             DlssNr::Cadence::kLightInputStops, DlssNr::Cadence::kLightMaxGainStops);
    assert(abs(input - prevInput) >= DlssNr::Cadence::kLightInputStops && r.gainStops == 0.0f);
    ++g_cases;
}
} // namespace

int main()
{
    Rules_DepthMismatch();
    Rules_ColourGate();
    Rules_CatmullRomIsExactAtTexelsAndNeverRings();
    StillScene_CarriesTheModelAnswerExactly();
    IntegerPan_CarriesExactlyAwayFromTheEnteringEdge();
    SubPixelPan_CarriesWithinInterpolationError();
    LeavingThePicture_HandsTheModelAnOffPictureVector();
    Occluder_RestartsTheChainAndFillsFromTheSameSurface();
    Occluder_UncoveredPixelsGetNoModelHistory();
    NoMatch_NoFill();
    UiProtection_KeepsTheFrameUnderTheInterface();
    ReversibleReplace_HighlightCarriedReachesClamp();
    FadedCarry_AtWeightZeroIsTheCarryExactly();
    Fade_WalksTheNewAnswerInInEqualSteps();
    Fade_IsMovedWithTheSurface();
    Fade_TakesTheNewAnswerAtOnceWhereTheCarryHadNoSurface();
    Fade_LeavesTheInterfaceToTheModel();
    Limiter_Rule();
    Limiter_LightAndGainAgree();
    Limiter_HoldsAReGradeToTheRate();
    if (g_cases != 20)
        return 2;
    std::printf("PASS %d rule cases over synthetic sequences: still scene exact, integer pan exact and sub-pixel pan "
                "within interpolation, chain off the picture, occluder restarts the chain, no match no fill, UI kept; "
                "smoothing: weight 0 is the carry to the bit, the walk in equal steps and moved with the surface, a "
                "disocclusion and the interface take the answer at once, the light limiter's rule, space and rate\n",
                g_cases);
    return 0;
}
