// SPDX-License-Identifier: CC-BY-NC-ND-4.0
#include "pch.h"

#include "LsfgShaderLibrary.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace retoque::lsfg
{

namespace
{

// ============================================================================
// Portable SHA-256 Implementation (FIPS 180-2)
// ============================================================================
struct Sha256Context
{
    uint32_t state[8] {};
    uint64_t count { 0 };
    uint8_t buffer[64] {};
};

constexpr std::array<uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline uint32_t Ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void Sha256Transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = Ror32(w[i - 15], 7) ^ Ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror32(w[i - 2], 17) ^ Ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; ++i)
    {
        const uint32_t s1 = Ror32(e, 6) ^ Ror32(e, 11) ^ Ror32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = Ror32(a, 2) ^ Ror32(a, 13) ^ Ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void Sha256Init(Sha256Context* ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void Sha256Update(Sha256Context* ctx, const uint8_t* data, size_t len)
{
    size_t index = static_cast<size_t>(ctx->count & 0x3f);
    ctx->count += len;

    size_t part_len = 64 - index;
    size_t i = 0;

    if (len >= part_len)
    {
        std::memcpy(&ctx->buffer[index], data, part_len);
        Sha256Transform(ctx->state, ctx->buffer);
        for (i = part_len; i + 63 < len; i += 64)
        {
            Sha256Transform(ctx->state, &data[i]);
        }
        index = 0;
    }

    std::memcpy(&ctx->buffer[index], &data[i], len - i);
}

void Sha256Final(Sha256Context* ctx, uint8_t digest[32])
{
    const uint64_t total_bits = ctx->count * 8;
    const size_t index = static_cast<size_t>(ctx->count & 0x3f);
    const size_t pad_len = (index < 56) ? (56 - index) : (120 - index);

    static const uint8_t kPadding[64] = { 0x80 };
    Sha256Update(ctx, kPadding, pad_len);

    uint8_t bits[8];
    for (int i = 0; i < 8; ++i)
    {
        bits[i] = static_cast<uint8_t>((total_bits >> ((7 - i) * 8)) & 0xff);
    }
    Sha256Update(ctx, bits, 8);

    for (int i = 0; i < 8; ++i)
    {
        digest[i * 4 + 0] = static_cast<uint8_t>((ctx->state[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((ctx->state[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((ctx->state[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>((ctx->state[i]) & 0xff);
    }
}

// ============================================================================
// Core Shader ID Table (from lsfg-vk library.cpp)
// ============================================================================
struct ShaderMapping
{
    std::string_view name;
    uint32_t baseIdx;
};

constexpr std::array<ShaderMapping, 24> kCoreShaders = {
    { { "alpha0", 13 },   { "alpha1", 14 },   { "alpha2", 15 },   { "alpha3", 16 },  { "beta0", 22 },
      { "beta1", 23 },    { "beta2", 24 },    { "beta3", 25 },    { "beta4", 26 },   { "gamma0", 3 },
      { "gamma1", 4 },    { "gamma2", 5 },    { "gamma3", 6 },    { "gamma4", 7 },   { "delta0", 8 },
      { "delta1", 9 },    { "delta2", 10 },   { "delta3", 11 },   { "delta4", 12 },  { "epsilon0", 17 },
      { "epsilon1", 18 }, { "epsilon2", 19 }, { "epsilon3", 20 }, { "epsilon4", 21 } }
};

// ============================================================================
// Steam VDF libraryfolders parser
// ============================================================================
std::vector<std::filesystem::path> ParseLibraryFoldersVdf(const std::string& vdfContent)
{
    std::vector<std::filesystem::path> libraries;
    std::istringstream stream(vdfContent);
    std::string line;

    while (std::getline(stream, line))
    {
        size_t pathToken = line.find("\"path\"");
        if (pathToken == std::string::npos)
            continue;

        size_t valStart = line.find('"', pathToken + 6);
        if (valStart == std::string::npos)
            continue;
        size_t valEnd = line.find('"', valStart + 1);
        if (valEnd == std::string::npos)
            continue;

        std::string rawPath = line.substr(valStart + 1, valEnd - (valStart + 1));
        std::string normalized;
        normalized.reserve(rawPath.size());
        for (size_t i = 0; i < rawPath.size(); ++i)
        {
            if (rawPath[i] == '\\' && i + 1 < rawPath.size() && rawPath[i + 1] == '\\')
            {
                normalized.push_back('/');
                ++i;
            }
            else if (rawPath[i] == '\\')
            {
                normalized.push_back('/');
            }
            else
            {
                normalized.push_back(rawPath[i]);
            }
        }

        if (!normalized.empty())
        {
            std::filesystem::path p(normalized);
            if (std::find(libraries.begin(), libraries.end(), p) == libraries.end())
            {
                libraries.push_back(std::move(p));
            }
        }
    }
    return libraries;
}

// Safe binary reading helper
template <typename T> bool ReadStruct(std::span<const uint8_t> data, size_t offset, T* out)
{
    if (offset + sizeof(T) > data.size())
    {
        return false;
    }
    std::memcpy(out, data.data() + offset, sizeof(T));
    return true;
}

} // namespace

// ============================================================================
// DefaultLsfgEnvironment Implementation
// ============================================================================
std::optional<std::string> DefaultLsfgEnvironment::GetEnv(const std::string& name) const
{
    const char* val = std::getenv(name.c_str());
    if (val && *val != '\0')
    {
        return std::string(val);
    }
    return std::nullopt;
}

std::optional<std::string>
DefaultLsfgEnvironment::GetRegistryString([[maybe_unused]] const std::string& key,
                                          [[maybe_unused]] const std::string& valueName) const
{
#if defined(_WIN32)
    HKEY hKey = nullptr;
    LSTATUS status = RegOpenKeyExA(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &hKey);
    if (status != ERROR_SUCCESS)
    {
        return std::nullopt;
    }

    char buffer[1024];
    DWORD bufferSize = sizeof(buffer);
    DWORD type = 0;
    status = RegQueryValueExA(hKey, valueName.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(buffer), &bufferSize);
    RegCloseKey(hKey);

    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ))
    {
        return std::string(buffer);
    }
#endif
    return std::nullopt;
}

bool DefaultLsfgEnvironment::FileExists(const std::filesystem::path& path) const
{
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool DefaultLsfgEnvironment::DirectoryExists(const std::filesystem::path& path) const
{
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::string DefaultLsfgEnvironment::ReadFileToString(const std::filesystem::path& path) const
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

const DefaultLsfgEnvironment& DefaultLsfgEnvironment::Instance()
{
    static DefaultLsfgEnvironment instance;
    return instance;
}

// ============================================================================
// SHA-256 Public Helper
// ============================================================================
std::string ComputeSha256Hex(std::span<const uint8_t> data)
{
    Sha256Context ctx;
    Sha256Init(&ctx);
    Sha256Update(&ctx, data.data(), data.size());
    uint8_t digest[32];
    Sha256Final(&ctx, digest);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t b : digest)
    {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

// ============================================================================
// LsfgShaderLibrary Discovery
// ============================================================================
std::filesystem::path LsfgShaderLibrary::FixDllPath(const std::filesystem::path& path, const ILsfgEnvironment* env)
{
    const auto& e = env ? *env : DefaultLsfgEnvironment::Instance();
    const std::string filename = path.filename().string();

    if (filename == "Lossless Scaling")
    {
        return path / "lsfg-vk.dll";
    }

    if (filename == "Lossless.dll" || filename == "LosslessScaling.dll")
    {
        return path.parent_path() / "lsfg-vk.dll";
    }

    const std::string parentName = path.parent_path().filename().string();
    if (filename.empty() && parentName == "Lossless Scaling")
    {
        return path.parent_path() / "lsfg-vk.dll";
    }

    if (e.DirectoryExists(path) && e.FileExists(path / "lsfg-vk.dll"))
    {
        return path / "lsfg-vk.dll";
    }

    return path;
}

std::optional<std::filesystem::path>
LsfgShaderLibrary::FindDll(const std::optional<std::filesystem::path>& overridePath, const ILsfgEnvironment* env)
{
    const auto& e = env ? *env : DefaultLsfgEnvironment::Instance();

    // 1. Explicit user override
    if (overridePath && !overridePath->empty())
    {
        const auto fixed = FixDllPath(*overridePath, env);
        if (e.FileExists(fixed))
        {
            return fixed;
        }
    }

    // 2. Windows Steam Registry & libraryfolders.vdf
    const auto steamReg = e.GetRegistryString("Software\\Valve\\Steam", "SteamPath");
    if (steamReg && !steamReg->empty())
    {
        const std::filesystem::path steamRoot(*steamReg);
        const auto directDll = steamRoot / "steamapps" / "common" / "Lossless Scaling" / "lsfg-vk.dll";
        if (e.FileExists(directDll))
        {
            return directDll;
        }

        const auto vdfPath = steamRoot / "steamapps" / "libraryfolders.vdf";
        if (e.FileExists(vdfPath))
        {
            const auto vdfContent = e.ReadFileToString(vdfPath);
            const auto libraries = ParseLibraryFoldersVdf(vdfContent);
            for (const auto& lib : libraries)
            {
                const auto dll = lib / "steamapps" / "common" / "Lossless Scaling" / "lsfg-vk.dll";
                if (e.FileExists(dll))
                {
                    return dll;
                }
            }
        }
    }

    // Common Linux Steam subpaths (relative to HOME or XDG_DATA_HOME)
    const std::array<std::filesystem::path, 5> steamRelPaths = {
        { ".local/share/Steam", ".steam/steam", ".steam/debian-installation",
          ".var/app/com.valvesoftware.Steam/.local/share/Steam", "snap/steam/common/.local/share/Steam" }
    };

    auto checkPrefix = [&](const std::filesystem::path& prefix) -> std::optional<std::filesystem::path>
    {
        for (const auto& rel : steamRelPaths)
        {
            const auto steamRoot = prefix / rel;
            const auto directDll = steamRoot / "steamapps" / "common" / "Lossless Scaling" / "lsfg-vk.dll";
            if (e.FileExists(directDll))
            {
                return directDll;
            }

            const auto vdfPath = steamRoot / "steamapps" / "libraryfolders.vdf";
            if (e.FileExists(vdfPath))
            {
                const auto vdfContent = e.ReadFileToString(vdfPath);
                const auto libraries = ParseLibraryFoldersVdf(vdfContent);
                for (const auto& lib : libraries)
                {
                    const auto dll = lib / "steamapps" / "common" / "Lossless Scaling" / "lsfg-vk.dll";
                    if (e.FileExists(dll))
                    {
                        return dll;
                    }
                    // Under Wine/Proton, Linux absolute paths (/mnt/...) appear via Z:
                    if (lib.is_absolute() && !lib.string().starts_with("Z:"))
                    {
                        const auto zDll = std::filesystem::path("Z:" + lib.string()) / "steamapps" / "common" /
                                          "Lossless Scaling" / "lsfg-vk.dll";
                        if (e.FileExists(zDll))
                        {
                            return zDll;
                        }
                    }
                }
            }
        }
        return std::nullopt;
    };

    // 3. XDG_DATA_HOME checks
    const auto xdgData = e.GetEnv("XDG_DATA_HOME");
    if (xdgData && !xdgData->empty())
    {
        const std::filesystem::path xdgPath(*xdgData);
        if (auto found = checkPrefix(xdgPath))
            return found;
        if (auto found = checkPrefix(std::filesystem::path("Z:" + xdgPath.string())))
            return found;
    }

    // 4. HOME checks (both native and Wine Z: drive mapping)
    const auto home = e.GetEnv("HOME");
    if (home && !home->empty())
    {
        const std::filesystem::path homePath(*home);
        if (auto found = checkPrefix(homePath))
            return found;
        if (auto found = checkPrefix(std::filesystem::path("Z:" + homePath.string())))
            return found;
    }

    // 5. Fallback: check Wine Z:\home\<user> if USER or LOGNAME is set
    const auto user = e.GetEnv("USER");
    if (user && !user->empty())
    {
        const std::filesystem::path zHome = std::filesystem::path("Z:/home") / *user;
        if (auto found = checkPrefix(zHome))
            return found;
    }

    // 6. Current directory fallback
    const std::filesystem::path currentDll = "lsfg-vk.dll";
    if (e.FileExists(currentDll))
    {
        return currentDll;
    }

    return std::nullopt;
}

// ============================================================================
// PE Resource Parsing (RT_RCDATA / Type 10)
// ============================================================================
bool LsfgShaderLibrary::LoadFromDll(const std::filesystem::path& dllPath, const ILsfgEnvironment* env)
{
    const auto& e = env ? *env : DefaultLsfgEnvironment::Instance();
    if (!e.FileExists(dllPath))
    {
        m_lastError = "DLL file does not exist: " + dllPath.string();
        return false;
    }

    std::ifstream file(dllPath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        m_lastError = "Unable to open DLL file: " + dllPath.string();
        return false;
    }

    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
    {
        m_lastError = "Failed to read DLL file: " + dllPath.string();
        return false;
    }

    return LoadFromBytes(bytes);
}

bool LsfgShaderLibrary::LoadFromBytes(std::span<const uint8_t> data)
{
    m_modules.clear();
    m_lastError.clear();
    m_dllHash = ComputeSha256Hex(data);

    // 1. DOS Header
    struct DosHeader
    {
        uint16_t e_magic;
        std::array<uint16_t, 29> e_res;
        int32_t e_lfanew;
    } dos {};

    if (!ReadStruct(data, 0, &dos) || dos.e_magic != 0x5A4D)
    {
        m_lastError = "Invalid DOS header magic (not MZ)";
        return false;
    }

    if (dos.e_lfanew <= 0 || static_cast<size_t>(dos.e_lfanew) + 24 > data.size())
    {
        m_lastError = "Invalid PE header offset";
        return false;
    }

    const size_t peOffset = static_cast<size_t>(dos.e_lfanew);

    // 2. PE Signature & File Header
    uint32_t peSig = 0;
    if (!ReadStruct(data, peOffset, &peSig) || peSig != 0x00004550)
    {
        m_lastError = "Invalid PE signature (not PE\\0\\0)";
        return false;
    }

    struct PeFileHeader
    {
        uint16_t machine;
        uint16_t numberOfSections;
        uint32_t timeDateStamp;
        uint32_t pointerToSymbolTable;
        uint32_t numberOfSymbols;
        uint16_t sizeOfOptionalHeader;
        uint16_t characteristics;
    } fileHdr {};

    if (!ReadStruct(data, peOffset + 4, &fileHdr))
    {
        m_lastError = "Truncated PE file header";
        return false;
    }

    const size_t optOffset = peOffset + 24;
    uint16_t optMagic = 0;
    if (!ReadStruct(data, optOffset, &optMagic))
    {
        m_lastError = "Truncated optional header";
        return false;
    }

    uint32_t rsrcRva = 0;
    uint32_t rsrcSize = 0;

    if (optMagic == 0x020B)
    { // PE32+ (64-bit)
        // DirectoryEntry[2] (Resource) is at optional header offset 112 (after 16 standard fields + 96 bytes)
        constexpr size_t kRsrcDirOffset = 112;
        struct DataDirectory
        {
            uint32_t virtualAddress;
            uint32_t size;
        } rsrcDir {};
        if (!ReadStruct(data, optOffset + kRsrcDirOffset + (2 * sizeof(DataDirectory)), &rsrcDir))
        {
            m_lastError = "Unable to read PE32+ resource directory entry";
            return false;
        }
        rsrcRva = rsrcDir.virtualAddress;
        rsrcSize = rsrcDir.size;
    }
    else if (optMagic == 0x010B)
    { // PE32 (32-bit)
        constexpr size_t kRsrcDirOffset = 96;
        struct DataDirectory
        {
            uint32_t virtualAddress;
            uint32_t size;
        } rsrcDir {};
        if (!ReadStruct(data, optOffset + kRsrcDirOffset + (2 * sizeof(DataDirectory)), &rsrcDir))
        {
            m_lastError = "Unable to read PE32 resource directory entry";
            return false;
        }
        rsrcRva = rsrcDir.virtualAddress;
        rsrcSize = rsrcDir.size;
    }
    else
    {
        m_lastError = "Unknown optional header magic";
        return false;
    }

    if (rsrcRva == 0 || rsrcSize == 0)
    {
        m_lastError = "No resource section in PE";
        return false;
    }

    // 3. Section Headers
    const size_t sectHeadersOffset = optOffset + fileHdr.sizeOfOptionalHeader;
    struct SectionHeader
    {
        std::array<char, 8> name;
        uint32_t virtualSize;
        uint32_t virtualAddress;
        uint32_t sizeOfRawData;
        uint32_t pointerToRawData;
        uint32_t pointerToRelocations;
        uint32_t pointerToLinenumbers;
        uint16_t numberOfRelocations;
        uint16_t numberOfLinenumbers;
        uint32_t characteristics;
    };

    std::vector<SectionHeader> sections(fileHdr.numberOfSections);
    for (size_t i = 0; i < fileHdr.numberOfSections; ++i)
    {
        if (!ReadStruct(data, sectHeadersOffset + i * sizeof(SectionHeader), &sections[i]))
        {
            m_lastError = "Truncated section headers";
            return false;
        }
    }

    auto rvaToFileOffset = [&](uint32_t rva) -> std::optional<size_t>
    {
        for (const auto& sect : sections)
        {
            const uint32_t sectSize = std::max(sect.virtualSize, sect.sizeOfRawData);
            if (rva >= sect.virtualAddress && rva < sect.virtualAddress + sectSize)
            {
                return static_cast<size_t>(rva - sect.virtualAddress) + sect.pointerToRawData;
            }
        }
        return std::nullopt;
    };

    const auto rsrcFileOffsetOpt = rvaToFileOffset(rsrcRva);
    if (!rsrcFileOffsetOpt)
    {
        m_lastError = "Cannot map resource section RVA to file offset";
        return false;
    }
    const size_t rsrcFileBase = *rsrcFileOffsetOpt;

    // 4. Resource Directory Structures
    struct ResourceDirectory
    {
        uint32_t characteristics;
        uint32_t timeDateStamp;
        uint16_t majorVersion;
        uint16_t minorVersion;
        uint16_t numberOfNamedEntries;
        uint16_t numberOfIdEntries;
    };

    struct ResourceDirectoryEntry
    {
        uint32_t nameOrId;
        uint32_t offsetToDataOrDir;
    };

    struct ResourceDataEntry
    {
        uint32_t offsetToData; // RVA
        uint32_t size;
        uint32_t codePage;
        uint32_t reserved;
    };

    ResourceDirectory rootDir {};
    if (!ReadStruct(data, rsrcFileBase, &rootDir))
    {
        m_lastError = "Failed to read root resource directory";
        return false;
    }

    const size_t totalRootEntries = rootDir.numberOfNamedEntries + rootDir.numberOfIdEntries;
    std::optional<uint32_t> rcDataDirOffset;

    for (size_t i = 0; i < totalRootEntries; ++i)
    {
        ResourceDirectoryEntry entry {};
        if (!ReadStruct(data, rsrcFileBase + sizeof(ResourceDirectory) + i * sizeof(ResourceDirectoryEntry), &entry))
        {
            break;
        }
        // Level 1: Type 10 == RT_RCDATA
        if ((entry.nameOrId & 0x80000000) == 0 && entry.nameOrId == 10)
        {
            if (entry.offsetToDataOrDir & 0x80000000)
            {
                rcDataDirOffset = entry.offsetToDataOrDir & 0x7FFFFFFF;
            }
            break;
        }
    }

    if (!rcDataDirOffset)
    {
        m_lastError = "RT_RCDATA (type 10) directory not found";
        return false;
    }

    // Level 2: RT_RCDATA entries
    const size_t rcDataDirFileOffset = rsrcFileBase + *rcDataDirOffset;
    ResourceDirectory rcDataDir {};
    if (!ReadStruct(data, rcDataDirFileOffset, &rcDataDir))
    {
        m_lastError = "Failed to read RT_RCDATA directory";
        return false;
    }

    const size_t totalRcDataEntries = rcDataDir.numberOfNamedEntries + rcDataDir.numberOfIdEntries;

    for (size_t i = 0; i < totalRcDataEntries; ++i)
    {
        ResourceDirectoryEntry entry {};
        if (!ReadStruct(data, rcDataDirFileOffset + sizeof(ResourceDirectory) + i * sizeof(ResourceDirectoryEntry),
                        &entry))
        {
            continue;
        }

        uint32_t resolvedRid = 0;
        if (entry.nameOrId & 0x80000000)
        {
            // Named resource: in lsfg-vk.dll, resource 0 (mipmaps) has name offset 0x1348 pointing to string "0"
            const size_t strOffset = rsrcFileBase + (entry.nameOrId & 0x7FFFFFFF);
            uint16_t strLen = 0;
            if (ReadStruct(data, strOffset, &strLen) && strLen > 0 && strLen < 32)
            {
                std::string nameStr;
                for (size_t c = 0; c < strLen; ++c)
                {
                    uint16_t ch = 0;
                    if (ReadStruct(data, strOffset + 2 + c * 2, &ch))
                    {
                        nameStr.push_back(static_cast<char>(ch));
                    }
                }
                try
                {
                    resolvedRid = static_cast<uint32_t>(std::stoul(nameStr));
                }
                catch (...)
                {
                    resolvedRid = 0;
                }
            }
        }
        else
        {
            resolvedRid = entry.nameOrId;
        }

        if ((entry.offsetToDataOrDir & 0x80000000) == 0)
        {
            continue; // Expected subdirectory
        }

        // Level 3: Language directory
        const size_t langDirFileOffset = rsrcFileBase + (entry.offsetToDataOrDir & 0x7FFFFFFF);
        ResourceDirectory langDir {};
        if (!ReadStruct(data, langDirFileOffset, &langDir))
        {
            continue;
        }

        const size_t totalLangEntries = langDir.numberOfNamedEntries + langDir.numberOfIdEntries;
        if (totalLangEntries == 0)
        {
            continue;
        }

        ResourceDirectoryEntry langEntry {};
        if (!ReadStruct(data, langDirFileOffset + sizeof(ResourceDirectory), &langEntry))
        {
            continue;
        }

        if (langEntry.offsetToDataOrDir & 0x80000000)
        {
            continue; // Expected data leaf
        }

        const size_t dataEntryFileOffset = rsrcFileBase + (langEntry.offsetToDataOrDir & 0x7FFFFFFF);
        ResourceDataEntry dataLeaf {};
        if (!ReadStruct(data, dataEntryFileOffset, &dataLeaf))
        {
            continue;
        }

        const auto payloadOffset = rvaToFileOffset(dataLeaf.offsetToData);
        if (!payloadOffset || *payloadOffset + dataLeaf.size > data.size())
        {
            continue;
        }

        // Validate SPIR-V
        if (dataLeaf.size < 4 || (dataLeaf.size % 4 != 0))
        {
            continue; // Not a valid SPIR-V module
        }

        uint32_t magic = 0;
        if (!ReadStruct(data, *payloadOffset, &magic) || magic != kSpirvMagic)
        {
            continue; // Non-SPIRV resource (e.g. metadata)
        }

        std::vector<uint8_t> spv(dataLeaf.size);
        std::memcpy(spv.data(), data.data() + *payloadOffset, dataLeaf.size);
        m_modules[resolvedRid] = std::move(spv);
    }

    if (m_modules.empty())
    {
        m_lastError = "No valid SPIR-V modules found in RT_RCDATA";
        return false;
    }

    return true;
}

bool LsfgShaderLibrary::HasModule(uint32_t rid) const { return m_modules.find(rid) != m_modules.end(); }

std::span<const uint8_t> LsfgShaderLibrary::GetModuleBytecode(uint32_t rid) const
{
    auto it = m_modules.find(rid);
    if (it != m_modules.end())
    {
        return it->second;
    }
    return {};
}

// ============================================================================
// High-Level Shader Name Resolution
// ============================================================================
uint32_t LsfgShaderLibrary::ResolveResourceId(std::string_view shaderName, bool fp16, bool performance)
{
    if (shaderName == "mipmaps")
        return 0;
    if (shaderName == "generate_8bit")
        return 1;
    if (shaderName == "generate_16bit")
        return 2;
    if (shaderName == "generate")
        return fp16 ? 2 : 1;

    for (const auto& item : kCoreShaders)
    {
        if (item.name == shaderName)
        {
            return item.baseIdx + (fp16 ? 48 : 0) + (performance ? 24 : 0);
        }
    }

    return UINT32_MAX;
}

std::span<const uint8_t> LsfgShaderLibrary::GetShader(std::string_view shaderName, bool fp16, bool performance) const
{
    uint32_t rid = ResolveResourceId(shaderName, fp16, performance);
    if (rid == UINT32_MAX)
    {
        return {};
    }
    return GetModuleBytecode(rid);
}

// ============================================================================
// Shader Cache Management (Keyed by DLL Content Hash)
// ============================================================================
void LsfgShaderLibrary::SetCacheDirectory(const std::filesystem::path& cacheDir) { m_cacheDir = cacheDir; }

std::filesystem::path LsfgShaderLibrary::GetCachedShaderPath(uint32_t rid) const
{
    if (m_cacheDir.empty())
    {
        return {};
    }
    return m_cacheDir / (std::to_string(rid) + ".cso");
}

bool LsfgShaderLibrary::HasCachedShader(uint32_t rid) const
{
    const auto path = GetCachedShaderPath(rid);
    std::error_code ec;
    return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

std::vector<uint8_t> LsfgShaderLibrary::LoadCachedShader(uint32_t rid) const
{
    const auto path = GetCachedShaderPath(rid);
    if (path.empty())
        return {};

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return {};

    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size)))
    {
        return {};
    }
    return buffer;
}

bool LsfgShaderLibrary::StoreCachedShader(uint32_t rid, std::span<const uint8_t> dxil)
{
    if (m_cacheDir.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(m_cacheDir, ec);

    const auto path = GetCachedShaderPath(rid);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(dxil.data()), static_cast<std::streamsize>(dxil.size()));
    return file.good();
}

bool LsfgShaderLibrary::IsCacheComplete() const
{
    if (m_cacheDir.empty())
        return false;
    for (uint32_t rid = 0; rid < kTotalShaderCount; ++rid)
    {
        if (!HasCachedShader(rid))
        {
            return false;
        }
    }
    return true;
}

} // namespace retoque::lsfg
