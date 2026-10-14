#include "pch.h"

#include "lsfg/LsfgEnvironment.h"
#include "lsfg/LsfgShaderLibrary.h"
#include "lsfg/LsfgShaderSource.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <set>

using namespace retoque::lsfg;

// ============================================================================
// Fake Environment for unit testing discovery
// ============================================================================
class MockLsfgEnvironment : public ILsfgEnvironment
{
  public:
    std::map<std::string, std::string> envVars;
    std::map<std::pair<std::string, std::string>, std::string> registryValues;
    std::set<std::string> existingFiles;
    std::set<std::string> existingDirectories;
    std::map<std::string, std::string> fileContents;

    std::optional<std::string> GetEnv(const std::string& name) const override
    {
        auto it = envVars.find(name);
        if (it != envVars.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<std::string> GetRegistryString(const std::string& key, const std::string& valueName) const override
    {
        auto it = registryValues.find({ key, valueName });
        if (it != registryValues.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    bool FileExists(const std::filesystem::path& path) const override
    {
        return existingFiles.contains(path.generic_string()) || existingFiles.contains(path.string());
    }

    bool DirectoryExists(const std::filesystem::path& path) const override
    {
        return existingDirectories.contains(path.generic_string()) || existingDirectories.contains(path.string());
    }

    std::string ReadFileToString(const std::filesystem::path& path) const override
    {
        auto it = fileContents.find(path.generic_string());
        if (it != fileContents.end())
        {
            return it->second;
        }
        it = fileContents.find(path.string());
        if (it != fileContents.end())
        {
            return it->second;
        }
        return {};
    }
};

void TestDiscovery_WindowsSteamLibraries()
{
    MockLsfgEnvironment env;
    env.registryValues[{ "Software\\Valve\\Steam", "SteamPath" }] = "C:/Program Files (x86)/Steam";
    env.existingDirectories.insert("C:/Program Files (x86)/Steam");

    const std::string vdf = R"("libraryfolders"
{
    "0"
    {
        "path"    "C:\\Program Files (x86)\\Steam"
    }
    "1"
    {
        "path"    "D:\\SteamLibrary"
    }
})";

    env.existingFiles.insert("C:/Program Files (x86)/Steam/steamapps/libraryfolders.vdf");
    env.fileContents["C:/Program Files (x86)/Steam/steamapps/libraryfolders.vdf"] = vdf;

    const std::string targetDll = "D:/SteamLibrary/steamapps/common/Lossless Scaling/lsfg-vk.dll";
    env.existingFiles.insert(targetDll);

    auto found = LsfgShaderLibrary::FindDll(std::nullopt, &env);
    assert(found.has_value());
    assert(found->generic_string() == targetDll);
    std::cout << "  PASS: TestDiscovery_WindowsSteamLibraries\n";
}

void TestDiscovery_WineZDrive()
{
    MockLsfgEnvironment env;
    env.envVars["HOME"] = "/home/testuser"; // privacy: allow
    const std::string targetDll =
        "Z:/home/testuser/.local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll"; // privacy: allow
    env.existingFiles.insert(targetDll);

    auto found = LsfgShaderLibrary::FindDll(std::nullopt, &env);
    assert(found.has_value());
    assert(found->generic_string() == targetDll);
    std::cout << "  PASS: TestDiscovery_WineZDrive\n";
}

void TestDiscovery_XdgDataHome()
{
    MockLsfgEnvironment env;
    env.envVars["XDG_DATA_HOME"] = "/var/custom/data";
    const std::string targetDll = "/var/custom/data/.local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll";
    env.existingFiles.insert(targetDll);

    auto found = LsfgShaderLibrary::FindDll(std::nullopt, &env);
    assert(found.has_value());
    assert(found->generic_string() == targetDll);
    std::cout << "  PASS: TestDiscovery_XdgDataHome\n";
}

void TestDiscovery_OverrideAndFixPath()
{
    MockLsfgEnvironment env;
    env.existingFiles.insert("/custom/dir/lsfg-vk.dll");
    env.existingDirectories.insert("/custom/dir/Lossless Scaling");
    env.existingFiles.insert("/custom/dir/Lossless Scaling/lsfg-vk.dll");

    // 1. Direct path
    auto p1 = LsfgShaderLibrary::FindDll("/custom/dir/lsfg-vk.dll", &env);
    assert(p1 && p1->generic_string() == "/custom/dir/lsfg-vk.dll");

    // 2. Folder mapping
    auto p2 = LsfgShaderLibrary::FixDllPath("/custom/dir/Lossless Scaling", &env);
    assert(p2.generic_string() == "/custom/dir/Lossless Scaling/lsfg-vk.dll");

    // 3. Lossless.dll mapping
    auto p3 = LsfgShaderLibrary::FixDllPath("/custom/dir/Lossless Scaling/Lossless.dll", &env);
    assert(p3.generic_string() == "/custom/dir/Lossless Scaling/lsfg-vk.dll");

    std::cout << "  PASS: TestDiscovery_OverrideAndFixPath\n";
}

void TestShaderIdResolution()
{
    // Base shaders
    assert(LsfgShaderLibrary::ResolveResourceId("mipmaps", false, false) == 0);
    assert(LsfgShaderLibrary::ResolveResourceId("generate_8bit", false, false) == 1);
    assert(LsfgShaderLibrary::ResolveResourceId("generate_16bit", true, false) == 2);
    assert(LsfgShaderLibrary::ResolveResourceId("generate", false, false) == 1);
    assert(LsfgShaderLibrary::ResolveResourceId("generate", true, false) == 2);

    // Core shader: gamma0 (base index 3)
    assert(LsfgShaderLibrary::ResolveResourceId("gamma0", false, false) == 3);
    assert(LsfgShaderLibrary::ResolveResourceId("gamma0", false, true) == 27); // +24
    assert(LsfgShaderLibrary::ResolveResourceId("gamma0", true, false) == 51); // +48
    assert(LsfgShaderLibrary::ResolveResourceId("gamma0", true, true) == 75);  // +48 + 24

    // Core shader: beta4 (base index 26, max)
    assert(LsfgShaderLibrary::ResolveResourceId("beta4", false, false) == 26);
    assert(LsfgShaderLibrary::ResolveResourceId("beta4", false, true) == 50); // +24
    assert(LsfgShaderLibrary::ResolveResourceId("beta4", true, false) == 74); // +48
    assert(LsfgShaderLibrary::ResolveResourceId("beta4", true, true) == 98);  // +48 + 24

    std::cout << "  PASS: TestShaderIdResolution\n";
}

void TestCacheManagement(const std::filesystem::path& tmpDir)
{
    LsfgShaderLibrary lib;
    const std::filesystem::path cacheDir = tmpDir / "test_cache";
    lib.SetCacheDirectory(cacheDir);

    const uint32_t rid = 42;
    assert(!lib.HasCachedShader(rid));

    const std::vector<uint8_t> dummyDxil = { 0x44, 0x58, 0x42, 0x43, 0x01, 0x02, 0x03, 0x04 };
    assert(lib.StoreCachedShader(rid, dummyDxil));
    assert(lib.HasCachedShader(rid));

    const auto loaded = lib.LoadCachedShader(rid);
    assert(loaded == dummyDxil);

    assert(!lib.IsCacheComplete()); // Needs all 99 shaders

    std::cout << "  PASS: TestCacheManagement\n";
}

void TestRealDllParsing()
{
    const char* home = std::getenv("HOME");
    if (!home)
    {
        std::cout << "  SKIP: TestRealDllParsing (HOME not set)\n";
        return;
    }

    const std::filesystem::path realDll =
        std::filesystem::path(home) / ".local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll";

    if (!std::filesystem::exists(realDll))
    {
        std::cout << "  SKIP: TestRealDllParsing (lsfg-vk.dll not present at Steam location)\n";
        return;
    }

    LsfgShaderLibrary lib;
    bool ok = lib.LoadFromDll(realDll);
    if (!ok)
    {
        std::cerr << "FAIL: Failed to load real DLL: " << lib.GetLastError() << "\n";
        assert(false);
    }

    assert(lib.IsLoaded());
    assert(lib.GetModuleCount() == kTotalShaderCount);

    // Verify content hash
    assert(lib.GetDllHash() == "d22b2d3f77b31fb7926a08dec2a118c6e83ba6e772fe0f16476abf986c865804");

    // Verify all 99 modules have valid SPIR-V magic and non-zero size
    for (uint32_t rid = 0; rid < kTotalShaderCount; ++rid)
    {
        assert(lib.HasModule(rid));
        auto spv = lib.GetModuleBytecode(rid);
        assert(spv.size() >= 4);
        assert(spv.size() % 4 == 0);
        uint32_t magic = *reinterpret_cast<const uint32_t*>(spv.data());
        assert(magic == kSpirvMagic);
    }

    // Check specific shader queries
    auto mipmaps = lib.GetShader("mipmaps", false, false);
    assert(!mipmaps.empty());

    auto gamma0_fp32 = lib.GetShader("gamma0", false, false);
    assert(!gamma0_fp32.empty());

    auto gamma0_fp16 = lib.GetShader("gamma0", true, false);
    assert(!gamma0_fp16.empty());

    std::cout << "  PASS: TestRealDllParsing (99/99 SPIR-V modules validated from user DLL)\n";
}

void TestSelection_Logic()
{
    MockLsfgEnvironment env;
    std::string refusal;

    // Case 1: Override wins
    env.existingDirectories.insert("C:/custom/dxil");
    auto src1 = Lsfg::MakeShaderSource(L"C:/custom/dxil", L"", "C:/cache", refusal, &env);
    assert(src1 != nullptr);
    assert(src1->Describe().find("C:/custom/dxil") != std::string::npos);

    // Case 2: Missing DLL -> clean refusal
    auto src2 = Lsfg::MakeShaderSource(L"", L"", "C:/cache", refusal, &env);
    assert(src2 == nullptr);
    assert(refusal.find("lsfg-vk.dll not found") != std::string::npos);

    // Case 3: DLL found, cache miss -> clean refusal with convert.py command
    env.existingFiles.insert("C:/games/lsfg-vk.dll");
    env.fileContents["C:/games/lsfg-vk.dll"] = "fake-dll-content-v1";
    const std::string hashV1 = ComputeSha256Hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("fake-dll-content-v1"), std::strlen("fake-dll-content-v1")));

    auto src3 = Lsfg::MakeShaderSource(L"", L"C:/games/lsfg-vk.dll", "C:/cache", refusal, &env);
    assert(src3 == nullptr);
    assert(refusal.find("shader cache missing") != std::string::npos);
    assert(refusal.find(hashV1.substr(0, 8)) != std::string::npos);
    assert(refusal.find("convert.py") != std::string::npos);

    // Case 4: Cache hit -> loads from cache
    const std::string cacheDirV1 = "C:/cache/" + hashV1;
    env.existingDirectories.insert(cacheDirV1);
    env.existingFiles.insert(cacheDirV1 + "/0.cso");
    env.existingFiles.insert(cacheDirV1 + "/1.cso");

    auto src4 = Lsfg::MakeShaderSource(L"", L"C:/games/lsfg-vk.dll", "C:/cache", refusal, &env);
    assert(src4 != nullptr);
    assert(src4->Describe().find(hashV1.substr(0, 8)) != std::string::npos);

    // Case 5: DLL changed -> new hash -> old cache invalidated (miss for new hash)
    env.fileContents["C:/games/lsfg-vk.dll"] = "fake-dll-content-v2";
    const std::string hashV2 = ComputeSha256Hex(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>("fake-dll-content-v2"), std::strlen("fake-dll-content-v2")));
    assert(hashV1 != hashV2);

    auto src5 = Lsfg::MakeShaderSource(L"", L"C:/games/lsfg-vk.dll", "C:/cache", refusal, &env);
    assert(src5 == nullptr);
    assert(refusal.find("shader cache missing") != std::string::npos);
    assert(refusal.find(hashV2.substr(0, 8)) != std::string::npos);

    std::cout << "  PASS: TestSelection_Logic\n";
}

int main(int argc, char** argv)
{
    std::filesystem::path tmpDir = "/tmp";
    if (argc > 1)
    {
        tmpDir = argv[1];
    }

    std::cout << "Running LsfgShaderLibrary tests:\n";
    TestDiscovery_WindowsSteamLibraries();
    TestDiscovery_WineZDrive();
    TestDiscovery_XdgDataHome();
    TestDiscovery_OverrideAndFixPath();
    TestShaderIdResolution();
    TestCacheManagement(tmpDir);
    TestSelection_Logic();
    TestRealDllParsing();
    std::cout << "All LsfgShaderLibrary tests passed successfully!\n";
    return 0;
}
