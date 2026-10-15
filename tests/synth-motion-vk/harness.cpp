// Real-GPU endpoint-error test for SynthMotion::Estimator_Vk (retoque/shaders/synth_motion/SynthMotion_Vk.*), the
// Vulkan twin of the D3D12 estimator, on a real Vulkan device under Wine (winevulkan to the host's driver), with the
// Khronos validation layer's core and synchronization checks on.
//
// The frames, the sequences and the scores are tests/synth-motion-d3d12's (sequences.h), so the Vulkan estimator is
// held to the same pixels against the same truth, and run.py judges the report with that suite's thresholds. Nothing
// below the estimator's public interface is mocked: the production VkPipelines, VkImages and Estimator_Vk, called the
// way the Vulkan present host calls them (DlssNrFeature_Vk.cpp) -- the images made and initialised in a command buffer
// of their own and waited for, a ticket per frame, ConfirmExecuted on submit, AbandonRecording for a frame never
// submitted, Completed once its fence is seen.
//
// Convention under test (the interface's promise): the field holds, per colour pixel, the displacement from the
// current frame to the previous one, in colour pixels, +x right, +y down. Content moving (+dx, +dy) reads (-dx, -dy).
#include "pch.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>

#include "SynthMotion_Vk.h"

namespace
{
FILE* g_report = nullptr;
const char* g_reportPath = "synth-motion-report.json";

std::atomic<unsigned> g_validationErrors { 0 };
std::atomic<unsigned> g_wineInjected { 0 };
std::atomic<bool> g_validationActive { false };
std::atomic<bool> g_validationProbe { false };
std::string g_firstValidation;

void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
    std::fflush(stdout);
}

// A failure ends the process without unwinding GPU-owned objects, as the D3D12 harness does.
[[noreturn]] void Fail(const std::string& what)
{
    Log("FAIL: %s", what.c_str());
    std::fflush(stdout);
    ExitProcess(1);
}

// The device cannot run the estimator (SynthMotion::VkUnsupported): not a failure of the estimator; run.py says SKIP.
[[noreturn]] void Skip(const std::string& why)
{
    std::string reason = why;
    for (char& c : reason)
        c = (c == '"' || c == '\\') ? '\'' : c;
    FILE* report = nullptr;
    if (fopen_s(&report, g_reportPath, "w") == 0 && report != nullptr)
    {
        std::fprintf(report, "{\"source\": \"ffx\", \"api\": \"vulkan\", \"skipped\": \"%s\", \"sequences\": []}\n",
                     reason.c_str());
        std::fclose(report);
    }
    Log("SYNTH-MOTION-VK SKIP: %s", reason.c_str());
    ExitProcess(0);
}

void Check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS)
        Fail(std::string(what) + " returned " + std::to_string(result));
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    const char* text = data->pMessage ? data->pMessage : "";
    const char* id = data->pMessageIdName ? data->pMessageIdName : "?";
    if (g_validationProbe && !std::strcmp(id, "VUID-VkFenceCreateInfo-flags-parameter"))
    {
        g_validationActive = true;
        Log("VALIDATION PROBE (expected): %s", text);
        return VK_TRUE;
    }
    // Wine's own VK_EXT_external_memory_dma_buf on the host-side device (tests/vulkan-present-host, harness.cpp).
    if (!std::strcmp(id, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387") &&
        std::strstr(text, "VK_EXT_external_memory_dma_buf") != nullptr &&
        std::strstr(text, "ppEnabledExtensionNames[0]") == nullptr)
    {
        ++g_wineInjected;
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        if (g_validationErrors++ == 0)
            g_firstValidation = std::string(id) + ": " + text;
        Log("VALIDATION: %s | %s", id, text);
    }
    return VK_FALSE;
}

// Procedural content, the sequences and the estimator's scoring: the D3D12 suite's, shared.
#include "../synth-motion-d3d12/sequences.h"

// ---------------------------------------------------------------------------------------------
// The device.
// ---------------------------------------------------------------------------------------------
struct Gpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double tickNs = 0.0;
    uint64_t tickMask = ~0ull;
    std::string deviceName;

    void Init()
    {
        VkDebugUtilsMessengerCreateInfoEXT debug { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        debug.pfnUserCallback = DebugMessage;

        const char* extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
        VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "OptiScaler synthesized motion, Vulkan";
        app.apiVersion = VK_API_VERSION_1_1; // what the estimator needs, no more: the floor it is tested at
        VkInstanceCreateInfo info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        info.pNext = &debug;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        Check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createDebug == nullptr)
            Fail("no debug messenger entry point");
        Check(createDebug(instance, &debug, nullptr, &messenger), "vkCreateDebugUtilsMessengerEXT");

        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        for (VkPhysicalDevice candidate : devices)
        {
            VkPhysicalDeviceProperties props {};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (physical == VK_NULL_HANDLE || props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                physical = candidate;
                deviceName = props.deviceName;
                tickNs = props.limits.timestampPeriod;
            }
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                break;
        }
        if (physical == VK_NULL_HANDLE)
            Fail("no Vulkan device");

        const std::string unsupported = SynthMotion::VkUnsupported(physical, app.apiVersion);
        if (!unsupported.empty())
            Skip("the estimator cannot run on " + deviceName + ": " + unsupported);

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, props.data());
        for (uint32_t i = 0; i < families && family == UINT32_MAX; ++i)
        {
            if ((props[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                family = i;
        }
        if (family == UINT32_MAX)
            Fail("no graphics and compute queue family");
        const uint32_t validBits = props[family].timestampValidBits;
        tickMask = validBits >= 64 ? ~0ull : ((1ull << validBits) - 1);

        // VK_KHR_push_descriptor, as the present host adds it for the model; nothing else, and no feature.
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
        VkDeviceCreateInfo deviceInfo { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        Check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);

        VkCommandPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &allocate, &cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        Check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        VkQueryPoolCreateInfo queryInfo { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        Check(vkCreateQueryPool(device, &queryInfo, nullptr, &timestamps), "vkCreateQueryPool");

        ProbeValidation();
    }

    // The negative control: an invalid fence flag must reach the messenger with its exact VUID.
    void ProbeValidation()
    {
        VkFenceCreateInfo invalid { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        invalid.flags = 0x80000000u;
        VkFence probe = VK_NULL_HANDLE;
        g_validationProbe = true;
        const VkResult result = vkCreateFence(device, &invalid, nullptr, &probe);
        g_validationProbe = false;
        if (probe != VK_NULL_HANDLE)
            vkDestroyFence(device, probe, nullptr);
        if (!g_validationActive || result != VK_ERROR_VALIDATION_FAILED_EXT)
            Fail("validation negative control failed: the invalid fence flag was not intercepted");
    }

    void Begin()
    {
        Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    }

    void SubmitAndWait()
    {
        Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        Check(vkResetFences(device, 1, &fence), "vkResetFences");
        Check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        Check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000), "vkWaitForFences");
    }

    // Recorded, ended, never submitted: what the present host does with a frame it discards.
    void EndWithoutSubmitting() { Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"); }

    double TimestampMs()
    {
        uint64_t ticks[2] = {};
        if (vkGetQueryPoolResults(device, timestamps, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            return 0.0;
        const uint64_t begin = ticks[0] & tickMask, end = ticks[1] & tickMask;
        return end > begin ? (double) (end - begin) * tickNs / 1e6 : 0.0;
    }

    uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties memory {};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        }
        Fail("no memory type");
    }

    struct HostBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* data = nullptr;
    };

    HostBuffer Buffer(VkDeviceSize size, VkBufferUsageFlags usage)
    {
        HostBuffer out;
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        Check(vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer");
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(device, out.buffer, &req);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex =
            MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory");
        void* mapped = nullptr;
        Check(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        out.data = static_cast<uint8_t*>(mapped);
        return out;
    }

    void Free(HostBuffer& b)
    {
        vkDestroyBuffer(device, b.buffer, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
        b = HostBuffer {};
    }
};

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
{
    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// ---------------------------------------------------------------------------------------------
// The report: the D3D12 suite's schema (its run.py judges it), without the HUD mask's fields.
// ---------------------------------------------------------------------------------------------
struct FrameResult
{
    int t = 0;
    bool recorded = false;
    bool abandoned = false;
    bool ready = false;
    bool sceneCutAfterRecord = false;
    bool sceneCutAfterConfirm = false;
    double gpuMs = 0.0;
    UINT fieldW = 0, fieldH = 0;
    std::vector<Score> scores;
};

void WriteSequence(const Sequence& s, const std::vector<FrameResult>& frames, bool first)
{
    std::fprintf(g_report,
                 "%s\n  {\"name\": \"%s\", \"width\": %u, \"height\": %u, \"dx\": %d, \"dy\": %d, \"timing_only\": %s, "
                 "\"frames\": [",
                 first ? "" : ",", s.name, s.width, s.height, s.dx, s.dy, s.timingOnly ? "true" : "false");
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto& f = frames[i];
        std::fprintf(
            g_report,
            "%s\n    {\"t\": %d, \"recorded\": %s, \"abandoned\": %s, \"ready\": %s, \"scene_cut_after_record\": "
            "%s, \"scene_cut_after_confirm\": %s, \"gpu_ms\": %.6f, \"field_width\": %u, \"field_height\": %u, "
            "\"field_format\": %d, \"scores\": [",
            i ? "," : "", f.t, f.recorded ? "true" : "false", f.abandoned ? "true" : "false",
            f.ready ? "true" : "false", f.sceneCutAfterRecord ? "true" : "false",
            f.sceneCutAfterConfirm ? "true" : "false", f.gpuMs, f.fieldW, f.fieldH, (int) SynthMotion::kVkMotionFormat);
        for (size_t j = 0; j < f.scores.size(); ++j)
        {
            const auto& sc = f.scores[j];
            std::fprintf(g_report,
                         "%s{\"region\": \"%s\", \"gt_x\": %.3f, \"gt_y\": %.3f, \"pixels\": %zu, \"epe_mean\": %.6f, "
                         "\"within_1px\": %.6f, \"over_half_px\": %.6f, \"median_x\": %.4f, \"median_y\": %.4f}",
                         j ? ", " : "", sc.region, sc.gx, sc.gy, sc.pixels, sc.epeMean, sc.within1, sc.overHalf,
                         sc.medianX, sc.medianY);
        }
        std::fprintf(g_report, "]}");
    }
    std::fprintf(g_report, "\n  ]}");
    std::fflush(g_report);
}

// ---------------------------------------------------------------------------------------------
// One sequence end to end.
// ---------------------------------------------------------------------------------------------
std::vector<FrameResult> RunSequence(Gpu& gpu, const SynthMotion::VkPipelines& pipelines, const Sequence& s,
                                     uint64_t& ticket)
{
    const VkDevice device = gpu.device;

    // The colour: RGBA8 UNORM, sampled, rewritten from a host buffer every frame.
    VkImage colour = VK_NULL_HANDLE;
    VkDeviceMemory colourMemory = VK_NULL_HANDLE;
    VkImageView colourView = VK_NULL_HANDLE;
    {
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = { s.width, s.height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(device, &info, nullptr, &colour), "vkCreateImage colour");
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(device, colour, &req);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = gpu.MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(device, &allocate, nullptr, &colourMemory), "vkAllocateMemory colour");
        Check(vkBindImageMemory(device, colour, colourMemory, 0), "vkBindImageMemory colour");
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = colour;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        Check(vkCreateImageView(device, &view, nullptr, &colourView), "vkCreateImageView colour");
    }

    const VkDeviceSize pixels = (VkDeviceSize) s.width * s.height;
    Gpu::HostBuffer upload = gpu.Buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    Gpu::HostBuffer fieldReadback = gpu.Buffer(pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    // The estimator's images, made and initialised in a command buffer of their own, waited for: the builder's shape.
    SynthMotion::VkImages images;
    {
        std::string why;
        gpu.Begin();
        if (!SynthMotion::CreateVkImages(device, gpu.physical, s.width, s.height, gpu.cmd, images, why))
            Fail("CreateVkImages: " + why);
        gpu.SubmitAndWait();
    }

    SynthMotion::Estimator_Vk estimator;
    estimator.Restart();

    std::vector<FrameResult> results;
    for (int t = 0; t < s.frames; ++t)
    {
        RenderFrame(s, t, upload.data, s.width * 4);

        gpu.Begin();
        VkCommandBuffer cmd = gpu.cmd;
        // The whole frame is rewritten: its old contents are not needed, so the layout it was left in is not either.
        ImageBarrier(cmd, colour, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { s.width, s.height, 1 };
        vkCmdCopyBufferToImage(cmd, upload.buffer, colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        ImageBarrier(cmd, colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        FrameResult fr {};
        fr.t = t;
        const uint64_t frameTicket = ++ticket;
        vkCmdResetQueryPool(cmd, gpu.timestamps, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpu.timestamps, 0);
        fr.recorded = estimator.Record(cmd, pipelines, images, colourView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       t == 0, frameTicket);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu.timestamps, 1);
        if (!fr.recorded)
            Fail("Record returned false");
        fr.ready = estimator.Ready();
        fr.sceneCutAfterRecord = estimator.SceneCut();

        // The field out to the host. Record left its writes visible to transfer reads, in GENERAL.
        {
            VkBufferImageCopy out {};
            out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            out.imageExtent = { s.width, s.height, 1 };
            vkCmdCopyImageToBuffer(cmd, images.motion.image, VK_IMAGE_LAYOUT_GENERAL, fieldReadback.buffer, 1, &out);
            VkBufferMemoryBarrier toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = fieldReadback.buffer;
            toHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &toHost, 0, nullptr);
        }

        if (s.kind == Kind::Abandon && t == kAbandonFrame)
        {
            // Recorded, ended, never submitted: the estimator must not count this frame.
            gpu.EndWithoutSubmitting();
            estimator.AbandonRecording();
            fr.abandoned = true;
            results.push_back(fr);
            continue;
        }

        gpu.SubmitAndWait();
        estimator.ConfirmExecuted();
        estimator.Completed(frameTicket, images);
        fr.sceneCutAfterConfirm = estimator.SceneCut();
        fr.gpuMs = gpu.TimestampMs();

        if (!s.timingOnly)
        {
            Field field;
            field.width = s.width;
            field.height = s.height;
            field.x.resize((size_t) pixels);
            field.y.resize((size_t) pixels);
            for (size_t i = 0; i < (size_t) pixels; ++i)
            {
                uint16_t h[2];
                std::memcpy(h, fieldReadback.data + i * 4, 4);
                field.x[i] = HalfToFloat(h[0]);
                field.y[i] = HalfToFloat(h[1]);
            }
            fr.fieldW = field.width;
            fr.fieldH = field.height;
            const bool hudOnly = std::strncmp(s.name, "hud_", 4) == 0;
            if (t > 0 && !hudOnly)
                fr.scores = ScoreFrame(s, t, field);
        }

        std::string line;
        for (const auto& sc : fr.scores)
        {
            char buf[240];
            std::snprintf(buf, sizeof(buf),
                          " %s: epe %.3f <1px %.1f%% >0.5px %.1f%% median (%.2f, %.2f) gt (%.0f, %.0f)", sc.region,
                          sc.epeMean, sc.within1 * 100.0, sc.overHalf * 100.0, sc.medianX, sc.medianY, sc.gx, sc.gy);
            line += buf;
        }
        Log("%-12s t=%d ready=%d cut=%d/%d gpu=%.3f ms%s", s.name, t, fr.ready ? 1 : 0, fr.sceneCutAfterRecord ? 1 : 0,
            fr.sceneCutAfterConfirm ? 1 : 0, fr.gpuMs, line.c_str());
        results.push_back(fr);
    }

    // Everything above was waited for (the abandoned frame never ran): the images are idle.
    Check(vkQueueWaitIdle(gpu.queue), "vkQueueWaitIdle");
    SynthMotion::DestroyVkImages(device, images);
    gpu.Free(upload);
    gpu.Free(fieldReadback);
    vkDestroyImageView(device, colourView, nullptr);
    vkDestroyImage(device, colour, nullptr);
    vkFreeMemory(device, colourMemory, nullptr);
    return results;
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--report") == 0 && i + 1 < argc)
            g_reportPath = argv[++i];
    }

    Gpu gpu;
    gpu.Init();
    Log("device up: %s, timestamp period %.3f ns", gpu.deviceName.c_str(), gpu.tickNs);

    SynthMotion::VkPipelines pipelines;
    {
        std::string why;
        if (!SynthMotion::CreateVkPipelines(gpu.device, pipelines, why))
            Fail("CreateVkPipelines: " + why);
    }

    // The D3D12 suite's sequences, without its HUD mask's own (hud_*): there is no HUD mask on Vulkan.
    const Sequence sequences[] = {
        { "pan_+1x", Kind::Pan, 1280, 720, 1, 0, 14 },
        { "pan_+1y", Kind::Pan, 1280, 720, 0, 1, 14 },
        { "pan_+4-4", Kind::Pan, 1280, 720, 4, -4, 14 },
        { "pan_-16x", Kind::Pan, 1280, 720, -16, 0, 14 },
        { "pan_-16y", Kind::Pan, 1280, 720, 0, -16, 14 },
        { "pan_+16+16", Kind::Pan, 1280, 720, 16, 16, 14 },
        { "pan_+48x", Kind::Pan, 1280, 720, 48, 0, 14 },
        { "pan_+48y", Kind::Pan, 1280, 720, 0, 48, 14 },
        { "overlay_+8x", Kind::Overlay, 1280, 720, 8, 0, 14, false, false },
        { "overlay_+8+8", Kind::Overlay, 1280, 720, 8, 8, 14, false, false },
        { "object", Kind::Object, 1280, 720, 8, 3, 14 },
        { "static", Kind::Static, 1280, 720, 0, 0, 14 },
        { "cut", Kind::Cut, 1280, 720, 4, 0, 18 },
        { "abandon", Kind::Abandon, 1280, 720, 4, 0, 14 },
        { "time_1080p", Kind::Hud, 1920, 1080, 4, 0, 32, true },
        { "time_3440", Kind::Hud, 3440, 1440, 4, 0, 32, true },
    };

    if (fopen_s(&g_report, g_reportPath, "w") != 0 || g_report == nullptr)
        Fail("open report");
    std::fprintf(g_report,
                 "{\"source\": \"ffx\", \"api\": \"vulkan\", \"device\": \"%s\", \"lag_frames\": 0, "
                 "\"sequences\": [",
                 gpu.deviceName.c_str());

    uint64_t ticket = 0;
    int frames = 0;
    bool first = true;
    for (const auto& s : sequences)
    {
        const auto results = RunSequence(gpu, pipelines, s, ticket);
        frames += (int) results.size();
        WriteSequence(s, results, first);
        first = false;
    }

    Check(vkDeviceWaitIdle(gpu.device), "vkDeviceWaitIdle");
    SynthMotion::DestroyVkPipelines(pipelines);
    std::fprintf(g_report,
                 "\n], \"validation_errors\": %u, \"first_validation\": \"%s\", \"validation_active\": %s, "
                 "\"wine_injected\": %u}\n",
                 g_validationErrors.load(), "", g_validationActive ? "true" : "false", g_wineInjected.load());
    std::fclose(g_report);
    g_report = nullptr;

    Log("SYNTH-MOTION-VK DONE sequences=%d frames=%d validation_errors=%u",
        (int) (sizeof(sequences) / sizeof(sequences[0])), frames, g_validationErrors.load());
    if (!g_firstValidation.empty())
        Log("first validation error: %s", g_firstValidation.c_str());
    return 0;
}
