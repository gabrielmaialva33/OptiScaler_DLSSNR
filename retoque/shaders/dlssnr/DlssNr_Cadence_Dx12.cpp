#include "pch.h"

#include "DlssNr_Cadence_Dx12.h"

#include <dlssnr/DlssNr_Cadence.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// The pass's bytecode, when the build carries it. Its absence is a supported state, not an error, and the
// build must not depend on it: without it the cadence reports itself unavailable and the model runs every
// frame. See dlssnr_cadence.hlsl for the command that makes it.
#if __has_include("precompile/DlssNr_Cadence_Shader.h")
#include "precompile/DlssNr_Cadence_Shader.h"
#define DLSSNR_CADENCE_BYTECODE 1
#else
#define DLSSNR_CADENCE_BYTECODE 0
#endif

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

// Orders the writes to a resource that stays in UNORDERED_ACCESS against what reads or writes it next.
void UavBarrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    cmdList->ResourceBarrier(1, &barrier);
}

constexpr auto kRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint32_t kThreads = 8;

// What an unused slot is described as. A null descriptor needs a format and a dimension; these match what
// the shader declares for the slot, so a read through one returns zeros of the right shape.
constexpr DXGI_FORMAT kNullSrv[] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t0 answer
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t1 proxy
    DXGI_FORMAT_R32_FLOAT,          // t2 depth
    DXGI_FORMAT_R32G32_FLOAT,       // t3 motion
    DXGI_FORMAT_R32G32_FLOAT,       // t4 chain
    DXGI_FORMAT_R32_FLOAT,          // t5 depth snapshot
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t6 proxy snapshot
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t7 edit
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t8 coarse edit
    DXGI_FORMAT_R8G8B8A8_UNORM,     // t9 UI alpha
    DXGI_FORMAT_R16G16B16A16_FLOAT, // t10 fade
};
constexpr DXGI_FORMAT kNullUav[] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u0
    DXGI_FORMAT_R16G16B16A16_FLOAT, // u1 proxy snapshot
    DXGI_FORMAT_R32_FLOAT,          // u2 depth snapshot
};
} // namespace

static_assert(sizeof(kNullSrv) / sizeof(kNullSrv[0]) == 11 && sizeof(kNullUav) / sizeof(kNullUav[0]) == 3,
              "one null format per slot");

bool DlssNr_Cadence_Dx12::Available() { return DLSSNR_CADENCE_BYTECODE != 0; }

// The cadence bytecode committed before smoothing, by its DXBC container digest (bytes 4..19 of the .cso). It runs
// modes 0 to 4 only, so with it smoothing stands down -- the carry runs as it did -- instead of leaving every smoothed
// carried frame unwritten. tests/nr-invariants warns while that binary is committed; drop this when it is rebuilt.
bool DlssNr_Cadence_Dx12::SmoothingAvailable()
{
#if DLSSNR_CADENCE_BYTECODE
    static constexpr unsigned char kPreSmoothing[16] = { 0x54, 0x53, 0x89, 0x09, 0x82, 0x42, 0x9a, 0x4a,
                                                         0xaf, 0xe2, 0x76, 0xf6, 0xa6, 0xcb, 0xe6, 0x6d };
    return sizeof(DlssNr_Cadence_cso) > 20 && std::memcmp(DlssNr_Cadence_cso + 4, kPreSmoothing, 16) != 0;
#else
    return false;
#endif
}

ID3D12Resource* DlssNr_Cadence_Dx12::CreateTexture(ID3D12Device* device, DXGI_FORMAT format, UINT width, UINT height,
                                                   const wchar_t* name, D3D12_RESOURCE_STATES state)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&resource))) ||
        resource == nullptr)
    {
        return nullptr;
    }

    resource->SetName(name);
    return resource;
}

DlssNr_Cadence_Dx12::DlssNr_Cadence_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight,
                                         bool InSmoothing)
    : Shader_Dx12(InName, InDevice), _smoothingAsked(InSmoothing), _width(InWidth), _height(InHeight),
      _lowWidth(DlssNr::Cadence::LowExtent(InWidth)), _lowHeight(DlssNr::Cadence::LowExtent(InHeight))
{
    if (InDevice == nullptr || !Available() || InWidth == 0 || InHeight == 0)
        return;

    // Eleven inputs, three outputs, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    // One upload buffer per slot, mapped for life: a single shared buffer is rewritten by the next dispatch's
    // record while the GPU may still be reading this one's.
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DLSSNR_CADENCE_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));
        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        result = _constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i]);
        if (FAILED(result) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{0}] Map constants error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

#if DLSSNR_CADENCE_BYTECODE
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_Cadence_cso, sizeof(DlssNr_Cadence_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_CADENCE_NUM_OF_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_CADENCE_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Constants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _residual = CreateTexture(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, _width, _height, L"DLSS-NR cadence edit");
    _proxyThen = CreateTexture(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, _width, _height, L"DLSS-NR cadence proxy");
    _depthThen = CreateTexture(InDevice, DXGI_FORMAT_R32_FLOAT, _width, _height, L"DLSS-NR cadence depth");
    _residualLow =
        CreateTexture(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, _lowWidth, _lowHeight, L"DLSS-NR cadence coarse edit");
    // 32-bit float: displacements in pixels reach the thousands, where 16-bit float has lost the sub-pixel.
    _chain[0] = CreateTexture(InDevice, DXGI_FORMAT_R32G32_FLOAT, _width, _height, L"DLSS-NR cadence chain A");
    _chain[1] = CreateTexture(InDevice, DXGI_FORMAT_R32G32_FLOAT, _width, _height, L"DLSS-NR cadence chain B");

    if (_residual == nullptr || _proxyThen == nullptr || _depthThen == nullptr || _residualLow == nullptr ||
        _chain[0] == nullptr || _chain[1] == nullptr)
    {
        LOG_WARN("[{}] Could not allocate the cadence surfaces for {}x{}", _name, _width, _height);
        return;
    }

    // Smoothing (docs/dlssnr/design/cadence-smoothing.md): the fade, the shown answer, and the light limiter's grid and
    // state. The state is read and written in place by one thread, so it lives in UNORDERED_ACCESS. If they cannot be
    // had, the carry still runs, unsmoothed: refusing the whole cadence over them would cost more than it saves.
    if (_smoothingAsked)
    {
        _fade = CreateTexture(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, _width, _height, L"DLSS-NR cadence fade");
        _shown = CreateTexture(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, _width, _height, L"DLSS-NR cadence shown");
        _lightGrid = CreateTexture(InDevice, DXGI_FORMAT_R32G32_FLOAT, DlssNr::Cadence::kLightGrid,
                                   DlssNr::Cadence::kLightGrid, L"DLSS-NR cadence light grid");
        _lightState = CreateTexture(InDevice, DXGI_FORMAT_R32_FLOAT, 4, 1, L"DLSS-NR cadence light state", kWrite);

        _smoothing = _fade != nullptr && _shown != nullptr && _lightGrid != nullptr && _lightState != nullptr;
        if (!_smoothing)
        {
            LOG_WARN("[{}] Could not allocate the cadence smoothing surfaces for {}x{}; carrying without smoothing",
                     _name, _width, _height);
            SAFE_RELEASE(_fade);
            SAFE_RELEASE(_shown);
            SAFE_RELEASE(_lightGrid);
            SAFE_RELEASE(_lightState);
        }
    }

    const double megabytes =
        (double) _width * _height * (8 + 8 + 4 + 8 + 8 + (_smoothing ? 16 : 0)) / (1024.0 * 1024.0);
    LOG_INFO("DLSS-NR cadence: surfaces built for {}x{}{} (about {:.0f} MB)", _width, _height,
             _smoothing ? ", with smoothing" : "", megabytes);
    _init = true;
}

DlssNr_Cadence_Dx12::~DlssNr_Cadence_Dx12()
{
    // Only ever deleted from the retired list, once the recordings that used it are done, or at a shutdown that
    // has already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    SAFE_RELEASE(_residual);
    SAFE_RELEASE(_proxyThen);
    SAFE_RELEASE(_depthThen);
    SAFE_RELEASE(_residualLow);
    for (auto& chain : _chain)
        SAFE_RELEASE(chain);
    SAFE_RELEASE(_fade);
    SAFE_RELEASE(_shown);
    SAFE_RELEASE(_lightGrid);
    SAFE_RELEASE(_lightState);

    for (uint32_t i = 0; i < DLSSNR_CADENCE_NUM_OF_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

DlssNr_Cadence_Dx12::Constants DlssNr_Cadence_Dx12::Base(uint32_t mode, const Frame& frame) const
{
    Constants c {};
    c.Mode = mode;
    c.Width = _width;
    c.Height = _height;
    c.LowWidth = _lowWidth;
    c.LowHeight = _lowHeight;
    c.LowBlock = DlssNr::Cadence::kLowBlock;
    c.DepthBaseX = frame.depthRegion.baseX;
    c.DepthBaseY = frame.depthRegion.baseY;
    c.DepthWidth = std::max(frame.depthRegion.width, 1u);
    c.DepthHeight = std::max(frame.depthRegion.height, 1u);
    c.MotionBaseX = frame.motionRegion.baseX;
    c.MotionBaseY = frame.motionRegion.baseY;
    c.MotionWidth = std::max(frame.motionRegion.width, 1u);
    c.MotionHeight = std::max(frame.motionRegion.height, 1u);
    c.MvToWorkX = std::isfinite(frame.mvToWorkX) ? frame.mvToWorkX : 0.0f;
    c.MvToWorkY = std::isfinite(frame.mvToWorkY) ? frame.mvToWorkY : 0.0f;
    c.DepthTol = DlssNr::Cadence::kDepthTolerance;
    c.ColourTol = DlssNr::Cadence::kColourTolerance;
    c.Scale = std::isfinite(frame.scale) && frame.scale > 0.0f ? frame.scale : 1.0f;
    // The snapshot is point-sampled from the guide, so a guide coarser than the working size repeats each of its
    // texels; the best-of-four depth test steps by one guide texel, not one working pixel.
    c.DepthStepX = std::max(1.0f, (float) _width / (float) c.DepthWidth);
    c.DepthStepY = std::max(1.0f, (float) _height / (float) c.DepthHeight);
    return c;
}

bool DlssNr_Cadence_Dx12::Run(ID3D12GraphicsCommandList* cmdList, const Constants& constants,
                              ID3D12Resource* const (&srvs)[kSrvCount], ID3D12Resource* const (&uavs)[kUavCount],
                              UINT groupsX, UINT groupsY)
{
    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_CADENCE_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    // Every slot gets a view, a null one where the mode reads nothing: an unbound descriptor is not an empty
    // read, it is a read from nothing.
    for (uint32_t i = 0; i < kSrvCount; ++i)
    {
        if (srvs[i] != nullptr)
        {
            CreateShaderResourceView(_device, srvs[i], heap.GetSrvCPU(i), DXGI_FORMAT_UNKNOWN, true);
            continue;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC empty {};
        empty.Format = kNullSrv[i];
        empty.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        empty.Texture2D.MipLevels = 1;
        _device->CreateShaderResourceView(nullptr, &empty, heap.GetSrvCPU(i));
    }

    for (uint32_t i = 0; i < kUavCount; ++i)
    {
        if (uavs[i] != nullptr)
        {
            CreateUnorderedAccessView(_device, uavs[i], heap.GetUavCPU(i), 0, true);
            continue;
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC empty {};
        empty.Format = kNullUav[i];
        empty.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        _device->CreateUnorderedAccessView(nullptr, nullptr, &empty, heap.GetUavCPU(i));
    }

    std::memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch(groupsX, groupsY, 1);
    return true;
}

bool DlssNr_Cadence_Dx12::Record(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InAnswer)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InAnswer == nullptr || InFrame.proxy == nullptr ||
        InFrame.depth == nullptr)
    {
        return false;
    }

    Transition(InCmdList, _residual, kRest, kWrite);
    Transition(InCmdList, _proxyThen, kRest, kWrite);
    Transition(InCmdList, _depthThen, kRest, kWrite);
    {
        ID3D12Resource* const srvs[kSrvCount] = { InAnswer, InFrame.proxy, InFrame.depth };
        ID3D12Resource* const uavs[kUavCount] = { _residual, _proxyThen, _depthThen };
        Run(InCmdList, Base(0, InFrame), srvs, uavs, (_width + kThreads - 1) / kThreads,
            (_height + kThreads - 1) / kThreads);
    }
    // The transitions double as the wait for the writes: the coarse pass reads the edit next.
    Transition(InCmdList, _residual, kWrite, kRest);
    Transition(InCmdList, _proxyThen, kWrite, kRest);
    Transition(InCmdList, _depthThen, kWrite, kRest);

    Transition(InCmdList, _residualLow, kRest, kWrite);
    {
        ID3D12Resource* const srvs[kSrvCount] = { nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, nullptr, _residual };
        ID3D12Resource* const uavs[kUavCount] = { _residualLow };
        Run(InCmdList, Base(1, InFrame), srvs, uavs, (_lowWidth + kThreads - 1) / kThreads,
            (_lowHeight + kThreads - 1) / kThreads);
    }
    Transition(InCmdList, _residualLow, kWrite, kRest);
    return true;
}

bool DlssNr_Cadence_Dx12::Carry(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, bool InChainStart,
                                ID3D12Resource* InUiAlpha, const Smooth& InSmooth, ID3D12Resource* InOut)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InOut == nullptr || InFrame.proxy == nullptr ||
        InFrame.depth == nullptr || InFrame.motion == nullptr)
    {
        return false;
    }

    // One link: the chain as of the last carried frame (none on the first) extended by this frame's vectors.
    ID3D12Resource* const previous = _chain[_current];
    ID3D12Resource* const next = _chain[_current ^ 1];
    Transition(InCmdList, next, kRest, kWrite);
    {
        Constants c = Base(2, InFrame);
        c.ChainStart = InChainStart ? 1u : 0u;
        ID3D12Resource* const srvs[kSrvCount] = {
            nullptr,    InFrame.proxy, InFrame.depth, InFrame.motion, InChainStart ? nullptr : previous,
            _depthThen, _proxyThen
        };
        ID3D12Resource* const uavs[kUavCount] = { next };
        Run(InCmdList, c, srvs, uavs, (_width + kThreads - 1) / kThreads, (_height + kThreads - 1) / kThreads);
    }
    Transition(InCmdList, next, kWrite, kRest);
    _current ^= 1;

    // Smoothing: the walk's share of the stored fade, and whether the answer goes through the light limiter first.
    // Without smoothing surfaces both are off, and the dispatch below is the one it always was.
    const bool limited = _smoothing && InSmooth.limit;
    const float fadeWeight = _smoothing && std::isfinite(InSmooth.fadeWeight) && InSmooth.fadeWeight > 0.0f
                                 ? std::min(InSmooth.fadeWeight, 1.0f)
                                 : 0.0f;
    ID3D12Resource* const answer = limited ? _shown : InOut;
    ID3D12Resource* const fade = fadeWeight > 0.0f ? _fade : nullptr;

    // The answer the model would have written: the proxy plus the edit moved along the chain just made.
    if (limited)
        Transition(InCmdList, _shown, kRest, kWrite);
    {
        Constants c = Base(3, InFrame);
        c.HaveUi = InUiAlpha != nullptr ? 1u : 0u;
        c.FadeWeight = fadeWeight;
        ID3D12Resource* const srvs[kSrvCount] = { nullptr,    InFrame.proxy, InFrame.depth, nullptr,   next, _depthThen,
                                                  _proxyThen, _residual,     _residualLow,  InUiAlpha, fade };
        ID3D12Resource* const uavs[kUavCount] = { answer };
        Run(InCmdList, c, srvs, uavs, (_width + kThreads - 1) / kThreads, (_height + kThreads - 1) / kThreads);
    }

    if (limited)
    {
        Transition(InCmdList, _shown, kWrite, kRest);
        return Show(InCmdList, InFrame, InUiAlpha, InSmooth, InOut);
    }

    // The caller moves InOut out of UNORDERED_ACCESS for the resolve, which orders the write; this orders it
    // against anything that reads it as a UAV first.
    D3D12_RESOURCE_BARRIER written {};
    written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written.UAV.pResource = InOut;
    InCmdList->ResourceBarrier(1, &written);
    return true;
}

ID3D12Resource* DlssNr_Cadence_Dx12::ModelMotion(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InFrame.proxy == nullptr || InFrame.depth == nullptr ||
        InFrame.motion == nullptr)
    {
        return nullptr;
    }

    // The last link -- this frame's vectors onto the last carried frame's chain -- validated, and off the picture
    // where it does not end on this pixel's surface. Not swapped in: the next carried frame starts a new chain.
    ID3D12Resource* const previous = _chain[_current];
    ID3D12Resource* const handed = _chain[_current ^ 1];
    Transition(InCmdList, handed, kRest, kWrite);
    {
        ID3D12Resource* const srvs[kSrvCount] = { nullptr,  InFrame.proxy, InFrame.depth, InFrame.motion,
                                                  previous, _depthThen,    _proxyThen };
        ID3D12Resource* const uavs[kUavCount] = { handed };
        Run(InCmdList, Base(4, InFrame), srvs, uavs, (_width + kThreads - 1) / kThreads,
            (_height + kThreads - 1) / kThreads);
    }
    Transition(InCmdList, handed, kWrite, kRest);
    return handed;
}

bool DlssNr_Cadence_Dx12::Fade(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InUiAlpha,
                               ID3D12Resource* InAnswer, const Smooth& InSmooth)
{
    if (!_init || !_smoothing || InCmdList == nullptr || _device == nullptr || InAnswer == nullptr ||
        InFrame.proxy == nullptr || InFrame.depth == nullptr || InFrame.motion == nullptr)
    {
        return false;
    }

    // The last link from the chain the last carried frame left, exactly as ModelMotion computed it for the model's
    // vectors; the carried answer it gives is what a carried frame would have shown. The stored edit is still the
    // previous model frame's: Record overwrites it after this.
    Transition(InCmdList, _shown, kRest, kWrite);
    Transition(InCmdList, _fade, kRest, kWrite);
    {
        Constants c = Base(5, InFrame);
        c.HaveUi = InUiAlpha != nullptr ? 1u : 0u;
        c.FadeWeight = std::isfinite(InSmooth.fadeWeight) ? std::clamp(InSmooth.fadeWeight, 0.0f, 1.0f) : 0.0f;
        ID3D12Resource* const srvs[kSrvCount] = { InAnswer,         InFrame.proxy, InFrame.depth, InFrame.motion,
                                                  _chain[_current], _depthThen,    _proxyThen,    _residual,
                                                  _residualLow,     InUiAlpha };
        ID3D12Resource* const uavs[kUavCount] = { _shown, _fade };
        Run(InCmdList, c, srvs, uavs, (_width + kThreads - 1) / kThreads, (_height + kThreads - 1) / kThreads);
    }
    Transition(InCmdList, _shown, kWrite, kRest);
    Transition(InCmdList, _fade, kWrite, kRest);
    return true;
}

bool DlssNr_Cadence_Dx12::ShowFaded(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame,
                                    ID3D12Resource* InUiAlpha, const Smooth& InSmooth, ID3D12Resource* InOut)
{
    return Show(InCmdList, InFrame, InUiAlpha, InSmooth, InOut);
}

bool DlssNr_Cadence_Dx12::Show(ID3D12GraphicsCommandList* InCmdList, const Frame& InFrame, ID3D12Resource* InUiAlpha,
                               const Smooth& InSmooth, ID3D12Resource* InOut)
{
    if (!_init || !_smoothing || InCmdList == nullptr || _device == nullptr || InOut == nullptr ||
        InFrame.proxy == nullptr)
    {
        return false;
    }

    const uint32_t passthrough = InSmooth.passthrough ? 1u : 0u;
    const uint32_t haveUi = InUiAlpha != nullptr ? 1u : 0u;

    // The light of what would be shown and of the proxy, summed per tile, the interface left out.
    Transition(InCmdList, _lightGrid, kRest, kWrite);
    {
        Constants c = Base(6, InFrame);
        c.Passthrough = passthrough;
        c.HaveUi = haveUi;
        ID3D12Resource* const srvs[kSrvCount] = { _shown,  InFrame.proxy, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr,       nullptr, nullptr, InUiAlpha };
        ID3D12Resource* const uavs[kUavCount] = { _lightGrid };
        Run(InCmdList, c, srvs, uavs, DlssNr::Cadence::kLightGrid, DlssNr::Cadence::kLightGrid);
    }
    Transition(InCmdList, _lightGrid, kWrite, kRest);

    // One group: the edit's light held to the last limited frame's, and the gain that does it, in the state. The
    // barrier before orders this write after the last frame's read, should both land in one command list.
    UavBarrier(InCmdList, _lightState);
    {
        Constants c = Base(7, InFrame);
        c.LimitValid = InSmooth.limit && !InSmooth.limitReset ? 1u : 0u;
        c.LimitStep = std::isfinite(InSmooth.limitStep) && InSmooth.limitStep > 0.0f ? InSmooth.limitStep : 0.0f;
        c.LimitInputTol = DlssNr::Cadence::kLightInputStops;
        c.LimitMaxGain = DlssNr::Cadence::kLightMaxGainStops;
        ID3D12Resource* const srvs[kSrvCount] = { _lightGrid };
        ID3D12Resource* const uavs[kUavCount] = { nullptr, nullptr, _lightState };
        Run(InCmdList, c, srvs, uavs, 1, 1);
    }
    UavBarrier(InCmdList, _lightState);

    // The shown answer times the gain, into the surface the resolve reads; the interface keeps its answer as it is.
    {
        Constants c = Base(8, InFrame);
        c.Passthrough = passthrough;
        c.HaveUi = haveUi;
        ID3D12Resource* const srvs[kSrvCount] = { _shown,  nullptr, nullptr, nullptr, nullptr,
                                                  nullptr, nullptr, nullptr, nullptr, InUiAlpha };
        ID3D12Resource* const uavs[kUavCount] = { InOut, nullptr, _lightState };
        Run(InCmdList, c, srvs, uavs, (_width + kThreads - 1) / kThreads, (_height + kThreads - 1) / kThreads);
    }

    // As Carry: the caller's transition orders the write for the resolve; this orders it against a UAV reader.
    UavBarrier(InCmdList, InOut);
    return true;
}
