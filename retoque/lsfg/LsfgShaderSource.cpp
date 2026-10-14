#include "pch.h"

#include "LsfgShaderSource.h"

#include <filesystem>
#include <fstream>

namespace Lsfg
{
namespace
{
// A DXIL module of this graph is 5..70 KB; anything far outside that is not one.
constexpr std::uintmax_t kMaxShaderBytes = 4u * 1024u * 1024u;

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
            error = std::format("{} is missing ({})", wstring_to_string(path.wstring()), ec.message());
            return false;
        }

        if (size < 32 || size > kMaxShaderBytes)
        {
            error = std::format("{} has an implausible size ({} bytes)", wstring_to_string(path.wstring()), size);
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        out.resize(size_t(size));
        if (!file || !file.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)))
        {
            out.clear();
            error = std::format("{} could not be read", wstring_to_string(path.wstring()));
            return false;
        }

        return true;
    }

    std::string Describe() const override
    {
        return _dir.empty() ? std::string("no ShaderDir") : wstring_to_string(_dir) + "\\<rid>.cso";
    }

  private:
    std::wstring _dir;
};
} // namespace

std::unique_ptr<IShaderSource> MakeDirectoryShaderSource(const std::wstring& dir)
{
    return std::make_unique<DirectoryShaderSource>(dir);
}

} // namespace Lsfg
