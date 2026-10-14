#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): where the pass graph's DXIL comes from.
//
// Shaders come from one of two sources:
// 1. [LSFG] ShaderDir: explicit override pointing to precompiled DXIL (<ShaderDir>\<rid>.cso).
// 2. Auto-discovery & content-hash cache: LsfgShaderLibrary locates lsfg-vk.dll (or [LSFG] DllPath),
//    computes its SHA-256 content hash, and loads DXIL from <cacheDir>\<hash>\<rid>.cso.
//    If the cache is missing, MakeShaderSource refuses cleanly with ONE actionable log line.
// LoadShader runs on the build worker, never on the present path.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace retoque::lsfg
{
class ILsfgEnvironment;
}

namespace Lsfg
{

class IShaderSource
{
  public:
    virtual ~IShaderSource() = default;

    // The DXIL container of resource id `rid`. False with a reason when it cannot be had.
    virtual bool LoadShader(uint32_t rid, std::vector<uint8_t>& out, std::string& error) = 0;

    // For the log: where the shaders come from.
    virtual std::string Describe() const = 0;
};

// <dir>\<rid>.cso. An empty dir yields a source whose every load fails with "[LSFG] ShaderDir is not set".
std::unique_ptr<IShaderSource> MakeDirectoryShaderSource(const std::wstring& dir);

// Resolves shader source according to priority:
// 1. shaderDirOverride (if set and non-empty): reads <shaderDirOverride>\<rid>.cso
// 2. auto-discovery via LsfgShaderLibrary::FindDll(dllPathOverride):
//    - Hashes lsfg-vk.dll
//    - Looks for cache at <baseCacheDir>/<hash>/
//    - If cache hit: loads from <baseCacheDir>/<hash>/<rid>.cso
//    - If cache miss: returns nullptr and sets refusalReason to ONE clear actionable line telling what to run
// Returns nullptr on refusal/missing source.
std::unique_ptr<IShaderSource> MakeShaderSource(const std::wstring& shaderDirOverride,
                                                const std::wstring& dllPathOverride,
                                                const std::filesystem::path& baseCacheDir, std::string& refusalReason,
                                                const retoque::lsfg::ILsfgEnvironment* env = nullptr);

} // namespace Lsfg
