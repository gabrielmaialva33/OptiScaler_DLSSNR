#include "pch.h"

#include "DlssNr_Stabilizer_Dx12.h"

#include "precompile/DlssNr_Stabilizer_Shader.h"

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

// One unit of the output format: the smallest step a store keeps. A UNORM output quantises in absolute
// terms, a float one relative to the value's own size (its mantissa). R11G11B10 has six mantissa bits
// in red and green and five in blue. 32-bit float and anything unlisted get zero, which is the plain
// creep with no minimum -- exact enough on the first, and what Feeder always did on the rest.
void OutputUnit(DXGI_FORMAT format, float& quantAbs, float (&quantRel)[3])
{
    quantAbs = 0.0f;
    quantRel[0] = quantRel[1] = quantRel[2] = 0.0f;

    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        quantAbs = 1.0f / 255.0f;
        break;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        quantAbs = 1.0f / 1023.0f;
        break;

    case DXGI_FORMAT_R16G16B16A16_UNORM:
        quantAbs = 1.0f / 65535.0f;
        break;

    case DXGI_FORMAT_R11G11B10_FLOAT:
        quantRel[0] = quantRel[1] = 1.0f / 64.0f;
        quantRel[2] = 1.0f / 32.0f;
        break;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        quantRel[0] = quantRel[1] = quantRel[2] = 1.0f / 1024.0f;
        break;

    default:
        break;
    }
}
} // namespace

ID3D12Resource* DlssNr_Stabilizer_Dx12::CreateHistory(ID3D12Device* device, DXGI_FORMAT format, const wchar_t* name)
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

DlssNr_Stabilizer_Dx12::DlssNr_Stabilizer_Dx12(std::string InName, ID3D12Device* InDevice,
                                               const D3D12_RESOURCE_DESC& InOutput)
    : Shader_Dx12(InName, InDevice), _width(InOutput.Width), _height(InOutput.Height), _format(InOutput.Format),
      _mipLevels(InOutput.MipLevels), _arraySize(InOutput.DepthOrArraySize), _sampleCount(InOutput.SampleDesc.Count)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    // The stabilized picture goes back with CopyResource, which copies every subresource. The history is
    // one plain 2D surface, so an output with mips, slices or samples cannot take it back.
    if (InOutput.MipLevels != 1 || InOutput.DepthOrArraySize != 1 || InOutput.SampleDesc.Count != 1)
    {
        LOG_WARN("[{}] Output has {} mips, {} slices, {} samples; the stabilizer only takes one plain surface", _name,
                 InOutput.MipLevels, InOutput.DepthOrArraySize, InOutput.SampleDesc.Count);
        return;
    }

    // Four inputs, two outputs, one constant buffer, and a clamped linear sampler for the input box,
    // which need not be the output's size.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, 4, 2, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    // One upload buffer per slot, mapped for life, for the reason DlssNr_Dx12 gives: a single shared
    // buffer is rewritten by the next frame's record while the GPU may still be reading this one's.
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DLSSNR_STABILIZER_NUM_OF_HEAPS; ++i)
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

    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_Stabilizer_cso, sizeof(DlssNr_Stabilizer_cso),
                               nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    if (!InitHeaps(InDevice, _frameHeaps, DLSSNR_STABILIZER_NUM_OF_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_STABILIZER_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Constants);
        InDevice->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    // The anchor is private, so its format is ours to pick: wide enough that a slow drift still moves it.
    _shown[0] = CreateHistory(InDevice, _format, L"DLSS-NR stabilizer shown A");
    _shown[1] = CreateHistory(InDevice, _format, L"DLSS-NR stabilizer shown B");
    _anchor[0] = CreateHistory(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS-NR stabilizer anchor A");
    _anchor[1] = CreateHistory(InDevice, DXGI_FORMAT_R16G16B16A16_FLOAT, L"DLSS-NR stabilizer anchor B");

    if (_shown[0] == nullptr || _shown[1] == nullptr || _anchor[0] == nullptr || _anchor[1] == nullptr)
    {
        LOG_WARN("[{}] Could not allocate history for {}x{} format {}", _name, _width, _height, (int) _format);
        return;
    }

    OutputUnit(_format, _quantAbs, _quantRel);

    LOG_INFO("DLSS-NR stabilizer: history built for {}x{}, format {}", _width, _height, (int) _format);
    _init = true;
}

DlssNr_Stabilizer_Dx12::~DlssNr_Stabilizer_Dx12()
{
    // Only ever deleted from the retired list, once the recordings that used it are done, or at a
    // shutdown that has already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    for (auto& texture : _shown)
        SAFE_RELEASE(texture);
    for (auto& texture : _anchor)
        SAFE_RELEASE(texture);

    for (uint32_t i = 0; i < DLSSNR_STABILIZER_NUM_OF_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

bool DlssNr_Stabilizer_Dx12::Fits(ID3D12Device* InDevice, const D3D12_RESOURCE_DESC& InOutput) const
{
    // A failed object still fits the output it failed on, so it is not rebuilt, and logged again, every
    // frame; it reports its reason once, from the constructor, and then does nothing.
    return InDevice == _device && InOutput.Width == _width && InOutput.Height == _height &&
           InOutput.Format == _format && InOutput.MipLevels == _mipLevels && InOutput.DepthOrArraySize == _arraySize &&
           InOutput.SampleDesc.Count == _sampleCount;
}

bool DlssNr_Stabilizer_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InInput,
                                      ID3D12Resource* InOutput, float InStrength, float InTolerance, float InWhitePoint,
                                      bool InPassthrough)
{
    if (!_init || InCmdList == nullptr || InInput == nullptr || InOutput == nullptr || _device == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_STABILIZER_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    ID3D12Resource* const shownPrev = _shown[_current];
    ID3D12Resource* const shownOut = _shown[_current ^ 1];
    ID3D12Resource* const anchorPrev = _anchor[_current];
    ID3D12Resource* const anchorOut = _anchor[_current ^ 1];

    // The model's answer is read as a shader resource: typed UAV loads are optional for some formats
    // the output can have, SRV loads are not.
    Transition(InCmdList, InOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(InCmdList, shownOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(InCmdList, anchorOut, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    CreateShaderResourceView(_device, InInput, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, anchorPrev, heap.GetSrvCPU(1));
    CreateShaderResourceView(_device, shownPrev, heap.GetSrvCPU(2));
    CreateShaderResourceView(_device, InOutput, heap.GetSrvCPU(3));
    CreateUnorderedAccessView(_device, shownOut, heap.GetUavCPU(0), 0);
    CreateUnorderedAccessView(_device, anchorOut, heap.GetUavCPU(1), 0);

    Constants constants {};
    constants.Strength = InStrength;
    constants.Tolerance = InTolerance;
    constants.Width = static_cast<uint32_t>(_width);
    constants.Height = _height;
    constants.Valid = _valid ? 1u : 0u;
    constants.QuantAbs = _quantAbs;
    constants.QuantRelR = _quantRel[0];
    constants.QuantRelG = _quantRel[1];
    constants.QuantRelB = _quantRel[2];
    constants.WhitePoint = std::isfinite(InWhitePoint) && InWhitePoint > 1e-6f ? InWhitePoint : 1.0f;
    constants.Passthrough = InPassthrough ? 1u : 0u;
    memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    InCmdList->Dispatch(static_cast<UINT>((_width + 7) / 8), (_height + 7) / 8, 1);

    // The stabilized picture goes back into the output in place, and everything rests where it began.
    Transition(InCmdList, anchorOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(InCmdList, shownOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(InCmdList, InOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    InCmdList->CopyResource(InOutput, shownOut);
    Transition(InCmdList, InOutput, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(InCmdList, shownOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    _current ^= 1;
    _valid = true;
    return true;
}
