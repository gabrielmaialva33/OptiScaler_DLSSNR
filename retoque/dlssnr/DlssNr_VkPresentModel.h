#pragma once

#include <vulkan/vulkan.h>

#include "DlssNr_VkPresentNr.h"

#include <atomic>
#include <cstdint>
#include <functional>
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
// depth, zero motion, at the model's working size, cleared once), a timestamp pool, and with [DlssNr] SynthMotion the
// synthesized-motion estimator (stage 5: SynthMotion::Estimator_Vk, its pipelines and its images), whose field
// replaces the zero motion guide once it has one. What it shares, because there
// is one of each in the process: the forwarder (nvngx.dll_dlssnr.dll, its fault isolation and float slot) and NGX's
// initialisation on a device. VkModelOwner keeps the two routes from both using them.
//
// Completion is the host's to observe: every recording carries the host's ticket, Submitted() or Discarded() says what
// became of it, and Completed() says its fence was seen signalled. Nothing a frame in flight may use is freed or
// rewritten before that: images and features replaced by a resize wait in a list for the tickets that used them, and
// at most kNrInFlightMax frames are in flight, which is what the pass's slot ring allows. A ticket never seen complete
// keeps what it used forever; on a lost device everything is abandoned (DEVELOPMENT.md rule 8).
//
// Built off the present path ("Off the present path" in the design note): everything that brings the model up or
// resizes it -- NGX's initialisation on the device, the driver core, the pass, the images, the feature's creation and
// its GPU initialisation -- runs on one process-wide builder thread, which takes no lock of the host's and submits its
// work through VkPresentModelDevice::submit. Admit only asks for a build and adopts it once over; while one is queued
// or running the present path makes no NGX call for the model, and frames are carried unchanged (NrSkip::Building).
//
// Everything is called with the host's transport lock held; the model takes none of its own beyond the forwarder's
// and, for the builder's hand-over, its job's.
namespace DlssNr
{
struct VkPresentModelDevice
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t timestampValidBits = 0; // of the work queue's family; 0: the pass is not timed
    uint32_t apiVersion = 0;         // the application's instance asked for it: the estimator needs 1.1 (stage 5)

    // The builder's way to a queue, the host's: `cmd` submitted on a queue of `family` with that queue's present and
    // submit locks taken as a borrower takes them, in short tries (VK_NOT_READY when they stay busy: nothing
    // submitted), the locks released, then the fence waited for with `timeoutNs` in slices (VK_TIMEOUT: still pending;
    // VK_INCOMPLETE: `cancelled` was set while waiting -- the device is going -- in either case the fence and the
    // buffer's pool are left alone). Called on the builder thread, which holds no lock of the host's.
    std::function<VkResult(VkCommandBuffer cmd, uint32_t family, uint64_t timeoutNs,
                           const std::atomic<bool>& cancelled)>
        submit;
};

// One frame, as the host hands it over: the working colour (kWorkingFormat), in GENERAL on entry and left there
// rewritten, sampled and stored by the pass. HDR instead hands over the raw transport image in TRANSFER_DST;
// Record performs the conversion around NR and returns it in TRANSFER_DST.
struct VkPresentNrFrame
{
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t ticket = 0;     // the host's, unique on the device, non-zero
    uint32_t hdrProfile = 0; // HDR: image is the raw transport copy, TRANSFER_DST on entry/exit; view unused
    bool cut = false; // the host knows the history is invalid: a new swapchain served, the model back after a gap
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

    // Whether a frame of this size, its round trip on `family`, may be shown to the model now. Never builds anything:
    // a product built for another size or family is retired (freed once the frames that used it are seen complete) and
    // a build is requested; while it is queued or running the answer is NrSkip::Building; once it is over it is
    // adopted here, or its failure latched (Unavailable from then on).
    VkPresentAdmission Admit(uint32_t width, uint32_t height, uint32_t family, uint32_t hdrProfile = 0,
                             uint32_t rawFormat = 0);

    // The pass into frame.cmd, after an Admit of the same size: encode, the downsample below native, the model, the
    // resolve into the working colour. False: something failed after recording began; the caller discards the recording
    // (Discarded) and records the plain round trip, and the model is Unavailable from then on.
    bool Record(const VkPresentNrFrame& frame);

    // What became of the recording Record made for `ticket`: submitted with its round trip, or never submitted.
    void Submitted(uint64_t ticket);
    void Discarded(uint64_t ticket);

    // The fence of `ticket`'s round trip was seen signalled. The pass's GPU time when that frame was timed.
    std::optional<double> Completed(uint64_t ticket);

    // Frames submitted and not yet seen complete, plus one while a build is running: either keeps the model (and the
    // token) from being released. A build only queued does not: Release calls it off.
    size_t InFlight() const;

    // A build queued or running: the present path makes no NGX call for the model.
    bool Building() const;

    // Waits up to `timeoutMs` for a build under way to be over (never on the present path: vkDestroyDevice only). True
    // when none is under way any more.
    bool WaitBuild(uint64_t timeoutMs);

    // The build under way is orphaned (its device is going, or lost): the builder stops at its next step or wait slice
    // and leaves whatever it made to the device, calling nothing more on it.
    void Orphan();

    // The device was lost during a build: the host marks the device lost, and Release frees nothing.
    bool DeviceLost() const;

    // Why the model is off on this device, empty while it is not.
    const std::string& Failure() const;

    // Whether the frame Record last made starts the model's history over.
    bool LastFrameReset() const { return _lastReset; }

    // Whether the frame Record last made told the model the synthesized field ([DlssNr] SynthMotion, stage 5), not
    // the zero motion guide.
    bool LastFrameSynthesized() const { return _lastSynthesized; }

    // What the model has to say (built, initialised, off and why), for the host to log once its lock is released.
    std::vector<std::string> TakeLines();

    // Free everything of the model's, on a live device with nothing in flight (otherwise it abandons). NGX and the
    // driver's core stay up on the device (ShutdownPresentNgx takes them down with it), and a latched failure stays.
    void Release();

    // The device was lost, or frames will never be seen complete: forget every handle, destroy nothing.
    void Abandon();

    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    bool _lastReset = false;
    bool _lastSynthesized = false;
};

// One owner of the model in the process (DlssNr_VkModelOwner.h), for the present host. Before a frame it would run the
// model on `device`: NrSkip::None when it may (the token is then taken), otherwise why not (AfterUpscale, D3d12,
// OtherDevice), with the full text.
VkPresentAdmission ClaimModelForPresentHost(VkDevice device);

// Whether the present host, holding the model on `device`, has to give it up now: another route wants it.
VkPresentAdmission PresentHostMustYield(VkDevice device);

// The present host gave the model up on `device`: released or abandoned, nothing of its in flight.
void ReleaseModelFromPresentHost(VkDevice device);

// The way back from the after-upscale route (DlssNr_VkModelOwner.h, "The way back"): a game that stops calling its
// upscaler never gives the model up, so once the route has not asked for VkModelOwner::kAfterUpscaleIdleMs the present
// host takes it back on the route's device. In this order, each with the host's transport lock held:
//
// AfterUpscaleIdleOn: whether the route holds the model on `device` and has gone quiet, so a hand-back may begin.
bool AfterUpscaleIdleOn(VkDevice device);

// The hand-back begins: false when it may not (the route asked since). From true on, the route asking again calls it
// off; the host drains every queue of the device behind a fence of its own, submitted only after this.
bool BeginAfterUpscaleHandBack(VkDevice device);

enum class AfterUpscaleHandBack : uint8_t
{
    Done,        // the route's objects were freed and the token released: the present host may claim the model
    Cancelled,   // the route asked again after the hand-back began: it keeps the model and everything of its
    Busy,        // the route's lock was held (it is evaluating): try again at the next present
    NotRetiring, // no hand-back was under way
};

// Every drain fence was seen signalled. Takes the route's lock without waiting (Busy when it is held); on Done frees
// the route's objects, which nothing the GPU runs can still use: the route recorded nothing since the hand-back began,
// and everything it recorded before was submitted ahead of the fences. Lines for the log.
AfterUpscaleHandBack FinishAfterUpscaleHandBack(VkDevice device, std::vector<std::string>& lines);

// The drain cannot finish (a drain submit failed, the device was lost or is being destroyed): the route keeps the
// model.
void CancelAfterUpscaleHandBack(VkDevice device);

// At vkDestroyDevice, before the call down and before NGX is shut down on the device: the after-upscale route's objects
// on `device` freed, the host having drained every queue of it behind its own fences, or, with `forgetOnly` (a lost
// device, work on it never seen complete, a drain that did not finish), only forgotten; and whichever route held the
// token on it gives it back, so a route that never evaluates again does not hold the model for a device that no longer
// exists. Lines for the log.
void AfterUpscaleDeviceGone(VkDevice device, bool forgetOnly, std::vector<std::string>& lines);

// At vkDestroyDevice, before the call down, with the device's model released or abandoned: the driver's NGX core the
// host brought up on it shut down, and NGX through the forwarder (which answers only for the device it initialised, so
// one the after-upscale route initialised goes too), so neither answers "initialised" for a later device that gets this
// handle value back. NGX's device-specific shutdown is called with the out count it takes. With `forgetOnly` (a lost
// device, or frames on it never seen complete) nothing is called into: both only forget the device. Lines for the log.
void ShutdownPresentNgx(VkDevice device, bool forgetOnly, std::vector<std::string>& lines);
} // namespace DlssNr
