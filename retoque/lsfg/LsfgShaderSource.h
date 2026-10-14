#pragma once

// LSFG study (branch study/lsfg-d3d12, never merged): where the pass graph's DXIL comes from.
//
// This step reads precompiled DXIL, one file per resource id (<ShaderDir>\<rid>.cso), produced on the user's
// machine from the user's own lsfg-vk.dll by tools/lsfg-study/convert.py. Nothing derived from Lossless
// Scaling is in git or in OptiScaler.dll. The run-time converter (LsfgShaderLibrary, SPIR-V from lsfg-vk.dll
// to DXIL) plugs in behind the same interface later. LoadShader runs on the build worker, never on the
// present path.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

} // namespace Lsfg
