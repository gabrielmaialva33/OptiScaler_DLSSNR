#pragma once

#include <d3d12.h>
#include <mutex>
#include "DlssNr_PreUpscale.h"

#include <shaders/dlssnr/DlssNr_Common.h>
#include <nvsdk_ngx.h>

// DLSS 5 Neural Rendering, run over the upscaler's output.
//
// Neural Rendering is a post-process, not an upscaler and not a denoiser: it takes a finished frame plus
// depth and motion vectors and synthesises detail. NVIDIA ships no public integration for it, so it is
// driven directly through nvngx_dlssnr.dll as feature 18.
//
// OptiScaler is the right host for it because of one thing it knows that an external hook cannot: which
// NGX evaluate belongs to the upscaler and which to frame generation. Both are handed depth and motion
// vectors, so anything guessing from the parameter block alone attaches to both and runs the model twice
// per rendered frame. Here it is a lookup on the feature handle.
class Config;

namespace DlssNr
{
// The model runs immediately after the game's upscaler, before the interface is drawn. It is shown a
// display-referred proxy of that frame -- the sort of picture it was trained on -- and its answer is
// composed back over the untouched original.
// Runs the model over Output on the same command list, immediately after the upscaler has written it.
// Called only for upscaler evaluates -- never for frame generation, which is the whole point.
//
// Safe to call every frame; it builds what it needs on first use and disables itself for the session if
// anything fails, rather than retrying into a crash.
// timingQueue is the queue this command list will be executed on, when the caller knows it.
// State::currentCommandQueue only exists once a D3D12 swapchain has been created, which a Vulkan
// game never does -- so without this the pass runs and never reports what it cost.
//
// preUpscaleDeclined is ScopedPreUpscale::Declined() from the scope wrapped around this same
// evaluate, when there was one. On stage 1 this pass stands down unless that scope declined the
// evaluate for a reason this side can serve.
void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue = nullptr, bool preUpscaleDeclined = false);

// The other place the model can run: before the upscaler, over the game's render-resolution colour.
//
// Stage 1 in the config. The model is shown the colour the game is about to hand the upscaler --
// jittered, aliased, at render size -- with depth and motion vectors that are for once the same size
// as the picture. Its edit is composed into a private copy of that colour, and for the duration of
// the upscaler's evaluate the parameter block names the copy as the colour input. The game's own
// buffer is never written. The upscaler then enlarges the edited frame exactly as it would have
// enlarged the original, and everything after it -- frame generation, the interface -- is untouched.
//
// Why it is a scope: the swap has to be undone. The parameter block is the game's, it is reused
// next frame, and a block still pointing at a texture of ours is a stale read the moment the copy
// is reallocated. Construct it around the upscaler's evaluate and the destructor puts the original
// back, whichever way the evaluate went.
//
//   bool declined = false;
//   {
//       DlssNr::ScopedPreUpscale pre(cmdList, params, isNotRayReconstruction);
//       result = upscaler->Evaluate(cmdList, params);
//       declined = pre.Declined();
//   }
//   DlssNr::EvaluateAfterUpscale(cmdList, params, queue, declined);
//
// applies says whether this evaluate is one the pre-upscale path should serve at all. Ray
// reconstruction is not: its colour input is undenoised, and the model has no business synthesising
// detail into noise. When the pre-upscale path declines an evaluate for such a reason, the
// after-upscale path is allowed to run for it instead, so the frame is still served -- and the
// caller carries that answer across, through Declined(), rather than the two sides sharing a global.
// Frame generation evaluates do not get a scope at all: they are not upscales, and in Jedi Survivor
// they arrive on their own command list interleaved with the upscaler's.
//
// Does nothing at all unless the pass is enabled and the stage is 1, so it is safe to leave in place
// around every upscaler evaluate.
class ScopedPreUpscale
{
  public:
    ScopedPreUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params, bool applies,
                     ID3D12CommandQueue* timingQueue = nullptr);
    ~ScopedPreUpscale();

    ScopedPreUpscale(const ScopedPreUpscale&) = delete;
    ScopedPreUpscale& operator=(const ScopedPreUpscale&) = delete;

    // Whether the upscaler is reading the model's edit this frame.
    bool Swapped() const { return _swapped; }

    // Whether this evaluate was handed to the after-upscale path instead. Pass it to
    // EvaluateAfterUpscale for the same evaluate.
    bool Declined() const { return _declined; }

  private:
    ID3D12GraphicsCommandList* _cmdList = nullptr;
    NVSDK_NGX_Parameter* _params = nullptr;
    Detail::ColorBinding<NVSDK_NGX_Parameter, ID3D12Resource> _color;
    ID3D12Resource* _scratch = nullptr;
    std::unique_lock<std::mutex> _lock;
    // The state the copy was handed to the upscaler in, to move it back out of.
    int _scratchState = 0;
    bool _swapped = false;
    bool _declined = false;
};



// Frame generation titles tag their UI layer through Streamline; a copy of it makes the HUD mask
// exact at the finished frame. Called at tag time.




// The settings panel, drawn inside OptiScaler's menu.
void RenderMenu(::Config* config, float menuResScale);

// Clears the session failure latch, so a failure caused by transient thrash does not cost a restart.
void RetryAfterFailure();


// Asks the model whether it will work on Direct3D 11 at all, once, and logs the answer.
//
// The bridge exists because of a claim nobody tested: "the model refuses on DX11, it answers
// FeatureNotSupported". Nothing in this project has ever called the snippet's own D3D11 entry points
// -- it exports ten of them, implemented in ngx_d3d11.cpp and sharing CreateFeatureCommon and
// EvaluateFeatureCommon with the D3D12 path. Nothing is created and nothing changes; it resolves the
// entry points and initialises on the game's own device, which is where a refusal would appear.
void ProbeD3D11(void* d3d11Device);

// What scale this game's buffer is on, measured from the untouched copy of each frame.
//
// A suggestion only. Nothing applies it: the menu shows it and the user takes it or does not, which
// keeps the number visible and adjustable rather than a value that moved on its own. Confidence is
// how settled recent readings are -- 1 means they agree, 0 means the scene is changing under the
// measurement and no single value would serve.
struct CalibrationReading
{
    float suggestion = 0.0f;

    // How much recent readings agree. This is steadiness, not correctness: a frozen frame agrees with
    // itself perfectly, so a loading screen scores full marks for a number that means nothing. Read it
    // together with usable.
    float steadiness = 0.0f;

    unsigned long long samples = 0;

    // Whether the scene is worth measuring at all. False when the frame is already tone mapped -- the
    // divisor does nothing there and the reading would be a meaningless 0.9 -- or when too little of
    // the picture is lit to say where the top of the range is. A dark cave gives a small number very
    // steadily, which is the trap this exists to close.
    bool usable = false;
    const char* why = "";
};

CalibrationReading Calibration();

// Whether the model is loaded and running, for the overlay.
bool IsRunning();

// Why it is not, if it is not. Empty while it is running or has not been tried yet.
const char* FailureReason();

// What the game offers by way of exposure. Observed every frame whether or not the setting is on, so
// the menu can say whether turning it on would do anything here.
struct ExposureStatus
{
    unsigned long long seenFrames = 0;   // evaluates observed; 0 means nothing has run yet
    bool offeredNow = false;             // a texture on the most recent frame
    bool everOffered = false;            // a texture on any frame so far
    float exposure = 0.0f;               // last value read back, 0 if none
    float preExposure = 1.0f;
};

ExposureStatus GameExposureStatus();

// The white point the exposure meter has settled on, or 0 if it has not taken a reading yet. For the
// overlay, so the number in use is visible rather than inferred.

// What the pass last cost on the GPU, in milliseconds, or nothing if it has not been measured yet.
std::optional<double> LastGpuTime();
const char* BeforeUpscaleStatus();

// What the white point meter last settled on, or 0 when it is not running. For the menu.


// Writes a run of consecutive frames, each as the upscaler produced it and again after the model's edit.
// The pair is a control: same frames, same run, one variable.
void RequestCapture(unsigned int frames);
bool CaptureInProgress();

void Shutdown();
} // namespace DlssNr
