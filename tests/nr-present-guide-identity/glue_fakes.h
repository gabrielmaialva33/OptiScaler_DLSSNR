// The least the identity glue in retoque/shaders/dlssnr/DlssNr_Dx12.cpp needs to compile on the host. run.py slices
// that glue out of the renderer, places it after this file inside namespace DlssNr, and appends glue_cases.h. A
// glue that grows a dependency fails to compile here, which is the point: it is supposed to need almost nothing.

#include <dlssnr/DlssNr_PresentGuideRing.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>
#include <shaders/dlssnr/DlssNr_Common.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using HRESULT = long;
#define FAILED(hr) (((HRESULT) (hr)) < 0)
#define IID_PPV_ARGS(pp) 0, reinterpret_cast<void**>(pp)

struct D3D12_RESOURCE_DESC
{
    uint64_t Width = 0;
    uint32_t Height = 0;
};

struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc {};
    D3D12_RESOURCE_DESC GetDesc() const { return desc; }
};

struct ID3D12Device
{
    int refs = 1;
    unsigned long Release() { return static_cast<unsigned long>(--refs); }
};

struct ID3D12GraphicsCommandList
{
    ID3D12Device* device = nullptr;

    HRESULT GetDevice(int, void** out)
    {
        *out = device;
        if (device == nullptr)
            return -1;
        ++device->refs;
        return 0;
    }
};

struct IDXGISwapChain3
{
};

#define NVSDK_NGX_Parameter_Output "Output"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X "DLSS.Output.Subrect.Base.X"
#define NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y "DLSS.Output.Subrect.Base.Y"

enum NVSDK_NGX_Result
{
    NVSDK_NGX_Result_Fail = 0,
    NVSDK_NGX_Result_Success = 1,
};

struct NVSDK_NGX_Parameter
{
    ID3D12Resource* output = nullptr;
    std::map<std::string, unsigned int> uints;

    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const
    {
        const auto it = uints.find(name);
        if (it == uints.end())
            return NVSDK_NGX_Result_Fail;
        *value = it->second;
        return NVSDK_NGX_Result_Success;
    }
};

inline std::vector<std::string> g_resourceNames;

inline ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    g_resourceNames = { a, b };
    return params->output;
}

template <typename T> struct FakeOption
{
    T value;
    T value_or_default() const { return value; }
};

struct Config
{
    FakeOption<bool> DlssNrEnabled { true };
    FakeOption<uint32_t> DlssNrHookMethod { 2 };
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

// The glue logs exactly one way: one preformatted line.
inline std::vector<std::string> g_logged;
inline void FakeLogInfo(const char* format, const std::string& line)
{
    assert(std::string(format) == "{}");
    g_logged.push_back(line);
}
#define LOG_INFO(...) FakeLogInfo(__VA_ARGS__)

namespace DlssNr
{
struct PresentGuideClone
{
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
};

static PresentGuideClone g_presentGuideClones[PresentGuides::kRingSize];
static PresentGuides::Ring g_presentGuideRing;
static unsigned long long g_nrLastEnhancedRecordedSeq = 0;

static long long g_fakeNowMs = 1000;
static long long NowMs() { return g_fakeNowMs; }
} // namespace DlssNr
