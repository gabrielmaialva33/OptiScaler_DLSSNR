#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <optional>
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

#define LOG_ERROR(...) ((void) 0)

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
    const void* pNext;
    uint32_t flags;
    uint32_t queryType;
    uint32_t queryCount;
    uint32_t pipelineStatistics;
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
    bool isShuttingDown = false;
    static State& Instance()
    {
        static State state;
        return state;
    }
};

// GpuTime_Vk keeps three start/end pairs, one per buffered frame.
static constexpr uint32_t kPairs = 3;

struct Reply
{
    VkResult result;
    std::array<uint64_t, 2> timestamps;
    unsigned writeCount = 2;
};
struct Command
{
    char kind; // 'R' reset of a pair, 'T' timestamp
    uint32_t query;
    bool operator==(const Command&) const = default;
};
static std::deque<Reply> replies;
static std::vector<uint32_t> polled; // first query of every vkGetQueryPoolResults
static std::vector<Command> recorded;
static float period = 1.0f;
static bool createFails = false;
static unsigned creates = 0;
static unsigned destroys = 0;
static constexpr VkDevice device = 1;
static constexpr VkPhysicalDevice physicalDevice = 2;
static constexpr VkCommandBuffer commandBuffer = 3;
static constexpr VkQueryPool pool = 4;

static VkResult vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* info, const void*, VkQueryPool* out)
{
    Check(d == device && info->sType == VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO &&
              info->queryType == VK_QUERY_TYPE_TIMESTAMP && info->queryCount == kPairs * 2,
          "create one timestamp pair per buffered frame");
    ++creates;
    if (createFails)
    {
        *out = 99; // garbage on failure; production must not keep it
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *out = pool;
    return VK_SUCCESS;
}
static void vkDestroyQueryPool(VkDevice d, VkQueryPool p, const void*)
{
    Check(d == device && p == pool, "destroy the pool that was created");
    ++destroys;
}
static void vkGetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties* out)
{
    Check(pd == physicalDevice, "physical device");
    out->limits.timestampPeriod = period;
}
static void vkCmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool p, uint32_t first, uint32_t count)
{
    Check(cmd == commandBuffer && p == pool && first % 2 == 0 && first < kPairs * 2 && count == 2,
          "record a reset of one whole pair");
    recorded.push_back({ 'R', first });
    // Recording a reset does not execute it or change host-visible availability.
}
static void vkCmdWriteTimestamp(VkCommandBuffer cmd, uint32_t stage, VkQueryPool p, uint32_t query)
{
    Check(cmd == commandBuffer && p == pool && query < kPairs * 2, "timestamp target");
    Check(stage == (query % 2 == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT),
          "start at top of pipe, end at bottom of pipe");
    recorded.push_back({ 'T', query });
}
static VkResult vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t first, uint32_t count, size_t size,
                                      void* data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    Check(d == device && p == pool && first % 2 == 0 && first < kPairs * 2 && count == 2, "read one whole pair");
    Check(size == 2 * sizeof(uint64_t) && stride == sizeof(uint64_t), "64-bit pair layout");
    Check(flags == VK_QUERY_RESULT_64_BIT, "nonblocking read, without WAIT or PARTIAL flags");
    Check(!replies.empty(), "unexpected query poll (pair already consumed or never armed)");
    polled.push_back(first);
    const auto reply = replies.front();
    replies.pop_front();
    auto* timestamps = static_cast<uint64_t*>(data);
    for (unsigned i = 0; i < reply.writeCount; ++i)
        timestamps[i] = reply.timestamps[i];
    return reply.result;
}
