#pragma once

// The composition pass for Neural Rendering.
//
// Neural Rendering is two things, and only one of them is a shader. The model is an NGX feature --
// created and evaluated, not dispatched -- and that stays where it is. This is the other half: the
// pass that builds the tone-mapped proxy the model is shown, and then transfers the model's answer
// back onto the real frame.
//
// It is an ordinary compute shader with a constant struct, so it belongs here alongside RCAS and
// Output Scaling rather than owning a bespoke root signature and descriptor ring of its own.
//
// One shader, three modes, because all three read and write the same set of resources and differ
// only in what they compute:
//
//   Encode   the frame -> a tone-mapped proxy, plus an untouched copy to transfer against later
//   Down     the proxy -> a smaller proxy, when the model is asked to work below full resolution
//   Resolve  proxy + model answer + untouched copy -> the frame, edited

#include "DlssNr_Common.h"
#include <dlssnr/DlssNr_Coverage.h>
#include <dlssnr/DlssNr_Chain.h>

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Three dispatches are recorded per frame and several frames can be in flight at once, more so with
// frame generation. Each dispatch needs descriptors and constants the GPU is not still reading, so
// there has to be enough for three passes times the deepest pipeline we might sit behind.
// Descriptor and constant slots, consumed one per dispatch and reused round-robin with no fence.
//
// The pass records four dispatches per frame -- meter, encode, downsample, resolve -- so sixteen slots
// is four frames of coverage before a slot is rewritten. The comment this replaces said "three passes
// times the deepest pipeline we might sit behind", and the pass count has since grown to four while
// the ring did not.
//
// Four frames is not enough. Frame generation deliberately runs the GPU several frames behind the CPU,
// and the constants live in an UPLOAD heap written at record time -- so a wrap while the GPU is still
// reading a slot rewrites descriptors and constants underneath it.
//
// A fifth dispatch has since been added -- the calibration grid -- which at thirty-two slots would
// have left six frames, spending exactly the headroom the previous note set aside. Forty-eight
// restores eight frames at five dispatches. If a sixth is ever added, raise this with it rather than
// spending the margin again.
#define DLSSNR_NUM_OF_HEAPS 48

class DlssNr_Dx12 : public Shader_Dx12, public DlssNr_Common
{
  private:
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];

    // One constant buffer per heap, not one for the class.
    //
    // The shared buffer in the base class suits a shader that dispatches once a frame. Three
    // dispatches recorded onto one command list all map and overwrite the same upload buffer before
    // any of them executes, so every pass ends up reading whichever constants were written last --
    // encode and downsample would run with the resolve's parameters.
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] = {};
    // Upload buffers stay mapped for their lifetime. Each slot still owns distinct storage;
    // DispatchPass only writes it when that same descriptor-ring slot is selected.
    void* _mappedConstants[DLSSNR_NUM_OF_HEAPS] = {};

    uint32_t _heapIndex = 0;

    // The shader reads five inputs and writes two, and not every mode uses all of them. Unused slots
    // still need a view bound -- an unbound descriptor is not an empty read, it is a read from
    // nothing -- so a stand-in is written into whichever are spare.
    static constexpr uint32_t kSrvCount = 5;
    static constexpr uint32_t kUavCount = 2;

    // The arguments every view in this pass is created with, in one place.
    //
    // DispatchPass passes these to Shader_Dx12 and copies them into the key below, so the two cannot
    // describe different things. They are named rather than written inline because translateTypeless
    // stopped being fixed in 97c94b05: a cache keyed on the resource alone would hand back a view
    // built under the other policy. Whatever these become, the key carries them.
    static constexpr DXGI_FORMAT kViewFormat = DXGI_FORMAT_UNKNOWN; // take the resource's own format
    static constexpr uint32_t kViewMipLevel = 0;
    static constexpr bool kViewTranslateTypeless = true;

    // What each slot's descriptors described last, so a binding that did not move is not re-created.
    //
    // A view here is a pure function of two things: the resource it names, and the arguments it was
    // created with. Shader_Dx12::CreateShaderResourceView and CreateUnorderedAccessView read nothing
    // but GetDesc and their own format, mip and translateTypeless parameters, so a slot whose key has
    // not moved already holds exactly the bytes the GPU will read, and re-creating the view costs a
    // driver call for nothing. Steady state is the common case -- the same few resources (the game's
    // output, the private copies, the guide clones) bound in the same order every frame.
    //
    // Which desc fields matter is not a guess. Dimension, DepthOrArraySize and MipLevels choose the
    // view dimension and its mip count in both functions, and Format is the view format; all four are
    // in the key. Width and Height shape no view at all -- they are in the key as discriminators, for
    // the case the pointer alone misses: a released resource whose address the allocator hands back
    // to a differently shaped one.
    //
    // That address case is also why the key carries a generation. Shape equality says the descriptor
    // bytes would be written the same, not that they still name the same GPU allocation, and an NR
    // scratch texture freed and re-created at the same size -- a feature rebuild does exactly that --
    // can land at the same pointer over different memory. Every CreateScratch advances the
    // generation, which retires every cached key at once. Game-owned resources have no such token and
    // rest on pointer and shape, as any cache of someone else's resources must.
    struct BindKey
    {
        ID3D12Resource* res = nullptr;
        uint64_t generation = 0;
        UINT64 width = 0;
        UINT height = 0;
        UINT16 depthOrArraySize = 0;
        UINT16 mipLevels = 0;
        D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        uint32_t mipLevel = 0;
        bool translateTypeless = false;

        bool operator==(const BindKey& o) const
        {
            return res == o.res && generation == o.generation && width == o.width && height == o.height &&
                   depthOrArraySize == o.depthOrArraySize && mipLevels == o.mipLevels && dimension == o.dimension &&
                   format == o.format && viewFormat == o.viewFormat && mipLevel == o.mipLevel &&
                   translateTypeless == o.translateTypeless;
        }
    };

    // A default-constructed key names no resource, so a slot that has never been written never
    // matches and always creates.
    BindKey _srvKey[DLSSNR_NUM_OF_HEAPS][kSrvCount] = {};
    BindKey _uavKey[DLSSNR_NUM_OF_HEAPS][kUavCount] = {};

    uint32_t _numThreadsX = 8;
    uint32_t _numThreadsY = 8;

  public:
    DlssNr_Dx12(std::string InName, ID3D12Device* InDevice);
    ~DlssNr_Dx12();

    // The pass. Resources in, and nothing read from anywhere the caller cannot see.
    //
    // This is the whole filter: it brings the model up if it is not already, builds the feature and
    // rebuilds it when the tuning or the resolution changes, evaluates it, and runs the compute passes
    // that show it the frame and bring its answer back. One call, like any other shader here.
    //
    // Sizes come from the output resource. Everything the pass cannot work out for itself is in
    // DlssNrFrameInfo; everything the user chose stays in Config. timingQueue is retained for caller
    // compatibility only; confirmed timing uses the queue observed at ExecuteCommandLists, never
    // this hint or State::currentCommandQueue.
    //
    // colour is what the model is shown and output is where the edited frame lands. They may be the
    // same resource, which is the after-upscale path: the pass reads the upscaler's output and writes
    // it back. When they differ -- the before-upscale path -- colour is read as a shader resource in
    // the state NGX requires of every input, NON_PIXEL_SHADER_RESOURCE, and never written; the
    // untouched original is taken from it and the edit is written to output alone. Only the source's
    // top-left output-sized region is read from a color larger than the output.
    //
    // Returns true only when the resolve wrote the output this frame. Every other way out -- the
    // model still being built, a skipped frame, a failure -- must not be used to replace the game's
    // input color. A false return makes no promise that scratch contents form a complete frame.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue* timingQueue = nullptr, DlssNr::Detail::CoverageSample* coverage = nullptr,
                  DlssNr::Chain::RecordingLease* recording = nullptr);

    // Records one pass. Resources that a given mode does not read may be null; a stand-in is bound in
    // their place so every descriptor in the table is valid.
    // One compute pass. The public entry below drives three of these plus the model.
    bool DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                      ID3D12Resource* InSource, ID3D12Resource* InModel, ID3D12Resource* InOriginal,
                      ID3D12Resource* InMotion,
                      // Vestigial. Fed to the slot the removed edit accumulator read its history from;
                      // nothing reads it now and every caller passes nullptr. Kept only so the binding
                      // table keeps its shape -- not evidence that temporal accumulation exists.
                      ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget, ID3D12Resource* OutKeep);
};
