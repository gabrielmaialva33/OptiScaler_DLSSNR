#include "pch.h"

#include "SynthMotion_Vk.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "precompile/SynthMotion_ClearFloat_Shader_Vk.h"
#include "precompile/SynthMotion_ClearUint_Shader_Vk.h"
#include "precompile/SynthMotion_Expand_Shader_Vk.h"
#include "precompile/SynthMotion_Filter_Shader_Vk.h"
#include "precompile/SynthMotion_LuminancePyramid_Shader_Vk.h"
#include "precompile/SynthMotion_PrepareLuma_Shader_Vk.h"
#include "precompile/SynthMotion_Scale_Shader_Vk.h"
#include "precompile/SynthMotion_ScdDivergence_Shader_Vk.h"
#include "precompile/SynthMotion_ScdHistogram_Shader_Vk.h"
#include "precompile/SynthMotion_Search_Shader_Vk.h"

namespace SynthMotion
{
namespace
{
// The D3D12 estimator's passes, in its order (SynthMotion_Dx12.cpp).
enum Pass : uint32_t
{
    PassPrepareLuma,
    PassPyramid,
    PassScdHistogram,
    PassScdDivergence,
    PassSearch,
    PassFilter,
    PassScale,
    PassExpand,
    PassClearUint,
    PassClearFloat,
    PassCount
};
static_assert(PassCount == kVkPasses, "one pipeline per pass");

struct Blob
{
    const unsigned char* data;
    size_t size;
};

const Blob kBlobs[PassCount] = {
    { SynthMotion_PrepareLuma_spv, sizeof(SynthMotion_PrepareLuma_spv) },
    { SynthMotion_LuminancePyramid_spv, sizeof(SynthMotion_LuminancePyramid_spv) },
    { SynthMotion_ScdHistogram_spv, sizeof(SynthMotion_ScdHistogram_spv) },
    { SynthMotion_ScdDivergence_spv, sizeof(SynthMotion_ScdDivergence_spv) },
    { SynthMotion_Search_spv, sizeof(SynthMotion_Search_spv) },
    { SynthMotion_Filter_spv, sizeof(SynthMotion_Filter_spv) },
    { SynthMotion_Scale_spv, sizeof(SynthMotion_Scale_spv) },
    { SynthMotion_Expand_spv, sizeof(SynthMotion_Expand_spv) },
    { SynthMotion_ClearUint_spv, sizeof(SynthMotion_ClearUint_spv) },
    { SynthMotion_ClearFloat_spv, sizeof(SynthMotion_ClearFloat_spv) },
};

constexpr uint32_t kBlock = 8;                    // FFX_OPTICALFLOW_BLOCK_SIZE: level-0 vectors per 8x8 pixels
constexpr uint32_t kHistogramWidth = 256 * 3 * 3; // FFX: HistogramBins * HistogramsPerDim^2
constexpr uint32_t kStorageBindings = 8;          // u0..u7
constexpr uint32_t kColourBinding = 8;            // t0, shifted by 8 at compilation (build.sh)
constexpr VkDeviceSize kReadbackStride = 16;      // scdOutput's three words, padded

// FFX's cbOF and cbOF_SPD, one push constant block (synth_motion_callbacks.h, SmConstants): 64 bytes, every dispatch.
// The expand and clear passes read the first 32 of them as their own eight words.
struct OfConstants
{
    int32_t width;
    int32_t height;
    uint32_t level;
    uint32_t levelCount;
    uint32_t frameIndex;
    uint32_t transferFunction;
    float minLuminance;
    float maxLuminance;
};

struct SpdConstants
{
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffsetX;
    uint32_t workGroupOffsetY;
    uint32_t numWorkGroupsPyramid;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ExpandConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t flowWidth;
    uint32_t flowHeight;
    uint32_t zero;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};

struct ClearConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t pad[6];
};

struct PushBlock
{
    uint32_t words[16];
};

static_assert(sizeof(OfConstants) == 32 && sizeof(SpdConstants) == 32 && sizeof(ExpandConstants) == 32 &&
                  sizeof(ClearConstants) == 32 && sizeof(PushBlock) == 64,
              "one 64-byte push constant block");

template <typename A, typename B = SpdConstants> PushBlock Push(const A& first, const B& second = B {})
{
    static_assert(sizeof(A) == 32 && sizeof(B) == 32, "two halves of eight words");
    PushBlock block {};
    std::memcpy(block.words, &first, 32);
    std::memcpy(block.words + 8, &second, 32);
    return block;
}

bool AtLeast11(uint32_t version)
{
    return VK_API_VERSION_MAJOR(version) > 1 ||
           (VK_API_VERSION_MAJOR(version) == 1 && VK_API_VERSION_MINOR(version) >= 1);
}

uint32_t FindMemoryType(VkPhysicalDevice physical, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }

    return UINT32_MAX;
}

void DestroyTexture(VkDevice device, VkTexture& texture)
{
    if (texture.view != VK_NULL_HANDLE)
        vkDestroyImageView(device, texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE)
        vkDestroyImage(device, texture.image, nullptr);
    if (texture.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, texture.memory, nullptr);
    texture = VkTexture {};
}

bool CreateTexture(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkFormat format,
                   VkImageUsageFlags extraUsage, VkTexture& out)
{
    VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = { std::max(width, 1u), std::max(height, 1u), 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extraUsage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &info, nullptr, &out.image) != VK_SUCCESS)
    {
        out.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device, out.image, &requirements);

    VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex =
        FindMemoryType(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocate.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS)
    {
        out.memory = VK_NULL_HANDLE;
        DestroyTexture(device, out);
        return false;
    }

    if (vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS)
    {
        DestroyTexture(device, out);
        return false;
    }

    VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    if (vkCreateImageView(device, &view, nullptr, &out.view) != VK_SUCCESS)
    {
        out.view = VK_NULL_HANDLE;
        DestroyTexture(device, out);
        return false;
    }

    return true;
}

// Every dispatch's writes before the next dispatch's reads and writes: all of them are compute, on GENERAL images.
void ComputeBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
}

VkWriteDescriptorSet StorageWrite(uint32_t binding, const VkDescriptorImageInfo* info)
{
    VkWriteDescriptorSet write { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = info;
    return write;
}

// One dispatch: its pipeline, its storage images at bindings 0.. in order (and the colour at binding 8 when given),
// the push constant block, the dispatch, and the barrier after it.
void Dispatch(VkCommandBuffer cmd, const VkPipelines& p, uint32_t pass, std::initializer_list<VkImageView> storage,
              const PushBlock& constants, uint32_t x, uint32_t y, uint32_t z, VkImageView colour = VK_NULL_HANDLE,
              VkImageLayout colourLayout = VK_IMAGE_LAYOUT_GENERAL)
{
    VkDescriptorImageInfo infos[kStorageBindings + 1] {};
    VkWriteDescriptorSet writes[kStorageBindings + 1] {};
    uint32_t count = 0;

    for (VkImageView view : storage)
    {
        infos[count] = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
        writes[count] = StorageWrite(count, &infos[count]);
        ++count;
    }

    if (colour != VK_NULL_HANDLE)
    {
        infos[count] = { VK_NULL_HANDLE, colour, colourLayout };
        writes[count] = StorageWrite(kColourBinding, &infos[count]);
        writes[count].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ++count;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipelines[pass]);
    p.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, count, writes);
    vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushBlock), &constants);
    vkCmdDispatch(cmd, std::max(x, 1u), std::max(y, 1u), std::max(z, 1u));
    ComputeBarrier(cmd);
}
} // namespace

bool VkPipelines::Valid() const
{
    return device != VK_NULL_HANDLE && layout != VK_NULL_HANDLE && pushDescriptorSet != nullptr &&
           std::all_of(pipelines.begin(), pipelines.end(), [](VkPipeline p) { return p != VK_NULL_HANDLE; });
}

std::string VkUnsupported(VkPhysicalDevice physical, uint32_t apiVersion)
{
    if (physical == VK_NULL_HANDLE)
        return "no physical device";

    VkPhysicalDeviceProperties properties {};
    vkGetPhysicalDeviceProperties(physical, &properties);

    if (!AtLeast11(apiVersion) || !AtLeast11(properties.apiVersion))
        return "it needs Vulkan 1.1 (the application asked for " + std::to_string(VK_API_VERSION_MAJOR(apiVersion)) +
               "." + std::to_string(VK_API_VERSION_MINOR(apiVersion)) + ")";

    VkPhysicalDeviceSubgroupProperties subgroup { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDevicePushDescriptorPropertiesKHR push {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR
    };
    subgroup.pNext = &push;
    VkPhysicalDeviceProperties2 properties2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties2.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physical, &properties2);

    const VkSubgroupFeatureFlags needed = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    if ((subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
        (subgroup.supportedOperations & needed) != needed)
        return "the device has no subgroup arithmetic in compute shaders (the search pass needs it)";

    // The search sums over a 64-thread group as one wave of 64 or two of 32
    // (ffx_opticalflow_compute_optical_flow_v5.h).
    if (subgroup.subgroupSize != 32 && subgroup.subgroupSize != 64)
        return "the device's subgroups are " + std::to_string(subgroup.subgroupSize) +
               " wide; the search pass is written for 32 or 64";

    if (push.maxPushDescriptors < kStorageBindings + 1)
        return "the device pushes at most " + std::to_string(push.maxPushDescriptors) + " descriptors, under " +
               std::to_string(kStorageBindings + 1);

    struct Need
    {
        VkFormat format;
        VkFormatFeatureFlags features;
        const char* name;
    };
    const Need needs[] = {
        { VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "R8_UINT storage" },
        { VK_FORMAT_R16G16_SINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "R16G16_SINT storage" },
        { kVkMotionFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
          "R16G16_SFLOAT storage and sampling" },
        { VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT,
          "R32_UINT storage atomics" },
        { VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "R32_SFLOAT storage" },
    };

    for (const Need& need : needs)
    {
        VkFormatProperties format {};
        vkGetPhysicalDeviceFormatProperties(physical, need.format, &format);
        if ((format.optimalTilingFeatures & need.features) != need.features)
            return std::string("the device has no ") + need.name;
    }

    return {};
}

bool CreateVkPipelines(VkDevice device, VkPipelines& out, std::string& why)
{
    out = VkPipelines {};
    out.device = device;

    out.pushDescriptorSet =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
    if (out.pushDescriptorSet == nullptr)
    {
        why = "vkCmdPushDescriptorSetKHR is not available (VK_KHR_push_descriptor is not enabled on the device)";
        out = VkPipelines {};
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[kStorageBindings + 1] {};
    for (uint32_t i = 0; i <= kStorageBindings; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            i == kColourBinding ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo setInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = kStorageBindings + 1;
    setInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &out.setLayout) != VK_SUCCESS)
    {
        out.setLayout = VK_NULL_HANDLE;
        why = "the descriptor set layout could not be made";
        DestroyVkPipelines(out);
        return false;
    }

    VkPushConstantRange range {};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.offset = 0;
    range.size = sizeof(PushBlock);

    VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &out.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;

    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &out.layout) != VK_SUCCESS)
    {
        out.layout = VK_NULL_HANDLE;
        why = "the pipeline layout could not be made";
        DestroyVkPipelines(out);
        return false;
    }

    for (uint32_t pass = 0; pass < PassCount; ++pass)
    {
        VkShaderModuleCreateInfo moduleInfo { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        moduleInfo.codeSize = kBlobs[pass].size;
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(kBlobs[pass].data);

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        {
            why = "shader module " + std::to_string(pass) + " could not be made";
            DestroyVkPipelines(out);
            return false;
        }

        VkComputePipelineCreateInfo pipelineInfo { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "CS";
        pipelineInfo.layout = out.layout;

        const VkResult made =
            vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &out.pipelines[pass]);
        vkDestroyShaderModule(device, module, nullptr);

        if (made != VK_SUCCESS)
        {
            out.pipelines[pass] = VK_NULL_HANDLE;
            why = "pipeline " + std::to_string(pass) + " could not be made (" + std::to_string(made) + ")";
            DestroyVkPipelines(out);
            return false;
        }
    }

    return true;
}

void DestroyVkPipelines(VkPipelines& p)
{
    if (p.device != VK_NULL_HANDLE)
    {
        for (VkPipeline pipeline : p.pipelines)
        {
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(p.device, pipeline, nullptr);
        }
        if (p.layout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(p.device, p.layout, nullptr);
        if (p.setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(p.device, p.setLayout, nullptr);
    }

    p = VkPipelines {};
}

bool CreateVkImages(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkCommandBuffer init,
                    VkImages& out, std::string& why)
{
    out = VkImages {};

    if (width < Estimator_Vk::MinWidth || height < Estimator_Vk::MinHeight)
    {
        why = std::to_string(width) + "x" + std::to_string(height) + " is below " +
              std::to_string(Estimator_Vk::MinWidth) + "x" + std::to_string(Estimator_Vk::MinHeight);
        return false;
    }

    out.width = width;
    out.height = height;

    // FFX's sizes (ffx_opticalflow.cpp getInternalResourceDescriptions): the luma halves by shifting, the 8x8-block
    // flow halves rounding up.
    for (uint32_t level = 0; level < kVkLevels; ++level)
    {
        out.lumaWidth[level] = width >> level;
        out.lumaHeight[level] = height >> level;
        out.flowWidth[level] = level == 0 ? (width + kBlock - 1) / kBlock : (out.flowWidth[level - 1] + 1) / 2;
        out.flowHeight[level] = level == 0 ? (height + kBlock - 1) / kBlock : (out.flowHeight[level - 1] + 1) / 2;
    }

    bool ok = true;
    for (uint32_t pyramid = 0; pyramid < 2 && ok; ++pyramid)
    {
        for (uint32_t level = 0; level < kVkLevels && ok; ++level)
        {
            ok = CreateTexture(device, physical, out.lumaWidth[level], out.lumaHeight[level], VK_FORMAT_R8_UINT, 0,
                               out.luma[pyramid][level]) &&
                 CreateTexture(device, physical, out.flowWidth[level], out.flowHeight[level], VK_FORMAT_R16G16_SINT, 0,
                               out.flow[pyramid][level]);
        }
    }

    ok =
        ok &&
        CreateTexture(device, physical, out.flowWidth[0], out.flowHeight[0], VK_FORMAT_R16G16_SINT, 0, out.flowFinal) &&
        CreateTexture(device, physical, kHistogramWidth, 1, VK_FORMAT_R32_UINT, 0, out.scdHistogram) &&
        CreateTexture(device, physical, kHistogramWidth, 1, VK_FORMAT_R32_SFLOAT, 0, out.scdPreviousHistogram) &&
        CreateTexture(device, physical, 3, 1, VK_FORMAT_R32_UINT, 0, out.scdTemp) &&
        CreateTexture(device, physical, 3, 1, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, out.scdOutput) &&
        CreateTexture(device, physical, width, height, kVkMotionFormat,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, out.motion);

    if (ok)
    {
        VkBufferCreateInfo bufferInfo { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = kVkReadbackSlots * kReadbackStride;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ok = vkCreateBuffer(device, &bufferInfo, nullptr, &out.readback) == VK_SUCCESS;
        if (!ok)
            out.readback = VK_NULL_HANDLE;

        if (ok)
        {
            VkMemoryRequirements requirements {};
            vkGetBufferMemoryRequirements(device, out.readback, &requirements);

            VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex =
                FindMemoryType(physical, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            void* mapped = nullptr;
            ok = allocate.memoryTypeIndex != UINT32_MAX &&
                 vkAllocateMemory(device, &allocate, nullptr, &out.readbackMemory) == VK_SUCCESS;
            if (!ok)
                out.readbackMemory = VK_NULL_HANDLE;

            ok = ok && vkBindBufferMemory(device, out.readback, out.readbackMemory, 0) == VK_SUCCESS &&
                 vkMapMemory(device, out.readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS &&
                 mapped != nullptr;

            if (ok)
            {
                // Zero until a slot has really been written: no history bit, no cut.
                std::memset(mapped, 0, static_cast<size_t>(kVkReadbackSlots * kReadbackStride));
                out.readbackData = static_cast<const uint32_t*>(mapped);
            }
        }
    }

    if (!ok)
    {
        why = "the estimator's images could not be made at " + std::to_string(width) + "x" + std::to_string(height);
        DestroyVkImages(device, out);
        return false;
    }

    // Into GENERAL, cleared: the estimator never reads a texel nothing wrote, and no layout is left to guess.
    std::vector<VkImageMemoryBarrier> toClear;
    std::vector<VkImageMemoryBarrier> toUse;
    const auto add = [&](const VkTexture& texture)
    {
        VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toClear.push_back(barrier);

        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        toUse.push_back(barrier);
    };

    for (uint32_t pyramid = 0; pyramid < 2; ++pyramid)
    {
        for (uint32_t level = 0; level < kVkLevels; ++level)
        {
            add(out.luma[pyramid][level]);
            add(out.flow[pyramid][level]);
        }
    }
    for (const VkTexture* texture :
         { &out.flowFinal, &out.scdHistogram, &out.scdPreviousHistogram, &out.scdTemp, &out.scdOutput, &out.motion })
        add(*texture);

    vkCmdPipelineBarrier(init, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, static_cast<uint32_t>(toClear.size()), toClear.data());

    const VkClearColorValue zero {};
    const VkImageSubresourceRange range { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for (const VkImageMemoryBarrier& barrier : toClear)
        vkCmdClearColorImage(init, barrier.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);

    vkCmdPipelineBarrier(init, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, static_cast<uint32_t>(toUse.size()), toUse.data());
    return true;
}

void DestroyVkImages(VkDevice device, VkImages& images)
{
    if (device != VK_NULL_HANDLE)
    {
        for (uint32_t pyramid = 0; pyramid < 2; ++pyramid)
        {
            for (uint32_t level = 0; level < kVkLevels; ++level)
            {
                DestroyTexture(device, images.luma[pyramid][level]);
                DestroyTexture(device, images.flow[pyramid][level]);
            }
        }
        for (VkTexture* texture : { &images.flowFinal, &images.scdHistogram, &images.scdPreviousHistogram,
                                    &images.scdTemp, &images.scdOutput, &images.motion })
            DestroyTexture(device, *texture);

        if (images.readback != VK_NULL_HANDLE)
            vkDestroyBuffer(device, images.readback, nullptr);
        if (images.readbackMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, images.readbackMemory, nullptr); // unmapped with it
    }

    images = VkImages {};
}

bool Estimator_Vk::Record(VkCommandBuffer cmd, const VkPipelines& p, const VkImages& images, VkImageView colour,
                          VkImageLayout colourLayout, bool reset, uint64_t ticket)
{
    // A previous recording nobody confirmed or abandoned is treated as dropped.
    if (_pending)
        AbandonRecording();

    _ready = false;
    _sceneCut = false;

    if (cmd == VK_NULL_HANDLE || !p.Valid() || !images.Valid() || colour == VK_NULL_HANDLE || ticket == 0)
        return false;

    const uint32_t width = images.width;
    const uint32_t height = images.height;
    const bool doReset = reset || _needReset;
    const uint32_t frameIndex = doReset ? 0 : static_cast<uint32_t>(_confirmedFrameIndex + 1);

    if (doReset)
    {
        ++_epoch;
        _cutOwed = false;
    }
    else if (_cutOwed)
    {
        _sceneCut = true;
        _cutOwed = false;
    }

    const uint32_t parity = _nextParity;
    const VkTexture* current = images.luma[parity];
    const VkTexture* previous = images.luma[parity ^ 1u];

    // Everything earlier on the queue first: the previous frame's estimate (this frame reads its luma and overwrites
    // its flow), every read of its field (the model's), and its scene-change copy (this frame's copy may write the
    // same read-back slot a later round).
    {
        VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
    }

    OfConstants of = {};
    of.width = static_cast<int32_t>(width);
    of.height = static_cast<int32_t>(height);
    of.levelCount = kVkLevels;
    of.frameIndex = frameIndex;
    of.transferFunction = 0; // the callbacks saturate the colour; see synth_motion_callbacks.h
    of.minLuminance = 0.0f;
    of.maxLuminance = 1.0f;

    // FFX clears the scene-change state before its first use and on every reset.
    if (doReset)
    {
        struct Clear
        {
            const VkTexture* texture;
            uint32_t width;
            uint32_t pass;
        };
        const Clear clears[] = {
            { &images.scdHistogram, kHistogramWidth, PassClearUint },
            { &images.scdPreviousHistogram, kHistogramWidth, PassClearFloat },
            { &images.scdTemp, 3, PassClearUint },
            { &images.scdOutput, 3, PassClearUint },
        };

        for (const Clear& clear : clears)
        {
            ClearConstants constants = {};
            constants.width = clear.width;
            constants.height = 1;
            Dispatch(cmd, p, clear.pass, { clear.texture->view }, Push(constants), (clear.width + 63) / 64, 1, 1);
        }
    }

    // Luma of the current frame, full resolution, from the colour.
    Dispatch(cmd, p, PassPrepareLuma, { current[0].view }, Push(of), ((width + 1) / 2 + 15) / 16,
             ((height + 1) / 2 + 15) / 16, 1, colour, colourLayout);

    // Levels 1..6 of the current luma (FFX's single-pass downsampler, one 64x64 tile per group).
    {
        const uint32_t groupsX = (width - 1) / 64 + 1;
        const uint32_t groupsY = (height - 1) / 64 + 1;

        SpdConstants spd = {};
        spd.mips = 4; // what FFX's host passes; the shader itself generates six
        spd.numWorkGroups = groupsX * groupsY;
        spd.numWorkGroupsPyramid = groupsX * groupsY;

        Dispatch(cmd, p, PassPyramid,
                 { current[0].view, current[1].view, current[2].view, current[3].view, current[4].view, current[5].view,
                   current[6].view },
                 Push(of, spd), groupsX, groupsY, 1);
    }

    // Scene-change detection: nine luma histograms, then their divergence from last frame's.
    {
        const uint32_t strataWidth = (width / 4) / 3;
        Dispatch(cmd, p, PassScdHistogram, { current[0].view, images.scdHistogram.view }, Push(of),
                 (strataWidth + 31) / 32, 16, 9);
        Dispatch(
            cmd, p, PassScdDivergence,
            { images.scdHistogram.view, images.scdPreviousHistogram.view, images.scdTemp.view, images.scdOutput.view },
            Push(of), 9, 3, 1);
    }

    // Coarse to fine: search, median filter, then the prediction for the next finer level. The two flow pyramids
    // alternate with the level exactly as in FFX's host (ffx_opticalflow.cpp dispatch()).
    const bool oddFrame = parity != 0;
    for (int32_t level = static_cast<int32_t>(kVkLevels) - 1; level >= 0; --level)
    {
        const bool oddLevel = (level & 1) != 0;
        const VkTexture* flowA = images.flow[oddFrame != oddLevel ? 1 : 0];
        const VkTexture* flowB = images.flow[oddFrame != oddLevel ? 0 : 1];
        const uint32_t l = static_cast<uint32_t>(level);

        of.level = l;

        {
            const uint32_t lumaWidth = std::max(images.lumaWidth[l], 1u);
            const uint32_t lumaHeight = std::max(images.lumaHeight[l], 1u);
            Dispatch(cmd, p, PassSearch, { current[l].view, previous[l].view, flowA[l].view, images.scdOutput.view },
                     Push(of), ((lumaWidth + 3) / 4 * 16 + 63) / 64, (lumaHeight + 15) / 16, 1);
        }

        const VkTexture& filtered = l == 0 ? images.flowFinal : flowB[l];
        Dispatch(cmd, p, PassFilter, { flowA[l].view, filtered.view }, Push(of), (images.flowWidth[l] + 15) / 16,
                 (images.flowHeight[l] + 3) / 4, 1);

        if (l > 0)
        {
            Dispatch(cmd, p, PassScale,
                     { current[l].view, previous[l].view, flowB[l].view, flowB[l - 1].view, images.scdOutput.view },
                     Push(of), (images.flowWidth[l - 1] + 3) / 4, (images.flowHeight[l - 1] + 3) / 4, 1);
        }
    }

    // Our pass: 8x8-block vectors to one current->previous vector per colour pixel, and per pixel zero where the
    // level-0 luma pair shows the pixel standing still (a static overlay; synthesized-motion.md, "Static overlays").
    {
        ExpandConstants expand = {};
        expand.width = width;
        expand.height = height;
        expand.flowWidth = images.flowWidth[0];
        expand.flowHeight = images.flowHeight[0];
        expand.zero = (doReset || frameIndex <= WarmupFrames) ? 1u : 0u;

        Dispatch(cmd, p, PassExpand, { images.flowFinal.view, images.motion.view, current[0].view, previous[0].view },
                 Push(expand), (width + 7) / 8, (height + 7) / 8, 1);
    }

    // The field to whoever reads it next, at any stage (the model's evaluate is opaque).
    {
        VkMemoryBarrier barrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                             nullptr, 0, nullptr);
    }

    // This frame's scene-change bits, copied out for Completed(ticket) -- into a slot no frame still in flight owns. A
    // frame that finds none copies nothing and only cannot report a cut.
    _recordedSlot = UINT32_MAX;
    {
        const uint32_t slot = static_cast<uint32_t>(_recordCount % kVkReadbackSlots);
        if (_slots[slot].ticket == 0)
        {
            VkBufferImageCopy region {};
            region.bufferOffset = slot * kReadbackStride;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { 3, 1, 1 };
            vkCmdCopyImageToBuffer(cmd, images.scdOutput.image, VK_IMAGE_LAYOUT_GENERAL, images.readback, 1, &region);

            VkBufferMemoryBarrier toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = images.readback;
            toHost.offset = slot * kReadbackStride;
            toHost.size = kReadbackStride;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                                 &toHost, 0, nullptr);

            _slots[slot] = { ticket, _epoch, frameIndex };
            _recordedSlot = slot;
        }
        ++_recordCount;
    }

    _recordedFrameIndex = frameIndex;
    _recordedReset = doReset;
    _pending = true;
    // A cut frame is Ready (Estimator_Dx12::Record says why): its zero field is a legitimate field, and SceneCut() is
    // read beside a Ready field.
    _ready = !doReset && frameIndex > WarmupFrames;
    return true;
}

void Estimator_Vk::ConfirmExecuted()
{
    if (!_pending)
        return;

    _pending = false;
    _confirmedFrameIndex = _recordedFrameIndex;

    if (_recordedReset)
        _needReset = false;

    // This frame's luma becomes the previous frame's.
    _nextParity ^= 1u;
}

void Estimator_Vk::AbandonRecording()
{
    if (!_pending)
        return;

    // Nothing recorded ran: a reset that was asked for is still owed, and the read-back slot is free again.
    _pending = false;
    if (_recordedReset)
        _needReset = true;
    if (_recordedSlot != UINT32_MAX)
        _slots[_recordedSlot] = Slot {};
    _recordedSlot = UINT32_MAX;

    _ready = false;
    _sceneCut = false;
}

void Estimator_Vk::Completed(uint64_t ticket, const VkImages& images)
{
    if (ticket == 0)
        return;

    for (Slot& slot : _slots)
    {
        if (slot.ticket != ticket)
            continue;

        // FFX counts its warm-up frames as scene changes; and a cut read in an older history is that history's.
        if (images.readbackData != nullptr && slot.epoch == _epoch && slot.frameIndex > WarmupFrames)
        {
            const size_t index = static_cast<size_t>(&slot - _slots.data());
            const uint32_t history = images.readbackData[index * (kReadbackStride / sizeof(uint32_t)) + 1];
            if ((history & 1u) != 0)
                _cutOwed = true;
        }

        slot = Slot {};
    }
}

void Estimator_Vk::Restart()
{
    _slots.fill(Slot {});
    ++_epoch;
    _recordCount = 0;
    _nextParity = 0;
    _confirmedFrameIndex = -1;
    _recordedSlot = UINT32_MAX;
    _recordedReset = false;
    _pending = false;
    _needReset = true;
    _cutOwed = false;
    _ready = false;
    _sceneCut = false;
}
} // namespace SynthMotion
