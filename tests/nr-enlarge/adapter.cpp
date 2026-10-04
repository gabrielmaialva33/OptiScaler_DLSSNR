// Transfer 2's NGX half (dlssnr/DlssNr_PrivateSr.cpp, linked as built) against a recording driver core: what
// it writes at creation and at evaluation, in which overload, and the order it frees things in.
#include "ngx_fakes.h"

#include <set>

using namespace DlssNr::PrivateSr;

namespace
{
int cases = 0;
ID3D12GraphicsCommandList createList { 1 }, evaluateList { 2 };
ID3D12Resource carrier, output, depth, motion, exposure;

const Shape kShape { 1720, 720, 3440, 1440, true };

Frame AFrame()
{
    Frame f;
    f.color = &carrier;
    f.output = &output;
    f.depth = &depth;
    f.motion = &motion;
    f.exposure = &exposure;
    f.width = kShape.width;
    f.height = kShape.height;
    f.reset = true;
    f.mvScaleX = 1720.0f;
    f.mvScaleY = -720.0f;
    f.frameTimeMs = 8.25f;
    return f;
}

std::set<std::string> Keys(const Ngx::Table& t)
{
    std::set<std::string> keys;
    for (const auto& [k, v] : t.values)
        keys.insert(k);
    return keys;
}

void CreateWritesTheSrContract()
{
    for (bool inverted : { true, false })
    {
        Ngx::Clear();
        Feature feature;
        Shape shape = kShape;
        shape.depthInverted = inverted;
        assert(feature.Create(Ngx::FullApi(), &createList, shape) == NVSDK_NGX_Result_Success);
        assert(feature.Live() && feature.Built() == shape);

        // One table, then one SuperSampling feature on the list it was asked to create on -- never RR.
        const auto& d = Ngx::driver;
        assert(d.calls.size() == 2 && d.calls[0].what == "allocate" && d.calls[1].what == "create");
        assert(d.calls[1].list == &createList && d.calls[1].feature == NVSDK_NGX_Feature_SuperSampling);
        assert(d.calls[1].table == d.tables.back().get());

        // Exactly the creation parameters, through the overloads NVIDIA's helper uses.
        const auto& t = d.LastTable();
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_CreationNodeMask) == 1u);
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_VisibilityNodeMask) == 1u);
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_Width) == 1720u);
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_Height) == 720u);
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_OutWidth) == 3440u);
        assert(t.At<unsigned>(NVSDK_NGX_Parameter_OutHeight) == 1440u);
        assert(t.At<int>(NVSDK_NGX_Parameter_PerfQualityValue) == NVSDK_NGX_PerfQuality_Value_MaxQuality);

        // LDR (no IsHDR), no auto exposure, no jitter flag; motion at the input's size; depth as the game has it.
        const int flags = t.At<int>(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        assert(flags ==
               (NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | (inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0)));
        assert((flags & (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                         NVSDK_NGX_DLSS_Feature_Flags_MVJittered | NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) == 0);
        assert(Keys(t).size() == 8);

        // A second create on a live feature is refused without a call.
        assert(feature.Create(Ngx::FullApi(), &createList, shape) == NVSDK_NGX_Result_FAIL_FeatureAlreadyExists);
        assert(d.calls.size() == 2);
        feature.Release(Ngx::FullApi());
        ++cases;
    }
}

void EvaluateWritesTheFrame()
{
    Ngx::Clear();
    Feature feature;
    assert(feature.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Success);
    const auto created = Keys(Ngx::driver.LastTable());

    Frame f = AFrame();
    assert(feature.Evaluate(Ngx::FullApi(), &evaluateList, f) == NVSDK_NGX_Result_Success);

    // On the list it was handed, through the feature's own handle and table, with no progress callback.
    const auto& d = Ngx::driver;
    const auto& call = d.calls.back();
    assert(call.what == "evaluate" && call.list == &evaluateList && !call.callback);
    assert(call.handle == d.handles.back().get() && call.table == d.tables.back().get());

    const auto& t = d.LastTable();
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Color) == &carrier);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Output) == &output);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_Depth) == &depth);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_MotionVectors) == &motion);
    assert(t.At<ID3D12Resource*>(NVSDK_NGX_Parameter_ExposureTexture) == &exposure);
    assert(t.At<unsigned>(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width) == 1720u);
    assert(t.At<unsigned>(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height) == 720u);
    assert(t.At<int>(NVSDK_NGX_Parameter_Reset) == 1);
    assert(t.At<float>(NVSDK_NGX_Parameter_Jitter_Offset_X) == 0.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_Jitter_Offset_Y) == 0.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_MV_Scale_X) == 1720.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_MV_Scale_Y) == -720.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec) == 8.25f);
    assert(t.At<float>(NVSDK_NGX_Parameter_DLSS_Pre_Exposure) == 1.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_DLSS_Exposure_Scale) == 1.0f);
    assert(t.At<float>(NVSDK_NGX_Parameter_Sharpness) == 0.0f);
    assert(Keys(t).size() == created.size() + 16);

    // Held: a motion scale of 0 goes through as 0, not as the helper's "unset means 1".
    f.reset = false;
    f.mvScaleX = f.mvScaleY = 0.0f;
    assert(feature.Evaluate(Ngx::FullApi(), &evaluateList, f) == NVSDK_NGX_Result_Success);
    assert(t.At<int>(NVSDK_NGX_Parameter_Reset) == 0 && t.At<float>(NVSDK_NGX_Parameter_MV_Scale_X) == 0.0f);

    // The driver's answer is the answer.
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_FAIL_InvalidParameter;
    assert(feature.Evaluate(Ngx::FullApi(), &evaluateList, f) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    Ngx::driver.evaluateResult = NVSDK_NGX_Result_Success;

    // Refused before the driver is asked: a frame of another size, a missing texture, no entry point.
    const auto before = d.calls.size();
    Frame wrong = AFrame();
    wrong.width = 1721;
    assert(feature.Evaluate(Ngx::FullApi(), &evaluateList, wrong) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    for (ID3D12Resource* Frame::* slot :
         { &Frame::color, &Frame::output, &Frame::depth, &Frame::motion, &Frame::exposure })
    {
        Frame missing = AFrame();
        missing.*slot = nullptr;
        assert(feature.Evaluate(Ngx::FullApi(), &evaluateList, missing) == NVSDK_NGX_Result_FAIL_InvalidParameter);
    }
    Api noEvaluate = Ngx::FullApi();
    noEvaluate.evaluate = nullptr;
    assert(feature.Evaluate(noEvaluate, &evaluateList, AFrame()) == NVSDK_NGX_Result_FAIL_NotInitialized);
    assert(feature.Evaluate(Ngx::FullApi(), nullptr, AFrame()) == NVSDK_NGX_Result_FAIL_NotInitialized);
    assert(d.calls.size() == before);

    feature.Release(Ngx::FullApi());
    Feature never;
    assert(never.Evaluate(Ngx::FullApi(), &evaluateList, AFrame()) == NVSDK_NGX_Result_FAIL_FeatureNotFound);
    cases += 3;
}

void ReleaseOrder()
{
    // The handle first, then its table; once. A second release, or one after abandon, calls nothing.
    Ngx::Clear();
    Feature feature;
    assert(feature.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Success);
    const auto* handle = Ngx::driver.handles.back().get();
    const auto* table = Ngx::driver.tables.back().get();
    feature.Release(Ngx::FullApi());
    const auto& c = Ngx::driver.calls;
    assert(c.size() == 4 && c[2].what == "release" && c[2].handle == handle);
    assert(c[3].what == "destroy" && c[3].table == table && Ngx::driver.tables.back()->destroyed);
    assert(!feature.Live() && feature.Built() == Shape {});
    feature.Release(Ngx::FullApi());
    assert(c.size() == 4);
    ++cases;

    // Process exit: nothing is called, whatever the api offers.
    Ngx::Clear();
    Feature exiting;
    assert(exiting.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Success);
    exiting.Abandon();
    exiting.Release(Ngx::FullApi());
    assert(Ngx::driver.calls.size() == 2 && !exiting.Live());
    ++cases;

    // A core the game shut down hands out null entry points: they are skipped, not called.
    Ngx::Clear();
    Feature orphan;
    assert(orphan.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Success);
    orphan.Release(Api {});
    assert(Ngx::driver.calls.size() == 2 && !orphan.Live());
    ++cases;
}

void CreateFailures()
{
    // Nothing is called without all five entry points or a list.
    for (int missing = 0; missing < 5; ++missing)
    {
        Ngx::Clear();
        Api api = Ngx::FullApi();
        if (missing == 0)
            api.allocate = nullptr;
        if (missing == 1)
            api.destroy = nullptr;
        if (missing == 2)
            api.create = nullptr;
        if (missing == 3)
            api.evaluate = nullptr;
        if (missing == 4)
            api.release = nullptr;
        Feature feature;
        assert(feature.Create(api, &createList, kShape) == NVSDK_NGX_Result_FAIL_NotInitialized);
        assert(Ngx::driver.calls.empty() && !feature.Live());
    }
    {
        Ngx::Clear();
        Feature feature;
        assert(feature.Create(Ngx::FullApi(), nullptr, kShape) == NVSDK_NGX_Result_FAIL_NotInitialized);
        assert(Ngx::driver.calls.empty());
    }
    ++cases;

    // The allocation refused, or "succeeded" with no table: no create, the result says so.
    Ngx::Clear();
    Ngx::driver.allocateResult = NVSDK_NGX_Result_FAIL_OutOfGPUMemory;
    Feature a;
    assert(a.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_FAIL_OutOfGPUMemory);
    assert(Ngx::driver.calls.size() == 1 && !a.Live());
    Ngx::Clear();
    Ngx::driver.allocateNull = true;
    Feature b;
    assert(b.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Fail);
    assert(Ngx::driver.calls.size() == 1 && !b.Live());
    ++cases;

    // The create refused (an RR-only title with no nvngx_dlss.dll, say), or "succeeded" with no handle: the
    // table goes back, nothing is kept, and a later attempt starts clean.
    for (bool nullHandle : { false, true })
    {
        Ngx::Clear();
        Ngx::driver.createResult = nullHandle ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        Ngx::driver.createNullHandle = nullHandle;
        Feature feature;
        const auto result = feature.Create(Ngx::FullApi(), &createList, kShape);
        assert(result == (nullHandle ? NVSDK_NGX_Result_Fail : NVSDK_NGX_Result_FAIL_FeatureNotSupported));
        const auto& c = Ngx::driver.calls;
        assert(c.size() == 3 && c[2].what == "destroy" && Ngx::driver.tables.back()->destroyed);
        assert(!feature.Live());
        feature.Release(Ngx::FullApi());
        assert(c.size() == 3);

        Ngx::driver.createResult = NVSDK_NGX_Result_Success;
        Ngx::driver.createNullHandle = false;
        assert(feature.Create(Ngx::FullApi(), &createList, kShape) == NVSDK_NGX_Result_Success);
        feature.Release(Ngx::FullApi());
    }
    ++cases;
}
} // namespace

int main()
{
    CreateWritesTheSrContract();
    EvaluateWritesTheFrame();
    ReleaseOrder();
    CreateFailures();
    std::printf("PASS: %d private DLSS SR adapter cases: creation and evaluation parameters by overload, release "
                "order, abandon, refusals before any driver call\n",
                cases);
    return 0;
}
