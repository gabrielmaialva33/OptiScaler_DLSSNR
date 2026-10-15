// Disposable Linux-only probe. Public SDK ABI only; no driver/snippet ABI casts.
#include <vulkan/vulkan.h>

#include <nvsdk_ngx_helpers_vk.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static void BlockChildExecution()
{
    // The core launches the updater even with allow_ngx_updater=false. Deny all execs
    // in this process and its descendants, before the first SDK call. No core patch.
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000, 3, 0), // no x32 alternate ABI
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execve, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_execveat, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
    };
    sock_fprog program { static_cast<unsigned short>(std::size(filter)), filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program))
    {
        std::perror("cannot install no-exec guard");
        std::exit(2);
    }
    char* args[] { const_cast<char*>("no-exec-control"), nullptr };
    errno = 0;
    const auto r = syscall(SYS_execve, "/nonexistent-native-ngx-guard-control", args, nullptr);
    std::printf("no_exec_guard result=%ld errno=%d expected=%d\n", r, errno, EPERM);
    if (r != -1 || errno != EPERM)
        std::exit(2);
}

static void VkCheck(VkResult r, const char* operation)
{
    std::printf("vulkan %s result=%d\n", operation, r);
    if (r != VK_SUCCESS)
        std::exit(2); // Process exit abandons uncertain work; never destroy a pending resource.
}

static bool Ngx(const char* operation, NVSDK_NGX_Result r)
{
    std::printf("ngx %s result=0x%08x\n", operation, static_cast<unsigned>(r));
    return r == NVSDK_NGX_Result_Success;
}

static void Log(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    std::printf("ngx_log level=%d feature=%d %s\n", level, feature, message);
}

static void Uuid(const char* label, const uint8_t* value)
{
    std::printf("%s=", label);
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        std::printf("%02x", value[i]);
    std::puts("");
}

static void Inventory(VkPhysicalDevice pd)
{
    VkPhysicalDeviceIDProperties id { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceDriverProperties driver { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &id };
    VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
    vkGetPhysicalDeviceProperties2(pd, &properties);
    std::printf("device name=%s api=%u driver=%s info=%s\n", properties.properties.deviceName,
                properties.properties.apiVersion, driver.driverName, driver.driverInfo);
    Uuid("device_uuid", id.deviceUUID);
    Uuid("driver_uuid", id.driverUUID);
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, queues.data());
    for (uint32_t i = 0; i < count; ++i)
        std::printf("queue family=%u count=%u flags=0x%x timestamp_bits=%u\n", i, queues[i].queueCount,
                    queues[i].queueFlags, queues[i].timestampValidBits);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 memory { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget };
    vkGetPhysicalDeviceMemoryProperties2(pd, &memory);
    for (uint32_t i = 0; i < memory.memoryProperties.memoryHeapCount; ++i)
        std::printf("heap index=%u flags=0x%x size=%llu budget=%llu usage=%llu\n", i,
                    memory.memoryProperties.memoryHeaps[i].flags,
                    static_cast<unsigned long long>(memory.memoryProperties.memoryHeaps[i].size),
                    static_cast<unsigned long long>(budget.heapBudget[i]),
                    static_cast<unsigned long long>(budget.heapUsage[i]));

    // Query exact image tuples, not just extension names. No allocation/import is performed.
    for (auto format : { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_A2R10G10B10_UNORM_PACK32,
                         VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT })
    {
        VkFormatProperties fp {};
        vkGetPhysicalDeviceFormatProperties(pd, format, &fp);
        std::printf("format id=%d optimal_features=0x%x\n", format, fp.optimalTilingFeatures);
        for (auto type :
             { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT })
            for (VkImageUsageFlags usage :
                 { VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT),
                   VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT) })
            {
                VkPhysicalDeviceExternalImageFormatInfo external {
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO
                };
                external.handleType = type;
                VkPhysicalDeviceImageFormatInfo2 input { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                         &external };
                input.format = format;
                input.type = VK_IMAGE_TYPE_2D;
                input.tiling = VK_IMAGE_TILING_OPTIMAL;
                input.usage = usage;
                VkExternalImageFormatProperties caps { VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
                VkImageFormatProperties2 output { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &caps };
                auto r = vkGetPhysicalDeviceImageFormatProperties2(pd, &input, &output);
                std::printf("external_image format=%d type=0x%x usage=0x%x result=%d features=0x%x compatible=0x%x "
                            "export_from=0x%x max=%ux%u\n",
                            format, type, usage, r, caps.externalMemoryProperties.externalMemoryFeatures,
                            caps.externalMemoryProperties.compatibleHandleTypes,
                            caps.externalMemoryProperties.exportFromImportedHandleTypes,
                            output.imageFormatProperties.maxExtent.width,
                            output.imageFormatProperties.maxExtent.height);
            }
    }
    for (auto type : { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT })
        for (auto kind : { VK_SEMAPHORE_TYPE_BINARY, VK_SEMAPHORE_TYPE_TIMELINE })
        {
            VkSemaphoreTypeCreateInfo semaphore { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            semaphore.semaphoreType = kind;
            VkPhysicalDeviceExternalSemaphoreInfo input { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                                          &semaphore };
            input.handleType = type;
            VkExternalSemaphoreProperties output { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
            vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &input, &output);
            std::printf("external_semaphore type=0x%x kind=%d features=0x%x compatible=0x%x export_from=0x%x\n", type,
                        kind, output.externalSemaphoreFeatures, output.compatibleHandleTypes,
                        output.exportFromImportedHandleTypes);
        }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: probe MODEL_DIRECTORY RESULT_DIRECTORY\n");
        return 2;
    }
    BlockChildExecution();
    const auto model = std::filesystem::absolute(argv[1]).wstring();
    const auto data = std::filesystem::absolute(argv[2]).wstring();
    const wchar_t* paths[] { model.c_str() };
    NVSDK_NGX_FeatureCommonInfo common {};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo = { Log, NVSDK_NGX_LOGGING_LEVEL_VERBOSE, true };
    // Own project ID, not a borrowed game identity. No updater invocation.
    constexpr auto project = "b02a8f34-334a-4b3b-8b91-57e3a10d7c60";
    NVSDK_NGX_FeatureDiscoveryInfo discovery {};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = { project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "native-ngx-probe-1" };
    discovery.ApplicationDataPath = data.c_str();
    discovery.FeatureInfo = &common;

    std::set<std::string> instanceNames;
    for (auto feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_Reserved18 })
    {
        discovery.FeatureID = feature;
        uint32_t count = 0;
        VkExtensionProperties* extensions = nullptr;
        std::printf("feature=%d phase=instance_requirements\n", feature);
        if (Ngx("instance_extensions",
                NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &extensions)))
            for (uint32_t i = 0; i < count; ++i)
                instanceNames.insert(extensions[i].extensionName);
    }
    std::vector<const char*> instanceExtensions;
    for (const auto& name : instanceNames)
    {
        std::printf("enable_instance_extension=%s\n", name.c_str());
        instanceExtensions.push_back(name.c_str());
    }
    VkApplicationInfo app { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Retoque disposable native NGX probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = instanceExtensions.size();
    ici.ppEnabledExtensionNames = instanceExtensions.data();
    VkInstance instance {};
    VkCheck(vkCreateInstance(&ici, nullptr, &instance), "create_instance");
    uint32_t count = 0;
    VkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate_devices");
    std::vector<VkPhysicalDevice> devices(count);
    VkCheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate_devices");
    VkPhysicalDevice pd {};
    for (auto candidate : devices)
    {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == 0x10de && std::string(properties.deviceName).find("RTX 4090") != std::string::npos)
            pd = candidate;
    }
    if (!pd)
    {
        std::puts("NO RTX 4090; no GPU probe performed");
        return 2;
    }
    Inventory(pd);
    VkCheck(vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr), "extension_count");
    std::vector<VkExtensionProperties> offered(count);
    VkCheck(vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, offered.data()), "extensions");
    std::set<std::string> names;
    for (const auto& e : offered)
        std::printf("offered_device_extension=%s version=%u\n", e.extensionName, e.specVersion);
    for (auto feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_Reserved18 })
    {
        discovery.FeatureID = feature;
        VkExtensionProperties* extensions = nullptr;
        count = 0;
        std::printf("feature=%d phase=device_requirements\n", feature);
        if (Ngx("device_extensions",
                NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, pd, &discovery, &count, &extensions)))
            for (uint32_t i = 0; i < count; ++i)
                names.insert(extensions[i].extensionName);
        NVSDK_NGX_FeatureRequirement requirement {};
        if (Ngx("feature_requirements",
                NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, pd, &discovery, &requirement)))
            std::printf("feature=%d supported_flags=0x%x min_arch=0x%x min_os=%s\n", feature,
                        requirement.FeatureSupported, requirement.MinHWArchitecture, requirement.MinOSVersion);
    }
    for (auto name : { "VK_KHR_buffer_device_address", "VK_KHR_push_descriptor" })
        if (std::any_of(offered.begin(), offered.end(),
                        [&](const auto& e) { return std::string(e.extensionName) == name; }))
            names.insert(name);
    std::vector<const char*> extensions;
    for (const auto& name : names)
    {
        extensions.push_back(name.c_str());
        std::printf("enable_device_extension=%s\n", name.c_str());
    }
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());
    uint32_t family = 0;
    while (family < count && (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                                 (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
        ++family;
    if (family == count)
        return 2;
    float priority = 1;
    VkDeviceQueueCreateInfo qci { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    VkPhysicalDeviceFeatures2 features { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bda };
    vkGetPhysicalDeviceFeatures2(pd, &features);
    std::printf("buffer_device_address=%u\n", bda.bufferDeviceAddress);
    bda.bufferDeviceAddressCaptureReplay = false;
    bda.bufferDeviceAddressMultiDevice = false;
    VkDeviceCreateInfo dci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &bda };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = extensions.size();
    dci.ppEnabledExtensionNames = extensions.data();
    VkDevice device {};
    VkCheck(vkCreateDevice(pd, &dci, nullptr, &device), "create_device");
    VkQueue queue {};
    vkGetDeviceQueue(device, family, 0, &queue);
    if (!Ngx("init", NVSDK_NGX_VULKAN_Init_with_ProjectID(project, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "native-ngx-probe-1",
                                                          data.c_str(), instance, pd, device, vkGetInstanceProcAddr,
                                                          vkGetDeviceProcAddr, &common)))
        return 2;
    NVSDK_NGX_Parameter* params = nullptr;
    if (!Ngx("capability_parameters", NVSDK_NGX_VULKAN_GetCapabilityParameters(&params)) || !params)
        return 2;
    for (auto prefix : { "SuperSampling", "DLSSNR" })
        for (auto suffix : { "Available", "NeedsUpdatedDriver", "FeatureInitResult", "MinDriverVersionMajor",
                             "MinDriverVersionMinor" })
        {
            auto key = std::string(prefix) + "." + suffix;
            unsigned value = 0;
            auto r = NVSDK_NGX_Parameter_GetUI(params, key.c_str(), &value);
            std::printf("cap key=%s result=0x%08x value=%u hex=0x%08x\n", key.c_str(), static_cast<unsigned>(r), value,
                        value);
        }
    VkCommandPoolCreateInfo pci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool {};
    VkCheck(vkCreateCommandPool(device, &pci, nullptr, &pool), "create_pool");
    VkCommandBufferAllocateInfo cai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer command {};
    VkCheck(vkAllocateCommandBuffers(device, &cai, &command), "allocate_command_buffer");
    bool sr = false, nr = false;
    for (auto feature : { NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_Reserved18 })
    {
        VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkCheck(vkBeginCommandBuffer(command, &begin), "begin_command_buffer");
        NVSDK_NGX_Handle* handle = nullptr;
        NVSDK_NGX_Result result;
        if (feature == NVSDK_NGX_Feature_SuperSampling)
        {
            NVSDK_NGX_DLSS_Create_Params create {};
            create.Feature = { 640, 360, 1280, 720, NVSDK_NGX_PerfQuality_Value_MaxQuality };
            create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            result = NGX_VULKAN_CREATE_DLSS_EXT1(device, command, 1, 1, &handle, params, &create);
        }
        else
        {
            // Existing Retoque key contract, through public SDK setters. No native NR ABI is invented.
            NVSDK_NGX_Parameter_SetUI(params, "DLSSNR.Enabled", 1);
            NVSDK_NGX_Parameter_SetUI(params, "DLSSNR.Width", 640);
            NVSDK_NGX_Parameter_SetUI(params, "DLSSNR.Height", 360);
            result = NVSDK_NGX_VULKAN_CreateFeature1(device, command, feature, params, &handle);
        }
        std::printf("create feature=%d result=0x%08x handle=%p\n", feature, static_cast<unsigned>(result),
                    static_cast<void*>(handle));
        VkCheck(vkEndCommandBuffer(command), "end_command_buffer");
        const bool created = result == NVSDK_NGX_Result_Success && handle;
        if (created)
        {
            VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            VkFence fence {};
            VkCheck(vkCreateFence(device, &fci, nullptr, &fence), "create_fence");
            VkSubmitInfo submit { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &command;
            VkCheck(vkQueueSubmit(queue, 1, &submit, fence), "submit_create");
            VkCheck(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000), "create_fence_completion");
            vkDestroyFence(device, fence, nullptr);
            Ngx("release_feature", NVSDK_NGX_VULKAN_ReleaseFeature(handle));
        }
        (feature == NVSDK_NGX_Feature_SuperSampling ? sr : nr) = created;
        VkCheck(vkResetCommandBuffer(command, 0), "reset_command_buffer");
    }
    // The trace is authoritative for attempted opens; maps show the surviving loaded modules.
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);)
        if (line.find("ngx") != std::string::npos)
            std::printf("module_map %s\n", line.c_str());
    Ngx("destroy_parameters", NVSDK_NGX_VULKAN_DestroyParameters(params));
    Ngx("shutdown", NVSDK_NGX_VULKAN_Shutdown1(device));
    VkCheck(vkDeviceWaitIdle(device), "final_drain");
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf("summary sr_created=%d nr_created=%d nr_evaluated=0\n", sr, nr);
    // A positive NR create needs a separately validated native evaluate contract and pixel control.
    return !sr ? 2 : nr ? 3 : 0;
}
