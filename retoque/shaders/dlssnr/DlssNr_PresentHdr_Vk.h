#pragma once

// Per-build HDR conversion state. Built only by the present model's worker, with two immutable descriptor sets
// and uniform records: decode and encode. No per-frame mutation, allocation or descriptor ring. This is a plain
// handle bundle on purpose: only Destroy() frees it, after all product tickets complete. Zeroing it abandons it.
#include <vulkan/vulkan.h>
#include <array>
#include <cstring>
#include <cstdint>
#include <string>
#include "precompile/DlssNr_PresentHdr_Shader_Vk.h"

namespace DlssNr::PresentHdr
{
struct VkConversion
{
    struct Constants
    {
        uint32_t Width;
        uint32_t Height;
        uint32_t Profile;
        uint32_t Mode;
    };
    static_assert(sizeof(Constants) == 16);

    // A2R10 is often sampleable/presentable but not a storage format (including NVIDIA). Encode to A2B10 and
    // let a same-size nearest blit reorder channels; the shader still reads the original in its own format.
    static VkFormat EncodedFormat(VkFormat raw)
    {
        return raw == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : raw;
    }

    bool lost = false;
    bool Check(VkResult result)
    {
        lost = lost || result == VK_ERROR_DEVICE_LOST;
        return result == VK_SUCCESS;
    }
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[2] {};
    VkBuffer constants = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    void Destroy(VkDevice device)
    {
        if (pool)
            vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipeline)
            vkDestroyPipeline(device, pipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (constants)
            vkDestroyBuffer(device, constants, nullptr);
        if (memory)
            vkFreeMemory(device, memory, nullptr);
        *this = {};
    }

    // Requires properties2 and format_feature_flags2/core1.3, already enabled by the model host. Conservatively
    // require explicit formatless-store support; availability of the extension alone is not per-format support.
    static bool Supports(VkPhysicalDevice physical, VkFormat raw, uint32_t width, uint32_t height)
    {
        if (vkGetPhysicalDeviceFormatProperties2 == nullptr)
            return false;
        constexpr VkFormatFeatureFlags2 base = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
                                               VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                                               VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
        constexpr VkFormatFeatureFlags2 storage =
            VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
        constexpr VkImageUsageFlags usage =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const bool reorder = EncodedFormat(raw) != raw;
        struct Need
        {
            VkFormat format;
            VkFormatFeatureFlags2 features;
            VkImageUsageFlags usage;
        };
        const Need needs[] = {
            { raw, base | (reorder ? VK_FORMAT_FEATURE_2_BLIT_DST_BIT : 0), usage },
            { VK_FORMAT_R16G16B16A16_SFLOAT, base | storage, usage | VK_IMAGE_USAGE_STORAGE_BIT },
            { EncodedFormat(raw), base | storage | (reorder ? VK_FORMAT_FEATURE_2_BLIT_SRC_BIT : 0),
              usage | VK_IMAGE_USAGE_STORAGE_BIT },
        };
        for (const auto& need : needs)
        {
            VkFormatProperties3 flags { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
            VkFormatProperties2 properties { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &flags };
            vkGetPhysicalDeviceFormatProperties2(physical, need.format, &properties);
            if ((flags.optimalTilingFeatures & need.features) != need.features)
                return false;
            VkImageFormatProperties image {};
            if (vkGetPhysicalDeviceImageFormatProperties(physical, need.format, VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, need.usage, 0,
                                                         &image) != VK_SUCCESS ||
                width > image.maxExtent.width || height > image.maxExtent.height ||
                (image.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0)
                return false;
        }
        return true;
    }

    bool Create(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, uint32_t profile,
                VkImageView original, VkImageView linear, VkImageView encoded)
    {
        const auto fail = [&]()
        {
            if (!lost)
                Destroy(device);
            return false;
        };
        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo setInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        setInfo.bindingCount = 4;
        setInfo.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout)))
            return fail();
        VkPipelineLayoutCreateInfo layoutInfo { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        if (!Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout)))
            return fail();
        // The generated array has byte alignment; copy into correctly aligned words for pCode.
        alignas(uint32_t) std::array<unsigned char, sizeof(dlssnr_present_hdr_spv)> code;
        std::memcpy(code.data(), dlssnr_present_hdr_spv, code.size());
        VkShaderModuleCreateInfo moduleInfo { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (!Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module)))
            return fail();
        VkComputePipelineCreateInfo pipelineInfo { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "CSMain";
        pipelineInfo.layout = layout;
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result == VK_ERROR_DEVICE_LOST)
        {
            lost = true;
            return false;
        }
        vkDestroyShaderModule(device, module, nullptr);
        if (result != VK_SUCCESS)
            return fail();

        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(physical, &properties);
        const VkDeviceSize stride = (16 + properties.limits.minUniformBufferOffsetAlignment - 1) &
                                    ~(properties.limits.minUniformBufferOffsetAlignment - 1);
        VkBufferCreateInfo buffer { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer.size = stride * 2;
        buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (!Check(vkCreateBuffer(device, &buffer, nullptr, &constants)))
            return fail();
        VkMemoryRequirements needs {};
        vkGetBufferMemoryRequirements(device, constants, &needs);
        VkPhysicalDeviceMemoryProperties types {};
        vkGetPhysicalDeviceMemoryProperties(physical, &types);
        VkMemoryAllocateInfo allocate { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex = UINT32_MAX;
        constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < types.memoryTypeCount; ++i)
            if ((needs.memoryTypeBits & (1u << i)) && (types.memoryTypes[i].propertyFlags & host) == host)
            {
                allocate.memoryTypeIndex = i;
                break;
            }
        if (allocate.memoryTypeIndex == UINT32_MAX || !Check(vkAllocateMemory(device, &allocate, nullptr, &memory)) ||
            !Check(vkBindBufferMemory(device, constants, memory, 0)))
            return fail();
        void* mapped = nullptr;
        if (!Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped)))
            return fail();
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            const Constants params { width, height, profile, mode };
            std::memcpy(static_cast<unsigned char*>(mapped) + mode * stride, &params, sizeof(params));
        }
        vkUnmapMemory(device, memory);
        const VkDescriptorPoolSize sizes[] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
                                               { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 },
                                               { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
        VkDescriptorPoolCreateInfo poolInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool)))
            return fail();
        const VkDescriptorSetLayout layouts[] = { setLayout, setLayout };
        VkDescriptorSetAllocateInfo setsInfo { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        setsInfo.descriptorPool = pool;
        setsInfo.descriptorSetCount = 2;
        setsInfo.pSetLayouts = layouts;
        if (!Check(vkAllocateDescriptorSets(device, &setsInfo, sets)))
            return fail();
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            const VkDescriptorBufferInfo uniform { constants, mode * stride, 16 };
            const VkDescriptorImageInfo views[] = {
                { VK_NULL_HANDLE, original, VK_IMAGE_LAYOUT_GENERAL },
                { VK_NULL_HANDLE, mode == 0 ? original : linear, VK_IMAGE_LAYOUT_GENERAL },
                { VK_NULL_HANDLE, mode == 0 ? linear : encoded, VK_IMAGE_LAYOUT_GENERAL }
            };
            VkWriteDescriptorSet writes[4] {};
            for (uint32_t i = 0; i < 4; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = sets[mode];
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = bindings[i].descriptorType;
                if (i == 0)
                    writes[i].pBufferInfo = &uniform;
                else
                    writes[i].pImageInfo = &views[i - 1];
            }
            vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
        }
        return true;
    }

    void Record(VkCommandBuffer cmd, uint32_t width, uint32_t height, uint32_t mode) const
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[mode], 0, nullptr);
        vkCmdDispatch(cmd, (width + 15) / 16, (height + 15) / 16, 1);
    }
};
} // namespace DlssNr::PresentHdr
