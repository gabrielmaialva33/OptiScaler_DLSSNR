#include <dlssnr/DlssNr_RenderSize.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_map>

struct FakeParams final : NVSDK_NGX_Parameter
{
    std::unordered_map<std::string, unsigned int> values;

    void Put(const char* n, unsigned int v) { values[n] = v; }

    void Set(const char* n, unsigned int v) override { Put(n, v); }
    void Set(const char* n, int v) override { Put(n, static_cast<unsigned int>(v)); }
    void Set(const char* n, unsigned long long v) override { Put(n, static_cast<unsigned int>(v)); }
    void Set(const char*, float) override {}
    void Set(const char*, double) override {}
    void Set(const char*, ID3D11Resource*) override {}
    void Set(const char*, ID3D12Resource*) override {}
    void Set(const char*, void*) override {}

    NVSDK_NGX_Result Get(const char* n, unsigned int* out) const override
    {
        auto it = values.find(n);
        if (it != values.end())
        {
            *out = it->second;
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_Fail;
    }

    NVSDK_NGX_Result Get(const char* n, int* out) const override
    {
        auto it = values.find(n);
        if (it != values.end())
        {
            *out = static_cast<int>(it->second);
            return NVSDK_NGX_Result_Success;
        }
        return NVSDK_NGX_Result_Fail;
    }

    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_Fail; }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_Fail; }
    void Reset() override { values.clear(); }
};

int main()
{
    unsigned int checks = 0;

    // Case 1: subrect present
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1280u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 720u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 1280 && res.height == 720);
        ++checks;
    }

    // Case 2: subrect absent and Width < OutWidth: returns Width/Height
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_Width, 1280u);
        p.Set(NVSDK_NGX_Parameter_Height, 720u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 1920u);
        p.Set(NVSDK_NGX_Parameter_OutHeight, 1080u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 1280 && res.height == 720);
        ++checks;
    }

    // Case 3: subrect absent and Width == OutWidth: returns 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_Width, 1920u);
        p.Set(NVSDK_NGX_Parameter_Height, 1080u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 1920u);
        p.Set(NVSDK_NGX_Parameter_OutHeight, 1080u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // Case 4: one key present and zero
    // 4a. subrect width present and zero, height absent -> 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // 4b. subrect height present and zero, width absent -> 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // 4c. both subrect keys present and zero -> 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // 4d. subrect width present and non-zero, height present and zero (incomplete subrect preserved for Stage 1
    // decline)
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 1280u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 1280 && res.height == 0);
        ++checks;
    }

    // 4e. subrect height present and non-zero, width present and zero (incomplete subrect preserved for Stage 1
    // decline)
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 720u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 720);
        ++checks;
    }

    // 4f. fallback with Width == 0 -> 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_Width, 0u);
        p.Set(NVSDK_NGX_Parameter_Height, 720u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 1920u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // 4g. fallback with Height == 0 -> 0/0
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_Width, 1280u);
        p.Set(NVSDK_NGX_Parameter_Height, 0u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 1920u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    // 4h. subrect present but zero, fallback available -> returns fallback Width/Height
    {
        FakeParams p;
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, 0u);
        p.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, 0u);
        p.Set(NVSDK_NGX_Parameter_Width, 1280u);
        p.Set(NVSDK_NGX_Parameter_Height, 720u);
        p.Set(NVSDK_NGX_Parameter_OutWidth, 1920u);
        const auto res = DlssNr::GetRenderSize(&p);
        assert(res.width == 1280 && res.height == 720);
        ++checks;
    }

    // 4i. nullptr params -> 0/0
    {
        const auto res = DlssNr::GetRenderSize(nullptr);
        assert(res.width == 0 && res.height == 0);
        ++checks;
    }

    assert(checks >= 11);
    std::printf("PASS: %u render-size cases checked\n", checks);
    return 0;
}
