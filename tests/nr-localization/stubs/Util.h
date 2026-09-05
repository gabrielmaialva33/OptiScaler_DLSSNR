#pragma once
#include <filesystem>
namespace Util
{
// Test DLL location: the temporary working directory, never a game installation.
inline std::filesystem::path DllPath() { return std::filesystem::current_path() / "OptiScaler.dll"; }
}
