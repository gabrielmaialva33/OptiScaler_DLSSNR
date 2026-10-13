#pragma once

#include "LsfgEnvironment.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace retoque::lsfg
{

constexpr uint32_t kSpirvMagic = 0x07230203; // Little-endian: 0x03, 0x02, 0x23, 0x07
constexpr uint32_t kTotalShaderCount = 99;   // IDs 0..98

class LsfgShaderLibrary
{
  public:
    LsfgShaderLibrary() = default;
    ~LsfgShaderLibrary() = default;

    // Discovery & Path resolution
    static std::optional<std::filesystem::path>
    FindDll(const std::optional<std::filesystem::path>& overridePath = std::nullopt,
            const ILsfgEnvironment* env = nullptr);

    static std::filesystem::path FixDllPath(const std::filesystem::path& path, const ILsfgEnvironment* env = nullptr);

    // Parse PE resources from DLL file or in-memory byte span
    bool LoadFromDll(const std::filesystem::path& dllPath, const ILsfgEnvironment* env = nullptr);
    bool LoadFromBytes(std::span<const uint8_t> dllBytes);

    // Query state
    bool IsLoaded() const { return !m_modules.empty(); }
    size_t GetModuleCount() const { return m_modules.size(); }
    const std::string& GetDllHash() const { return m_dllHash; }
    const std::string& GetLastError() const { return m_lastError; }

    // Direct resource ID query
    bool HasModule(uint32_t rid) const;
    std::span<const uint8_t> GetModuleBytecode(uint32_t rid) const;

    // High-level shader name lookup with LSFG 3.1 ID mapping rules:
    // Base: mipmaps (0), generate_8bit (1), generate_16bit (2)
    // Core (alpha0..3, beta0..4, gamma0..4, delta0..4, epsilon0..4):
    //   rid = base_idx + (fp16 ? 48 : 0) + (performance ? 24 : 0)
    static uint32_t ResolveResourceId(std::string_view shaderName, bool fp16, bool performance);
    std::span<const uint8_t> GetShader(std::string_view shaderName, bool fp16, bool performance) const;

    // Cache management (keyed by DLL content hash)
    void SetCacheDirectory(const std::filesystem::path& cacheDir);
    std::filesystem::path GetCacheDirectory() const { return m_cacheDir; }

    std::filesystem::path GetCachedShaderPath(uint32_t rid) const;
    bool HasCachedShader(uint32_t rid) const;
    std::vector<uint8_t> LoadCachedShader(uint32_t rid) const;
    bool StoreCachedShader(uint32_t rid, std::span<const uint8_t> dxil);
    bool IsCacheComplete() const;

  private:
    std::unordered_map<uint32_t, std::vector<uint8_t>> m_modules;
    std::string m_dllHash;
    std::filesystem::path m_cacheDir;
    std::string m_lastError;
};

// Portable SHA-256 helper
std::string ComputeSha256Hex(std::span<const uint8_t> data);

} // namespace retoque::lsfg

namespace Lsfg
{
using LsfgShaderLibrary = retoque::lsfg::LsfgShaderLibrary;
using ILsfgEnvironment = retoque::lsfg::ILsfgEnvironment;
using DefaultLsfgEnvironment = retoque::lsfg::DefaultLsfgEnvironment;
} // namespace Lsfg
