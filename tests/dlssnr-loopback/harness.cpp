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

int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    std::printf("dlssnr-loopback: stage 1 skeleton\n");
    return 0;
}
