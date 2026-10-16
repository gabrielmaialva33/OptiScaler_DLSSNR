#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sl.h>

#define LOG_FUNC() ((void) 0)
#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
using UINT = unsigned;
using UINT64 = uint64_t;
using D3D12_RESOURCE_STATES = unsigned;
constexpr unsigned D3D12_RESOURCE_STATE_COMMON = 0;
struct D3D12_RESOURCE_DESC
{
    uint64_t Width = 4096;
    uint32_t Height = 2160;
};
struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    auto GetDesc() { return desc; }
};
struct ID3D12GraphicsCommandList
{
};
using VkImage = uintptr_t;
using VkFormat = unsigned;
using VkImageLayout = unsigned;
using VkCommandBuffer = void*;
enum class FG_ResourceType
{
    Depth,
    Velocity,
    HudlessColor,
    UIColor
};
enum class FG_ResourceValidity
{
    ValidNow,
    UntilPresent
};
// @resource@
struct VkFGResource
{
    FG_ResourceType type;
    VkImage image {};
    VkFormat format {};
    VkImageLayout layout {};
    uint32_t imageWidth {}, imageHeight {}, width {}, height {};
    VkCommandBuffer cmdBuffer {};
    FG_ResourceValidity validity {};
    int frameIndex {};
};
struct Consumer
{
    unsigned calls = 0;
    uint64_t rectWidth = 0;
    unsigned rectHeight = 0;
    Dx12Resource captured {};
    VkFGResource vkCaptured {};
    int GetIndexWillBeDispatched() { return 2; }
    int GetIndex() { return 3; }
    bool HasResource(FG_ResourceType, int) { return false; }
    void SetInterpolationRect(uint64_t w, unsigned h)
    {
        rectWidth = w;
        rectHeight = h;
    }
    void GetInterpolationRect(uint64_t& w, unsigned& h, unsigned)
    {
        w = rectWidth;
        h = rectHeight;
    }
    void SetResource(Dx12Resource* res)
    {
        ++calls;
        captured = *res;
    }
    void SetResource(VkFGResource* res)
    {
        ++calls;
        vkCaptured = *res;
    }
};
template <typename T> struct Option
{
    T value {};
    T value_or_default() const { return value; }
};
struct Config
{
    bool enabled = true;
    Option<bool> FGDepthValidNow, FGVelocityValidNow, FGHudlessValidNow;
    bool IsFGEnabled() { return enabled; }
    static Config* Instance()
    {
        static Config cfg;
        return &cfg;
    }
};
enum class GameQuirk
{
    IgnoreValidUntilEvaluateForFG
};
struct State
{
    uint64_t dlssgLastFrame = 0, fgLastFrame = 7;
    Consumer* currentFG = nullptr;
    Consumer* currentVkFG = nullptr;
    std::map<GameQuirk, bool> gameQuirks;
    Consumer* CurrentFG() { return currentFG ? currentFG : currentVkFG; }
    static State& Instance()
    {
        static State state;
        return state;
    }
};
class Sl_Inputs
{
  public:
    uint64_t mvsWidth = 0;
    unsigned mvsHeight = 0;
    unsigned _currentIndex = 2;
    void CheckForFrame(Consumer*, uint32_t) {}
    int IndexForFrameId(uint32_t) { return 2; }
    bool reportResource(const sl::ResourceTag&, void*, uint32_t);
};
