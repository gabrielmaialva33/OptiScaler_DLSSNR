// GPU/Win32 types above come from this suite's shared fakes.h. These doubles
// supply the upscaler bridge's dependencies; Init/Process/IsInited are production.
#define LOG_FUNC() ((void) 0)
#define LOG_INFO(...) ((void) 0)
constexpr DWORD INFINITE = UINT32_MAX;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 8;
constexpr unsigned DX11WDX12_NUM_OF_BUFFERS = 2;
constexpr int NVSDK_NGX_ENGINE_TYPE_UNREAL = 1;
namespace GameEngineType
{
constexpr int Unreal = 1;
}
namespace GameQuirk
{
constexpr int ForceUnrealEngine = 1;
}
namespace Upscaler
{
constexpr int XeSS = 1, XeSS_on12 = 2;
}
struct NVSDK_NGX_Parameter
{
};
struct ID3D11Device
{
};
struct ID3D11DeviceContext
{
};
template <class T> struct Option
{
    T value {};
    T value_or(T) const { return value; }
    T value_or_default() const { return value; }
    void set_volatile_value(T v) { value = v; }
};
struct Config
{
    Option<int> ColorResourceBarrier, MVResourceBarrier, Dx11Upscaler;
    Option<bool> DisableReactiveMask, DontUseNTShared;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
struct State
{
    int NVNGX_Engine = 0, gameEngine = 0, gameQuirks = 0;
    bool autoExposure = false, changeBackend[1] {};
    static State& Instance()
    {
        static State s;
        return s;
    }
};
struct Dx11WithDx12
{
    struct ResourceMask
    {
        enum
        {
            Color = 1,
            Mv = 2,
            Depth = 4,
            Output = 8,
            Exposure = 16,
            Reactive = 32
        };
    };
    struct Result
    {
        bool Success = true, MissingExposure = false, MissingReactive = false;
    };
    static UINT64 NextUpscalerFrameId() { return 1; }
    static void SetUpscalerFrameIndex(UINT) {}
    static Result PrepareUpscalerResources(const NVSDK_NGX_Parameter*, int, UINT, UINT64, bool, bool, bool)
    {
        return {};
    }
};
struct IFeature
{
    bool initialized = false;
    virtual bool IsInited() { return initialized; }
    void SetInit(bool v) { initialized = v; }
};
struct Backend : IFeature
{
    bool initResult = true;
    int inits = 0;
    bool Init(ID3D12Device*, ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*)
    {
        ++inits;
        SetInit(initResult);
        return initResult;
    }
};
struct IFeature_Dx11wDx12 : IFeature
{
    Backend* dx12Feature = nullptr;
    template <class F, class D> auto CallFeature(F f, D d) { return dx12Feature ? f(dx12Feature) : d; }
    // Production IsInited() is inserted here by run.py.
