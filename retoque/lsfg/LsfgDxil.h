#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): reads what a compiled DXIL shader binds, from the
// container's PSV0 part, so the root signatures can be built without dxcompiler.dll's reflection.
//
// PSV0 is the part the D3D12 runtime itself checks a root signature against at PSO creation, so it is
// exactly the information needed: resource class, register space, register range and (record version 1)
// the texture dimension. Header-only and free of <windows.h>, so tests/lsfg-integration drives it on the
// host with synthetic containers. Layout: DxilContainer.h and DxilPipelineStateValidation.h in
// DirectXShaderCompiler (University of Illinois/NCSA licence), written fresh from the structure.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Lsfg::Dxil
{

enum class ResClass : uint8_t
{
    Sampler,
    Cbv,
    Srv,   // typed SRV only; raw and structured are refused
    Uav,   // typed UAV only
    Other, // anything else: the graph has no use for it
};

// DXIL::ResourceKind values that occur here.
constexpr uint32_t kKindTexture2D = 2;
constexpr uint32_t kKindTexture2DArray = 7;

struct Binding
{
    ResClass cls = ResClass::Other;
    uint32_t space = 0;
    uint32_t lower = 0;
    uint32_t count = 0; // registers lower..lower+count-1
    uint32_t kind = 0;  // DXIL::ResourceKind, 0 when the record is version 0 (no kind)
};

struct Reflection
{
    std::vector<Binding> bindings;
    uint32_t numThreads[3] {};
    bool hasNumThreads = false;
    bool hasKinds = false;
};

namespace detail
{
inline bool ReadU32(const uint8_t* data, size_t size, size_t offset, uint32_t& out)
{
    if (offset > size || size - offset < 4)
        return false;
    std::memcpy(&out, data + offset, 4);
    return true;
}
} // namespace detail

// Parses a DXIL container ("DXBC" header, a part table, one part per fourcc) and its PSV0 part.
// Returns false with a reason on any structural problem; a shader this cannot read is never used.
inline bool Parse(const uint8_t* data, size_t size, Reflection& out, std::string& error)
{
    using detail::ReadU32;

    out = {};

    // Header: 'DXBC', 16-byte digest, u16 major, u16 minor, u32 container size, u32 part count, u32 offsets[].
    if (data == nullptr || size < 32 || std::memcmp(data, "DXBC", 4) != 0)
    {
        error = "not a DXIL container (no DXBC header)";
        return false;
    }

    uint32_t containerSize = 0, partCount = 0;
    ReadU32(data, size, 24, containerSize);
    ReadU32(data, size, 28, partCount);

    if (containerSize > size)
    {
        error = "container size field exceeds the file";
        return false;
    }

    if (partCount == 0 || partCount > 64)
    {
        error = "implausible part count";
        return false;
    }

    const uint8_t* psv = nullptr;
    uint32_t psvSize = 0;
    bool hasDxil = false;

    for (uint32_t i = 0; i < partCount; ++i)
    {
        uint32_t partOffset = 0;
        if (!ReadU32(data, size, 32 + size_t(i) * 4, partOffset))
        {
            error = "part table truncated";
            return false;
        }

        uint32_t partSize = 0;
        if (!ReadU32(data, size, partOffset + size_t(4), partSize) || size_t(partOffset) + 8 + partSize > size)
        {
            error = "part runs past the end of the file";
            return false;
        }

        const uint8_t* fourcc = data + partOffset;
        if (std::memcmp(fourcc, "PSV0", 4) == 0)
        {
            psv = data + partOffset + 8;
            psvSize = partSize;
        }
        else if (std::memcmp(fourcc, "DXIL", 4) == 0)
        {
            hasDxil = true;
        }
    }

    if (!hasDxil)
    {
        error = "no DXIL program part (DXBC shader model 5 bytecode is not this graph's)";
        return false;
    }

    if (psv == nullptr)
    {
        error = "no PSV0 part";
        return false;
    }

    // PSV0: u32 runtime-info size, runtime info, u32 resource count, [u32 record size, records].
    uint32_t infoSize = 0;
    if (!ReadU32(psv, psvSize, 0, infoSize) || size_t(infoSize) + 4 > psvSize)
    {
        error = "PSV0 runtime info truncated";
        return false;
    }

    // PSVRuntimeInfo2 (48 bytes and up) carries NumThreadsX/Y/Z at offset 36.
    if (infoSize >= 48)
    {
        for (int axis = 0; axis < 3; ++axis)
            ReadU32(psv, psvSize, 4 + 36 + size_t(axis) * 4, out.numThreads[axis]);
        out.hasNumThreads = true;
    }

    size_t offset = 4 + size_t(infoSize);
    uint32_t resourceCount = 0;
    if (!ReadU32(psv, psvSize, offset, resourceCount))
    {
        error = "PSV0 resource count missing";
        return false;
    }
    offset += 4;

    if (resourceCount > 256)
    {
        error = "implausible resource count";
        return false;
    }

    if (resourceCount == 0)
        return true;

    uint32_t recordSize = 0;
    if (!ReadU32(psv, psvSize, offset, recordSize) || (recordSize != 16 && recordSize < 24))
    {
        error = "unknown PSV0 resource record size";
        return false;
    }
    offset += 4;

    if (size_t(resourceCount) * recordSize > psvSize - offset)
    {
        error = "PSV0 resource records truncated";
        return false;
    }

    out.hasKinds = recordSize >= 24;
    out.bindings.reserve(resourceCount);

    for (uint32_t i = 0; i < resourceCount; ++i, offset += recordSize)
    {
        uint32_t type = 0, space = 0, lower = 0, upper = 0, kind = 0;
        ReadU32(psv, psvSize, offset, type);
        ReadU32(psv, psvSize, offset + 4, space);
        ReadU32(psv, psvSize, offset + 8, lower);
        ReadU32(psv, psvSize, offset + 12, upper);
        if (out.hasKinds)
            ReadU32(psv, psvSize, offset + 16, kind);

        if (upper < lower || upper == 0xFFFFFFFFu)
        {
            error = "unbounded or inverted register range";
            return false;
        }

        Binding binding;
        binding.space = space;
        binding.lower = lower;
        binding.count = upper - lower + 1;
        binding.kind = kind;

        // PSVResourceType: 1 sampler, 2 CBV, 3 typed SRV, 6 typed UAV; the rest are raw or structured.
        switch (type)
        {
        case 1:
            binding.cls = ResClass::Sampler;
            break;
        case 2:
            binding.cls = ResClass::Cbv;
            break;
        case 3:
            binding.cls = ResClass::Srv;
            break;
        case 6:
            binding.cls = ResClass::Uav;
            break;
        default:
            binding.cls = ResClass::Other;
            break;
        }

        out.bindings.push_back(binding);
    }

    return true;
}

} // namespace Lsfg::Dxil
