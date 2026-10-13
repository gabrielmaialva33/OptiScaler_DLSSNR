#pragma once

// Two questions asked of a mapped PE64 image, answered from bytes alone.
//
// Header-only and free of <windows.h> on purpose: both answers decide whether the model is loaded at
// all, so both are tested on the host tier (tests/nr-model-loader) against synthetic images rather
// than trusted to a real driver. Offsets are relative to the image base, which for a loaded module
// is the same thing as an RVA.
//
// 1. FindCallerPathImports: where the model imports GetModuleFileNameW/A. The model's caller check
//    turns its return address into a module (RtlPcToFileHeader) and that module into a path
//    (GetModuleFileName), and refuses a path without "nvngx.dll" in it. The first half cannot be
//    faked from outside; the second is an ordinary import slot. The direct model loader
//    (DlssNr_DirectRuntime) answers through those slots instead of shipping a DLL named to pass.
//    Resolved by name from the loaded image, never by offset, so a rebuilt model that moves its
//    imports still works. The approach is wilsjo2's (OptiScaler-DLSSNR-PreSR-Multipass, v0.8.1-0.8.3,
//    GPL-3, `DlssNr_RuntimeImports.h`); this is a rewrite against our own minimal structures.
//
// 2. FindFeature18Route: whether the driver's NGX loader (_nvngx.dll) routes feature 18 itself. From
//    NVIDIA 32.0.16.1664 (616.64) the loader's 19-entry feature-id -> snippet-name table names
//    L"dlssnr" at index 18, where 32.0.16.1656 had L"". The table layout and the pattern that finds it
//    (entry 1 "dlss", entry 11 "dlssg", entry 18 "dlssnr") are NIGos/dlss5-bridge's (MIT,
//    `PatchNgxFeatureTable`); here it is only read, never written.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace DlssNr::PeScan
{

struct ImportSlot
{
    size_t slotOffset = 0; // offset of the 8-byte IAT entry from the image base
    bool wide = false;     // GetModuleFileNameW rather than ...A
};

enum class Feature18Route
{
    Unknown, // not a PE64 image we could read, or no table matched
    Absent,  // the table is there and entry 18 names nothing (32.0.16.1656 and earlier)
    Present, // entry 18 names "dlssnr": the loader creates feature 18 itself (32.0.16.1664 and later)
};

namespace detail
{
template <typename T> bool Read(const uint8_t* image, size_t size, size_t offset, T& out)
{
    if (offset > size || sizeof(T) > size - offset)
        return false;
    std::memcpy(&out, image + offset, sizeof(T));
    return true;
}

constexpr uint16_t kDosMagic = 0x5A4D;     // "MZ"
constexpr uint32_t kNtSignature = 0x4550;  // "PE\0\0"
constexpr uint16_t kMachineAmd64 = 0x8664; //
constexpr uint16_t kPe32Plus = 0x20B;      // optional header magic for PE32+
constexpr uint32_t kScnMemWrite = 0x80000000u;

struct Headers
{
    size_t nt = 0;            // offset of the NT headers
    size_t optional = 0;      // offset of the optional header
    uint16_t sections = 0;    // number of section headers
    size_t sectionTable = 0;  // offset of the first section header
    uint32_t sizeOfImage = 0; //
    uint32_t importRva = 0;   // import directory
    uint32_t importSize = 0;  //
    uint64_t preferredBase = 0;
};

inline bool ParseHeaders(const uint8_t* image, size_t size, Headers& h)
{
    uint16_t magic = 0;
    int32_t lfanew = 0;
    if (!Read(image, size, 0, magic) || magic != kDosMagic || !Read(image, size, 0x3C, lfanew) || lfanew < 0)
        return false;

    h.nt = static_cast<size_t>(lfanew);
    uint32_t signature = 0;
    uint16_t machine = 0;
    uint16_t optionalSize = 0;
    if (!Read(image, size, h.nt, signature) || signature != kNtSignature || !Read(image, size, h.nt + 4, machine) ||
        machine != kMachineAmd64 || !Read(image, size, h.nt + 6, h.sections) ||
        !Read(image, size, h.nt + 20, optionalSize))
        return false;

    h.optional = h.nt + 24;
    uint16_t optionalMagic = 0;
    uint32_t rvaCount = 0;
    // PE32+ optional header: ImageBase at 24, SizeOfImage at 56, NumberOfRvaAndSizes at 108, then the
    // data directories at 112; the import directory is entry 1.
    if (optionalSize < 112 + 2 * 8 || !Read(image, size, h.optional, optionalMagic) || optionalMagic != kPe32Plus ||
        !Read(image, size, h.optional + 24, h.preferredBase) || !Read(image, size, h.optional + 56, h.sizeOfImage) ||
        !Read(image, size, h.optional + 108, rvaCount) || rvaCount < 2 ||
        !Read(image, size, h.optional + 112 + 8, h.importRva) ||
        !Read(image, size, h.optional + 112 + 8 + 4, h.importSize))
        return false;

    h.sectionTable = h.optional + optionalSize;
    return true;
}
} // namespace detail

// SizeOfImage from the headers, or 0 when they do not read as a PE64 image.
inline uint32_t ImageSize(const uint8_t* image, size_t readable)
{
    detail::Headers h;
    return detail::ParseHeaders(image, readable, h) ? h.sizeOfImage : 0;
}

// Every IAT slot importing GetModuleFileNameW or GetModuleFileNameA by name, from any DLL. Returns
// false for an image whose import table does not parse -- malformed, unterminated, or pointing outside
// the image -- so a half-understood table is never patched. Ordinal and bound-only imports are skipped:
// they carry no names to match.
inline bool FindCallerPathImports(const uint8_t* image, size_t size, std::vector<ImportSlot>& out)
{
    out.clear();
    detail::Headers h;
    if (!detail::ParseHeaders(image, size, h) || h.importRva == 0)
        return false;

    constexpr size_t kDescriptor = 20; // IMAGE_IMPORT_DESCRIPTOR
    std::vector<ImportSlot> found;

    for (size_t d = h.importRva; d + kDescriptor <= size; d += kDescriptor)
    {
        uint32_t originalFirstThunk = 0;
        uint32_t name = 0;
        uint32_t firstThunk = 0;
        if (!detail::Read(image, size, d, originalFirstThunk) || !detail::Read(image, size, d + 12, name) ||
            !detail::Read(image, size, d + 16, firstThunk))
            return false;

        if (originalFirstThunk == 0 && name == 0 && firstThunk == 0)
        {
            out = std::move(found);
            return true;
        }

        if (firstThunk == 0)
            return false;

        if (originalFirstThunk == 0)
            continue; // bound only: no names left to inspect

        for (size_t i = 0;; ++i)
        {
            uint64_t entry = 0;
            const size_t nameSlot = static_cast<size_t>(originalFirstThunk) + i * 8;
            const size_t iatSlot = static_cast<size_t>(firstThunk) + i * 8;
            if (!detail::Read(image, size, nameSlot, entry) || iatSlot > size || size - iatSlot < 8)
                return false;

            if (entry == 0)
                break;

            if ((entry >> 63) != 0)
                continue; // by ordinal

            // IMAGE_IMPORT_BY_NAME: a 2-byte hint, then the NUL-terminated name.
            const size_t nameOffset = static_cast<size_t>(entry & 0x7FFFFFFFull) + 2;
            if (nameOffset >= size)
                return false;

            const auto* text = reinterpret_cast<const char*>(image + nameOffset);
            const auto* end = static_cast<const char*>(std::memchr(text, 0, size - nameOffset));
            if (end == nullptr)
                return false;

            const std::string_view imported(text, static_cast<size_t>(end - text));
            if (imported == "GetModuleFileNameW" || imported == "GetModuleFileNameA")
            {
                if (iatSlot % 8 != 0)
                    return false;
                found.push_back({ iatSlot, imported.back() == 'W' });
            }
        }
    }

    return false; // ran off the image before the terminating descriptor
}

namespace detail
{
// Whether `pointer` (an absolute address inside the image mapped at `base`) points at the wide string
// `want`, NUL included.
inline bool PointsAtWide(const uint8_t* image, size_t size, uint64_t base, uint64_t pointer, std::u16string_view want)
{
    if (pointer < base || pointer - base >= size)
        return false;

    const size_t offset = static_cast<size_t>(pointer - base);
    const size_t bytes = (want.size() + 1) * 2;
    if (bytes > size - offset)
        return false;

    for (size_t i = 0; i <= want.size(); ++i)
    {
        uint16_t c = 0;
        std::memcpy(&c, image + offset + i * 2, 2);
        const uint16_t expected = i < want.size() ? static_cast<uint16_t>(want[i]) : 0;
        if (c != expected)
            return false;
    }
    return true;
}
} // namespace detail

// Finds the loader's feature-id -> snippet-name table in a writable section and reports what entry 18
// names. `base` is the address the image is mapped at (the table holds absolute pointers). `tableOffset`
// receives the table's offset from the base when one matches.
inline Feature18Route FindFeature18Route(const uint8_t* image, size_t size, uint64_t base,
                                         size_t* tableOffset = nullptr)
{
    detail::Headers h;
    if (!detail::ParseHeaders(image, size, h))
        return Feature18Route::Unknown;

    constexpr size_t kSection = 40;
    for (uint16_t s = 0; s < h.sections; ++s)
    {
        const size_t header = h.sectionTable + s * kSection;
        uint32_t virtualSize = 0;
        uint32_t virtualAddress = 0;
        uint32_t characteristics = 0;
        if (!detail::Read(image, size, header + 8, virtualSize) ||
            !detail::Read(image, size, header + 12, virtualAddress) ||
            !detail::Read(image, size, header + 36, characteristics))
            return Feature18Route::Unknown;

        if ((characteristics & detail::kScnMemWrite) == 0 || virtualAddress >= size)
            continue;

        const size_t end = virtualAddress + static_cast<size_t>(virtualSize) < size
                               ? virtualAddress + static_cast<size_t>(virtualSize)
                               : size;
        const size_t first = (static_cast<size_t>(virtualAddress) + 7) & ~static_cast<size_t>(7);

        for (size_t p = first; p + 19 * 8 <= end; p += 8)
        {
            uint64_t entry1 = 0;
            uint64_t entry11 = 0;
            uint64_t entry18 = 0;
            std::memcpy(&entry1, image + p + 1 * 8, 8);
            if (!detail::PointsAtWide(image, size, base, entry1, u"dlss"))
                continue;
            std::memcpy(&entry11, image + p + 11 * 8, 8);
            if (!detail::PointsAtWide(image, size, base, entry11, u"dlssg"))
                continue;
            std::memcpy(&entry18, image + p + 18 * 8, 8);

            if (detail::PointsAtWide(image, size, base, entry18, u"dlssnr"))
            {
                if (tableOffset != nullptr)
                    *tableOffset = p;
                return Feature18Route::Present;
            }

            if (detail::PointsAtWide(image, size, base, entry18, u""))
            {
                if (tableOffset != nullptr)
                    *tableOffset = p;
                return Feature18Route::Absent;
            }
        }
    }

    return Feature18Route::Unknown;
}

// The pairing dlss5-bridge measured faulting (RTX 5090, 32.0.16.1664 and 1686): a loader that routes
// feature 18 itself, driving a model at 310.8.0.0 or older. The snippet hands D3D12 a descriptor heap
// whose description reads back as garbage and D3D12 faults in SetDescriptorHeaps. A newer model is
// presumed fixed, as dlss5-bridge presumes it, so the check stands down on its own once one ships.
// version[4] is major.minor.patch.build; all zero means "unknown", which is not a verdict.
inline bool ModelKnownToFaultOnLoaderRoute(const uint16_t version[4])
{
    if (version[0] == 0 && version[1] == 0 && version[2] == 0 && version[3] == 0)
        return false;
    if (version[0] != 310)
        return version[0] < 310;
    if (version[1] != 8)
        return version[1] < 8;
    return version[2] == 0 && version[3] == 0;
}

inline bool KnownFaultingPairing(Feature18Route route, const uint16_t modelVersion[4])
{
    return route == Feature18Route::Present && ModelKnownToFaultOnLoaderRoute(modelVersion);
}

inline const char* Describe(Feature18Route route)
{
    switch (route)
    {
    case Feature18Route::Present:
        return "routes feature 18 itself (entry 18 names \"dlssnr\")";
    case Feature18Route::Absent:
        return "does not route feature 18 (entry 18 names nothing)";
    default:
        return "feature table not recognised";
    }
}

} // namespace DlssNr::PeScan
