#include <sl_consts.h>
#include <sl_appidentity.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <atomic>
#include <map>
#include <algorithm>
#if __has_include(<framegen/dlssg/GameCamera.h>)
#include <framegen/dlssg/GameCamera.h>
#define HAVE_CAMERA 1
#endif

using XMMATRIX = sl::float4x4;
using XMFLOAT4X4 = sl::float4x4;
XMMATRIX XMMatrixIdentity()
{
    XMMATRIX m;
    for (int r = 0; r < 4; r++)
        m[r] = { 0, 0, 0, 0 };
    m[0].x = m[1].y = m[2].z = m[3].w = 1;
    return m;
}
XMMATRIX XMMatrixPerspectiveFovRH(float, float, float, float) { return XMMatrixIdentity(); }
XMMATRIX XMMatrixInverse(void*, XMMATRIX m) { return m; }
void XMStoreFloat4x4(XMFLOAT4X4* output, XMMATRIX m) { *output = m; }
bool XMScalarNearEqual(float a, float b, float eps) { return std::abs(a - b) < eps; }
#define LOG_WARN(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define BUFFER_COUNT 4
struct Token
{
};
sl::Constants received;
struct StreamlineProxy
{
    static auto SetConstants()
    {
        return +[](const sl::Constants& data, const Token&, int)
        {
            received = data;
            return 0;
        };
    }
};
struct Backend
{
    float _cameraFar[4] { 100, 100, 100, 100 }, _cameraNear[4] { 1, 1, 1, 1 };
    float _cameraVFov[4] { 1, 1, 1, 1 }, _cameraAspectRatio[4] { 1, 1, 1, 1 };
    float _cameraPosition[4][3] {}, _cameraUp[4][3] {}, _cameraForward[4][3] {}, _cameraRight[4][3] {};
#ifdef HAVE_CAMERA
    DlssgGameCamera<4> _gameCamera;
#endif
    bool IsInfiniteDepth() { return false; }
    bool IsInvertedDepth() { return false; }
    sl::Constants Dispatch(uint64_t willDispatchFrame)
    {
        auto fIndex = willDispatchFrame % 4;
        (void) fIndex;
        // @camera@
    }
    void Store(uint64_t frame, const sl::Constants& data)
    {
#ifdef HAVE_CAMERA
        _gameCamera.BeginFrame(frame);
        _gameCamera.Store(frame, data, true);
#else
        (void) frame;
        (void) data;
#endif
    }
};
struct IFGFeature
{
    uint64_t _frameCount = 0, _lastDispatchedFrame = 0, _lastFGFrame = 0, _targetFrame = 0;
    bool _isActive = true, _waitingNewFrameData = false;
    std::map<int, bool> _resourceReady[4];
    std::map<int, bool> resources[4];
    bool _waitingExecute[4] {}, _noUi[4] {}, _noDistortionField[4] {}, _noHudless[4] {};
    virtual ~IFGFeature() = default;
    bool IsActive();
    bool IsPaused();
    bool IsDispatched();
    uint64_t FrameCount();
    uint64_t StartNewFrame();
    int GetIndex();
    int GetDispatchIndex(uint64_t& frame);
    void SetFrameCount(uint64_t frame);
    bool HasResource(int type, int index = -1) { return resources[index < 0 ? GetIndex() : index].contains(type); }
    void NewFrame() {}
};
struct FG_ResourceType
{
    static constexpr int Depth = 0, Velocity = 1, UIColor = 2, HudlessColor = 3;
};
struct Config
{
    struct Allowed
    {
        uint64_t value = 2;
        uint64_t value_or_default() { return value; }
    } FGAllowedFrameAhead;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
struct Util
{
    static int64_t MillisecondsNow() { return 0; }
};
struct State
{
    uint64_t fgLastFrame = 0;
    static inline IFGFeature* current = nullptr;
    static State& Instance()
    {
        static State state;
        return state;
    }
    IFGFeature* CurrentFG() { return current; }
};
using UINT64 = uint64_t;
// @frame-counter@
struct DLSSG_Dx12 : Backend, IFGFeature
{
    sl::Constants NextDispatch(uint64_t& dispatchedFrame)
    {
        int index = GetDispatchIndex(dispatchedFrame);
        assert(index >= 0 && index == static_cast<int>(dispatchedFrame % 4));
        return Dispatch(dispatchedFrame);
    }
    void BeginGameCameraFrame(uint64_t frame) { _gameCamera.BeginFrame(frame); }
    void SetGameCamera(uint64_t frame, const sl::Constants& data, bool valid) { _gameCamera.Store(frame, data, valid); }
    void Deactivate()
    {
        // @Deactivate@
    }
    void ReleaseObjects()
    {
        // @ReleaseObjects@
    }
};
// @mapping@
sl::Constants gameCamera()
{
    sl::Constants data;
    data.cameraViewToClip = XMMatrixIdentity();
    data.cameraViewToClip[0].x = 2;
    data.cameraViewToClip[1].y = 3;
    data.clipToCameraView = XMMatrixIdentity();
    data.clipToCameraView[0].x = 0.5f;
    data.clipToCameraView[1].y = 1.f / 3;
    data.clipToPrevClip = XMMatrixIdentity();
    data.clipToPrevClip[3].x = 7;
    data.prevClipToClip = XMMatrixIdentity();
    data.prevClipToClip[3].x = -7;
    return data;
}
int main()
{
    Backend backend;
    auto data = gameCamera();
    backend.Store(1, data);
    auto output = backend.Dispatch(1);
    assert(output.cameraViewToClip[0].x == 2);
    assert(output.clipToPrevClip[3].x == 7);
    assert(output.prevClipToClip[3].x == -7);
    assert(output.mvecScale.x == 0.5f && output.reset == sl::Boolean::eTrue);
    assert(output.motionVectorsDilated == sl::Boolean::eTrue);

    // Complete finite camera matrices required; malformed replacement clears the old valid camera.
    for (float invalid :
         { sl::INVALID_FLOAT, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
    {
        backend.Store(1, data);
        auto broken = data;
        broken.clipToPrevClip[0].x = invalid;
        backend._gameCamera.Store(1, broken, true);
        assert(backend.Dispatch(1).cameraViewToClip[0].x == 1);
    }
    for (int kind = 0; kind < 5; kind++)
    {
        auto broken = data;
        if (kind == 0)
            broken.cameraViewToClip = XMMatrixIdentity();
        if (kind == 1)
            broken.cameraViewToClip = sl::float4x4 {};
        if (kind == 2)
            broken.clipToCameraView = sl::float4x4 {};
        if (kind == 3)
            broken.clipToCameraView[0].x = 0.9f;
        if (kind == 4)
            broken.prevClipToClip[3].x = 8;
        backend.Store(1, broken);
        assert(backend.Dispatch(1).cameraViewToClip[0].x == 1);
    }
    data.cameraPos = { 0, 0, 0 };
    data.cameraUp = { 0, 1, 0 };
    data.cameraRight = { 1, 0, 0 };
    data.cameraFwd = { 0, 0, 1 };
    data.cameraPinholeOffset = { 0.125f, 0.25f };
    data.clipToLensClip = XMMatrixIdentity();
    backend.Store(1, data);
    output = backend.Dispatch(1);
    assert(output.cameraUp.y == 1 && output.cameraFwd.z == 1);
    assert(output.cameraPinholeOffset.x == 0.125f && output.clipToLensClip[0].x == 1);
    data.cameraPos.x = sl::INVALID_FLOAT;
    data.cameraPinholeOffset.x = std::numeric_limits<float>::infinity();
    data.clipToLensClip[0].x = sl::INVALID_FLOAT;
    backend.Store(1, data);
    output = backend.Dispatch(1);
    assert(output.cameraUp.z == 1 && output.cameraPinholeOffset.x == 0);
    assert(output.cameraViewToClip[0].x == 2);

    // Exact SL->logical mapping, including a high initial external ID and delayed constants.
    Sl_Inputs inputs;
    DLSSG_Dx12 dlssg;
    inputs.CheckForFrame(&dlssg, 100);
    inputs.CaptureGameCamera(&dlssg, 100, gameCamera(), true);
    assert(dlssg.Dispatch(1).clipToPrevClip[3].x == 7);
    inputs.CheckForFrame(&dlssg, 101);
    auto previous = gameCamera();
    previous.clipToPrevClip[3].x = 9;
    previous.prevClipToClip[3].x = -9;
    inputs.CaptureGameCamera(&dlssg, 100, previous, true);
    assert(dlssg.Dispatch(1).clipToPrevClip[3].x == 9);
    assert(dlssg.Dispatch(2).cameraViewToClip[0].x == 1);
    inputs.CaptureGameCamera(&dlssg, 101, gameCamera(), true);
    assert(dlssg.Dispatch(2).cameraViewToClip[0].x == 2);
    inputs.CaptureGameCamera(&dlssg, 101, gameCamera(), false);
    assert(dlssg.Dispatch(2).cameraViewToClip[0].x == 1);
    inputs.engineType = sl::EngineType::eUnreal;
    inputs.CaptureGameCamera(&dlssg, 101, gameCamera(), true);
    assert(dlssg.Dispatch(2).cameraViewToClip[0].x == 1);
    inputs.engineType = sl::EngineType::eCount;
    inputs.CaptureGameCamera(&dlssg, 999, previous, true);
    assert(dlssg.Dispatch(2).cameraViewToClip[0].x == 1);
    for (uint32_t frame = 102; frame <= 104; frame++)
        inputs.CheckForFrame(&dlssg, frame);
    // Reused slot without constants has no camera; an old update cannot overwrite its registration.
    inputs.CaptureGameCamera(&dlssg, 100, previous, true);
    assert(dlssg.Dispatch(5).cameraViewToClip[0].x == 1);
    inputs.CaptureGameCamera(&dlssg, 104, gameCamera(), true);
    assert(dlssg.Dispatch(5).cameraViewToClip[0].x == 2);
    dlssg.Deactivate();
    inputs.CaptureGameCamera(&dlssg, 104, previous, true);
    assert(dlssg.Dispatch(5).cameraViewToClip[0].x == 1);
    inputs.CheckForFrame(&dlssg, 105);
    inputs.CaptureGameCamera(&dlssg, 105, gameCamera(), true);
    dlssg.ReleaseObjects();
    assert(dlssg.Dispatch(6).cameraViewToClip[0].x == 1);

    // Present overwrites the logical counter with its own ID; it must not change the old input mapping.
    Sl_Inputs presentedInputs;
    DLSSG_Dx12 presentedBackend;
    State::current = &presentedBackend;
    presentedInputs.CheckForFrame(&presentedBackend, 100);
    presentedInputs.CaptureGameCamera(&presentedBackend, 100, gameCamera(), true);
    presentedInputs.markPresent(100);
    presentedInputs.CheckForFrame(&presentedBackend, 101);
    presentedInputs.CaptureGameCamera(&presentedBackend, 101, gameCamera(), true);
    presentedInputs.CaptureGameCamera(&presentedBackend, 100, previous, true);
    uint64_t dispatched = 0;
    assert(presentedBackend.NextDispatch(dispatched).clipToPrevClip[3].x == 7);
    assert(dispatched == 101 && presentedBackend.IsDispatched());
    // Anonymous callbacks follow the current boundary, with an empty new registration.
    presentedInputs.markPresent(101);
    presentedInputs.CheckForFrame(&presentedBackend, 0);
    assert(presentedBackend.Dispatch(102).cameraViewToClip[0].x == 1);
    presentedInputs.CaptureGameCamera(&presentedBackend, 0, gameCamera(), true);
    assert(presentedBackend.NextDispatch(dispatched).cameraViewToClip[0].x == 2);
    assert(dispatched == 102);

    // Real StartNewFrame/GetDispatchIndex choose the queued frame within the allowance, or jump to a
    // newer frame when it has resources. Delayed constants belong to the older dispatch in either case.
    for (int branch = 0; branch < 3; branch++)
    {
        Sl_Inputs queuedInputs;
        DLSSG_Dx12 queued;
        Config::Instance()->FGAllowedFrameAhead.value = branch == 0 ? 2 : 1;
        queuedInputs.CheckForFrame(&queued, 1000);
        queuedInputs.CaptureGameCamera(&queued, 1000, gameCamera(), true);
        assert(queued.NextDispatch(dispatched).clipToPrevClip[3].x == 7 && dispatched == 1);
        queuedInputs.CheckForFrame(&queued, 1001);
        queuedInputs.CaptureGameCamera(&queued, 1001, gameCamera(), true);
        queuedInputs.CheckForFrame(&queued, 1002);
        queuedInputs.CaptureGameCamera(&queued, 1002, gameCamera(), true);
        queuedInputs.CaptureGameCamera(&queued, 1001, previous, true);
        if (branch != 2)
            queued.resources[queued.GetIndex()][FG_ResourceType::Depth] = true;
        output = queued.NextDispatch(dispatched);
        if (branch == 1)
        {
            assert(dispatched == 3 && output.clipToPrevClip[3].x == 7);
        }
        else
        {
            assert(dispatched == 2 && output.clipToPrevClip[3].x == 9);
            output = queued.NextDispatch(dispatched);
            assert(dispatched == 3 && output.clipToPrevClip[3].x == 7);
        }
    }
    Config::Instance()->FGAllowedFrameAhead.value = 2;

    // Input writes and present reads use owned values under the same cache mutex.
    DlssgGameCamera<4> concurrent;
    concurrent.BeginFrame(42);
    std::atomic<bool> done = false;
    std::thread writer(
        [&]()
        {
            for (int i = 0; i < 2000; i++)
            {
                auto camera = gameCamera();
                camera.clipToPrevClip[3].x = static_cast<float>(i);
                camera.prevClipToClip[3].x = -static_cast<float>(i);
                concurrent.Store(42, camera, true);
            }
            done = true;
        });
    do
    {
        sl::Constants camera;
        if (concurrent.Apply(42, camera))
            assert(camera.clipToPrevClip[3].x == -camera.prevClipToClip[3].x);
    } while (!done);
    writer.join();
}
