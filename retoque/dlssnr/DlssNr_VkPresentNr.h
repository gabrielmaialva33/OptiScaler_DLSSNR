#pragma once

// The decisions of the Vulkan present host's stage 3, cold NR (docs/dlssnr/design/nr-vulkan-present-host.md, "Stage 3
// as built"): which device extensions a device is given at creation, which swapchains the model may be shown, why a
// frame the round trip carried was not shown to it, and the counters the log reports. DlssNr_VkPresentHost.cpp makes
// the Vulkan calls and DlssNrFeature_Vk.cpp runs the model (DlssNr_VkPresentModel.h); every choice they act on that
// can be made from plain values is made here, so tests/nr-vk-present-host drives it on the host.
//
// Standard library only; Vulkan's values are plain integers, and tests/nr-vk-present-host checks each one repeated
// here against vulkan_core.h.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace DlssNr::VkPresentHost
{
// --- Formats and colour spaces ---------------------------------------------------------------------------------

inline constexpr uint32_t kFormatR8G8B8A8Unorm = 37;
inline constexpr uint32_t kFormatR8G8B8A8Srgb = 43;
inline constexpr uint32_t kFormatB8G8R8A8Unorm = 44;
inline constexpr uint32_t kFormatB8G8R8A8Srgb = 50;
inline constexpr uint32_t kFormatA2R10G10B10UnormPack32 = 58;
inline constexpr uint32_t kFormatA2B10G10R10UnormPack32 = 64;
inline constexpr uint32_t kFormatR16G16B16A16Sfloat = 97;
inline constexpr uint32_t kFormatR32Sfloat = 100;
inline constexpr uint32_t kFormatR16G16Sfloat = 83;

inline constexpr uint32_t kColourSpaceSrgbNonlinear = 0;
inline constexpr uint32_t kColourSpaceExtendedSrgbLinear = 1000104002;
inline constexpr uint32_t kColourSpaceHdr10St2084 = 1000104008;
inline constexpr uint32_t kColourSpaceHdr10Hlg = 1000104010;

// VkFormatFeatureFlagBits the conversion and the model's images need.
inline constexpr uint32_t kFeatureSampledImage = 0x1;
inline constexpr uint32_t kFeatureStorageImage = 0x2;
inline constexpr uint32_t kFeatureBlitSrc = 0x400;
inline constexpr uint32_t kFeatureBlitDst = 0x800;
inline constexpr uint32_t kFeatureTransferSrc = 0x4000;
inline constexpr uint32_t kFeatureTransferDst = 0x8000;

// The working colour the model's pass reads and writes: the format of the after-upscale route's own images.
inline constexpr uint32_t kWorkingFormat = kFormatR16G16B16A16Sfloat;

// The raw copy of the presented image: what its bytes are copied into before the blit to the working colour, and out
// of after the blit back. The sRGB formats' UNORM twins, so the bytes are converted as numbers and never decoded: the
// model is shown the sRGB-encoded frame, as the D3D12 present pass shows it (a copy of the backbuffer's own bytes).
struct ColourPlan
{
    bool sdr = false;       // the model may be shown this swapchain
    uint32_t raw = 0;       // the raw image's format; the swapchain's own when !sdr (stage 2's unchanged round trip)
    std::string refusal;    // why not, when !sdr
    std::string formatName; // for the lines
};

inline std::string FormatName(uint32_t format)
{
    switch (format)
    {
    case kFormatR8G8B8A8Unorm:
        return "R8G8B8A8_UNORM";
    case kFormatR8G8B8A8Srgb:
        return "R8G8B8A8_SRGB";
    case kFormatB8G8R8A8Unorm:
        return "B8G8R8A8_UNORM";
    case kFormatB8G8R8A8Srgb:
        return "B8G8R8A8_SRGB";
    case kFormatA2R10G10B10UnormPack32:
        return "A2R10G10B10_UNORM_PACK32";
    case kFormatA2B10G10R10UnormPack32:
        return "A2B10G10R10_UNORM_PACK32";
    case kFormatR16G16B16A16Sfloat:
        return "R16G16B16A16_SFLOAT";
    default:
        return "format " + std::to_string(format);
    }
}

// SDR only, as the D3D12 present pass without its HDR conversion (DlssNr_PresentColour.h): eight-bit RGBA or BGRA,
// UNORM or sRGB, in the sRGB non-linear colour space. Everything else is refused with its format and colour space
// named, and goes to the screen as the game rendered it.
inline ColourPlan PlanColour(uint32_t format, uint32_t colourSpace)
{
    ColourPlan plan;
    plan.raw = format;
    plan.formatName = FormatName(format);

    const auto refuse = [&plan, colourSpace](const std::string& what)
    {
        plan.refusal = what + " (" + plan.formatName + ", colour space " + std::to_string(colourSpace) +
                       "): the present host's NR takes SDR only; the frame is shown as the game rendered it";
        return plan;
    };

    if (colourSpace == kColourSpaceHdr10St2084)
        return refuse("an HDR10 swapchain (PQ, Rec.2020)");
    if (colourSpace == kColourSpaceExtendedSrgbLinear)
        return refuse("an scRGB swapchain (linear light)");
    if (colourSpace == kColourSpaceHdr10Hlg)
        return refuse("an HLG swapchain");
    if (colourSpace != kColourSpaceSrgbNonlinear)
        return refuse("a colour space other than sRGB non-linear");

    switch (format)
    {
    case kFormatR8G8B8A8Unorm:
    case kFormatB8G8R8A8Unorm:
        plan.raw = format;
        break;
    case kFormatR8G8B8A8Srgb:
        plan.raw = kFormatR8G8B8A8Unorm;
        break;
    case kFormatB8G8R8A8Srgb:
        plan.raw = kFormatB8G8R8A8Unorm;
        break;
    case kFormatR16G16B16A16Sfloat:
        return refuse("a floating-point swapchain");
    case kFormatA2R10G10B10UnormPack32:
    case kFormatA2B10G10R10UnormPack32:
        return refuse("a ten-bit swapchain");
    default:
        return refuse("a format other than RGBA8/BGRA8");
    }

    plan.sdr = true;
    return plan;
}

// --- Device extensions, at vkCreateDevice ------------------------------------------------------------------------

// What the model needs of a device, by name: the same list the after-upscale route asks for (DlssNr_VkExtensions.h),
// repeated as strings so it is decided here. NGX loads its kernels through the two NVX extensions, and no game enables
// them; a device's extensions are fixed at creation.
inline constexpr const char* kNrExtensions[] = {
    "VK_NVX_binary_import",
    "VK_NVX_image_view_handle",
    "VK_KHR_push_descriptor",
    "VK_KHR_buffer_device_address",
};
inline constexpr const char kBdaKhr[] = "VK_KHR_buffer_device_address";

// The composition pass's SPIR-V writes storage images without a format (StorageImageWriteWithoutFormat), which a device
// allows with the shaderStorageImageWriteWithoutFormat feature, at Vulkan 1.3, or with this extension
// (VUID-VkShaderModuleCreateInfo-pCode-08740). Asked for only when neither of the first two holds.
inline constexpr const char kFormatFeatureFlags2[] = "VK_KHR_format_feature_flags2";
inline constexpr const char kBdaExt[] = "VK_EXT_buffer_device_address";

struct ExtensionPlan
{
    std::vector<std::string> add;     // appended to the application's list
    std::vector<std::string> already; // the application asked for them itself
    std::vector<std::string> missing; // not possible on this device, with why
    bool ready = false;               // every one of kNrExtensions will be enabled (the EXT address counts for KHR)
};

inline bool Listed(const std::vector<std::string>& list, const std::string& name)
{
    for (const std::string& item : list)
    {
        if (item == name)
            return true;
    }
    return false;
}

// What the host knows of the device being created, beside the extension lists.
struct DeviceCreateFacts
{
    // The instance is Vulkan 1.1 or enabled VK_KHR_get_physical_device_properties2, which push_descriptor, the KHR
    // buffer device address and format_feature_flags2 depend on (asking for them without it makes the creation
    // invalid).
    bool properties2 = false;
    // The instance is Vulkan 1.1: the KHR buffer device address also depends on VK_KHR_device_group below it, which the
    // host does not add.
    bool vulkan11 = false;
    // The device already allows the composition pass's format-less storage writes (the feature is enabled, or the
    // instance is Vulkan 1.3).
    bool storageWithoutFormat = true;
    // The application's chain has VkPhysicalDeviceVulkan14Features, and whether its pushDescriptor is on: with it off,
    // VK_KHR_push_descriptor may not be enabled beside it.
    bool vulkan14Features = false;
    bool pushDescriptorFeature = false;
};

// `asked`: the application's enabled extensions; `supported`: what the physical device offers. Without the two NVX
// extensions the model cannot run on the device at all, so nothing is added to it then (an AMD or Intel device keeps
// its create info as the application made it).
inline ExtensionPlan PlanDeviceExtensions(const std::vector<std::string>& asked,
                                          const std::vector<std::string>& supported, const DeviceCreateFacts& facts)
{
    ExtensionPlan plan;

    for (const char* nvx : { "VK_NVX_binary_import", "VK_NVX_image_view_handle" })
    {
        if (!Listed(asked, nvx) && !Listed(supported, nvx))
            plan.missing.push_back(std::string(nvx) + " (the device does not offer it)");
    }

    if (!plan.missing.empty())
    {
        for (const char* name : kNrExtensions)
        {
            if (Listed(asked, name))
                plan.already.push_back(name);
        }
        return plan;
    }

    std::vector<std::string> wanted(std::begin(kNrExtensions), std::end(kNrExtensions));
    if (!facts.storageWithoutFormat)
        wanted.push_back(kFormatFeatureFlags2);

    for (const std::string& want : wanted)
    {
        if (Listed(asked, want))
        {
            plan.already.push_back(want);
            continue;
        }

        // The KHR and EXT buffer device addresses may not be enabled together (VUID-VkDeviceCreateInfo-
        // ppEnabledExtensionNames-03328): the application's EXT stands for it.
        if (want == kBdaKhr && Listed(asked, kBdaExt))
        {
            plan.already.push_back(kBdaExt);
            continue;
        }

        if (!Listed(supported, want))
        {
            plan.missing.push_back(want + " (the device does not offer it)");
            continue;
        }

        if ((want == "VK_KHR_push_descriptor" || want == kBdaKhr || want == kFormatFeatureFlags2) && !facts.properties2)
        {
            plan.missing.push_back(want + " (needs Vulkan 1.1 or VK_KHR_get_physical_device_properties2 on the "
                                          "instance)");
            continue;
        }

        if (want == kBdaKhr && !facts.vulkan11)
        {
            plan.missing.push_back(want + " (needs a Vulkan 1.1 instance: VK_KHR_device_group is not added)");
            continue;
        }

        if (want == "VK_KHR_push_descriptor" && facts.vulkan14Features && !facts.pushDescriptorFeature)
        {
            plan.missing.push_back(want + " (the application's VkPhysicalDeviceVulkan14Features has pushDescriptor "
                                          "off)");
            continue;
        }

        plan.add.push_back(want);
    }

    plan.ready = plan.missing.empty();
    return plan;
}

inline std::string JoinNames(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names)
        text += (text.empty() ? "" : ", ") + name;
    return text.empty() ? std::string("none") : text;
}

// "NR extensions: added VK_NVX_binary_import, ...; already enabled none; not possible none"
inline std::string ExtensionText(const ExtensionPlan& plan)
{
    return "NR extensions: added " + JoinNames(plan.add) + "; already enabled " + JoinNames(plan.already) +
           "; not possible " + JoinNames(plan.missing);
}

// --- The bufferDeviceAddress feature, at vkCreateDevice ------------------------------------------------------------

// The model allocates memory it takes device addresses of (VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT), which needs the
// bufferDeviceAddress feature enabled, not only its extension (VUID-VkMemoryAllocateInfo-flags-03331; the design's
// "advertising buffer-device-address is not the same as enabling bufferDeviceAddress"). What the application's
// pNext chain already says about it decides what the host may do: a feature struct is prepended to a copy of the
// create info only where the chain has none that could conflict (one struct of a type, and never the
// buffer-device-address struct beside VkPhysicalDeviceVulkan12Features). A chain that has one with the feature off is
// left alone, since editing it means copying every struct before it, of types the host cannot know; the model then
// runs as the after-upscale route has always run it, with the extensions alone (which has run in games that way),
// and the device line says so: the validation layer reports those allocations there.
inline constexpr uint32_t kStructBdaFeatures = 1000257000;    // VkPhysicalDeviceBufferDeviceAddressFeatures
inline constexpr uint32_t kStructBdaFeaturesExt = 1000244000; // VkPhysicalDeviceBufferDeviceAddressFeaturesEXT
inline constexpr uint32_t kStructVulkan12Features = 51;       // VkPhysicalDeviceVulkan12Features

struct BdaChain
{
    bool khrNode = false; // VkPhysicalDeviceBufferDeviceAddressFeatures in the chain
    bool extNode = false; // ... the EXT one
    bool v12Node = false; // VkPhysicalDeviceVulkan12Features
    bool enabled = false; // one of them has bufferDeviceAddress on
};

enum class BdaAction : uint8_t
{
    Already,       // the application enabled it
    AddKhr,        // prepend VkPhysicalDeviceBufferDeviceAddressFeatures with it on
    AddExt,        // prepend the EXT struct (the application enabled VK_EXT_buffer_device_address)
    NotSupported,  // the physical device does not offer it
    ChainHasItOff, // a struct in the chain says off: not edited
};

inline BdaAction PlanBufferDeviceAddress(const BdaChain& chain, bool supported, bool extExtension)
{
    if (chain.enabled)
        return BdaAction::Already;
    if (!supported)
        return BdaAction::NotSupported;
    if (chain.khrNode || chain.extNode || chain.v12Node)
        return BdaAction::ChainHasItOff;
    return extExtension ? BdaAction::AddExt : BdaAction::AddKhr;
}

inline const char* BdaText(BdaAction action)
{
    switch (action)
    {
    case BdaAction::Already:
        return "bufferDeviceAddress already enabled";
    case BdaAction::AddKhr:
    case BdaAction::AddExt:
        return "bufferDeviceAddress enabled by the host";
    case BdaAction::NotSupported:
        return "bufferDeviceAddress not possible (the device does not offer it)";
    default:
        return "bufferDeviceAddress off in the application's create info, not edited: the model runs without it, as "
               "on the after-upscale route";
    }
}

inline bool BdaReady(BdaAction action) { return action != BdaAction::NotSupported; }

// --- At present ---------------------------------------------------------------------------------------------------

// Why a present the round trip carried was not shown to the model. Every one is decided before anything of the model
// is recorded, except RecordFailed, after which the plain round trip is recorded instead and the model is off.
enum class NrSkip : uint8_t
{
    None,
    Off,            // [DlssNr] Enabled is off now (the host was installed with it on)
    Colour,         // not an SDR RGBA8/BGRA8 swapchain (ColourPlan::refusal)
    Extensions,     // the device was created without the model's extensions (ExtensionPlan::missing)
    FormatFeatures, // the device cannot blit, store or sample a format the conversion or the model needs
    TooSmall,       // below kDlssNrMinExtent, frame or working size
    AfterUpscale,   // the after-upscale route owns or wants the model (VkModelOwner)
    D3d12,          // the D3D12 route runs the model in this process
    OtherDevice,    // the model is in use on another device
    OtherSwapchain, // the model serves another live swapchain of this device
    Unavailable,    // the model could not be brought up, or faulted: off for this device (the reason is logged)
    Busy,           // as many of the model's frames in flight as its descriptor ring allows
    RecordFailed,   // recording the pass failed; the frame carried unchanged and the model is off
    Count,
};

inline const char* NrSkipName(NrSkip skip)
{
    switch (skip)
    {
    case NrSkip::Off:
        return "NR off";
    case NrSkip::Colour:
        return "colour";
    case NrSkip::Extensions:
        return "device extensions";
    case NrSkip::FormatFeatures:
        return "format features";
    case NrSkip::TooSmall:
        return "too small";
    case NrSkip::AfterUpscale:
        return "after-upscale route";
    case NrSkip::D3d12:
        return "D3D12 route";
    case NrSkip::OtherDevice:
        return "another device";
    case NrSkip::OtherSwapchain:
        return "another swapchain";
    case NrSkip::Unavailable:
        return "model unavailable";
    case NrSkip::Busy:
        return "model busy";
    case NrSkip::RecordFailed:
        return "record failed";
    default:
        return "none";
    }
}

// The model's frames in flight at once: DlssNr_Vk rotates eighteen constant/descriptor slots with no completion test
// of its own, and a present-host frame takes at most three (encode, downsample, resolve), so four frames in flight
// leave six slots unused. The host's fences are the completion test.
inline constexpr size_t kNrInFlightMax = 4;

// Whether this frame starts the model's history over. The model is told to forget on its first frame, on a frame the
// host knows is a cut, and on every frame when [DlssNr] ZeroGuideReset asks (zero guides say nothing moved, so its
// history is a ghost of the last frame under a moving camera).
inline bool NrReset(bool firstFrame, bool cut, bool zeroGuideReset) { return firstFrame || cut || zeroGuideReset; }

// --- Counters ----------------------------------------------------------------------------------------------------

struct NrStats
{
    uint64_t evaluated = 0; // frames the model's pass was recorded and submitted for
    uint64_t refused = 0;   // frames the round trip carried and the model was not shown
    std::array<uint64_t, static_cast<size_t>(NrSkip::Count)> skips {};
    uint64_t resets = 0;    // evaluated frames that started the model's history over
    uint64_t timed = 0;     // pass GPU times read back
    double lastGpuMs = 0.0; // the latest of them
    double maxGpuMs = 0.0;
    double sumGpuMs = 0.0;
    std::string firstRefusal; // the first refusal's full text
};

inline std::string Ms3(double ms)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", ms);
    return text;
}

// "NR: evaluated 98, refused 2 (model unavailable 2), history resets 1, pass GPU last 3.512 ms, mean 3.498 ms,
// max 4.120 ms; first refusal: ..."
inline std::string NrText(const NrStats& s)
{
    const auto n = [](uint64_t value) { return std::to_string(value); };

    std::string text = "NR: evaluated " + n(s.evaluated) + ", refused " + n(s.refused);

    if (s.refused != 0)
    {
        std::string reasons;
        for (size_t i = 1; i < s.skips.size(); ++i)
        {
            if (s.skips[i] != 0)
                reasons += (reasons.empty() ? "" : ", ") + std::string(NrSkipName(static_cast<NrSkip>(i))) + " " +
                           n(s.skips[i]);
        }
        text += " (" + reasons + ")";
    }

    text += ", history resets " + n(s.resets);

    if (s.timed != 0)
        text += ", pass GPU last " + Ms3(s.lastGpuMs) + " ms, mean " + Ms3(s.sumGpuMs / static_cast<double>(s.timed)) +
                " ms, max " + Ms3(s.maxGpuMs) + " ms";
    else
        text += ", pass GPU not measured";

    if (!s.firstRefusal.empty())
        text += "; first refusal: " + s.firstRefusal;

    return text;
}
} // namespace DlssNr::VkPresentHost
