#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <cstring>
template <typename... T> void testLog(T&&...) {}
#define LOG_TRACE(...) testLog(__VA_ARGS__)
#define LOG_DEBUG(...) testLog(__VA_ARGS__)
#define LOG_INFO(...) testLog(__VA_ARGS__)
#define LOG_WARN(...) testLog(__VA_ARGS__)
// @owned-mutex@
using HRESULT = int;
using UINT = unsigned int;
using HWND = void*;
using IUnknown = void;
using ID3D12CommandQueue = void;
enum class FGOutput
{
    FSRFG,
    DLSSG
};
struct Setting
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Setting FGUseMutexForSwapchain;
    static Config* Instance()
    {
        static Config c;
        return &c;
    }
};
struct FG
{
    OwnedMutex Mutex;
};
using IFGFeature_Dx12 = FG;
struct State
{
    FG* currentFG = nullptr;
    FGOutput activeFgOutput = FGOutput::FSRFG;
    void* currentCommandQueue = nullptr;
    void* currentFGSwapchain = nullptr;
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State s;
        return s;
    }
};
static void* _device = nullptr;
static std::function<int()> operation;
static std::function<void()> cleanup;
void CheckForHdrOutput() {}
namespace MenuOverlayDx
{
void CleanupRenderTarget(bool, HWND) { cleanup(); }
} // namespace MenuOverlayDx
namespace FfxApiProxy
{
const char* ReturnCodeToString(int) { return "OK"; }
} // namespace FfxApiProxy
namespace Lsfg
{
void OnSwapchainReleased() {}
namespace Presenter
{
int D3D12_DestroyContext(void**, void*) { return 0; }
} // namespace Presenter
} // namespace Lsfg
struct Backend : FG
{
    HWND _hwnd = reinterpret_cast<void*>(1);
    void* _fgContext = nullptr;
    void* _swapChainContext = nullptr;
    void DestroyFGContext() {}
    void DestroySwapchainContext() {}
    void ReleaseObjects() {}
};
struct FSRFG_Dx12 : Backend
{
    bool ReleaseSwapchain(HWND hwnd);
    bool DispatchTail();
};
struct DLSSG_Dx12 : Backend
{
    bool ReleaseSwapchain(HWND hwnd);
};
struct XeFG_Dx12 : Backend
{
    bool ReleaseSwapchain(HWND hwnd);
};
