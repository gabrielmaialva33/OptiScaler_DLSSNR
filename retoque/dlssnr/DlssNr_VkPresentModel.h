#pragma once

#include <vulkan/vulkan.h>

#include "DlssNr_VkPresentNr.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The model at present on Vulkan, cold (docs/dlssnr/design/nr-vulkan-present-host.md, stage 3): the Vulkan present
// host's own instance of the NR model plumbing in DlssNrFeature_Vk.cpp, with no game parameters and no application NGX
// initialisation. One per device the host carries presents on; the host records it into the round trip's command
// buffer, on the work queue, between the conversion of the presented image into a working colour and the conversion
// back.
//
// What it owns, per owner rather than shared with the after-upscale route: the parameter block, the feature, the
// composition pass and its descriptor/constant ring, the proxy, keep and output images, two zero guides (constant
// depth, zero motion, at the model's working size, cleared once), and a timestamp pool. What it shares, because there
// is one of each in the process: the forwarder (nvngx.dll_dlssnr.dll, its fault isolation and float slot) and NGX's
// initialisation on a device. VkModelOwner keeps the two routes from both using them.
//
// Completion is the host's to observe: every recording carries the host's ticket, Submitted() or Discarded() says what
// became of it, and Completed() says its fence was seen signalled. Nothing a frame in flight may use is freed or
// rewritten before that: images and features replaced by a resize wait in a list for the tickets that used them, and
// at most kNrInFlightMax frames are in flight, which is what the pass's slot ring allows. A ticket never seen complete
// keeps what it used forever; on a lost device everything is abandoned (DEVELOPMENT.md rule 8).
//
// Everything is called with the host's transport lock held; the model takes none of its own beyond the forwarder's.
namespace DlssNr
{
struct VkPresentModelDevice
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t timestampValidBits = 0; // of the work queue's family; 0: the pass is not timed
};

// One frame, as the host hands it over: the working colour (kWorkingFormat), in GENERAL on entry and left there
// rewritten, sampled and stored by the pass.
struct VkPresentNrFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t ticket = 0; // the host's, unique on the device, non-zero
    bool cut = false;    // the host knows the history is invalid: a new swapchain served, the model back after a gap
};

struct VkPresentAdmission
{
    VkPresentHost::NrSkip skip = VkPresentHost::NrSkip::None;
    std::string text; // the full refusal, when skip is set
};

class VkPresentModel
{
  public:
    explicit VkPresentModel(const VkPresentModelDevice& device);
    ~VkPresentModel(); // calls no Vulkan: Release() or Abandon() comes first
    VkPresentModel(const VkPresentModel&) = delete;
    VkPresentModel& operator=(const VkPresentModel&) = delete;

    // Everything a frame of this size needs that needs no command buffer: the forwarder, NGX on the device, the
    // parameter block, the pass, the images (rebuilt for a new size; the old ones are freed once the frames that used
    // them are seen complete). A failure that will not mend itself is latched: Unavailable from then on.
    VkPresentAdmission Admit(uint32_t width, uint32_t height);

    // The pass into frame.cmd, after an Admit of the same size: the zero guides' clear when owed, the feature's
    // creation when there is none (its initialisation is recorded here too), encode, the downsample below native, the
    // model, the resolve into the working colour. False: something failed after recording began; the caller discards
    // the recording (Discarded) and records the plain round trip, and the model is Unavailable from then on.
    bool Record(const VkPresentNrFrame& frame);

    // What became of the recording Record made for `ticket`: submitted with its round trip, or never submitted.
    void Submitted(uint64_t ticket);
    void Discarded(uint64_t ticket);

    // The fence of `ticket`'s round trip was seen signalled. The pass's GPU time when that frame was timed.
    std::optional<double> Completed(uint64_t ticket);

    // Frames submitted and not yet seen complete.
    size_t InFlight() const;

    // Why the model is off on this device, empty while it is not.
    const std::string& Failure() const;

    // Whether the frame Record last made starts the model's history over.
    bool LastFrameReset() const { return _lastReset; }

    // What the model has to say (built, initialised, off and why), for the host to log once its lock is released.
    std::vector<std::string> TakeLines();

    // Free everything, on a live device with nothing in flight. With `deviceGoing` (vkDestroyDevice, before the call
    // down), also shut NGX down on the device through the forwarder, so a later device with this handle value is
    // initialised again.
    void Release(bool deviceGoing);

    // The device was lost, or frames will never be seen complete: forget every handle, destroy nothing.
    void Abandon();

    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    bool _lastReset = false;
};

// One owner of the model in the process (DlssNr_VkModelOwner.h), for the present host. Before a frame it would run the
// model on `device`: NrSkip::None when it may (the token is then taken), otherwise why not (AfterUpscale, D3d12,
// OtherDevice), with the full text.
VkPresentAdmission ClaimModelForPresentHost(VkDevice device);

// Whether the present host, holding the model on `device`, has to give it up now: another route wants it.
VkPresentAdmission PresentHostMustYield(VkDevice device);

// The present host gave the model up on `device`: released or abandoned, nothing of its in flight.
void ReleaseModelFromPresentHost(VkDevice device);
} // namespace DlssNr
