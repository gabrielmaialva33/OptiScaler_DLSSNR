#pragma once

// Synthesized motion on Vulkan: SynthMotion::Estimator_Dx12's twin, for the Vulkan present host
// (docs/dlssnr/design/nr-vulkan-present-host.md, "Stage 5"). The same FidelityFX optical-flow port and the same
// expand pass, compiled to SPIR-V from the same sources under VK_MODE (precompile/build.sh), and the same output
// contract: a field of R16G16_SFLOAT at the colour's extent, .rg the displacement from the current frame to the
// previous one in colour pixels, +x right, +y down; zero on a reset, during FFX's five warm-up frames after one, on a
// scene cut, and per pixel where the expand's static-overlay rule keeps a HUD still.
//
// Split by lifetime, so the present host can make everything off its present path:
//  - VkPipelines: the device's (ten pipelines, their push-descriptor set layout and pipeline layout). Made once per
//    device, on the host's builder thread; read-only afterwards.
//  - VkImages: one frame size's (two luma pyramids, two flow pyramids, the final flow, the four scene-change textures,
//    the field, a host-visible read-back of the scene-change bit). Made by a build; their move into GENERAL and their
//    clear are recorded into the build's command buffer, which the caller submits and sees complete before the first
//    Record over them.
//  - Estimator_Vk: the history (which luma pyramid is the current one, FFX's frame index, the scene-change bit). Holds
//    no Vulkan object; records onto the caller's command buffer.
//
// Descriptors are pushed (VK_KHR_push_descriptor, which the present host enables for the model): every dispatch pushes
// its own bindings, so nothing per frame outlives the command buffer. Every storage image names its format in the
// SPIR-V, so no device feature is needed beyond Vulkan 1.1's subgroup arithmetic (the search pass).
//
// Nothing here logs: a failure comes back as a reason, for the caller to say where its lines go.
//
// Outside the DLSS-NR module on purpose, like the D3D12 estimator: the module has to stay removable as one block.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <string>

namespace SynthMotion
{
inline constexpr uint32_t kVkLevels = 7;        // FFX's OpticalFlowMaxPyramidLevels, all of them used
inline constexpr uint32_t kVkPasses = 10;       // prepare, pyramid, two scene-change, search, filter, scale, expand,
                                                // and the two clears
inline constexpr uint32_t kVkReadbackSlots = 8; // above the present host's four frames in flight
inline constexpr VkFormat kVkMotionFormat = VK_FORMAT_R16G16_SFLOAT;

// The device's part. Valid() once made; destroyed only when nothing recorded with it can still run.
struct VkPipelines
{
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkPipeline, kVkPasses> pipelines {};
    PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;

    bool Valid() const;
};

struct VkTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// One frame size's part.
struct VkImages
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t lumaWidth[kVkLevels] {};
    uint32_t lumaHeight[kVkLevels] {};
    uint32_t flowWidth[kVkLevels] {};
    uint32_t flowHeight[kVkLevels] {};

    VkTexture luma[2][kVkLevels];   // R8_UINT: the current frame's and the previous one's, swapped as history advances
    VkTexture flow[2][kVkLevels];   // R16G16_SINT, 8x8 blocks: FFX's two, which the levels alternate between
    VkTexture flowFinal;            // level 0, filtered: what the expand reads
    VkTexture scdHistogram;         // R32_UINT
    VkTexture scdPreviousHistogram; // R32_SFLOAT
    VkTexture scdTemp;              // R32_UINT, 3x1
    VkTexture scdOutput;            // R32_UINT, 3x1: [1] holds the scene-change history bits
    VkTexture motion;               // the field, kVkMotionFormat at the colour's extent

    VkBuffer readback = VK_NULL_HANDLE; // kVkReadbackSlots x 16 bytes, host-visible and coherent, mapped for good
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    const uint32_t* readbackData = nullptr;

    bool Valid() const { return motion.view != VK_NULL_HANDLE && readbackData != nullptr; }
};

// Why the estimator cannot run on this device, empty when it can. `apiVersion` is the version the application's
// instance asked for (the SPIR-V is 1.3, and the search needs subgroup arithmetic in compute: both Vulkan 1.1).
// Formats, subgroup support and the push-descriptor limit are read from the physical device.
std::string VkUnsupported(VkPhysicalDevice physical, uint32_t apiVersion);

// The device's part: shader modules (destroyed once the pipelines exist), the set layout, the pipeline layout and the
// ten pipelines. On failure everything made is destroyed and `why` says what failed.
bool CreateVkPipelines(VkDevice device, VkPipelines& out, std::string& why);
void DestroyVkPipelines(VkPipelines& pipelines);

// One size's part, at width x height (at least MinWidth x MinHeight). `init` is a command buffer being recorded, which
// gets every image's move from UNDEFINED into GENERAL and its clear to zero; the caller submits it and sees it complete
// before any Record over these images. On failure everything made is destroyed and nothing was recorded.
bool CreateVkImages(VkDevice device, VkPhysicalDevice physical, uint32_t width, uint32_t height, VkCommandBuffer init,
                    VkImages& out, std::string& why);
void DestroyVkImages(VkDevice device, VkImages& images);

class Estimator_Vk
{
  public:
    static constexpr uint32_t MinWidth = 256;  // FFX's level-6 luma must be at least 4 pixels wide
    static constexpr uint32_t MinHeight = 128; // and the scene-change strata non-empty
    static constexpr uint32_t WarmupFrames = 5;

    // Records the whole estimate onto `cmd`. `colour`: a sampled-image view of the frame at the images' extent, a
    // float or UNORM RGBA format, in `colourLayout` (GENERAL or SHADER_READ_ONLY_OPTIMAL), its writes already made
    // visible to compute-shader reads. `reset`: the caller knows the history is broken. `ticket`: the caller's, unique
    // and non-zero; Completed(ticket) later reads that frame's scene-change bit. Begins with a dependency on everything
    // earlier on the queue (the previous frame's estimate and every read of its field), ends with the field's writes
    // made visible to every later stage's shader reads, and leaves every image in GENERAL. False: nothing recorded
    // (invalid images or pipelines), nothing pending.
    bool Record(VkCommandBuffer cmd, const VkPipelines& pipelines, const VkImages& images, VkImageView colour,
                VkImageLayout colourLayout, bool reset, uint64_t ticket);

    // A real field was produced this frame: false on a reset and during the warm-up after one. A scene cut's zero field
    // is Ready, with SceneCut() raised.
    bool Ready() const { return _ready; }

    // A scene change, read back from a frame the caller saw complete (Completed): raised on the first Record after
    // that, so at least a frame late (more with frames in flight). The field on the cut frame itself is already zero on
    // the GPU (FFX's search writes zero vectors then). Callers OR it into the model's reset.
    bool SceneCut() const { return _sceneCut; }

    void ConfirmExecuted();  // the command buffer that carried the last Record was submitted
    void AbandonRecording(); // it never was: the history does not advance

    // The frame `ticket` recorded was seen complete: its scene-change bit, if it was copied out, is read.
    void Completed(uint64_t ticket, const VkImages& images);

    // New images, or the estimator taken off them: the history starts over, read-backs pending on older images are
    // forgotten.
    void Restart();

  private:
    struct Slot
    {
        uint64_t ticket = 0; // 0: free
        uint64_t epoch = 0;
        uint32_t frameIndex = 0;
    };

    std::array<Slot, kVkReadbackSlots> _slots {};
    uint64_t _epoch = 1; // advanced by every reset and Restart: read-backs of an older history are ignored
    uint64_t _recordCount = 0;

    // History, advanced only by ConfirmExecuted.
    uint32_t _nextParity = 0;
    int64_t _confirmedFrameIndex = -1;
    uint32_t _recordedFrameIndex = 0;
    uint32_t _recordedSlot = UINT32_MAX;
    bool _recordedReset = false;
    bool _recordedOwedCut = false; // the recording raised an owed cut: owed again if it is abandoned
    bool _pending = false;
    bool _needReset = true;

    bool _cutOwed = false; // a completed frame read a scene change: raised at the next Record
    bool _ready = false;
    bool _sceneCut = false;
};
} // namespace SynthMotion
