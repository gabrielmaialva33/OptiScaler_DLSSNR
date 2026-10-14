#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): the LSFG 3.1 pass graph as plain data, and the small
// decisions around it, with no D3D12 in sight so tests/lsfg-integration runs them on the host.
//
// The graph is the one tools/lsfg-study/harness/main.cpp runs and checks against lsfg-vk-cli (72-75 dB in
// FP32): 34 pre-pass dispatches once per base frame, 66 main-pass dispatches per generated frame
// (tools/lsfg-study/PIPELINE_V3_1.md). It was read from lsfg-vk 2.0.0 (CC BY-NC-ND 4.0) for a private
// study; tools/lsfg-study/OPTI_INTEGRATION.md, "Read this first", says why none of this may be published.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Lsfg
{

// ---------------------------------------------------------------------------------------------------------
// Images and passes

enum class ImageFormat : uint8_t
{
    R8,      // DXGI_FORMAT_R8_UNORM
    Rgba8,   // DXGI_FORMAT_R8G8B8A8_UNORM
    Rgba16F, // DXGI_FORMAT_R16G16B16A16_FLOAT
    Display, // the presented frame's own format: the source array and the generated output
};

struct GraphImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    ImageFormat format = ImageFormat::Rgba8;
};

struct GraphPass
{
    uint32_t rid = 0;     // resource id of the shader (lsfg-vk's library.cpp numbering, variant applied)
    uint32_t index = 0;   // push constant pc_Iteration: the pyramid level this dispatch works on
    uint32_t special = 0; // push constant pc_FirstIter
    uint32_t groupsX = 0;
    uint32_t groupsY = 0;
    std::vector<uint32_t> reads;  // image ids sampled (SRV)
    std::vector<uint32_t> writes; // image ids written (UAV)
};

struct Graph
{
    std::vector<GraphImage> images;
    // Vulkan binding (= register space after conversion) -> the images its descriptor array covers, in
    // array order. Each UAV binding N has the SRV binding N + 1 over the same images.
    std::map<uint32_t, std::vector<uint32_t>> pools;
    std::vector<GraphPass> pre;  // once per base frame
    std::vector<GraphPass> main; // once per generated frame
    uint32_t source = 0;         // two-layer source array: slot iteration % 2 holds the current frame
    uint32_t output = 0;         // the generated frame
    uint32_t flowWidth = 0;
    uint32_t flowHeight = 0;
};

constexpr uint32_t kPrePasses = 34;
constexpr uint32_t kMainPasses = 66;
constexpr uint32_t kShaderCount = 27; // mipmaps, generate (8-bit and 16-bit), 24 core shaders
constexpr uint32_t kSourceBinding = 4;
constexpr uint32_t kOutputBinding = 55;

// The shader a resource id is a variant of: 0 mipmaps, 1/2 generate, 3..26 the core shaders. Performance
// variants are +24, FP16 variants +48 (only the core shaders have variants).
inline uint32_t BaseShader(uint32_t rid) { return rid < 3 ? rid : ((rid - 3) % 24) + 3; }

inline uint32_t VariantRid(uint32_t baseRid, bool performance, bool fp16)
{
    if (baseRid < 3)
        return baseRid;
    return baseRid + (performance ? 24u : 0u) + (fp16 ? 48u : 0u);
}

inline bool BuildGraph(uint32_t width, uint32_t height, float flow, bool performance, bool fp16, Graph& g,
                       std::string& error)
{
    g = {};

    if (width < 64 || height < 64 || width > 16384 || height > 16384)
    {
        error = "frame size outside 64..16384";
        return false;
    }

    if (!(flow >= 0.25f && flow <= 1.0f))
    {
        error = "flow scale outside 0.25..1.0";
        return false;
    }

    const uint32_t fw = uint32_t(float(width) * flow);
    const uint32_t fh = uint32_t(float(height) * flow);
    const uint32_t mul = performance ? 1u : 2u;

    if (fw < 64 || fh < 64)
    {
        error = "flow-scaled size below 64 on an axis";
        return false;
    }

    g.flowWidth = fw;
    g.flowHeight = fh;

    auto image = [&](uint32_t w, uint32_t h, uint32_t layers, ImageFormat format)
    {
        g.images.push_back({ w, h, layers, format });
        return uint32_t(g.images.size() - 1);
    };

    // A UAV binding and the SRV binding after it cover the same images (binding 55 is the output, UAV only).
    auto add = [&](uint32_t binding, uint32_t w, uint32_t h, uint32_t layers, ImageFormat format = ImageFormat::Rgba8)
    {
        const uint32_t id = image(w, h, layers, format);
        g.pools[binding].push_back(id);
        if (binding != kOutputBinding)
            g.pools[binding + 1].push_back(id);
        return id;
    };

    auto pass = [&](std::vector<GraphPass>& dst, uint32_t rid, uint32_t index, uint32_t special, uint32_t w, uint32_t h,
                    uint32_t group, std::vector<uint32_t> reads, std::vector<uint32_t> writes)
    {
        GraphPass p;
        p.rid = VariantRid(rid, performance, fp16);
        p.index = index;
        p.special = special;
        p.groupsX = (w + group - 1) / group;
        p.groupsY = (h + group - 1) / group;
        p.reads = std::move(reads);
        p.writes = std::move(writes);
        dst.push_back(std::move(p));
    };

    g.source = image(width, height, 2, ImageFormat::Display);
    g.pools[kSourceBinding] = { g.source };

    for (uint32_t i = 0; i < 7; ++i)
        add(5, std::max(fw >> i, 1u), std::max(fh >> i, 1u), 1, ImageFormat::R8);

    pass(g.pre, 0, 0, 0, fw, fh, 64, { g.source }, g.pools[5]);

    uint32_t alpha[7] {}, gamma[7] {}, delta[3] {}, epsilon[3] {};

    for (uint32_t i = 0; i < 7; ++i)
    {
        uint32_t w = ((fw >> (6 - i)) + 1) / 2, h = ((fh >> (6 - i)) + 1) / 2;
        const uint32_t a = add(7, w, h, mul);
        const uint32_t b = add(9, w, h, mul);
        pass(g.pre, 13, i, 0, w, h, 8, { g.pools[5][6 - i] }, { a });
        pass(g.pre, 14, i, 0, w, h, 8, { a }, { b });
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        const uint32_t c = add(11, w, h, 2 * mul);
        alpha[i] = add(13, w, h, 6 * mul);
        pass(g.pre, 15, i, 0, w, h, 8, { b }, { c });
        pass(g.pre, 16, i, 0, w, h, 8, { c }, { alpha[i] });
    }

    const uint32_t bw = g.images[alpha[6]].width, bh = g.images[alpha[6]].height;
    uint32_t prev = alpha[6];
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t t = add(15 + 2 * i, bw, bh, 2);
        pass(g.pre, 22 + i, 0, 0, bw, bh, 8, { prev }, { t });
        prev = t;
    }

    for (uint32_t i = 0; i < 6; ++i)
        add(23, std::max(bw >> i, 1u), std::max(bh >> i, 1u), 1, ImageFormat::R8);

    pass(g.pre, 26, 0, 0, bw, bh, 32, { prev }, g.pools[23]);

    for (uint32_t i = 0; i < 7; ++i)
    {
        const uint32_t w = g.images[alpha[i]].width, h = g.images[alpha[i]].height;

        auto chain = [&](uint32_t firstRid, uint32_t binding, uint32_t index, uint32_t layers0, uint32_t layersN,
                         const std::vector<uint32_t>& reads0, const std::vector<uint32_t>& reads4, uint32_t special)
        {
            uint32_t t = 0;
            for (uint32_t j = 0; j < 5; ++j)
            {
                const uint32_t next = add(binding + 2 * j, w, h,
                                          j == 4   ? 1
                                          : j == 0 ? layers0
                                                   : layersN,
                                          j == 4 ? ImageFormat::Rgba16F : ImageFormat::Rgba8);
                std::vector<uint32_t> reads = j == 0 ? reads0 : std::vector<uint32_t> { t };
                if (j == 4)
                    reads.insert(reads.end(), reads4.begin(), reads4.end());
                pass(g.main, firstRid + j, index, (j == 0 || j == 4) ? special : 0, w, h, 8, reads, { next });
                t = next;
            }
            return t;
        };

        std::vector<uint32_t> gr { alpha[i] }, gr4 { g.pools[23][i == 0 ? 5 : 6 - i] };
        if (i != 0)
        {
            gr.push_back(gamma[i - 1]);
            gr4.push_back(gamma[i - 1]);
        }
        gamma[i] = chain(3, 25, i, 3, 2 * mul, gr, gr4, i == 0 ? 1 : 0);

        if (i >= 4)
        {
            const uint32_t j = i - 4;
            std::vector<uint32_t> dr { alpha[i] }, dr4 { g.pools[23][6 - i] };
            if (j != 0)
            {
                dr.push_back(delta[j - 1]);
                dr4.push_back(delta[j - 1]);
            }
            delta[j] = chain(8, 35, j, 3, 2 * mul, dr, dr4, j == 0 ? 1 : 0);

            std::vector<uint32_t> er { alpha[i], gamma[i - 1] }, er4;
            if (j != 0)
            {
                er.push_back(delta[j - 1]);
                er4.push_back(epsilon[j - 1]);
            }
            epsilon[j] = chain(17, 45, j, mul, mul, er, er4, j == 0 ? 1 : 0);
        }
    }

    g.output = add(kOutputBinding, width, height, 1, ImageFormat::Display);
    pass(g.main, 1, 0, 0, width, height, 16, { g.source, gamma[6], delta[2], epsilon[2] }, { g.output });

    if (g.pre.size() != kPrePasses || g.main.size() != kMainPasses)
    {
        error = "graph pass count drift";
        return false;
    }

    for (const auto* passes : { &g.pre, &g.main })
    {
        for (const auto& p : *passes)
        {
            for (auto r : p.reads)
            {
                if (std::find(p.writes.begin(), p.writes.end(), r) != p.writes.end())
                {
                    error = "a pass reads and writes the same image";
                    return false;
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------------
// Constant buffers
//
// Every shader may declare two cbuffers: `cb` at (b0, space0), the frame's six values, and the push constants
// (pc_FirstIter, pc_Iteration), which spirv-cross leaves unregistered so dxc puts them at the first free
// register of space 0: b1 when the shader reads `cb`, b0 when it does not. PSV0 has no names, so the table
// below (from reflection of the 99 modules, tools/lsfg-study/harness) says which a lone b0 is. It holds for
// all four variants of a shader; a module whose CBV count disagrees is refused rather than guessed.

struct CbufferUse
{
    bool cb;
    bool pc;
};

inline CbufferUse CbuffersOf(uint32_t baseShader)
{
    switch (baseShader)
    {
    case 0:  // mipmaps
    case 1:  // generate (8-bit)
    case 2:  // generate (16-bit)
    case 22: // beta0
    case 26: // beta4
        return { true, false };
    case 3:  // gamma0
    case 7:  // gamma4
    case 8:  // delta0
    case 12: // delta4
    case 16: // alpha3
    case 17: // epsilon0
        return { true, true };
    case 23: // beta1
    case 24: // beta2
    case 25: // beta3
        return { false, false };
    default: // gamma1-3, delta1-3, alpha0-2, epsilon1-4
        return { false, true };
    }
}

// Root constants per cbuffer: `cb` is 24 bytes (padded to 32 as a cbuffer), the push constants 8 (16).
constexpr uint32_t kCbWords = 8;
constexpr uint32_t kPcWords = 4;

// The values of `cb`, in declaration order (PIPELINE_V3_1.md, section 5).
struct FrameConstants
{
    float timestamp = 0.0f;   // interpolation position in (0, 1); 0 in the pre-pass
    uint32_t sourceIndex = 0; // the iteration counter: source slot % 2, feature history % 3
    uint32_t advancedColorKind = 0;
    uint32_t hdrSupport = 0;
    float resolutionInvScale = 1.0f; // 1 / flow scale
    float uiThreshold = 0.5f;
};
static_assert(sizeof(FrameConstants) == 24, "cb is six 32-bit values");

// Generated frame k of n sits at k / (n + 1) between the previous base frame and the current one.
inline float TimestampFor(uint32_t k, uint32_t generatedFrames) { return float(k) / float(generatedFrames + 1); }

// ---------------------------------------------------------------------------------------------------------
// History
//
// The only state carried between base frames is the iteration counter: it picks the source slot (% 2) and
// the alpha feature slot (% 3) inside the shaders. A reset (scene cut, the synthesized input's policy, a
// context rebuild, a gap in the frame IDs) keeps ingesting but generates nothing until one more base frame has
// arrived, so the first generated frame always blends two consecutive frames of the same stream.

constexpr uint32_t kWarmupFrames = 1;

struct History
{
    uint32_t iteration = 0;  // cb_SourceIndex of the next base frame
    uint32_t sinceReset = 0; // base frames ingested since the last reset, before the current one
    uint64_t lastFrame = 0;  // FSR-FG frame ID of the last base frame ingested
    bool hasLast = false;

    uint32_t SourceSlot() const { return iteration % 2; }

    // Whether frame `frameId` continues the stream. Anything else (a frame skipped, refused or interpolated by FFX,
    // a jump in FSR-FG's frame count) is treated as a reset by the caller: FFX resets on the same gap.
    bool Follows(uint64_t frameId) const { return hasLast && frameId == lastFrame + 1; }

    bool Generates(bool reset) const { return !reset && sinceReset >= kWarmupFrames; }

    void Advance(bool reset, uint64_t frameId)
    {
        ++iteration;
        sinceReset = reset ? 1 : sinceReset + 1;
        lastFrame = frameId;
        hasLast = true;
    }

    void Restart()
    {
        sinceReset = 0;
        hasLast = false;
    }
};

// ---------------------------------------------------------------------------------------------------------
// Which presented frames LSFG takes

// The DXGI_FORMAT values involved, so this header needs no <dxgiformat.h>.
namespace Format
{
constexpr uint32_t Rgba16FTypeless = 9;
constexpr uint32_t Rgba16F = 10;
constexpr uint32_t Rgb10A2Typeless = 23;
constexpr uint32_t Rgb10A2 = 24;
constexpr uint32_t Rgba8Typeless = 27;
constexpr uint32_t Rgba8 = 28;
constexpr uint32_t Rgba8Srgb = 29;
constexpr uint32_t Bgra8 = 87;
constexpr uint32_t Bgra8Typeless = 90;
constexpr uint32_t Bgra8Srgb = 91;
} // namespace Format

// The UNORM format LSFG's own copies of a presented format use: the same typeless family, so copies in and
// out are legal, and never _SRGB, because the shaders work on the encoded values. 0 when not supported.
inline uint32_t WorkingFormat(uint32_t dxgiFormat)
{
    switch (dxgiFormat)
    {
    case Format::Rgba8Typeless:
    case Format::Rgba8:
    case Format::Rgba8Srgb:
        return Format::Rgba8;
    case Format::Bgra8Typeless:
    case Format::Bgra8:
    case Format::Bgra8Srgb:
        return Format::Bgra8;
    case Format::Rgb10A2Typeless:
    case Format::Rgb10A2:
        return Format::Rgb10A2;
    default:
        return 0;
    }
}

// FfxApiBackbufferTransferFunction: 0 SRGB, 1 PQ, 2 SCRGB.
constexpr uint32_t kTransferSrgb = 0;
constexpr uint32_t kTransferPq = 1;
constexpr uint32_t kTransferScRgb = 2;

// NaN and out-of-range values fall back to the nearest end (NaN to 1.0, the default).
inline float ClampFlow(float flow) { return (flow >= 0.25f && flow <= 1.0f) ? flow : (flow < 0.25f ? 0.25f : 1.0f); }

enum class Surface : uint8_t
{
    Ok,
    UnsupportedFormat, // not 8-bit RGBA/BGRA or 10-bit RGB10A2 (scRGB's FP16 included)
    FormatMismatch,    // the output FFX hands over is not of the frame's family
    Hdr,               // PQ or scRGB: the transfer constants and the FP16 output are not done yet
    SizeMismatch,      // the output is not the frame's size
    TooSmall,          // below 64 px, or below 64 px once flow-scaled
};

inline Surface ClassifySurface(uint32_t frameFormat, uint32_t frameWidth, uint32_t frameHeight, uint32_t outputFormat,
                               uint32_t outputWidth, uint32_t outputHeight, uint32_t transfer, float flow)
{
    if (transfer == kTransferPq || transfer == kTransferScRgb || frameFormat == Format::Rgba16F ||
        frameFormat == Format::Rgba16FTypeless)
        return Surface::Hdr;

    const uint32_t working = WorkingFormat(frameFormat);
    if (working == 0 || transfer != kTransferSrgb)
        return Surface::UnsupportedFormat;

    if (WorkingFormat(outputFormat) != working)
        return Surface::FormatMismatch;

    if (frameWidth != outputWidth || frameHeight != outputHeight)
        return Surface::SizeMismatch;

    const float f = ClampFlow(flow);
    if (frameWidth < 64 || frameHeight < 64 || uint32_t(float(frameWidth) * f) < 64 ||
        uint32_t(float(frameHeight) * f) < 64)
        return Surface::TooSmall;

    return Surface::Ok;
}

inline const char* SurfaceText(Surface s)
{
    switch (s)
    {
    case Surface::Ok:
        return "ok";
    case Surface::UnsupportedFormat:
        return "unsupported swapchain format (8-bit RGBA/BGRA and 10-bit RGB10A2 with an sRGB transfer only)";
    case Surface::FormatMismatch:
        return "FFX's interpolation output is not in the frame's format family";
    case Surface::Hdr:
        return "HDR10/scRGB swapchain: not handled yet";
    case Surface::SizeMismatch:
        return "FFX's interpolation output is not the frame's size";
    case Surface::TooSmall:
        return "frame below 64 px once flow-scaled";
    }
    return "?";
}

} // namespace Lsfg
