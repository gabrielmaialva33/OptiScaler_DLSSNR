#include "pch.h"

#include "DlssNr_UiMask_Dx12.h"

#include "precompile/DlssNr_UiMask_Shader.h"

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

// The detector's thresholds, in the luma of the model's proxy (0..1, paper-white relative). Fixed rather
// than configurable: one quantity, one control, and the control is the key itself. hud-protection.md has
// what each one does, and why the second set exists (the rim a moving silhouette left on still scenery).
constexpr float kStaticEps = 0.008f; // about two steps of an 8-bit frame: still
constexpr float kCoreEps = 0.012f;   // the same for every pixel within 2 px, with a little room for noise
constexpr float kMotionTau = 0.02f;  // a side's sample moved
constexpr float kDetailMin = 0.15f;  // a hard edge, as glyphs and HUD lines have; sand grains mostly don't
constexpr float kDecay = 0.985f;     // about a second and a half at 60 fps
constexpr float kDropTau = 0.2f;     // the pixel itself changed completely
constexpr uint32_t kStreakMin = 8;   // frames in a row as a candidate before a new pixel is protected
constexpr float kSupportMin = 3.0f;  // protected neighbours in the 5x5 for a pixel to be exported
constexpr float kSidesMin = 4.0f;    // right, left, down and up must all see the scene move
} // namespace

ID3D12Resource* DlssNr_UiMask_Dx12::CreateTexture(ID3D12Device* device, DXGI_FORMAT format, const wchar_t* name)
{
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc =
        CD3DX12_RESOURCE_DESC::Tex2D(format, _width, _height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ID3D12Resource* resource = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&resource))) ||
        resource == nullptr)
    {
        return nullptr;
    }

    resource->SetName(name);
    return resource;
}

DlssNr_UiMask_Dx12::DlssNr_UiMask_Dx12(std::string InName, ID3D12Device* InDevice, UINT InWidth, UINT InHeight)
    : Shader_Dx12(InName, InDevice), _width(InWidth), _height(InHeight)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    if (InWidth == 0 || InHeight == 0)
    {
        LOG_WARN("[{}] Empty size {}x{}", _name, InWidth, InHeight);
        return;
    }

    // Ours, not the game's: the exposure scan must not take these for a candidate.
    ScopedInternalResourceCreation internalResources {};

    // Three inputs, three outputs, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(InDevice, 3, 3, 1))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    // One upload buffer per slot, mapped for life: a single shared buffer is rewritten by the next frame's
    // record while the GPU may still be reading this one's.
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DLSSNR_UIMASK_NUM_OF_HEAPS; ++i)
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

    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_UiMask_cso, sizeof(DlssNr_UiMask_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_UIMASK_NUM_OF_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_UIMASK_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Constants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _luma[0] = CreateTexture(InDevice, DXGI_FORMAT_R16_FLOAT, L"DLSS-NR UI mask luma A");
    _luma[1] = CreateTexture(InDevice, DXGI_FORMAT_R16_FLOAT, L"DLSS-NR UI mask luma B");
    _acc[0] = CreateTexture(InDevice, DXGI_FORMAT_R16G16_FLOAT, L"DLSS-NR UI mask state A");
    _acc[1] = CreateTexture(InDevice, DXGI_FORMAT_R16G16_FLOAT, L"DLSS-NR UI mask state B");
    _mask = CreateTexture(InDevice, DXGI_FORMAT_R8G8B8A8_UNORM, L"DLSS-NR UI mask");

    if (_luma[0] == nullptr || _luma[1] == nullptr || _acc[0] == nullptr || _acc[1] == nullptr || _mask == nullptr)
    {
        LOG_WARN("[{}] Could not allocate the mask for {}x{}", _name, _width, _height);
        return;
    }

    LOG_INFO("DLSS-NR UI protection: static-overlay mask built for {}x{}", _width, _height);
    _init = true;
}

DlssNr_UiMask_Dx12::~DlssNr_UiMask_Dx12()
{
    // Only ever deleted from the retired list, once the recordings that used it are done, or at a shutdown
    // that has already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    for (auto& texture : _luma)
        SAFE_RELEASE(texture);
    for (auto& texture : _acc)
        SAFE_RELEASE(texture);
    SAFE_RELEASE(_mask);

    for (uint32_t i = 0; i < DLSSNR_UIMASK_NUM_OF_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

ID3D12Resource* DlssNr_UiMask_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InInput)
{
    if (!_init || InCmdList == nullptr || InInput == nullptr || _device == nullptr)
        return nullptr;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_UIMASK_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    ID3D12Resource* const lumaPrev = _luma[_current];
    ID3D12Resource* const lumaOut = _luma[_current ^ 1];
    ID3D12Resource* const accPrev = _acc[_current];
    ID3D12Resource* const accOut = _acc[_current ^ 1];

    Transition(InCmdList, lumaOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(InCmdList, accOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(InCmdList, _mask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CreateShaderResourceView(_device, InInput, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, lumaPrev, heap.GetSrvCPU(1));
    CreateShaderResourceView(_device, accPrev, heap.GetSrvCPU(2));
    CreateUnorderedAccessView(_device, lumaOut, heap.GetUavCPU(0), 0);
    CreateUnorderedAccessView(_device, accOut, heap.GetUavCPU(1), 0);
    CreateUnorderedAccessView(_device, _mask, heap.GetUavCPU(2), 0);

    Constants constants {};
    constants.Width = _width;
    constants.Height = _height;
    constants.Valid = _valid ? 1u : 0u;
    constants.StreakMin = kStreakMin;
    constants.StaticEps = kStaticEps;
    constants.MotionTau = kMotionTau;
    constants.DetailMin = kDetailMin;
    constants.Decay = kDecay;
    constants.DropTau = kDropTau;
    constants.CoreEps = kCoreEps;
    constants.SupportMin = kSupportMin;
    constants.SidesMin = kSidesMin;
    memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);

    Transition(InCmdList, lumaOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(InCmdList, accOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(InCmdList, _mask, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _current ^= 1;
    _valid = true;
    return _mask;
}
