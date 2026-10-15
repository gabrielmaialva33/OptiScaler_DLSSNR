#include "pch.h"

#include "DlssNr_Dx11Guides.h"
#include "DlssNrFeature_Dx12.h"
#include "DlssNr_Dred.h"
#include "DlssNr_Submission.h"

#include <Config.h>
#include <Logger.h>
#include <State.h>
#include <with_dx12/with_dx12.h>
#include <with_dx12/dx11_with_dx12.h>

#include <d3d11_4.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

// The native D3D11 guide transport (DlssNr_Dx11Guides.h; docs/dlssnr/design/dx11-native-dlss-guides.md, option (c)).
//
// Right after the driver's own D3D11 DLSS has evaluated, on the game's immediate context and inside the game's call:
//
//   1. The game's motion, and a depth upstream copies safely, are copied into shared textures by upstream's
//      Dx11WithDx12 cache -- the same transport dlss_12 uses, including its depth conversion for typeless
//      depth-stencil formats with a stencil plane. A depth bound as a depth-stencil without one is copied instead
//      into a shader-resource-only texture of the transport's own (Dx11Guides::DepthCopy: under Proton D3D12 never sees
//      D3D11's copy into a shared depth-stencil, tests/dx11-guides-transport). Then the bridge's D3D12 queue is made
//      to wait for all of it on the GPU (Dx11WithDx12::SyncDx11ToDx12: D3D12 waits on D3D11, the direction the bridge
//      already takes at every present). D3D11 never waits on D3D12.
//   2. A list of the transport's own, on the bridge's device, copies the shared textures into the present guide ring
//      (CaptureBridgedGuides), and is executed on that same queue. The ring stamps the capture like any other; the
//      bridge's present host, presenting on that queue too, admits it by queue order and decides on it through the
//      identity rules of present-real-guides.md.
//
// A set of shared textures is written again only once the list that read it has completed, which is checked on the
// CPU and never waited for: a set still in flight refuses the frame (SlotInFlight), as a ring slot being read does.
// The one step that runs a shader on the game's context (upstream's DepthTransfer_Dx11) has the compute bindings it
// changes, and the output merger it needs the depth off, put back exactly as the game left them.

namespace DlssNr
{
namespace
{
using Dx11Guides::kSlots;
using Dx11Guides::Reason;

// The transport's set index is the index upstream's cache prepares (frameIndex % DX11_WITH_DX12_INPUT_FRAMES), and the
// rotation's SlotFree is only sound while the two are one.
static_assert(kSlots == DX11_WITH_DX12_INPUT_FRAMES, "the transport's sets are upstream's cache sets");

// The transport's own copy of a depth bound as a depth-stencil (Dx11Guides::DepthCopy): a shader-resource-only
// texture, shared, and its D3D12 opening. One per set, rebuilt for another D3D11 device, size or format.
struct OwnDepth
{
    ID3D11Texture2D* texture = nullptr;
    ID3D12Resource* opened = nullptr;
    const void* d3d11 = nullptr; // the D3D11 device the texture was made on, as an identity only
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool ntHandle = false;
};

struct Transport
{
    std::mutex mutex;
    Dx11WithDx12::D3D11_UPSCALER_RESOURCE_CACHE_C cache {};
    std::array<OwnDepth, kSlots> ownDepth {};

    // The bridge's device and queue, referenced while the transport's objects live on them.
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    std::array<ID3D12CommandAllocator*, kSlots> allocators {};
    std::array<ID3D12GraphicsCommandList*, kSlots> lists {};
    std::array<uint64_t, kSlots> lastRead {}; // fence value signalled after the list that last read each set
    ID3D12Fence* fence = nullptr;
    uint64_t fenceValue = 0;
    uint64_t sequence = 0; // sets written so far, for the rotation

    // The bridge device and queue the D3D12 side could not be built on, as identities only: not tried again on every
    // evaluate (each try would log), and tried again when either changes.
    const void* failedDevice = nullptr;
    const void* failedQueue = nullptr;
    Dx11Guides::Window window;
    bool reportedFirst = false;
};

// Heap-allocated on first use and never destroyed by a static destructor, which would release D3D12 objects after
// their device at process exit. ShutdownNativeDx11Guides releases them once their last list has completed, when
// DlssNr::Shutdown runs; nothing in the DLL calls that today, so in practice they live until the process exits, like
// the rest of the module's D3D12 objects.
Transport* g_transport = nullptr;
std::mutex g_transportCreate;
std::atomic<long long> g_carriedAtMs { 0 };

long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ID3D11Resource* D3D11Resource(NVSDK_NGX_Parameter* params, const char* name)
{
    ID3D11Resource* resource = nullptr;
    if (params->Get(name, &resource) != NVSDK_NGX_Result_Success)
        params->Get(name, reinterpret_cast<void**>(&resource));
    return resource;
}

Dx11Guides::Texture Describe(ID3D11Resource* resource)
{
    Dx11Guides::Texture texture;
    if (resource == nullptr)
        return texture;

    texture.present = true;

    ID3D11Texture2D* texture2D = nullptr;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture2D))) || texture2D == nullptr)
        return texture;

    D3D11_TEXTURE2D_DESC desc {};
    texture2D->GetDesc(&desc);
    texture2D->Release();

    texture.texture2D = true;
    texture.depthStencil = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    texture.width = desc.Width;
    texture.height = desc.Height;
    texture.mipLevels = desc.MipLevels;
    texture.arraySize = desc.ArraySize;
    texture.sampleCount = desc.SampleDesc.Count;
    texture.format = static_cast<uint32_t>(desc.Format);
    return texture;
}

// What upstream's DepthTransfer_Dx11 changes on the game's context, put back as it was. It is the only step of the
// transport that runs a shader (Dx11Guides::RunsShader; tests/nr-dx11-native-guides holds DT_Dx11.cpp to setting the
// compute shader, shader resources and unordered-access views and nothing else), and the copies change no binding.
// When the depth is a depth-stencil, the output merger is unbound for the duration so that the depth, which may still
// be bound as the depth-stencil view, can be read as a shader resource; its render targets, depth-stencil view and
// unordered-access views are all put back. Append and counter offsets are kept (-1) on both stages.
class ScopedDepthTransferState
{
    ID3D11DeviceContext* _context;
    bool _outputMerger;
    ID3D11ComputeShader* _shader = nullptr;
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> _instances {};
    UINT _instanceCount = D3D11_SHADER_MAX_INTERFACES;
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> _srvs {};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> _uavs {};
    UINT _uavCount = D3D11_PS_CS_UAV_REGISTER_COUNT; // 64 from feature level 11.1, 8 below it
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> _rtvs {};
    ID3D11DepthStencilView* _dsv = nullptr;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> _omUavs {};

    template <typename T, size_t N> static void ReleaseAll(std::array<T*, N>& objects)
    {
        for (auto*& object : objects)
        {
            if (object != nullptr)
                object->Release();
            object = nullptr;
        }
    }

  public:
    ScopedDepthTransferState(ID3D11DeviceContext* context, bool outputMerger)
        : _context(context), _outputMerger(outputMerger)
    {
        ID3D11Device* device = nullptr;
        _context->GetDevice(&device);
        if (device != nullptr)
        {
            if (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
                _uavCount = D3D11_1_UAV_SLOT_COUNT;
            device->Release();
        }

        _context->CSGetShader(&_shader, _instances.data(), &_instanceCount);
        _context->CSGetShaderResources(0, static_cast<UINT>(_srvs.size()), _srvs.data());
        _context->CSGetUnorderedAccessViews(0, _uavCount, _uavs.data());

        if (_outputMerger)
        {
            _context->OMGetRenderTargetsAndUnorderedAccessViews(static_cast<UINT>(_rtvs.size()), _rtvs.data(), &_dsv, 0,
                                                                _uavCount, _omUavs.data());
            _context->OMSetRenderTargets(0, nullptr, nullptr);
        }
    }

    ~ScopedDepthTransferState()
    {
        std::array<UINT, D3D11_1_UAV_SLOT_COUNT> keep;
        keep.fill(static_cast<UINT>(-1));

        _context->CSSetShader(_shader, _instanceCount != 0 ? _instances.data() : nullptr, _instanceCount);
        _context->CSSetShaderResources(0, static_cast<UINT>(_srvs.size()), _srvs.data());
        _context->CSSetUnorderedAccessViews(0, _uavCount, _uavs.data(), keep.data());

        if (_outputMerger)
        {
            // Render targets and unordered-access views share the output merger's slots: the views go after the last
            // bound target, where the game's were (D3D11 refuses them below it).
            UINT targets = 0;
            for (UINT i = 0; i < _rtvs.size(); ++i)
            {
                if (_rtvs[i] != nullptr)
                    targets = i + 1;
            }

            _context->OMSetRenderTargetsAndUnorderedAccessViews(
                targets, _rtvs.data(), _dsv, targets, _uavCount - targets, _omUavs.data() + targets, keep.data());
        }

        if (_shader != nullptr)
            _shader->Release();
        for (UINT i = 0; i < _instanceCount && i < _instances.size(); ++i)
        {
            if (_instances[i] != nullptr)
                _instances[i]->Release();
        }
        ReleaseAll(_srvs);
        ReleaseAll(_uavs);
        ReleaseAll(_rtvs);
        ReleaseAll(_omUavs);
        if (_dsv != nullptr)
            _dsv->Release();
    }

    ScopedDepthTransferState(const ScopedDepthTransferState&) = delete;
    ScopedDepthTransferState& operator=(const ScopedDepthTransferState&) = delete;
};

void ReleaseOwnDepth(OwnDepth& own)
{
    if (own.opened != nullptr)
        own.opened->Release();
    if (own.texture != nullptr)
        own.texture->Release();
    own = {};
}

// The set's own depth copy, for this depth: kept when it fits, else rebuilt. Called only for a set no list reads.
bool EnsureOwnDepth(OwnDepth& own, ID3D11Device* d3d11, ID3D12Device* d3d12, const Dx11Guides::Texture& depth,
                    bool ntHandle)
{
    const uint32_t format = Dx11Guides::OwnCopyFormat(depth.format);
    if (own.opened != nullptr && own.d3d11 == d3d11 && own.width == depth.width && own.height == depth.height &&
        own.format == format && own.ntHandle == ntHandle)
    {
        return true;
    }

    ReleaseOwnDepth(own);

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = depth.width;
    desc.Height = depth.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = static_cast<DXGI_FORMAT>(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags =
        ntHandle ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE : D3D11_RESOURCE_MISC_SHARED;

    if (FAILED(d3d11->CreateTexture2D(&desc, nullptr, &own.texture)) || own.texture == nullptr)
        return false;

    HANDLE handle = nullptr;
    IDXGIResource1* dxgi = nullptr;
    HRESULT hr = own.texture->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
    {
        hr = ntHandle ? dxgi->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &handle)
                      : dxgi->GetSharedHandle(&handle);
        dxgi->Release();
    }

    if (SUCCEEDED(hr) && handle != nullptr)
        hr = d3d12->OpenSharedHandle(handle, IID_PPV_ARGS(&own.opened));

    if (ntHandle && handle != nullptr)
        CloseHandle(handle);

    if (FAILED(hr) || own.opened == nullptr)
    {
        ReleaseOwnDepth(own);
        return false;
    }

    own.d3d11 = d3d11;
    own.width = depth.width;
    own.height = depth.height;
    own.format = format;
    own.ntHandle = ntHandle;
    return true;
}

// Waits, bounded, for the transport's last list. True when it has completed, so its objects may be released. Zero
// only looks: nothing on the game's thread waits for the GPU.
bool DrainLocked(Transport& t, DWORD timeoutMs)
{
    if (t.fence == nullptr || t.fenceValue == 0 || t.fence->GetCompletedValue() >= t.fenceValue)
        return true;

    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    bool done = false;
    if (timeoutMs != 0 && SUCCEEDED(t.fence->SetEventOnCompletion(t.fenceValue, event)))
        done = WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;

    CloseHandle(event);
    return done || t.fence->GetCompletedValue() >= t.fenceValue;
}

// The D3D12 objects, released only once nothing the GPU may still run uses them; abandoned (left alive) otherwise.
void ReleaseD3D12Locked(Transport& t, bool drained)
{
    if (!drained)
    {
        LOG_WARN("DLSS-NR native D3D11 guides: the transport's last list did not complete; its objects are left "
                 "alive rather than freed under the GPU");
        t.allocators = {};
        t.lists = {};
        t.fence = nullptr;
        t.device = nullptr;
        t.queue = nullptr;
        t.cache = {};
        t.ownDepth = {};
        return;
    }

    for (auto& own : t.ownDepth)
        ReleaseOwnDepth(own);

    for (auto*& list : t.lists)
    {
        if (list != nullptr)
            list->Release();
        list = nullptr;
    }
    for (auto*& allocator : t.allocators)
    {
        if (allocator != nullptr)
            allocator->Release();
        allocator = nullptr;
    }
    if (t.fence != nullptr)
        t.fence->Release();
    t.fence = nullptr;

    Dx11WithDx12::ResetUpscalerResourceCache(t.cache);

    if (t.queue != nullptr)
        t.queue->Release();
    if (t.device != nullptr)
        t.device->Release();
    t.queue = nullptr;
    t.device = nullptr;
}

// The lists, allocators and fence of a build that failed part-way. Nothing was ever executed on them.
void ReleaseUnusedD3D12Locked(Transport& t)
{
    for (auto*& list : t.lists)
    {
        if (list != nullptr)
            list->Release();
        list = nullptr;
    }
    for (auto*& allocator : t.allocators)
    {
        if (allocator != nullptr)
            allocator->Release();
        allocator = nullptr;
    }
    if (t.fence != nullptr)
        t.fence->Release();
    t.fence = nullptr;
}

// The transport's D3D12 objects on the bridge's device and queue, rebuilt when those change. The old ones are freed if
// their last list has already completed, and left alive otherwise: the game's thread does not wait for the GPU here.
// A device and queue the build failed on are not tried again until one of them changes.
bool EnsureD3D12Locked(Transport& t, ID3D12Device* device, ID3D12CommandQueue* queue)
{
    if (t.device == device && t.queue == queue && t.fence != nullptr)
        return true;

    if (t.failedDevice == device && t.failedQueue == queue)
        return false;

    if (t.device != nullptr)
        ReleaseD3D12Locked(t, DrainLocked(t, 0));

    t.lastRead = {};
    t.fenceValue = 0;
    t.sequence = 0;

    bool built = SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&t.fence))) && t.fence != nullptr;

    for (size_t i = 0; built && i < kSlots; ++i)
    {
        built =
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&t.allocators[i]))) &&
            t.allocators[i] != nullptr &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, t.allocators[i], nullptr,
                                                IID_PPV_ARGS(&t.lists[i]))) &&
            t.lists[i] != nullptr;

        if (built)
        {
            // Created recording; closed so that every use starts with the same Reset.
            t.lists[i]->Close();
            Dred::Name(t.allocators[i], L"Retoque native D3D11 guides allocator", static_cast<int>(i));
            Dred::Name(t.lists[i], L"Retoque native D3D11 guides list", static_cast<int>(i));
        }
    }

    if (!built)
    {
        ReleaseUnusedD3D12Locked(t);
        t.failedDevice = device;
        t.failedQueue = queue;
        LOG_ERROR("DLSS-NR native D3D11 guides: the transport's fence, allocators or lists could not be created on the "
                  "bridge's device; not tried again until the bridge's device or queue changes");
        return false;
    }

    device->AddRef();
    queue->AddRef();
    t.device = device;
    t.queue = queue;
    t.failedDevice = nullptr;
    t.failedQueue = nullptr;
    return true;
}

// Steps 1 and 2 above, for an evaluate Decide has let through.
Reason CarryLocked(Transport& t, ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params,
                   const Dx11Guides::Inputs& in)
{
    if (!WithDx12::IsInited())
        return Reason::Transport;

    ID3D12Device* device = WithDx12::GetD3D12Device();
    ID3D12CommandQueue* queue = WithDx12::GetD3D12CommandQueue();

    if (device == nullptr || queue == nullptr || !EnsureD3D12Locked(t, device, queue))
        return Reason::D3D12;

    const size_t slot = static_cast<size_t>(t.sequence % kSlots);
    if (!Dx11Guides::SlotFree(t.lastRead[slot], t.fence->GetCompletedValue()))
        return Reason::SlotInFlight;

    ID3D11Device* d3d11Device = nullptr;
    context->GetDevice(&d3d11Device);
    if (d3d11Device == nullptr)
        return Reason::Transport;

    // The same bridge statics dlss_12 uses: the game's device and immediate context, the bridge's D3D12 device and
    // queue. Repeated calls with the same objects change nothing; other ones make every cache stale.
    Dx11WithDx12::Init(d3d11Device, context);

    // A stale cache is released whole by the prepare below, every set at once, so not while any list reads one.
    if (!Dx11Guides::CacheFree(t.cache.Generation, Dx11WithDx12::CacheGeneration, t.fence->GetCompletedValue(),
                               t.fenceValue))
    {
        d3d11Device->Release();
        return Reason::SlotInFlight;
    }

    // A depth-stencil depth crosses as a shader-resource-only copy of the transport's own (Dx11Guides::DepthCopy);
    // anything else through upstream's copy path. Both copies are recorded before upstream's signal, on this context,
    // so the bridge queue's wait for that signal covers them both.
    const auto depthRoute = Dx11Guides::DepthCopy(in.depth, in.ntHandles);
    OwnDepth& own = t.ownDepth[slot];
    auto mask = Dx11WithDx12::ResourceMask::Mv;
    if (depthRoute == Dx11Guides::DepthRoute::Upstream)
        mask |= Dx11WithDx12::ResourceMask::Depth;

    Dx11WithDx12::PrepareResourcesResult prepared;
    bool ownCopied = depthRoute != Dx11Guides::DepthRoute::OwnCopy;
    {
        std::optional<ScopedDepthTransferState> state;
        if (Dx11Guides::RunsShader(in.depth, in.ntHandles))
            state.emplace(context, in.depth.depthStencil);

        if (!ownCopied)
        {
            ID3D11Resource* gameDepth = D3D11Resource(params, NVSDK_NGX_Parameter_Depth);
            if (gameDepth != nullptr && EnsureOwnDepth(own, d3d11Device, device, in.depth, in.ntHandles))
            {
                context->CopySubresourceRegion(own.texture, 0, 0, 0, 0, gameDepth, 0, nullptr);
                ownCopied = true;
            }
        }

        if (ownCopied)
            prepared = Dx11WithDx12::PrepareUpscalerResources(
                t.cache, params, mask, static_cast<UINT>(slot), Dx11WithDx12::NextUpscalerFrameId(),
                Config::Instance()->DontUseNTShared.value_or_default(), false, true);
    }
    d3d11Device->Release();

    if (!ownCopied || !prepared.Success)
        return Reason::Transport;

    const auto& motion = t.cache.Mv[slot];
    ID3D12Resource* depth12 = nullptr;
    ID3D12Device* depthDevice = device;

    if (depthRoute == Dx11Guides::DepthRoute::OwnCopy)
    {
        depth12 = own.opened;
    }
    else
    {
        // Only a copy of ours is safe to read after the game has moved on. A texture the game created shared already
        // is opened as it is, and the game's next frame would write it while the D3D12 copy reads it.
        const auto& depth = t.cache.Depth[slot];
        if (!depth.OwnsSharedTexture && depth.DepthTransfer == nullptr)
            return Reason::AlreadyShared;

        depth12 = depth.Dx12Resource;
        depthDevice = depth.OpenedDx12Device;
    }

    if (!motion.OwnsSharedTexture)
        return Reason::AlreadyShared;

    if (depth12 == nullptr || motion.Dx12Resource == nullptr || depthDevice != device ||
        motion.OpenedDx12Device != device)
    {
        return Reason::Transport;
    }

    ID3D12CommandAllocator* allocator = t.allocators[slot];
    ID3D12GraphicsCommandList* list = t.lists[slot];

    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr)))
        return Reason::D3D12;

    // The shared textures cross between the APIs in COMMON, and are left there.
    const bool captured = CaptureBridgedGuides(list, depth12, motion.Dx12Resource, D3D12_RESOURCE_STATE_COMMON, params,
                                               in.output.width, in.output.height);

    if (FAILED(list->Close()))
    {
        Submission::Abandon(list);
        return Reason::D3D12;
    }

    if (!captured)
    {
        // Closed and never executed: what the capture tracked on it before refusing is dropped with it.
        Submission::Abandon(list);
        return Reason::Ring;
    }

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);

    // Recorded before the Signal, as the bridge does: a failed Signal leaves the set looking in flight for good, which
    // refuses frames rather than handing a set the GPU may still be reading back to D3D11.
    const uint64_t value = ++t.fenceValue;
    t.lastRead[slot] = value;
    ++t.sequence;

    if (FAILED(queue->Signal(t.fence, value)))
    {
        LOG_ERROR("DLSS-NR native D3D11 guides: the transport's fence signal failed; set {} stays in flight", slot);
        return Reason::D3D12;
    }

    return Reason::Carried;
}
} // namespace

void CarryNativeDx11Guides(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params, bool nativeDlss, bool evaluated)
{
    const Config& cfg = *Config::Instance();

    Dx11Guides::Inputs in;
    in.nrEnabled = cfg.DlssNrEnabled.value_or_default();
    in.hookMethod = cfg.DlssNrHookMethod.value_or_default();
    in.nativeDlss = nativeDlss;
    in.presentHost = State::Instance().nrPresentHostActive;

    // No host to carry to: said once, and nothing else looked at.
    if (Dx11Guides::NoHost(in))
    {
        static std::atomic<bool> s_saidNoHost { false };
        if (!s_saidNoHost.exchange(true))
            LOG_INFO("{}", Dx11Guides::kNoHostText);
        return;
    }

    // Nothing is looked at -- not the context, not the parameters -- on any other route.
    if (Dx11Guides::Decide(in) == Reason::NotWanted || context == nullptr || params == nullptr)
        return;

    in.evaluated = evaluated;
    in.immediateContext = context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
    in.depth = Describe(D3D11Resource(params, NVSDK_NGX_Parameter_Depth));
    in.motion = Describe(D3D11Resource(params, NVSDK_NGX_Parameter_MotionVectors));
    in.output = Describe(D3D11Resource(params, NVSDK_NGX_Parameter_Output));

    in.ntHandles = !cfg.DontUseNTShared.value_or_default();

    const unsigned long long presented = BridgePresentedExtent();
    in.presentedWidth = static_cast<uint32_t>(presented >> 32);
    in.presentedHeight = static_cast<uint32_t>(presented & 0xFFFFFFFFu);

    {
        std::lock_guard<std::mutex> create(g_transportCreate);
        if (g_transport == nullptr)
            g_transport = new Transport();
    }

    Transport& t = *g_transport;
    std::lock_guard<std::mutex> lock(t.mutex);

    Reason reason = Dx11Guides::Decide(in);
    if (reason == Reason::Carried)
        reason = CarryLocked(t, context, params, in);

    if (reason == Reason::Carried)
    {
        g_carriedAtMs = NowMs();

        if (!t.reportedFirst)
        {
            t.reportedFirst = true;
            LOG_INFO("DLSS-NR native D3D11 guides: carrying the game's depth ({}) and motion ({}) from its native "
                     "D3D11 DLSS to the present pass through the D3D12 bridge ({} shared handles); output {}x{}",
                     Dx11Guides::TextureText(in.depth), Dx11Guides::TextureText(in.motion),
                     cfg.DontUseNTShared.value_or_default() ? "legacy" : "NT", in.output.width, in.output.height);
        }
    }

    std::string line;
    if (t.window.Note(reason, in, std::chrono::steady_clock::now(), &line))
        LOG_INFO("{}", line);
}

bool NativeDx11GuidesCarriedRecently()
{
    const long long at = g_carriedAtMs.load();
    return at != 0 && NowMs() - at < 1000;
}

void ShutdownNativeDx11Guides()
{
    Transport* t = nullptr;
    {
        std::lock_guard<std::mutex> create(g_transportCreate);
        t = g_transport;
    }

    if (t == nullptr)
        return;

    std::lock_guard<std::mutex> lock(t->mutex);
    if (t->device != nullptr)
        ReleaseD3D12Locked(*t, DrainLocked(*t, 2000));
}
} // namespace DlssNr
