#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <string_view>
#include <vector>

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

using VkDevice = uintptr_t;
using VkPhysicalDevice = uintptr_t;
using VkCommandBuffer = uintptr_t;
using VkQueryPool = uint64_t;
using VkDeviceSize = uint64_t;
using VkQueryResultFlags = uint32_t;
constexpr VkQueryPool VK_NULL_HANDLE = 0;
constexpr uint32_t VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO = 11;
constexpr uint32_t VK_QUERY_TYPE_TIMESTAMP = 2;
constexpr uint32_t VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT = 1;
constexpr uint32_t VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT = 0x2000;
constexpr VkQueryResultFlags VK_QUERY_RESULT_64_BIT = 1;
enum VkResult
{
    VK_SUCCESS = 0,
    VK_NOT_READY = 1,
    VK_ERROR_OUT_OF_HOST_MEMORY = -1,
    VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
    VK_ERROR_DEVICE_LOST = -4,
    VK_ERROR_UNKNOWN = -13
};
struct VkQueryPoolCreateInfo
{
    uint32_t sType;
    uint32_t queryType;
    uint32_t queryCount;
};
struct VkPhysicalDeviceProperties
{
    struct
    {
        float timestampPeriod;
    } limits;
};

struct State
{
    std::mutex frameTimeMutex;
    std::deque<double> upscaleTimes { 10.0, 20.0, 30.0 };
    static State& Instance()
    {
        static State state;
        return state;
    }
};

struct Reply
{
    VkResult result;
    std::array<uint64_t, 2> timestamps;
    unsigned writeCount = 2;
};
static std::deque<Reply> replies;
static unsigned polls = 0;
static float period = 1.0f;
static std::vector<unsigned> recorded;
static constexpr VkDevice device = 1;
static constexpr VkPhysicalDevice physicalDevice = 2;
static constexpr VkCommandBuffer commandBuffer = 3;
static constexpr VkQueryPool pool = 4;

static VkResult vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* info, const void*, VkQueryPool* out)
{
    Check(d == device && info->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO &&
              info->queryType == VK_QUERY_TYPE_TIMESTAMP && info->queryCount == 2,
          "create the timestamp pair");
    *out = pool;
    return VK_SUCCESS;
}
static void vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* out)
{
    Check(pd == physicalDevice, "physical device");
    out->limits.timestampPeriod = period;
}
static void vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool p, uint32_t first, uint32_t count)
{
    Check(cmd == commandBuffer && p == pool && first == 0 && count == 2, "record pair reset");
    recorded.push_back(0);
    // Recording a reset does not execute it or change host-visible availability.
}
static void vkCmdWriteTimestamp(VkCommandBuffer cmd, uint32_t stage, VkQueryPool p, uint32_t query)
{
    Check(cmd == commandBuffer && p == pool && query < 2, "timestamp target");
    Check(stage == (query == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT),
          "timestamp stage");
    recorded.push_back(query + 1);
}
static VkResult vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t first, uint32_t count, size_t size,
                                      void* data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    Check(d == device && p == pool && first == 0 && count == 2, "read the timestamp pair");
    Check(size == 2 * sizeof(uint64_t) && stride == sizeof(uint64_t), "64-bit pair layout");
    Check(flags == VK_QUERY_RESULT_64_BIT, "nonblocking read, without WAIT or PARTIAL flags");
    Check(!replies.empty(), "unexpected query poll (already consumed or not armed)");
    ++polls;
    const auto reply = replies.front();
    replies.pop_front();
    auto* timestamps = static_cast<uint64_t*>(data);
    for (unsigned i = 0; i < reply.writeCount; ++i)
        timestamps[i] = reply.timestamps[i];
    return reply.result;
}
