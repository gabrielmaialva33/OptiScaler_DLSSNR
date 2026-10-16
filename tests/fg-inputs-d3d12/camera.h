#pragma once

#include <DirectXMath.h>
#include <framegen/dlssg/GameCamera.h>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace CameraFixture
{
using namespace DirectX;

struct CameraOutput
{
    float _cameraFar[4] { 100, 100, 100, 100 }, _cameraNear[4] { 1, 1, 1, 1 };
    float _cameraVFov[4] { 1, 1, 1, 1 }, _cameraAspectRatio[4] { 1.5f, 1.5f, 1.5f, 1.5f };
    float _cameraPosition[4][3] {}, _cameraUp[4][3] {}, _cameraForward[4][3] {}, _cameraRight[4][3] {};
    float _jitterX[4] {}, _jitterY[4] {}, _mvScaleX[4] {}, _mvScaleY[4] {};
    unsigned _reset[4] {};
    struct Motion
    {
        unsigned width = 256, height = 128;
    } motion;
    Motion* GetResource(FG_ResourceType, uint64_t) { return &motion; }
    bool IsInvertedDepth() { return false; }
    bool IsLowResMV() { return false; }
    bool IsJitteredMVs() { return false; }
    DlssgGameCamera<4> _gameCamera;
    sl::Constants received;
    bool IsInfiniteDepth() { return false; }
    bool Dispatch(uint64_t willDispatchFrame)
    {
        auto fIndex = willDispatchFrame % 4;
#include "camera_production.inc"
    }
    sl::Constants Evaluate(uint64_t frame)
    {
        Require(Dispatch(frame), "camera dispatch fixture");
        return received;
    }
};

sl::float4x4 Store(FXMMATRIX matrix)
{
    XMFLOAT4X4 values;
    XMStoreFloat4x4(&values, matrix);
    sl::float4x4 result;
    static_assert(sizeof(result) == sizeof(values));
    std::memcpy(&result, &values, sizeof(result));
    return result;
}

bool Same(const sl::float4x4& a, const sl::float4x4& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

// Scalar row-vector multiplication keeps the oracle independent of XMVector4Transform and the inverse chain.
using Point = std::array<double, 4>;
Point Transform(const Point& point, const sl::float4x4& matrix)
{
    float values[16];
    std::memcpy(values, &matrix, sizeof(values));
    Point result {};
    for (size_t col = 0; col < 4; col++)
        for (size_t row = 0; row < 4; row++)
            result[col] += point[row] * values[row * 4 + col];
    return result;
}

void Near(const Point& actual, const Point& expected, const char* message)
{
    Require(std::abs(actual[3]) > 1e-6 && std::abs(expected[3]) > 1e-6, message);
    for (size_t i = 0; i < 3; i++)
        Require(std::abs(actual[i] / actual[3] - expected[i] / expected[3]) < 0.0002, message);
}

void Run()
{
    CameraOutput output;
    const auto fallback = output.Evaluate(1);
    const auto currentView =
        XMMatrixLookAtRH(XMVectorSet(0.5f, 0.25f, 2, 1), XMVectorSet(0, 0, -2, 1), XMVectorSet(0, 1, 0, 0));
    const auto previousView =
        XMMatrixLookAtRH(XMVectorSet(0.25f, 0.1f, 2.2f, 1), XMVectorSet(0.1f, 0, -2, 1), XMVectorSet(0, 1, 0, 0));
    const auto currentScalars = Store(currentView), previousScalars = Store(previousView);
    const Point worldPoints[] { { 0.2, -0.1, -2, 1 }, { -0.7, 0.5, -4, 1 }, { 1, -0.6, -7, 1 } };
    for (int kind = 0; kind < 4; kind++)
    {
        const auto projection = kind == 0   ? XMMatrixPerspectiveFovRH(0.9f, 1.7f, 0.2f, 250.f)
                                : kind == 1 ? XMMatrixPerspectiveOffCenterRH(-0.15f, 0.3f, -0.2f, 0.1f, 0.2f, 250.f)
                                : kind == 2 ? XMMatrixPerspectiveFovRH(0.9f, 1.7f, 250.f, 0.2f)
                                            : XMMatrixOrthographicOffCenterRH(-4, 5, -3, 2, 0.2f, 250.f);
        const auto currentVP = XMMatrixMultiply(currentView, projection);
        const auto previousVP = XMMatrixMultiply(previousView, projection);
        const auto temporal = XMMatrixMultiply(XMMatrixInverse(nullptr, currentVP), previousVP);
        sl::Constants game;
        game.cameraViewToClip = Store(projection);
        game.clipToCameraView = Store(XMMatrixInverse(nullptr, projection));
        game.clipToPrevClip = Store(temporal);
        game.prevClipToClip = Store(XMMatrixInverse(nullptr, temporal));
        game.orthographicProjection = kind == 3 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        game.cameraPos = { 0.5f, 0.25f, 2 };
        game.cameraUp = { 0, 1, 0 };
        game.cameraRight = { 1, 0, 0 };
        game.cameraFwd = { 0, 0, -1 };
        game.cameraPinholeOffset = { 0.125f, -0.25f };
        game.clipToLensClip = Store(XMMatrixScaling(1.05f, 0.95f, 1));
        game.cameraNear = kind == 2 ? 250.f : 0.2f;
        game.cameraFar = kind == 2 ? 0.2f : 250.f;
        game.cameraFOV = 0.9f;
        game.cameraAspectRatio = 1.7f;
        const uint64_t frame = 20 + kind;
        output._gameCamera.BeginFrame(frame);
        output._gameCamera.Store(frame, game, true);
        const auto received = output.Evaluate(frame);
        Require(Same(received.cameraViewToClip, game.cameraViewToClip), "camera projection preservation");
        Require(Same(received.clipToCameraView, game.clipToCameraView) &&
                    Same(received.clipToPrevClip, game.clipToPrevClip) &&
                    Same(received.prevClipToClip, game.prevClipToClip),
                "camera inverse and temporal preservation");
        Require(received.orthographicProjection == game.orthographicProjection &&
                    Same(received.clipToLensClip, game.clipToLensClip) && received.cameraPinholeOffset.x == 0.125f &&
                    received.cameraPinholeOffset.y == -0.25f && received.cameraPos.x == 0.5f &&
                    received.cameraPos.y == 0.25f && received.cameraPos.z == 2 && received.cameraUp.y == 1 &&
                    received.cameraRight.x == 1 && received.cameraFwd.z == -1 &&
                    received.cameraNear == game.cameraNear && received.cameraFar == game.cameraFar &&
                    received.cameraFOV == 0.9f && received.cameraAspectRatio == 1.7f,
                "camera optional values preservation");
        for (const auto& world : worldPoints)
        {
            const auto currentCamera = Transform(world, currentScalars);
            const auto currentClip = Transform(currentCamera, game.cameraViewToClip);
            const auto previousClip = Transform(Transform(world, previousScalars), game.cameraViewToClip);
            Near(Transform(currentClip, received.clipToPrevClip), previousClip, "camera temporal reprojection");
            Near(Transform(previousClip, received.prevClipToClip), currentClip, "camera reverse reprojection");
            Near(Transform(currentClip, received.clipToCameraView), currentCamera, "camera projection inverse");
        }
        // A malformed replacement of this same frame must discard its prior valid camera.
        game.prevClipToClip[0].x = std::numeric_limits<float>::quiet_NaN();
        output._gameCamera.Store(frame, game, true);
        const auto invalid = output.Evaluate(frame);
        Require(Same(invalid.cameraViewToClip, fallback.cameraViewToClip) &&
                    Same(invalid.clipToPrevClip, fallback.clipToPrevClip),
                "camera invalid replacement fallback");
    }
}
} // namespace CameraFixture

void TestCamera()
{
    CameraFixture::Run();
    std::puts("PASS: camera DirectXMath cases=4");
}
