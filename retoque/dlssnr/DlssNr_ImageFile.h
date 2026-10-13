// Image files for the comparison capture (docs/dlssnr/design/comparison-capture.md, section 4.3).
//
// A PNG writer, a PFM header, and the row conversions from the surface formats the pass works on. Nothing in the
// tree wrote images before this, and WIC is Windows-only, COM, and not something this machine can show is present
// under Wine, so these are our own and dependency-free: no zlib, no WIC, no D3D. tests/nr-capture decodes what they
// write with Python's zlib and checks every byte.
//
// The PNG is uncompressed on purpose: stored deflate blocks inside a valid zlib stream. 3440x1440 RGB is 14.9 MB a
// file, which is acceptable for a deliberate capture, and it removes the only complex part of an encoder. A
// compressing deflate can replace the block framing later without touching the interface.

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace DlssNr::ImageFile
{
static_assert(std::endian::native == std::endian::little, "the row conversions read little-endian surfaces");

// --- Checksums -------------------------------------------------------------------------------------------------

// CRC-32 as PNG uses it (ISO 3309, reflected 0xEDB88320), over each chunk's type and data.
inline constexpr std::array<uint32_t, 256> kCrcTable = []
{
    std::array<uint32_t, 256> table {};
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}();

class Crc32
{
  public:
    void Update(const uint8_t* data, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            state_ = kCrcTable[(state_ ^ data[i]) & 0xFFu] ^ (state_ >> 8);
    }
    uint32_t Value() const { return ~state_; }

  private:
    uint32_t state_ = 0xFFFFFFFFu;
};

// Adler-32 (RFC 1950), the zlib stream's trailer, over the uncompressed bytes.
class Adler32
{
  public:
    void Update(const uint8_t* data, size_t size)
    {
        // 5552 is the most bytes the sums can take before b overflows 32 bits.
        while (size > 0)
        {
            size_t run = size < 5552 ? size : 5552;
            size -= run;
            while (run-- > 0)
            {
                a_ += *data++;
                b_ += a_;
            }
            a_ %= 65521u;
            b_ %= 65521u;
        }
    }
    uint32_t Value() const { return (b_ << 16) | a_; }

  private:
    uint32_t a_ = 1;
    uint32_t b_ = 0;
};

inline void PutBe32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

// --- PNG -------------------------------------------------------------------------------------------------------

// Whether text can go into a tEXt chunk as written: printable ASCII only (a subset of Latin-1, which is what the
// chunk is defined over), and a keyword of 1 to 79 characters without leading, trailing or doubled spaces.
inline bool PngKeywordValid(std::string_view keyword)
{
    if (keyword.empty() || keyword.size() > 79 || keyword.front() == ' ' || keyword.back() == ' ')
        return false;
    for (size_t i = 0; i < keyword.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(keyword[i]);
        if (c < 0x20 || c > 0x7E || (c == ' ' && keyword[i + 1] == ' '))
            return false;
    }
    return true;
}

inline bool PngTextValid(std::string_view text)
{
    for (const char ch : text)
    {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

// Writes an RGB PNG, 8 or 16 bits a channel, one row at a time, into sink: any callable taking
// (const uint8_t* data, size_t size) and returning false on failure. Rows go straight from the caller to the sink
// through one 64 KiB block, so the whole image is never held twice.
//
// Layout: signature; IHDR (colour type 2); sRGB when asked (8-bit display-referred content); tEXt with the
// provenance, before the image so a reader that stops early still has it; IDAT chunks, one stored deflate block
// each, the first opening the zlib stream and the last closing it with the Adler-32; IEND. Filter 0 on every row.
template <class Sink> class PngStream
{
  public:
    static constexpr size_t kBlock = 65535; // the largest stored deflate block

    explicit PngStream(Sink& sink) : sink_(sink) {}

    bool Begin(uint32_t width, uint32_t height, uint32_t bitDepth, bool srgb, std::string_view keyword,
               std::string_view text)
    {
        if (begun_ || width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu ||
            (bitDepth != 8 && bitDepth != 16) || !PngKeywordValid(keyword) || !PngTextValid(text))
            return Fail();

        begun_ = true;
        width_ = width;
        height_ = height;
        rowBytes_ = static_cast<size_t>(width) * 3u * (bitDepth / 8u);
        block_.resize(kBlock);

        static constexpr uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (!Put(kSignature, sizeof(kSignature)))
            return false;

        uint8_t ihdr[13] {};
        PutBe32(ihdr, width);
        PutBe32(ihdr + 4, height);
        ihdr[8] = static_cast<uint8_t>(bitDepth);
        ihdr[9] = 2; // truecolour, no alpha: swapchain alpha is often garbage
        if (!Chunk("IHDR", { { ihdr, sizeof(ihdr) } }))
            return false;

        if (srgb)
        {
            const uint8_t intent = 0; // perceptual
            if (!Chunk("sRGB", { { &intent, 1 } }))
                return false;
        }

        if (!text.empty())
        {
            const uint8_t zero = 0;
            if (!Chunk("tEXt", { { reinterpret_cast<const uint8_t*>(keyword.data()), keyword.size() },
                                 { &zero, 1 },
                                 { reinterpret_cast<const uint8_t*>(text.data()), text.size() } }))
                return false;
        }

        return true;
    }

    size_t RowBytes() const { return rowBytes_; }

    // One row of samples in PNG order: R, G, B, and big-endian for 16 bits.
    bool Row(const uint8_t* samples)
    {
        if (!begun_ || failed_ || rows_ >= height_)
            return Fail();
        const uint8_t filterNone = 0;
        if (!Feed(&filterNone, 1) || !Feed(samples, rowBytes_))
            return false;
        ++rows_;
        return true;
    }

    bool End()
    {
        if (!begun_ || failed_ || rows_ != height_ || ended_)
            return Fail();
        ended_ = true;
        // The last block always holds at least the final row, because a full block is only written once more
        // data arrives; so the stream's final block is never an empty one tacked on.
        if (!FlushBlock(true))
            return false;
        return Chunk("IEND", {});
    }

    bool Failed() const { return failed_; }

  private:
    struct Part
    {
        const uint8_t* data;
        size_t size;
    };

    bool Fail()
    {
        failed_ = true;
        return false;
    }

    bool Put(const uint8_t* data, size_t size)
    {
        if (failed_)
            return false;
        if (size != 0 && !sink_(data, size))
            return Fail();
        return true;
    }

    bool Chunk(const char (&type)[5], std::initializer_list<Part> parts)
    {
        size_t length = 0;
        for (const Part& part : parts)
            length += part.size;
        if (length > 0x7FFFFFFFu)
            return Fail();

        uint8_t head[8];
        PutBe32(head, static_cast<uint32_t>(length));
        std::memcpy(head + 4, type, 4);
        Crc32 crc;
        crc.Update(head + 4, 4);
        if (!Put(head, sizeof(head)))
            return false;
        for (const Part& part : parts)
        {
            crc.Update(part.data, part.size);
            if (!Put(part.data, part.size))
                return false;
        }
        uint8_t tail[4];
        PutBe32(tail, crc.Value());
        return Put(tail, sizeof(tail));
    }

    bool Feed(const uint8_t* data, size_t size)
    {
        adler_.Update(data, size);
        while (size > 0)
        {
            if (used_ == kBlock && !FlushBlock(false))
                return false;
            const size_t take = size < kBlock - used_ ? size : kBlock - used_;
            std::memcpy(block_.data() + used_, data, take);
            used_ += take;
            data += take;
            size -= take;
        }
        return true;
    }

    bool FlushBlock(bool final)
    {
        uint8_t head[7];
        size_t headSize = 0;
        if (!zlibOpened_)
        {
            // CMF 0x78: deflate, 32 KiB window. FLG 0x01: no dictionary, "fastest", and (0x78 << 8 | 0x01) % 31 == 0.
            head[headSize++] = 0x78;
            head[headSize++] = 0x01;
            zlibOpened_ = true;
        }
        // A stored block: BFINAL in bit 0, BTYPE 00, padded to the byte; LEN and its one's complement, little-endian.
        const uint16_t length = static_cast<uint16_t>(used_);
        const uint16_t complement = static_cast<uint16_t>(~length);
        head[headSize++] = final ? 1 : 0;
        head[headSize++] = static_cast<uint8_t>(length & 0xFFu);
        head[headSize++] = static_cast<uint8_t>(length >> 8);
        head[headSize++] = static_cast<uint8_t>(complement & 0xFFu);
        head[headSize++] = static_cast<uint8_t>(complement >> 8);

        uint8_t trailer[4];
        size_t trailerSize = 0;
        if (final)
        {
            PutBe32(trailer, adler_.Value());
            trailerSize = 4;
        }

        const bool ok = Chunk("IDAT", { { head, headSize }, { block_.data(), used_ }, { trailer, trailerSize } });
        used_ = 0;
        return ok;
    }

    Sink& sink_;
    std::vector<uint8_t> block_;
    size_t used_ = 0;
    size_t rowBytes_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t rows_ = 0;
    Adler32 adler_;
    bool begun_ = false;
    bool ended_ = false;
    bool zlibOpened_ = false;
    bool failed_ = false;
};

// --- PFM -------------------------------------------------------------------------------------------------------

// Portable float map, three channels: "PF", the size, and a negative scale meaning little-endian. The rows that
// follow go bottom to top, three float32 a pixel. It has no metadata slot, which is why every capture also gets a
// JSON sidecar.
inline std::string PfmHeader(uint32_t width, uint32_t height)
{
    char text[64];
    std::snprintf(text, sizeof(text), "PF\n%u %u\n-1.0\n", width, height);
    return text;
}

// --- Surface formats -------------------------------------------------------------------------------------------

// DXGI_FORMAT numbers, written out so this header needs no Windows SDK. DlssNr_PairCapture.cpp asserts each equals
// its DXGI_FORMAT counterpart.
namespace Dxgi
{
constexpr uint32_t R32G32B32A32_TYPELESS = 1;
constexpr uint32_t R32G32B32A32_FLOAT = 2;
constexpr uint32_t R16G16B16A16_TYPELESS = 9;
constexpr uint32_t R16G16B16A16_FLOAT = 10;
constexpr uint32_t R10G10B10A2_TYPELESS = 23;
constexpr uint32_t R10G10B10A2_UNORM = 24;
constexpr uint32_t R11G11B10_FLOAT = 26;
constexpr uint32_t R8G8B8A8_TYPELESS = 27;
constexpr uint32_t R8G8B8A8_UNORM = 28;
constexpr uint32_t R8G8B8A8_UNORM_SRGB = 29;
constexpr uint32_t B8G8R8A8_UNORM = 87;
constexpr uint32_t B8G8R8A8_TYPELESS = 90;
constexpr uint32_t B8G8R8A8_UNORM_SRGB = 91;
} // namespace Dxgi

// How a pixel is laid out in the readback, which is what the conversions care about.
enum class Source : uint8_t
{
    Unsupported,
    Rgba8,   // R8G8B8A8, any variant
    Bgra8,   // B8G8R8A8, any variant
    Rgb10a2, // R10G10B10A2_UNORM
    Rgba16f, // R16G16B16A16_FLOAT
    Rgba32f, // R32G32B32A32_FLOAT
    Rg11b10f // R11G11B10_FLOAT
};

inline Source SourceFor(uint32_t format)
{
    switch (format)
    {
    case Dxgi::R8G8B8A8_TYPELESS:
    case Dxgi::R8G8B8A8_UNORM:
    case Dxgi::R8G8B8A8_UNORM_SRGB:
        return Source::Rgba8;
    case Dxgi::B8G8R8A8_TYPELESS:
    case Dxgi::B8G8R8A8_UNORM:
    case Dxgi::B8G8R8A8_UNORM_SRGB:
        return Source::Bgra8;
    case Dxgi::R10G10B10A2_TYPELESS:
    case Dxgi::R10G10B10A2_UNORM:
        return Source::Rgb10a2;
    case Dxgi::R16G16B16A16_TYPELESS:
    case Dxgi::R16G16B16A16_FLOAT:
        return Source::Rgba16f;
    case Dxgi::R32G32B32A32_TYPELESS:
    case Dxgi::R32G32B32A32_FLOAT:
        return Source::Rgba32f;
    case Dxgi::R11G11B10_FLOAT:
        return Source::Rg11b10f;
    default:
        return Source::Unsupported;
    }
}

inline uint32_t BytesPerPixel(Source source)
{
    switch (source)
    {
    case Source::Rgba8:
    case Source::Bgra8:
    case Source::Rgb10a2:
    case Source::Rg11b10f:
        return 4;
    case Source::Rgba16f:
        return 8;
    case Source::Rgba32f:
        return 16;
    default:
        return 0;
    }
}

// The format's name for the provenance record, or "dxgi-<number>" for one this table does not know.
inline std::string FormatName(uint32_t format)
{
    switch (format)
    {
    case Dxgi::R32G32B32A32_TYPELESS:
        return "R32G32B32A32_TYPELESS";
    case Dxgi::R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case Dxgi::R16G16B16A16_TYPELESS:
        return "R16G16B16A16_TYPELESS";
    case Dxgi::R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case Dxgi::R10G10B10A2_TYPELESS:
        return "R10G10B10A2_TYPELESS";
    case Dxgi::R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case Dxgi::R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case Dxgi::R8G8B8A8_TYPELESS:
        return "R8G8B8A8_TYPELESS";
    case Dxgi::R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case Dxgi::R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case Dxgi::B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case Dxgi::B8G8R8A8_TYPELESS:
        return "B8G8R8A8_TYPELESS";
    case Dxgi::B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    default:
        return "dxgi-" + std::to_string(format);
    }
}

enum class FileKind : uint8_t
{
    None,
    Png8,  // 8-bit RGB PNG
    Png16, // 16-bit RGB PNG
    Pfm    // three-channel float32, plus the sidecar
};

inline const char* FileKindName(FileKind kind)
{
    switch (kind)
    {
    case FileKind::Png8:
        return "png8";
    case FileKind::Png16:
        return "png16";
    case FileKind::Pfm:
        return "pfm";
    default:
        return "none";
    }
}

inline const char* Extension(FileKind kind) { return kind == FileKind::Pfm ? "pfm" : "png"; }

// The refusal for a surface the table below does not cover. A literal: the menu translates it.
inline constexpr const char* kRefusalFormat = "This frame's format is not one the capture can write yet.";

// Which file a surface becomes (comparison-capture.md, section 4.3). linearHdr is the pass's own answer for the
// frame (isHdrBuffer): a float surface holding a display-referred frame -- the bridge's working colour, or a game
// that renders its tone-mapped frame in float -- is a picture and is quantised into an 8-bit PNG; a float surface of
// linear light is kept as numbers, because a PNG would hide the range. Nothing outside the table is guessed.
struct Plan
{
    FileKind kind = FileKind::None;
    Source source = Source::Unsupported;
    bool srgb = false;                    // write the sRGB chunk
    const char* quantisedFrom = nullptr;  // a float surface written as 8-bit codes: which one
    const char* refusal = kRefusalFormat; // null when the plan is usable
};

inline Plan PlanFor(uint32_t format, bool linearHdr)
{
    Plan plan;
    plan.source = SourceFor(format);
    switch (plan.source)
    {
    case Source::Rgba8:
    case Source::Bgra8:
        plan.kind = FileKind::Png8;
        plan.srgb = true;
        break;
    case Source::Rgb10a2:
        // The ten-bit code k becomes (k << 6) | (k >> 4): lossless and standard. No sRGB chunk: a ten-bit present may
        // be HDR10 once the present hosts accept it, and a chunk would then be a false statement.
        plan.kind = FileKind::Png16;
        break;
    case Source::Rgba16f:
    case Source::Rgba32f:
    case Source::Rg11b10f:
        if (linearHdr)
        {
            plan.kind = FileKind::Pfm;
        }
        else
        {
            plan.kind = FileKind::Png8;
            plan.srgb = true;
            plan.quantisedFrom = plan.source == Source::Rgba16f   ? "fp16"
                                 : plan.source == Source::Rgba32f ? "fp32"
                                                                  : "r11g11b10f";
        }
        break;
    default:
        return plan;
    }
    plan.refusal = nullptr;
    return plan;
}

// --- Conversions -----------------------------------------------------------------------------------------------
//
// Bit-level wherever a NaN or an infinity can appear: the DLL is built with /fp:fast, under which a comparison with
// a NaN is not something to rely on. A NaN in a capture is information, not an error, so the float paths keep it.

inline uint32_t LoadLe32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline bool FiniteBits(float value) { return (std::bit_cast<uint32_t>(value) & 0x7F800000u) != 0x7F800000u; }

// IEEE half to float, exactly: subnormals normalised, infinities and NaN payloads kept.
inline float HalfToFloat(uint16_t half)
{
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            bits = sign;
        }
        else
        {
            uint32_t shift = 0;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                ++shift;
            }
            // value = 1.m * 2^(-14 - shift + 1) ... with the hidden bit at 0x400 after shift steps
            bits = sign | ((113u - shift) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31)
    {
        bits = sign | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    return std::bit_cast<float>(bits);
}

// The unsigned small floats of R11G11B10_FLOAT: five exponent bits, six (red, green) or five (blue) mantissa bits,
// no sign, bias 15. Exactly representable in float, so the conversion is exact.
inline float SmallFloatToFloat(uint32_t value, uint32_t mantissaBits)
{
    const uint32_t mantissa = value & ((1u << mantissaBits) - 1u);
    const uint32_t exponent = (value >> mantissaBits) & 0x1Fu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            return 0.0f;
        uint32_t m = mantissa;
        uint32_t shift = 0;
        while ((m & (1u << mantissaBits)) == 0)
        {
            m <<= 1;
            ++shift;
        }
        bits = ((113u - shift) << 23) | ((m & ((1u << mantissaBits) - 1u)) << (23 - mantissaBits));
    }
    else if (exponent == 31)
    {
        bits = 0x7F800000u | (mantissa << (23 - mantissaBits));
    }
    else
    {
        bits = ((exponent + 112u) << 23) | (mantissa << (23 - mantissaBits));
    }
    return std::bit_cast<float>(bits);
}

inline void Rg11b10ToFloat(uint32_t packed, float out[3])
{
    out[0] = SmallFloatToFloat(packed & 0x7FFu, 6);
    out[1] = SmallFloatToFloat((packed >> 11) & 0x7FFu, 6);
    out[2] = SmallFloatToFloat((packed >> 22) & 0x3FFu, 5);
}

// round(saturate(v) * 255), with NaN as 0 and decided on the bits.
inline uint8_t Quantise8(float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u || (bits & 0x80000000u) != 0)
        return 0; // NaN, or anything negative (negative zero included)
    if (bits >= 0x3F800000u)
        return 255; // 1.0 and above, +Inf included
    return static_cast<uint8_t>(static_cast<int>(value * 255.0f + 0.5f));
}

inline void ReadFloat3(Source source, const uint8_t* pixel, float out[3])
{
    switch (source)
    {
    case Source::Rgba16f:
        for (int c = 0; c < 3; ++c)
        {
            uint16_t half;
            std::memcpy(&half, pixel + 2 * c, 2);
            out[c] = HalfToFloat(half);
        }
        break;
    case Source::Rgba32f:
        std::memcpy(out, pixel, 12);
        break;
    case Source::Rg11b10f:
        Rg11b10ToFloat(LoadLe32(pixel), out);
        break;
    default:
        out[0] = out[1] = out[2] = 0.0f;
        break;
    }
}

// One row into 8-bit RGB. Alpha is dropped; BGRA is swizzled; float is quantised. False for a source that is not
// written as 8 bits.
inline bool ToRgb8(Source source, const uint8_t* in, uint32_t width, uint8_t* out)
{
    switch (source)
    {
    case Source::Rgba8:
        for (uint32_t x = 0; x < width; ++x, in += 4, out += 3)
        {
            out[0] = in[0];
            out[1] = in[1];
            out[2] = in[2];
        }
        return true;
    case Source::Bgra8:
        for (uint32_t x = 0; x < width; ++x, in += 4, out += 3)
        {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
        }
        return true;
    case Source::Rgba16f:
    case Source::Rgba32f:
    case Source::Rg11b10f:
    {
        const uint32_t stride = BytesPerPixel(source);
        for (uint32_t x = 0; x < width; ++x, in += stride, out += 3)
        {
            float rgb[3];
            ReadFloat3(source, in, rgb);
            out[0] = Quantise8(rgb[0]);
            out[1] = Quantise8(rgb[1]);
            out[2] = Quantise8(rgb[2]);
        }
        return true;
    }
    default:
        return false;
    }
}

// The ten-bit code widened to sixteen by replicating its top bits, so 0 stays 0 and 1023 becomes 65535.
inline uint16_t Widen10(uint32_t code) { return static_cast<uint16_t>((code << 6) | (code >> 4)); }

// One row into 16-bit RGB, big-endian as PNG wants it. Only R10G10B10A2 is written as 16 bits.
inline bool ToRgb16(Source source, const uint8_t* in, uint32_t width, uint8_t* out)
{
    if (source != Source::Rgb10a2)
        return false;
    for (uint32_t x = 0; x < width; ++x, in += 4, out += 6)
    {
        const uint32_t v = LoadLe32(in);
        for (int c = 0; c < 3; ++c)
        {
            const uint16_t wide = Widen10((v >> (10 * c)) & 0x3FFu);
            out[2 * c] = static_cast<uint8_t>(wide >> 8);
            out[2 * c + 1] = static_cast<uint8_t>(wide & 0xFFu);
        }
    }
    return true;
}

// One row into float32 RGB for the PFM, values exactly as stored (NaN and infinities kept).
inline bool ToRgbF32(Source source, const uint8_t* in, uint32_t width, float* out)
{
    if (source != Source::Rgba16f && source != Source::Rgba32f && source != Source::Rg11b10f)
        return false;
    const uint32_t stride = BytesPerPixel(source);
    for (uint32_t x = 0; x < width; ++x, in += stride, out += 3)
        ReadFloat3(source, in, out);
    return true;
}

// The mean over every channel of every pixel of what is written, so a dark capture is labelled rather than thrown
// away (comparison-capture.md, section 4.5): codes over their maximum for a PNG, linear values for a PFM, where
// values that are not finite are counted instead of summed.
struct Mean
{
    double sum = 0.0;
    uint64_t count = 0;
    uint64_t nonFinite = 0;

    void Add(float value)
    {
        if (!FiniteBits(value))
        {
            ++nonFinite;
            return;
        }
        sum += value;
        ++count;
    }

    double Value() const { return count != 0 ? sum / static_cast<double>(count) : 0.0; }
};

inline void AccumulateRgb8(const uint8_t* rgb, uint32_t width, Mean& mean)
{
    for (uint32_t i = 0; i < width * 3u; ++i)
        mean.Add(static_cast<float>(rgb[i]) / 255.0f);
}

inline void AccumulateRgb16(const uint8_t* rgb, uint32_t width, Mean& mean)
{
    for (uint32_t i = 0; i < width * 3u; ++i)
        mean.Add(static_cast<float>((rgb[2 * i] << 8) | rgb[2 * i + 1]) / 65535.0f);
}

inline void AccumulateRgbF32(const float* rgb, uint32_t width, Mean& mean)
{
    for (uint32_t i = 0; i < width * 3u; ++i)
        mean.Add(rgb[i]);
}

// --- A readback as rows ----------------------------------------------------------------------------------------

// A surface as a readback holds it: Height rows of Width pixels, RowPitch bytes apart from Offset. The pitch is a
// multiple of 256 (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT), so a row usually ends in padding, which no file gets.
struct Rows
{
    const uint8_t* base = nullptr;
    uint64_t offset = 0;
    uint32_t rowPitch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0; // the whole buffer

    const uint8_t* Row(uint32_t y) const { return base + offset + static_cast<size_t>(y) * rowPitch; }

    // Every row ends inside the buffer. GetCopyableFootprints promises it; this checks it rather than trust it.
    bool Fits(Source source) const
    {
        const uint64_t rowBytes = static_cast<uint64_t>(width) * BytesPerPixel(source);
        return base != nullptr && width != 0 && height != 0 && rowBytes != 0 && rowBytes <= rowPitch &&
               offset + static_cast<uint64_t>(height - 1) * rowPitch + rowBytes <= bytes;
    }
};

// One pass over the surface in the file's terms, for the record's mean.
inline Mean MeanOf(const Rows& rows, const Plan& plan)
{
    Mean mean;
    std::vector<uint8_t> bytes(static_cast<size_t>(rows.width) * 6);
    std::vector<float> floats(static_cast<size_t>(rows.width) * 3);
    for (uint32_t y = 0; y < rows.height; ++y)
    {
        switch (plan.kind)
        {
        case FileKind::Png8:
            ToRgb8(plan.source, rows.Row(y), rows.width, bytes.data());
            AccumulateRgb8(bytes.data(), rows.width, mean);
            break;
        case FileKind::Png16:
            ToRgb16(plan.source, rows.Row(y), rows.width, bytes.data());
            AccumulateRgb16(bytes.data(), rows.width, mean);
            break;
        case FileKind::Pfm:
            ToRgbF32(plan.source, rows.Row(y), rows.width, floats.data());
            AccumulateRgbF32(floats.data(), rows.width, mean);
            break;
        default:
            break;
        }
    }
    return mean;
}

// The whole file for one surface, into sink: a PNG carrying text in its tEXt chunk, or a PFM (whose text goes in the
// sidecar). False on a sink failure, a plan with no file, or rows that do not fit their buffer.
template <class Sink>
bool WriteImage(Sink& sink, const Rows& rows, const Plan& plan, std::string_view keyword, std::string_view text)
{
    if (plan.kind == FileKind::None || !rows.Fits(plan.source))
        return false;

    if (plan.kind == FileKind::Pfm)
    {
        const std::string header = PfmHeader(rows.width, rows.height);
        if (!sink(reinterpret_cast<const uint8_t*>(header.data()), header.size()))
            return false;
        std::vector<float> row(static_cast<size_t>(rows.width) * 3);
        for (uint32_t y = rows.height; y-- > 0;) // bottom to top
        {
            if (!ToRgbF32(plan.source, rows.Row(y), rows.width, row.data()) ||
                !sink(reinterpret_cast<const uint8_t*>(row.data()), row.size() * sizeof(float)))
                return false;
        }
        return true;
    }

    PngStream<Sink> png(sink);
    const bool sixteen = plan.kind == FileKind::Png16;
    if (!png.Begin(rows.width, rows.height, sixteen ? 16 : 8, plan.srgb, keyword, text))
        return false;
    std::vector<uint8_t> row(png.RowBytes());
    for (uint32_t y = 0; y < rows.height; ++y)
    {
        const bool converted = sixteen ? ToRgb16(plan.source, rows.Row(y), rows.width, row.data())
                                       : ToRgb8(plan.source, rows.Row(y), rows.width, row.data());
        if (!converted || !png.Row(row.data()))
            return false;
    }
    return png.End();
}
} // namespace DlssNr::ImageFile
