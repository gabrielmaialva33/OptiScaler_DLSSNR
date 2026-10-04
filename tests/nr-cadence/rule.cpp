// Model cadence's per-pixel rules (OptiScaler/shaders/dlssnr/precompile/dlssnr_cadence_rule.h), the exact code
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
using std::floor;
using std::isfinite;
using std::max;
using std::min;
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
Image<float3> g_proxyNow, g_proxyThen, g_residual, g_low;
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
    if (g_cases != 12)
        return 2;
    std::printf("PASS %d rule cases over synthetic sequences: still scene exact, integer pan exact and sub-pixel pan "
                "within interpolation, chain off the picture, occluder restarts the chain, no match no fill, UI kept\n",
                g_cases);
    return 0;
}
