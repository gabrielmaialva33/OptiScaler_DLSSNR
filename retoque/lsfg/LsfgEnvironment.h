#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace retoque::lsfg
{

class ILsfgEnvironment
{
  public:
    virtual ~ILsfgEnvironment() = default;

    virtual std::optional<std::string> GetEnv(const std::string& name) const = 0;
    virtual std::optional<std::string> GetRegistryString(const std::string& key,
                                                         const std::string& valueName) const = 0;
    virtual bool FileExists(const std::filesystem::path& path) const = 0;
    virtual bool DirectoryExists(const std::filesystem::path& path) const = 0;
    virtual std::string ReadFileToString(const std::filesystem::path& path) const = 0;
};

class DefaultLsfgEnvironment : public ILsfgEnvironment
{
  public:
    std::optional<std::string> GetEnv(const std::string& name) const override;
    std::optional<std::string> GetRegistryString(const std::string& key, const std::string& valueName) const override;
    bool FileExists(const std::filesystem::path& path) const override;
    bool DirectoryExists(const std::filesystem::path& path) const override;
    std::string ReadFileToString(const std::filesystem::path& path) const override;

    static const DefaultLsfgEnvironment& Instance();
};

} // namespace retoque::lsfg
