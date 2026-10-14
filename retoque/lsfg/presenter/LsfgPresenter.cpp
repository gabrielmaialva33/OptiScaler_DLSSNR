#include "pch.h"

#include "LsfgPresenter.h"
#include "MfgLatch.h"
#include "MfgSwapChain_Dx12.h"

#include <Config.h>
#include <Logger.h>
#include <proxies/FfxApi_Proxy.h>

#include <atomic>
#include <mutex>
#include <new>
#include <vector>

namespace Lsfg::Presenter
{
namespace
{
// What _swapChainContext holds when the swapchain is ours: FFX's provider keeps the same (InternalFgScContext, a
// swapchain pointer with a reference of its own; ffx_provider_fsr3framegenerationswapchain_dx12.cpp:40-45).
struct Record
{
    LsfgMfg::MfgSwapChainDX12* swapChain = nullptr;
    std::atomic<int> inUse { 0 }; // calls into swapChain in progress (Pin), waited out before its reference goes
};

struct Registry
{
    std::mutex mutex;
    std::vector<Record*> records;
};

// Never destroyed: a static destructor would run under the loader lock at exit, after the driver may have detached.
Registry& R()
{
    static Registry* registry = new Registry();
    return *registry;
}

// -1 undecided, 0 FFX's swapchain, 1 ours. Decided at the first FG swapchain FSR-FG creates.
std::atomic<int> g_latch { -1 };
std::atomic<int> g_mismatchLoggedFor { 0 }; // the Enabled/Multiplier pair last said to mismatch the latch, 0 none
std::atomic<uint32_t> g_lastGenerated { 0 };

int Multiplier()
{
    const int m = Config::Instance()->LsfgMultiplier.value_or_default();
    return LsfgMfg::MultiplierValid(m) ? m : 2;
}

bool WantedNow() { return Config::Instance()->LsfgEnabled.value_or_default() && Multiplier() > 2; }

// A record found and pinned under R().mutex. D3D12_DestroyContext erases the record under the same lock and then waits
// for every pin to go before it drops the record's reference, so a swapchain in use here is never freed. The lock is
// not held while the swapchain is called: a configure may wait for the GPU and the presenter thread, and the
// presenter thread can reach FSR-FG's Deactivate (MenuOverlayDx::CleanupRenderTarget), which comes back here.
class Pin
{
  public:
    explicit Pin(Record* record) : _record(record) {}
    ~Pin()
    {
        if (_record != nullptr)
            _record->inUse.fetch_sub(1, std::memory_order_release);
    }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;

    Record* get() const { return _record; }
    explicit operator bool() const { return _record != nullptr; }

  private:
    Record* _record;
};

// Under R().mutex.
Record* PinIf(Record* record)
{
    if (record != nullptr)
        record->inUse.fetch_add(1, std::memory_order_acquire);
    return record;
}

Record* FindRecord(ffxContext context)
{
    if (context == nullptr)
        return nullptr;
    std::lock_guard lock(R().mutex);
    for (auto* record : R().records)
    {
        if (record == context)
            return PinIf(record);
    }
    return nullptr;
}

Record* FindSwapChain(const void* swapChain)
{
    if (swapChain == nullptr)
        return nullptr;
    std::lock_guard lock(R().mutex);
    for (auto* record : R().records)
    {
        if (static_cast<const void*>(static_cast<IDXGISwapChain4*>(record->swapChain)) == swapChain)
            return PinIf(record);
    }
    return nullptr;
}

// A config that asks for the other swapchain than the latched one: said once per Enabled/Multiplier pair, so 3 then 4
// chosen in the menu with FFX's swapchain are both said.
void NoteLatchMismatch()
{
    const int latch = g_latch.load(std::memory_order_relaxed);
    if (latch < 0)
        return;

    const bool mismatch = (latch == 1) != WantedNow();
    const int pair = 1 + (Config::Instance()->LsfgEnabled.value_or_default() ? 8 : 0) + Multiplier();
    if (mismatch && g_mismatchLoggedFor.exchange(pair, std::memory_order_relaxed) != pair)
    {
        // Ours serves x2 too (N = 1), so only FFX's swapchain makes a choice wait for a restart.
        if (latch == 1)
            LOG_INFO("LSFG presenter: [LSFG] Enabled {}, Multiplier {}: x2 from the next frame through our presenter "
                     "(one generated frame); FFX's own swapchain only from the next start (MFG_PRESENTER.md 1.4)",
                     Config::Instance()->LsfgEnabled.value_or_default(), Multiplier());
        else
            LOG_INFO("LSFG presenter: [LSFG] Enabled {}, Multiplier {} asks for our presenter, but this process runs "
                     "FFX's swapchain (x2); x{} takes effect after a restart (MFG_PRESENTER.md 1.4)",
                     Config::Instance()->LsfgEnabled.value_or_default(), Multiplier(), Multiplier());
    }
    else if (!mismatch)
    {
        g_mismatchLoggedFor.store(0, std::memory_order_relaxed);
    }
}

ffxReturnCode_t CreateForHwnd(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, HWND hwnd,
                              const DXGI_SWAP_CHAIN_DESC1* desc1, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                              IDXGISwapChain4** swapChain, ffxContext* context)
{
    if (factory == nullptr || gameQueue == nullptr || desc1 == nullptr || swapChain == nullptr || context == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;

    IDXGIFactory2* factory2 = nullptr;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
    {
        LOG_ERROR("LSFG presenter: the DXGI factory has no IDXGIFactory2; no frame-generation swapchain");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    auto* sc = new (std::nothrow) LsfgMfg::MfgSwapChainDX12();
    if (sc == nullptr)
    {
        factory2->Release();
        return FFX_API_RETURN_ERROR_MEMORY;
    }

    const HRESULT hr = sc->init(hwnd, desc1, fullscreenDesc, gameQueue, factory2);
    factory2->Release();

    if (FAILED(hr))
    {
        LOG_ERROR("LSFG presenter: swapchain creation failed: {:X}", (UINT) hr);
        sc->Release(); // the only reference: shuts down what init made
        return FFX_API_RETURN_ERROR;
    }

    auto* record = new (std::nothrow) Record();
    if (record == nullptr)
    {
        sc->Release();
        return FFX_API_RETURN_ERROR_MEMORY;
    }

    record->swapChain = sc;
    sc->AddRef(); // the record's reference; the game holds the first

    {
        std::lock_guard lock(R().mutex);
        R().records.push_back(record);
    }

    const uint32_t generated = LsfgMfg::GeneratedFramesFor(Multiplier());
    sc->setGeneratedFrames(generated);
    g_lastGenerated.store(generated, std::memory_order_relaxed);

    *swapChain = sc;
    *context = record;

    LOG_INFO("LSFG presenter: our frame-generation swapchain for window {:X}: {}x{} format {}, {} buffers, flags {:X}; "
             "x{} ({} generated frame(s) per real frame)",
             (size_t) hwnd, desc1->Width, desc1->Height, (UINT) desc1->Format, desc1->BufferCount, desc1->Flags,
             generated + 1, generated);
    return FFX_API_RETURN_OK;
}

// The configuration FFX's FG provider builds for its swapchain from the same descriptor
// (ffx_provider_fsr3framegeneration.cpp:216-436), with the callbacks called directly instead of through FFX's
// per-frame-ID trampolines.
LsfgMfg::FrameGenerationConfig ConfigFrom(const ffxConfigureDescFrameGeneration& desc)
{
    LsfgMfg::FrameGenerationConfig cfg;
    cfg.swapChain = desc.swapChain;
    cfg.presentCallback = desc.presentCallback;
    cfg.presentCallbackContext = desc.presentCallback != nullptr ? desc.presentCallbackUserContext : nullptr;
    cfg.frameGenerationCallback = desc.frameGenerationCallback;
    cfg.frameGenerationCallbackContext =
        desc.frameGenerationCallback != nullptr ? desc.frameGenerationCallbackUserContext : nullptr;
    cfg.frameGenerationEnabled = desc.frameGenerationEnabled;
    cfg.allowAsyncWorkloads = desc.allowAsyncWorkloads;
    cfg.flags = desc.flags;
    cfg.onlyPresentInterpolated =
        desc.onlyPresentGenerated || (desc.flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW) != 0;
    cfg.interpolationRect = desc.generationRect;
    cfg.frameID = desc.frameID;
    cfg.drawDebugPacingLines = (desc.flags & FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES) != 0;
    return cfg;
}

// A swapchain-context descriptor aimed at one of our records (FFX's swapchain provider's Configure,
// ffx_provider_fsr3framegenerationswapchain_dx12.cpp:174-195).
ffxReturnCode_t ConfigureOurs(Record* record, const ffxConfigureDescHeader* desc)
{
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12)
    {
        auto ui = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*>(desc);
        record->swapChain->registerUiResource(ui->uiResource, ui->flags);
        return FFX_API_RETURN_OK;
    }

    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12)
    {
        auto kv = reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainKeyValueDX12*>(desc);
        if (kv->key == 0) // FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_WAITCALLBACK
            record->swapChain->setWaitCallback(reinterpret_cast<LsfgMfg::FfxWaitCallbackFunc>(kv->ptr));
        else if (kv->key == 2) // FFX_API_CONFIGURE_FG_SWAPCHAIN_KEY_FRAMEPACINGTUNING
            record->swapChain->setFramePacingTuning(static_cast<const FfxApiSwapchainFramePacingTuning*>(kv->ptr));
        else
            return FFX_API_RETURN_ERROR_PARAMETER;
        return FFX_API_RETURN_OK;
    }

    return FFX_API_RETURN_ERROR_PARAMETER;
}
} // namespace

bool Wanted()
{
    const bool want = WantedNow();

    int expected = -1;
    if (g_latch.compare_exchange_strong(expected, want ? 1 : 0))
    {
        // With LSFG off nothing is said: FSR-FG users who never enabled it see the log they saw before.
        if (!Config::Instance()->LsfgEnabled.value_or_default())
            return want;

        if (want)
            LOG_INFO("LSFG presenter: [LSFG] Multiplier {}: our frame-generation swapchain (a fork of FFX's, "
                     "N generated frames per real frame) replaces FFX's for this process",
                     Multiplier());
        else
            LOG_INFO("LSFG presenter: [LSFG] Enabled {}, Multiplier {}: FFX's swapchain, unchanged, for this process",
                     Config::Instance()->LsfgEnabled.value_or_default(), Multiplier());
        return want;
    }

    NoteLatchMismatch();
    return expected == 1;
}

bool InUse() { return g_latch.load(std::memory_order_relaxed) == 1; }

LsfgMfg::Latch CurrentLatch()
{
    switch (g_latch.load(std::memory_order_relaxed))
    {
    case 0:
        return LsfgMfg::Latch::Ffx;
    case 1:
        return LsfgMfg::Latch::Ours;
    default:
        return LsfgMfg::Latch::Undecided;
    }
}

ffxReturnCode_t CreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                IDXGISwapChain** swapChain, ffxContext* context)
{
    if (desc == nullptr)
        return FFX_API_RETURN_ERROR_PARAMETER;

    // FFX's ffxCreateFrameinterpolationSwapchainDX12 (FrameInterpolationSwapchainDX12.cpp:489-521).
    DXGI_SWAP_CHAIN_DESC1 desc1 {};
    desc1.Width = desc->BufferDesc.Width;
    desc1.Height = desc->BufferDesc.Height;
    desc1.Format = desc->BufferDesc.Format;
    desc1.SampleDesc = desc->SampleDesc;
    desc1.BufferUsage = desc->BufferUsage;
    desc1.BufferCount = desc->BufferCount;
    desc1.SwapEffect = desc->SwapEffect;
    desc1.Flags = desc->Flags;

    // for clarity, params not part of DXGI_SWAP_CHAIN_DESC
    // implicit behavior of DXGI when you call the IDXGIFactory::CreateSwapChain
    desc1.Scaling = DXGI_SCALING_STRETCH;
    desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Stereo = FALSE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc {};
    fullscreenDesc.Scaling = desc->BufferDesc.Scaling;
    fullscreenDesc.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Windowed = desc->Windowed;

    return CreateForHwnd(factory, gameQueue, desc->OutputWindow, &desc1, &fullscreenDesc,
                         reinterpret_cast<IDXGISwapChain4**>(swapChain), context);
}

ffxReturnCode_t CreateSwapChain1(IDXGIFactory* factory, ID3D12CommandQueue* gameQueue, HWND hwnd,
                                 DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                 IDXGISwapChain1** swapChain, ffxContext* context)
{
    return CreateForHwnd(factory, gameQueue, hwnd, desc, fullscreenDesc, reinterpret_cast<IDXGISwapChain4**>(swapChain),
                         context);
}

ffxReturnCode_t D3D12_Configure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    if (context == nullptr || desc == nullptr)
        return FfxApiProxy::D3D12_Configure(context, desc);

    if (Pin record { FindRecord(*context) })
        return ConfigureOurs(record.get(), desc);

    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION)
    {
        auto fg = reinterpret_cast<const ffxConfigureDescFrameGeneration*>(desc);
        NoteLatchMismatch();

        if (Pin record { FindSwapChain(fg->swapChain) })
        {
            auto* swapChain = record.get()->swapChain;

            // FFX's FG context keeps everything but the swapchain: with this flag its provider skips the ABI probe and
            // the swapchain configure, and still records the callbacks, the enable state, the HUD-less resource and
            // the frame ID its own interpolation checks (ffx_provider_fsr3framegeneration.cpp:241, :440, :566).
            ffxConfigureDescFrameGeneration forFfx = *fg;
            forFfx.flags |= FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;

            const auto result = FfxApiProxy::D3D12_Configure(context, &forFfx.header);
            if (result != FFX_API_RETURN_OK)
                return result;

            // Read at every real frame: 2, 3 and 4 all apply at the next one (MfgLatch.h).
            const uint32_t generated =
                LsfgMfg::GeneratedFramesLive(Config::Instance()->LsfgEnabled.value_or_default(), Multiplier());
            if (g_lastGenerated.exchange(generated, std::memory_order_relaxed) != generated)
                LOG_INFO("LSFG presenter: x{} from frame {} ({} generated frame(s) per real frame)", generated + 1,
                         fg->frameID, generated);

            swapChain->setGeneratedFrames(generated);

            const auto cfg = ConfigFrom(*fg);
            swapChain->setFrameGenerationConfig(&cfg);
            return result;
        }
    }

    return FfxApiProxy::D3D12_Configure(context, desc);
}

ffxReturnCode_t D3D12_DestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)
{
    if (context != nullptr && *context != nullptr)
    {
        Record* record = nullptr;
        {
            std::lock_guard lock(R().mutex);
            auto& records = R().records;
            for (auto it = records.begin(); it != records.end(); ++it)
            {
                if (*it == *context)
                {
                    record = *it;
                    records.erase(it);
                    break;
                }
            }
        }

        if (record != nullptr)
        {
            // Erased above, so no new pin can take it; wait out the calls already in progress.
            while (record->inUse.load(std::memory_order_acquire) != 0)
                Sleep(0);

            // As FFX's provider: release the context's reference. The swapchain shuts down (drains and frees) when
            // the last reference goes, which may be the game's, later.
            const ULONG left = record->swapChain->Release();
            delete record;
            LOG_INFO("LSFG presenter: swapchain context destroyed ({} reference(s) left)", left);
            return FFX_API_RETURN_OK;
        }
    }

    return FfxApiProxy::D3D12_DestroyContext(context, memCb);
}

} // namespace Lsfg::Presenter
