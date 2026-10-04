// The two byte-level answers the NR model loaders act on (OptiScaler/dlssnr/DlssNr_PeScan.h), against
// synthetic PE64 images; plus, when run.py finds them on this machine, the real NGX loader and model.
//
// DlssNr_PeScan.h is included as shipped. DriverFromLoaderVersion is sliced out of DlssNr_NgxInfo.cpp
// by run.py and injected at the marker below, because that file needs <windows.h>.

#include "DlssNr_PeScan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace DlssNr::NgxInfo
{
// @@PRODUCTION_SLICES@@
} // namespace DlssNr::NgxInfo

namespace
{
using namespace DlssNr::PeScan;

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what, int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// ------------------------------------------------------------------------------------------------
// A minimal PE32+ image, laid out as the loader maps it (offsets are RVAs).
// ------------------------------------------------------------------------------------------------

constexpr size_t kNt = 0x80;
constexpr size_t kOptional = kNt + 24;
constexpr uint16_t kOptionalSize = 0xF0;
constexpr size_t kSections = kOptional + kOptionalSize;

struct Image
{
    std::vector<uint8_t> bytes;
    uint16_t sectionCount = 0;

    explicit Image(size_t size, uint64_t preferredBase = 0x180000000ull) : bytes(size, 0)
    {
        Put<uint16_t>(0, 0x5A4D);
        Put<int32_t>(0x3C, static_cast<int32_t>(kNt));
        Put<uint32_t>(kNt, 0x4550);
        Put<uint16_t>(kNt + 4, 0x8664);
        Put<uint16_t>(kNt + 6, 0);
        Put<uint16_t>(kNt + 20, kOptionalSize);
        Put<uint16_t>(kOptional, 0x20B);
        Put<uint64_t>(kOptional + 24, preferredBase);
        Put<uint32_t>(kOptional + 56, static_cast<uint32_t>(size));
        Put<uint32_t>(kOptional + 108, 16);
    }

    template <typename T> void Put(size_t offset, T value) { std::memcpy(bytes.data() + offset, &value, sizeof(T)); }

    void PutName(size_t offset, std::string_view name, uint16_t hint = 0x1234)
    {
        Put<uint16_t>(offset, hint);
        std::memcpy(bytes.data() + offset + 2, name.data(), name.size());
        bytes[offset + 2 + name.size()] = 0;
    }

    void PutWide(size_t offset, std::u16string_view text)
    {
        for (size_t i = 0; i < text.size(); ++i)
            Put<uint16_t>(offset + i * 2, static_cast<uint16_t>(text[i]));
        Put<uint16_t>(offset + text.size() * 2, 0);
    }

    void SetImports(uint32_t rva, uint32_t size = 0x100)
    {
        Put<uint32_t>(kOptional + 112 + 8, rva);
        Put<uint32_t>(kOptional + 112 + 12, size);
    }

    void AddSection(uint32_t rva, uint32_t virtualSize, uint32_t characteristics)
    {
        const size_t header = kSections + sectionCount * 40;
        std::memcpy(bytes.data() + header, ".sect\0\0\0", 8);
        Put<uint32_t>(header + 8, virtualSize);
        Put<uint32_t>(header + 12, rva);
        Put<uint32_t>(header + 36, characteristics);
        ++sectionCount;
        Put<uint16_t>(kNt + 6, sectionCount);
    }

    const uint8_t* data() const { return bytes.data(); }
    size_t size() const { return bytes.size(); }
};

constexpr uint32_t kReadOnly = 0x40000040u;
constexpr uint32_t kWritable = 0xC0000040u;

// One import descriptor: names (or ordinals when the entry starts with '#') written at `names`,
// the lookup table at `oft` (0 = bound only), the IAT at `iat`.
struct Import
{
    uint32_t oft;
    uint32_t iat;
    std::vector<std::string> names;
};

void WriteImports(Image& image, uint32_t descriptors, const std::vector<Import>& imports, uint32_t names)
{
    uint32_t cursor = names;
    for (size_t d = 0; d < imports.size(); ++d)
    {
        const auto& imp = imports[d];
        const size_t desc = descriptors + d * 20;
        image.Put<uint32_t>(desc + 0, imp.oft);
        image.Put<uint32_t>(desc + 12, 0x3F00 + static_cast<uint32_t>(d) * 16); // DLL name, never read
        image.Put<uint32_t>(desc + 16, imp.iat);

        for (size_t i = 0; i < imp.names.size(); ++i)
        {
            uint64_t entry = 0;
            if (imp.names[i][0] == '#')
                entry = (1ull << 63) | static_cast<uint64_t>(std::atoi(imp.names[i].c_str() + 1));
            else
            {
                image.PutName(cursor, imp.names[i]);
                entry = cursor;
                cursor += static_cast<uint32_t>(imp.names[i].size() + 4) & ~1u;
            }
            if (imp.oft != 0)
                image.Put<uint64_t>(imp.oft + i * 8, entry);
            image.Put<uint64_t>(imp.iat + i * 8, 0x7FF800000000ull + i); // "bound" addresses
        }
    }
    image.SetImports(descriptors);
}

bool Has(const std::vector<ImportSlot>& slots, size_t offset, bool wide)
{
    for (const auto& s : slots)
        if (s.slotOffset == offset && s.wide == wide)
            return true;
    return false;
}

// ------------------------------------------------------------------------------------------------

void ImportsFound()
{
    Image image(0x4000);
    WriteImports(image, 0x1000,
                 {
                     { 0x1200, 0x1400, { "GetModuleFileNameW", "Sleep", "GetModuleFileNameA" } },
                     { 0x1280, 0x1480, { "MessageBoxW" } },
                 },
                 0x1800);

    std::vector<ImportSlot> slots;
    CHECK(FindCallerPathImports(image.data(), image.size(), slots));
    CHECK(slots.size() == 2);
    CHECK(Has(slots, 0x1400, true));
    CHECK(Has(slots, 0x1410, false));
    CHECK(ImageSize(image.data(), 4096) == 0x4000);
}

void ImportsFromTwoDlls()
{
    // An api-set and kernel32 can both hand out GetModuleFileNameW; every slot is the model's to answer.
    Image image(0x4000);
    WriteImports(image, 0x1000,
                 {
                     { 0x1200, 0x1400, { "GetModuleFileNameW" } },
                     { 0x1280, 0x1480, { "CloseHandle", "GetModuleFileNameW" } },
                 },
                 0x1800);

    std::vector<ImportSlot> slots;
    CHECK(FindCallerPathImports(image.data(), image.size(), slots));
    CHECK(slots.size() == 2);
    CHECK(Has(slots, 0x1400, true));
    CHECK(Has(slots, 0x1488, true));
}

void ImportsNoneMatching()
{
    // A table that parses and holds no caller-path import: true with nothing found, which the loader
    // turns into a refusal of its own ("imports no GetModuleFileNameW/A").
    Image image(0x4000);
    WriteImports(image, 0x1000,
                 { { 0x1200, 0x1400,
                     { "GetModuleFileNameExW", "GetModuleFileNameW2", "getmodulefilenamew", "GetModuleFileName" } } },
                 0x1800);

    std::vector<ImportSlot> slots;
    CHECK(FindCallerPathImports(image.data(), image.size(), slots));
    CHECK(slots.empty());
}

void ImportsOrdinalSkipped()
{
    // Ordinal 0x1802 with bit 63 set: read as an RVA it would land on a real "GetModuleFileNameW" name.
    Image image(0x4000);
    WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "GetModuleFileNameW" } } }, 0x1800);
    image.Put<uint64_t>(0x1200, (1ull << 63) | 0x1800);

    std::vector<ImportSlot> slots;
    CHECK(FindCallerPathImports(image.data(), image.size(), slots));
    CHECK(slots.empty());
}

void ImportsBoundOnlySkipped()
{
    // No lookup table: the names are gone, so the descriptor is passed over and the rest still parse.
    Image image(0x4000);
    WriteImports(image, 0x1000,
                 {
                     { 0, 0x1400, { "GetModuleFileNameW" } },
                     { 0x1280, 0x1480, { "GetModuleFileNameA" } },
                 },
                 0x1800);

    std::vector<ImportSlot> slots;
    CHECK(FindCallerPathImports(image.data(), image.size(), slots));
    CHECK(slots.size() == 1);
    CHECK(Has(slots, 0x1480, false));
}

void ImportsMalformed()
{
    std::vector<ImportSlot> slots;

    {
        Image image(0x4000); // no import directory at all
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // Unterminated: non-zero descriptors all the way to the end of the image.
        Image image(0x1100);
        image.SetImports(0x1000);
        for (size_t d = 0x1000; d + 20 <= image.size(); d += 20)
        {
            image.Put<uint32_t>(d + 0, 0); // bound only, so each one is skipped and the walk goes on
            image.Put<uint32_t>(d + 12, 0x80);
            image.Put<uint32_t>(d + 16, 0x80);
        }
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // A descriptor with a lookup table and no IAT.
        Image image(0x4000);
        WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "GetModuleFileNameW" } } }, 0x1800);
        image.Put<uint32_t>(0x1000 + 16, 0);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // A name RVA outside the image.
        Image image(0x4000);
        WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "GetModuleFileNameW" } } }, 0x1800);
        image.Put<uint64_t>(0x1200, 0x7FFFFF00ull);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // A name that runs to the end of the image with no NUL.
        Image image(0x2000);
        WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "Sleep" } } }, 0x1800);
        image.Put<uint64_t>(0x1200, 0x1FF0);
        std::memset(image.bytes.data() + 0x1FF2, 'A', 0x2000 - 0x1FF2);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // A lookup table that runs off the image before its terminating zero.
        Image image(0x1300);
        image.SetImports(0x1000);
        image.Put<uint32_t>(0x1000 + 0, 0x12F8);
        image.Put<uint32_t>(0x1000 + 12, 0x80);
        image.Put<uint32_t>(0x1000 + 16, 0x1100);
        image.Put<uint64_t>(0x12F8, 0x1234); // an entry, then the end of the image
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        // A matched slot in a misaligned IAT: the loader would write 8 bytes across a boundary.
        Image image(0x4000);
        WriteImports(image, 0x1000, { { 0x1200, 0x1404, { "GetModuleFileNameW" } } }, 0x1800);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        Image image(0x4000); // PE32, not PE32+
        WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "GetModuleFileNameW" } } }, 0x1800);
        image.Put<uint16_t>(kOptional, 0x10B);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
        CHECK(ImageSize(image.data(), 4096) == 0);
    }
    {
        Image image(0x4000); // i386
        WriteImports(image, 0x1000, { { 0x1200, 0x1400, { "GetModuleFileNameW" } } }, 0x1800);
        image.Put<uint16_t>(kNt + 4, 0x14C);
        CHECK(!FindCallerPathImports(image.data(), image.size(), slots));
    }
    {
        Image image(0x4000); // not MZ
        image.Put<uint16_t>(0, 0x0000);
        CHECK(ImageSize(image.data(), 4096) == 0);
    }
    {
        Image image(0x4000); // headers cut short
        CHECK(ImageSize(image.data(), kOptional + 40) == 0);
    }
    {
        Image image(0x4000); // e_lfanew negative
        image.Put<int32_t>(0x3C, -8);
        CHECK(ImageSize(image.data(), 4096) == 0);
    }
    CHECK(slots.empty()); // a refusal never leaves a partial answer behind
}

// ------------------------------------------------------------------------------------------------
// The loader's feature table: 19 absolute pointers to wide strings, in a writable section.
// ------------------------------------------------------------------------------------------------

constexpr uint64_t kBase = 0x180000000ull;

struct TableImage
{
    Image image { 0x6000, kBase };
    uint32_t strings = 0x1000; // .rdata
    uint32_t cursor = 0x1000;

    TableImage()
    {
        image.AddSection(0x1000, 0x1000, kReadOnly);
        image.AddSection(0x3000, 0x2000, kWritable);
    }

    uint64_t String(std::u16string_view text)
    {
        const uint32_t at = cursor;
        image.PutWide(at, text);
        cursor += static_cast<uint32_t>((text.size() + 1) * 2 + 7) & ~7u;
        return kBase + at;
    }

    // Writes a table at `offset`; `name18` of nullptr leaves entry 18 pointing nowhere.
    void Table(size_t offset, const char16_t* name1, const char16_t* name11, const char16_t* name18)
    {
        const uint64_t empty = String(u"");
        for (int i = 0; i < 19; ++i)
            image.Put<uint64_t>(offset + i * 8, empty);
        image.Put<uint64_t>(offset + 1 * 8, String(name1));
        image.Put<uint64_t>(offset + 11 * 8, String(name11));
        image.Put<uint64_t>(offset + 18 * 8, name18 != nullptr ? String(name18) : 0);
    }
};

void TablePresentAndAbsent()
{
    {
        TableImage t;
        t.Table(0x3100, u"dlss", u"dlssg", u"dlssnr");
        size_t at = 0;
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase, &at) == Feature18Route::Present);
        CHECK(at == 0x3100);
    }
    {
        TableImage t;
        t.Table(0x3100, u"dlss", u"dlssg", u"");
        size_t at = 0;
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase, &at) == Feature18Route::Absent);
        CHECK(at == 0x3100);
    }
}

void TableNotRecognised()
{
    {
        TableImage t; // entry 18 names something else
        t.Table(0x3100, u"dlss", u"dlssg", u"dlssnrx");
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        TableImage t; // entry 18 null
        t.Table(0x3100, u"dlss", u"dlssg", nullptr);
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        TableImage t; // entry 11 wrong: not the table
        t.Table(0x3100, u"dlss", u"dlssd", u"dlssnr");
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        TableImage t; // mapped somewhere else than the pointers say
        t.Table(0x3100, u"dlss", u"dlssg", u"dlssnr");
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase + 0x10000) == Feature18Route::Unknown);
    }
    {
        TableImage t; // in a read-only section: a table the loader could not own
        t.Table(0x1800, u"dlss", u"dlssg", u"dlssnr");
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        TableImage t; // cut by the end of its section before entry 18
        t.Table(0x4F80, u"dlss", u"dlssg", u"dlssnr");
        t.image.Put<uint32_t>(kSections + 40 + 8, 0x1F80 + 18 * 8); // .data VirtualSize ends at entry 18
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        TableImage t; // a section header pointing past the image is skipped, not read
        t.image.AddSection(0x90000, 0x1000, kWritable);
        CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase) == Feature18Route::Unknown);
    }
    {
        Image image(0x2000, kBase); // not an image at all
        image.Put<uint16_t>(0, 0);
        CHECK(FindFeature18Route(image.data(), image.size(), kBase) == Feature18Route::Unknown);
    }
}

void TableAfterDecoy()
{
    // A near-match earlier in the section (entry 1 right, entry 11 wrong) must not stop the scan.
    TableImage t;
    t.Table(0x3100, u"dlss", u"xess", u"dlssnr");
    t.Table(0x3400, u"dlss", u"dlssg", u"dlssnr");
    size_t at = 0;
    CHECK(FindFeature18Route(t.image.data(), t.image.size(), kBase, &at) == Feature18Route::Present);
    CHECK(at == 0x3400);
}

// ------------------------------------------------------------------------------------------------

void FaultingPairing()
{
    struct Row
    {
        uint16_t v[4];
        bool faults;
    };
    const Row rows[] = {
        { { 0, 0, 0, 0 }, false },     // unknown is not a verdict
        { { 310, 8, 0, 0 }, true },    // the model dlss5-bridge measured
        { { 310, 8, 0, 1 }, false },   //
        { { 310, 8, 1, 0 }, false },   //
        { { 310, 8, 2, 0 }, false },   // what this repository's test machines run
        { { 310, 7, 9, 9 }, true },    //
        { { 310, 5, 0, 0 }, true },    //
        { { 310, 9, 0, 0 }, false },   //
        { { 309, 99, 9, 9 }, true },   //
        { { 311, 0, 0, 0 }, false },   //
        { { 1, 0, 0, 0 }, true },      // older than any NR model: older than the fix, too
    };
    for (const auto& row : rows)
    {
        if (ModelKnownToFaultOnLoaderRoute(row.v) != row.faults)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: ModelKnownToFaultOnLoaderRoute(%u.%u.%u.%u) != %d\n", row.v[0], row.v[1],
                         row.v[2], row.v[3], row.faults);
        }
        ++g_checks;
        CHECK(KnownFaultingPairing(Feature18Route::Present, row.v) == row.faults);
        CHECK(!KnownFaultingPairing(Feature18Route::Absent, row.v));
        CHECK(!KnownFaultingPairing(Feature18Route::Unknown, row.v));
    }

    CHECK(std::strcmp(Describe(Feature18Route::Present), Describe(Feature18Route::Absent)) != 0);
    CHECK(std::strcmp(Describe(Feature18Route::Present), Describe(Feature18Route::Unknown)) != 0);
    CHECK(std::strstr(Describe(Feature18Route::Present), "routes feature 18 itself") != nullptr);
}

void DriverNumbering()
{
    using DlssNr::NgxInfo::DriverFromLoaderVersion;
    const uint16_t v1664[4] = { 32, 0, 16, 1664 };
    const uint16_t v1686[4] = { 32, 0, 16, 1686 };
    const uint16_t v1656[4] = { 32, 0, 16, 1656 };
    const uint16_t v1691[4] = { 32, 0, 16, 1691 }; // the wine loader shipped with Linux 615.71.09
    const uint16_t v6636[4] = { 32, 0, 15, 6636 }; // Windows 566.36
    const uint16_t v0102[4] = { 32, 0, 16, 102 };  // the minor keeps its leading zero
    const uint16_t wine[4] = { 1, 0, 0, 0 };
    const uint16_t none[4] = { 0, 0, 0, 0 };
    const uint16_t wide[4] = { 32, 0, 100, 0 };
    const uint16_t long4[4] = { 32, 0, 16, 10000 };

    CHECK(DriverFromLoaderVersion(v1664) == "616.64");
    CHECK(DriverFromLoaderVersion(v1686) == "616.86");
    CHECK(DriverFromLoaderVersion(v1656) == "616.56");
    CHECK(DriverFromLoaderVersion(v1691) == "616.91");
    CHECK(DriverFromLoaderVersion(v6636) == "566.36");
    CHECK(DriverFromLoaderVersion(v0102) == "601.02");
    CHECK(DriverFromLoaderVersion(wine).empty());
    CHECK(DriverFromLoaderVersion(none).empty());
    CHECK(DriverFromLoaderVersion(wide).empty());
    CHECK(DriverFromLoaderVersion(long4).empty());
}

// ------------------------------------------------------------------------------------------------
// Local evidence: a real DLL from disk, mapped by section as the Windows loader would (unrelocated,
// so the preferred ImageBase is the base its absolute pointers assume).
// ------------------------------------------------------------------------------------------------

bool MapFromFile(const char* path, std::vector<uint8_t>& mapped, uint64_t& base)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    std::vector<uint8_t> file(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size())))
        return false;

    auto read32 = [&](size_t o) { uint32_t v = 0; std::memcpy(&v, file.data() + o, 4); return v; };
    auto read16 = [&](size_t o) { uint16_t v = 0; std::memcpy(&v, file.data() + o, 2); return v; };

    if (file.size() < 0x400 || read16(0) != 0x5A4D)
        return false;
    const size_t nt = read32(0x3C);
    const size_t optional = nt + 24;
    const uint16_t sections = read16(nt + 6);
    const size_t table = optional + read16(nt + 20);
    std::memcpy(&base, file.data() + optional + 24, 8);
    const uint32_t sizeOfImage = read32(optional + 56);
    const uint32_t sizeOfHeaders = read32(optional + 60);

    mapped.assign(sizeOfImage, 0);
    std::memcpy(mapped.data(), file.data(), std::min<size_t>(sizeOfHeaders, file.size()));
    for (uint16_t s = 0; s < sections; ++s)
    {
        const size_t h = table + s * 40;
        const uint32_t va = read32(h + 12);
        const uint32_t rawSize = read32(h + 16);
        const uint32_t raw = read32(h + 20);
        const size_t count = std::min<size_t>({ rawSize, file.size() - std::min<size_t>(raw, file.size()),
                                                mapped.size() - std::min<size_t>(va, mapped.size()) });
        std::memcpy(mapped.data() + va, file.data() + raw, count);
    }
    return true;
}

void RealLoader(const char* path)
{
    std::vector<uint8_t> mapped;
    uint64_t base = 0;
    if (!MapFromFile(path, mapped, base))
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: could not map %s\n", path);
        return;
    }
    size_t at = 0;
    const auto route = FindFeature18Route(mapped.data(), mapped.size(), base, &at);
    std::printf("local loader %s: %s (table at +0x%zx)\n", path, Describe(route), at);
    // The route may be either -- that depends on the driver -- but the table must be recognised: an
    // Unknown here means the log line in the field says nothing, which is the gap this closes.
    CHECK(route != Feature18Route::Unknown);
}

void RealModel(const char* path)
{
    std::vector<uint8_t> mapped;
    uint64_t base = 0;
    if (!MapFromFile(path, mapped, base))
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: could not map %s\n", path);
        return;
    }
    std::vector<ImportSlot> slots;
    const bool parsed = FindCallerPathImports(mapped.data(), mapped.size(), slots);
    size_t wide = 0;
    for (const auto& s : slots)
        wide += s.wide ? 1 : 0;
    std::printf("local model %s: import table %s, %zu GetModuleFileNameW slot(s), %zu ...A slot(s)\n", path,
                parsed ? "parsed" : "REFUSED", wide, slots.size() - wide);
    // ModelLoader=direct depends on both halves: a parsable table and at least one slot to answer from.
    CHECK(parsed);
    CHECK(!slots.empty());
}
} // namespace

int main(int argc, char** argv)
{
    ImportsFound();
    ImportsFromTwoDlls();
    ImportsNoneMatching();
    ImportsOrdinalSkipped();
    ImportsBoundOnlySkipped();
    ImportsMalformed();
    TablePresentAndAbsent();
    TableNotRecognised();
    TableAfterDecoy();
    FaultingPairing();
    DriverNumbering();

    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (std::strcmp(argv[i], "--loader") == 0)
            RealLoader(argv[i + 1]);
        else if (std::strcmp(argv[i], "--model") == 0)
            RealModel(argv[i + 1]);
    }

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("PASS: %d checks (PE import scan, feature-18 table scan, faulting pairing, driver numbering)\n",
                g_checks);
    return 0;
}
