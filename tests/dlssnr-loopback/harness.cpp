// Deterministic D3D12 driver for the production Neural Rendering path.
//
// This process renders a scene it fully controls and then calls the NGX entry points that
// OptiScaler.dll exports, which is how a game reaches NR. Nothing below that line is mocked.
//
// Two properties are the reason this exists, and neither is available in a game:
//   - The motion vectors are computed from matrices we own, not estimated from the image.
//   - Frame N is identical across runs, because every animated quantity is driven by the frame
//     counter and never by wall-clock time. Two runs are therefore comparable pixel by pixel.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_params.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace DirectX;

// --------------------------------------------------------------------------------------------
// Failure discipline: a harness that cannot reach its own instrumentation must fail loudly.
// A silent pass is the exact failure mode the manual in-game test had.
// --------------------------------------------------------------------------------------------

static void Require(bool ok, const char* what)
{
    if (!ok)
        throw std::runtime_error(what);
}

static void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s failed with 0x%08lX", what, (unsigned long) hr);
        throw std::runtime_error(buf);
    }
}

template <class T> struct Com
{
    T* p = nullptr;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

// --------------------------------------------------------------------------------------------
// Deterministic camera and animation.
//
// Everything here is a pure function of the frame index. No timers, no randomness that is not
// seeded from the index. That is what makes run-to-run image comparison meaningful.
// --------------------------------------------------------------------------------------------

struct Pose
{
    XMMATRIX view;
    XMMATRIX proj;
    XMMATRIX model;
};

static Pose PoseForFrame(uint64_t frame, float aspect)
{
    // A slow orbit plus a slower bob. Chosen so that consecutive frames differ enough to give the
    // temporal path real work, without the per-frame motion exceeding what DLSS expects.
    const float t = static_cast<float>(frame) * (1.0f / 120.0f);
    const float radius = 4.25f;
    const float eyeX = radius * std::sin(t * 0.35f);
    const float eyeZ = radius * std::cos(t * 0.35f);
    const float eyeY = 1.15f + 0.35f * std::sin(t * 0.21f);

    Pose pose;
    pose.view = XMMatrixLookAtLH(XMVectorSet(eyeX, eyeY, eyeZ, 1.0f), XMVectorSet(0.0f, 0.6f, 0.0f, 1.0f),
                                 XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    pose.proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(58.0f), aspect, 0.05f, 120.0f);
    pose.model = XMMatrixRotationY(t * 0.55f) * XMMatrixRotationX(0.18f * std::sin(t * 0.4f));
    return pose;
}

// Halton, the sequence DLSS integrations conventionally use. The jitter must be reported to the
// upscaler; it is deliberately NOT fed into the motion vector, which is scene motion only.
static float Halton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float f = 1.0f;
    while (index > 0)
    {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

struct Jitter
{
    float x, y;
};

static Jitter JitterForFrame(uint64_t frame)
{
    const uint32_t i = static_cast<uint32_t>(frame % 32) + 1;
    return { Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f };
}

// --------------------------------------------------------------------------------------------
// Constant buffer. The layout mirrors the cbuffer in scene.hlsl; the two are one ordered list.
// --------------------------------------------------------------------------------------------

struct alignas(256) FrameConstants
{
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 viewProjPrev;
    XMFLOAT4X4 model;
    XMFLOAT4X4 modelPrev;
    float jitter[2];
    float jitterPrev[2];
    float renderExtent[2];
    float frameIndex;
    float pad;
};

static_assert(sizeof(FrameConstants) % 256 == 0, "constant buffers are 256-byte aligned");

// --------------------------------------------------------------------------------------------
// NGX entry points, taken from the production DLL.
//
// OptiScaler exports the whole NVSDK_NGX_D3D12_* set, so this process calls the same functions a
// game reaches. Loading it by handle rather than linking keeps the harness honest about which
// binary it is testing: the path is printed, and a mismatch is visible rather than assumed.
// --------------------------------------------------------------------------------------------

using PFN_Init = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                                const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using PFN_Shutdown = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using PFN_AllocParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using PFN_DestroyParams = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using PFN_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                        NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using PFN_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using PFN_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                                          NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);

struct Ngx
{
    HMODULE dll = nullptr;
    PFN_Init Init = nullptr;
    PFN_Shutdown Shutdown = nullptr;
    PFN_AllocParams AllocateParameters = nullptr;
    PFN_DestroyParams DestroyParameters = nullptr;
    PFN_CreateFeature CreateFeature = nullptr;
    PFN_ReleaseFeature ReleaseFeature = nullptr;
    PFN_EvaluateFeature EvaluateFeature = nullptr;
};

static Ngx LoadNgx(const char* path)
{
    Ngx n;
    n.dll = LoadLibraryA(path);
    Require(n.dll != nullptr, "LoadLibrary on the OptiScaler DLL failed");

    auto get = [&](const char* name)
    {
        FARPROC p = GetProcAddress(n.dll, name);
        if (!p)
            throw std::runtime_error(std::string("export missing: ") + name);
        return p;
    };

    n.Init = (PFN_Init) get("NVSDK_NGX_D3D12_Init");
    n.Shutdown = (PFN_Shutdown) get("NVSDK_NGX_D3D12_Shutdown1");
    n.AllocateParameters = (PFN_AllocParams) get("NVSDK_NGX_D3D12_AllocateParameters");
    n.DestroyParameters = (PFN_DestroyParams) get("NVSDK_NGX_D3D12_DestroyParameters");
    n.CreateFeature = (PFN_CreateFeature) get("NVSDK_NGX_D3D12_CreateFeature");
    n.ReleaseFeature = (PFN_ReleaseFeature) get("NVSDK_NGX_D3D12_ReleaseFeature");
    n.EvaluateFeature = (PFN_EvaluateFeature) get("NVSDK_NGX_D3D12_EvaluateFeature");
    return n;
}

// --------------------------------------------------------------------------------------------
// Resources. Contents do not matter for a lifetime or settling test -- what the gate observes is
// the resource extent -- so these are committed textures left uninitialised rather than a rendered
// scene. The scene shader exists for the image-comparison work, which is a different question.
//
// States follow what the DLSS integration actually requires, which the SDK documentation omits and
// is only written down in an NVIDIA forum thread: inputs in PIXEL|NON_PIXEL_SHADER_RESOURCE, the
// target in UNORDERED_ACCESS, and the target must be a UAV rather than a render target.
// --------------------------------------------------------------------------------------------

static ID3D12Resource* MakeTexture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
                                   D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* r = nullptr;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)),
          "CreateCommittedResource");
    return r;
}

struct Frame
{
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;

    void Release()
    {
        for (auto* r : { colour, output, depth, motion })
            if (r)
                r->Release();
        colour = output = depth = motion = nullptr;
    }
};

static Frame MakeFrame(ID3D12Device* device, UINT renderW, UINT renderH, UINT outW, UINT outH)
{
    const auto srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Frame f;
    f.colour = MakeTexture(device, renderW, renderH, DXGI_FORMAT_R16G16B16A16_FLOAT, false, srv);
    f.depth = MakeTexture(device, renderW, renderH, DXGI_FORMAT_R32_FLOAT, false, srv);
    f.motion = MakeTexture(device, renderW, renderH, DXGI_FORMAT_R16G16_FLOAT, false, srv);
    f.output = MakeTexture(device, outW, outH, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return f;
}

int main(int argc, char** argv)
{
    const char* dllPath = argc > 1 ? argv[1] : "OptiScaler.dll";

    try
    {
        std::printf("dlssnr-loopback: driving the production NR path\n");

        Com<IDXGIFactory4> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

        // A null adapter asks the runtime to choose, and under Wine that returns E_INVALIDARG,
        // which reads like a bad argument when it means "no default was resolved".
        Com<ID3D12Device> device;
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* adapter = nullptr;
            if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 ad {};
            adapter->GetDesc1(&ad);
            const bool software = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            const HRESULT hr =
                software ? E_FAIL : D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
            std::printf("  adapter %u: vendor=%04X device=%04X -> 0x%08lX\n", i, ad.VendorId, ad.DeviceId,
                        (unsigned long) hr);
            adapter->Release();
            if (SUCCEEDED(hr))
                break;
        }
        Require(device.p != nullptr, "no adapter produced a D3D12 device");

        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Com<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        Com<ID3D12CommandAllocator> alloc;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
              "CreateCommandAllocator");
        Com<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list)),
              "CreateCommandList");
        list->Close();
        Com<ID3D12Fence> fence;
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        Require(fenceEvent != nullptr, "CreateEvent");
        UINT64 fenceValue = 0;

        Ngx ngx = LoadNgx(dllPath);
        std::printf("  NGX exports resolved from the production DLL\n");

        NVSDK_NGX_Result r = ngx.Init(0x1337, L".", device, nullptr, NVSDK_NGX_Version_API);
        std::printf("  NVSDK_NGX_D3D12_Init -> 0x%08X\n", (unsigned) r);
        Require(NVSDK_NGX_SUCCEED(r), "NGX init refused");

        // The sweep. Each entry is a render extent; the output stays fixed, which is what a real
        // upscaler contract looks like and what the settling gate observes changing.
        const UINT outW = 3440, outH = 1440;
        const struct { UINT w, h; int holdMs; } steps[] = {
            { 2292, 960, 1500 },  { 2292, 960, 1500 },  { 1720, 720, 1500 },
            { 2292, 960, 200 },   { 1720, 720, 200 },   { 2292, 960, 200 },  // oscillation, gate should hold
            { 2292, 960, 1500 },
        };

        NVSDK_NGX_Handle* handle = nullptr;
        NVSDK_NGX_Parameter* params = nullptr;
        UINT builtW = 0, builtH = 0;
        unsigned evaluates = 0, creates = 0;

        for (const auto& step : steps)
        {
            if (step.w != builtW || step.h != builtH)
            {
                if (handle)
                {
                    ngx.ReleaseFeature(handle);
                    handle = nullptr;
                }
                if (params)
                {
                    ngx.DestroyParameters(params);
                    params = nullptr;
                }
                Check(ngx.AllocateParameters(&params) == NVSDK_NGX_Result_Success ? S_OK : E_FAIL,
                      "AllocateParameters");
                params->Set(NVSDK_NGX_Parameter_Width, step.w);
                params->Set(NVSDK_NGX_Parameter_Height, step.h);
                params->Set(NVSDK_NGX_Parameter_OutWidth, outW);
                params->Set(NVSDK_NGX_Parameter_OutHeight, outH);
                params->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxQuality);
                params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
                params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

                Check(alloc->Reset(), "allocator reset");
                Check(list->Reset(alloc, nullptr), "list reset");
                r = ngx.CreateFeature(list, NVSDK_NGX_Feature_SuperSampling, params, &handle);
                list->Close();
                ID3D12CommandList* lists[] = { list.p };
                queue->ExecuteCommandLists(1, lists);
                queue->Signal(fence, ++fenceValue);
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                WaitForSingleObject(fenceEvent, 5000);

                std::printf("  create %ux%u -> %ux%u : 0x%08X\n", step.w, step.h, outW, outH, (unsigned) r);
                Require(NVSDK_NGX_SUCCEED(r), "CreateFeature refused");
                builtW = step.w;
                builtH = step.h;
                ++creates;
            }

            Frame frame = MakeFrame(device, step.w, step.h, outW, outH);
            const auto deadline = GetTickCount64() + (ULONGLONG) step.holdMs;
            while (GetTickCount64() < deadline)
            {
                Check(alloc->Reset(), "allocator reset");
                Check(list->Reset(alloc, nullptr), "list reset");
                params->Set(NVSDK_NGX_Parameter_Color, frame.colour);
                params->Set(NVSDK_NGX_Parameter_Output, frame.output);
                params->Set(NVSDK_NGX_Parameter_Depth, frame.depth);
                params->Set(NVSDK_NGX_Parameter_MotionVectors, frame.motion);
                params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
                params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
                params->Set(NVSDK_NGX_Parameter_MV_Scale_X, (float) step.w);
                params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, (float) step.h);
                params->Set(NVSDK_NGX_Parameter_Reset, evaluates == 0 ? 1 : 0);
                params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, step.w);
                params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, step.h);

                r = ngx.EvaluateFeature(list, handle, params, nullptr);
                list->Close();
                ID3D12CommandList* lists[] = { list.p };
                queue->ExecuteCommandLists(1, lists);
                queue->Signal(fence, ++fenceValue);
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                WaitForSingleObject(fenceEvent, 5000);
                ++evaluates;
                if (!NVSDK_NGX_SUCCEED(r))
                {
                    std::printf("  evaluate %ux%u refused: 0x%08X\n", step.w, step.h, (unsigned) r);
                    break;
                }
            }
            frame.Release();
        }

        std::printf("\n  creates=%u evaluates=%u\n", creates, evaluates);
        Require(creates > 0, "ZERO COVERAGE: no feature was created");
        Require(evaluates > 0, "ZERO COVERAGE: EvaluateFeature was never reached");

        if (handle)
            ngx.ReleaseFeature(handle);
        if (params)
            ngx.DestroyParameters(params);
        ngx.Shutdown(device);
        CloseHandle(fenceEvent);
        std::printf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
