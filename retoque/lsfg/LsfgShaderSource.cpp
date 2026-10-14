#include "pch.h"

#include "LsfgShaderSource.h"
#include <lsfg/LsfgShaderLibrary.h>

#include <filesystem>
#include <format>
#include <fstream>

namespace Lsfg
{
namespace
{
// A DXIL module of this graph is 5..70 KB; anything far outside that is not one.
constexpr std::uintmax_t kMaxShaderBytes = 4u * 1024u * 1024u;

inline std::string PathToString(const std::filesystem::path& p) { return p.string(); }

class DirectoryShaderSource final : public IShaderSource
{
  public:
    explicit DirectoryShaderSource(std::wstring dir) : _dir(std::move(dir)) {}

    bool LoadShader(uint32_t rid, std::vector<uint8_t>& out, std::string& error) override
    {
        out.clear();

        if (_dir.empty())
        {
            error = "[LSFG] ShaderDir is not set";
            return false;
        }

        const auto path = std::filesystem::path(_dir) / (std::to_wstring(rid) + L".cso");

        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            error = std::format("{} is missing ({})", PathToString(path), ec.message());
            return false;
        }

        if (size < 32 || size > kMaxShaderBytes)
        {
            error = std::format("{} has an implausible size ({} bytes)", PathToString(path), size);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        out.resize(size_t(size));
        if (!file || !file.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)))
        {
            out.clear();
            error = std::format("{} could not be read", PathToString(path));
            return false;
        }

        return true;
    }

    std::string Describe() const override
    {
        return _dir.empty() ? std::string("no ShaderDir") : PathToString(std::filesystem::path(_dir)) + "\\<rid>.cso";
    }

  private:
    std::wstring _dir;
};

class CacheShaderSource final : public IShaderSource
{
  public:
    CacheShaderSource(std::filesystem::path cacheDir, std::string hash, std::filesystem::path dllPath)
        : _dir(std::move(cacheDir)), _hash(std::move(hash)), _dllPath(std::move(dllPath))
    {
    }

    bool LoadShader(uint32_t rid, std::vector<uint8_t>& out, std::string& error) override
    {
        out.clear();

        const auto path = _dir / (std::to_string(rid) + ".cso");

        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec)
        {
            error = std::format("{} is missing ({})", PathToString(path), ec.message());
            return false;
        }

        if (size < 32 || size > kMaxShaderBytes)
        {
            error = std::format("{} has an implausible size ({} bytes)", PathToString(path), size);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        out.resize(size_t(size));
        if (!file || !file.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)))
        {
            out.clear();
            error = std::format("{} could not be read", PathToString(path));
            return false;
        }

        return true;
    }

    std::string Describe() const override
    {
        return std::format("cache {} (lsfg-vk.dll {:.8})", PathToString(_dir), _hash.substr(0, 8));
    }

  private:
    std::filesystem::path _dir;
    std::string _hash;
    std::filesystem::path _dllPath;
};

} // namespace

std::unique_ptr<IShaderSource> MakeDirectoryShaderSource(const std::wstring& dir)
{
    return std::make_unique<DirectoryShaderSource>(dir);
}

std::unique_ptr<IShaderSource> MakeShaderSource(const std::wstring& shaderDirOverride,
                                                const std::wstring& dllPathOverride,
                                                const std::filesystem::path& baseCacheDir, std::string& refusalReason,
                                                const retoque::lsfg::ILsfgEnvironment* env)
{
    const auto& e = env ? *env : retoque::lsfg::DefaultLsfgEnvironment::Instance();

    // 1. Explicit ShaderDir override wins
    if (!shaderDirOverride.empty() && shaderDirOverride != L"auto")
    {
        std::filesystem::path p(shaderDirOverride);
        if (!e.DirectoryExists(p))
        {
            refusalReason = std::format("[LSFG] ShaderDir '{}' does not exist", PathToString(p));
            return nullptr;
        }
        return MakeDirectoryShaderSource(shaderDirOverride);
    }

    // 2. Auto-discovery via LsfgShaderLibrary::FindDll
    std::optional<std::filesystem::path> optOverride;
    if (!dllPathOverride.empty() && dllPathOverride != L"auto")
    {
        optOverride = std::filesystem::path(dllPathOverride);
    }

    auto dllPath = retoque::lsfg::LsfgShaderLibrary::FindDll(optOverride, &e);
    if (!dllPath)
    {
        refusalReason = "lsfg-vk.dll not found (checked [LSFG] DllPath, Steam libraries, and Wine Z: drive)";
        return nullptr;
    }

    // 3. Read and hash the DLL
    std::string dllContent = e.ReadFileToString(*dllPath);
    if (dllContent.empty())
    {
        refusalReason = std::format("failed to read lsfg-vk.dll at '{}'", PathToString(*dllPath));
        return nullptr;
    }

    std::string hash = retoque::lsfg::ComputeSha256Hex(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(dllContent.data()), dllContent.size()));

    // 4. Cache directory check: <baseCacheDir>/<hash>/
    std::filesystem::path cacheDir = baseCacheDir / hash;

    // Check if cache directory exists and contains shaders (e.g. 0.cso and 1.cso)
    const auto cso0 = cacheDir / "0.cso";
    const auto cso1 = cacheDir / "1.cso";
    if (!e.DirectoryExists(cacheDir) || !e.FileExists(cso0) || !e.FileExists(cso1))
    {
        refusalReason = std::format(
            "shader cache missing for lsfg-vk.dll (hash {:.8}). Run: python tools/lsfg-study/convert.py \"{}\"",
            hash.substr(0, 8), PathToString(*dllPath));
        return nullptr;
    }

    return std::make_unique<CacheShaderSource>(cacheDir, hash, *dllPath);
}

} // namespace Lsfg
