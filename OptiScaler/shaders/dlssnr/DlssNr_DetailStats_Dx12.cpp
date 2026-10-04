#include "pch.h"

#include "DlssNr_DetailStats_Dx12.h"
#include <Logger.h>
#include <State.h>

#if __has_include("precompile/DlssNr_DetailStats_Shader.h")
#include "precompile/DlssNr_DetailStats_Shader.h"
#define DLSSNR_DETAIL_STATS_BYTECODE 1
#else
#define DLSSNR_DETAIL_STATS_BYTECODE 0
#endif

bool DlssNr_DetailStats_Dx12::Available() { return DLSSNR_DETAIL_STATS_BYTECODE != 0; }

DlssNr_DetailStats_Dx12::DlssNr_DetailStats_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr || !Available())
        return;

    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(device, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    const auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Params));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const size_t gridBytes = kGridWidth * kGridHeight * sizeof(float) * 4;
    const auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(gridBytes);

    for (uint32_t i = 0; i < DLSSNR_DETAIL_STATS_SLOTS; ++i)
    {
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&_constantBuffers[i]))))
        {
            LOG_ERROR("[{}] Failed to create constant buffer {}", _name, i);
            return;
        }

        const D3D12_RANGE noRead { 0, 0 };
        if (FAILED(_constantBuffers[i]->Map(0, &noRead, &_mappedConstants[i])) || _mappedConstants[i] == nullptr)
        {
            LOG_ERROR("[{}] Failed to map constant buffer {}", _name, i);
            return;
        }

        if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&_readbackBuffers[i]))))
        {
            LOG_ERROR("[{}] Failed to create readback buffer {}", _name, i);
            return;
        }

        const D3D12_RANGE readRange { 0, gridBytes };
        if (FAILED(_readbackBuffers[i]->Map(0, &readRange, &_mappedReadback[i])) || _mappedReadback[i] == nullptr)
        {
            LOG_ERROR("[{}] Failed to map readback buffer {}", _name, i);
            return;
        }
    }

#if DLSSNR_DETAIL_STATS_BYTECODE
    if (!CreateComputePipeline(device, &_pipelineState, DlssNr_DetailStats_cso, sizeof(DlssNr_DetailStats_cso),
                               nullptr))
    {
        LOG_ERROR("[{}] Failed to create compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(device, _frameHeaps, DLSSNR_DETAIL_STATS_SLOTS))
        return;

    const auto gridTexDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kGridWidth, kGridHeight, 1, 1,
                                                          1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gridTexDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_grid))) ||
        _grid == nullptr)
    {
        LOG_ERROR("[{}] Failed to create grid scratch texture", _name);
        return;
    }

    for (uint32_t i = 0; i < DLSSNR_DETAIL_STATS_SLOTS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Params);
        device->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _init = true;
}

DlssNr_DetailStats_Dx12::~DlssNr_DetailStats_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    SAFE_RELEASE(_grid);

    for (uint32_t i = 0; i < DLSSNR_DETAIL_STATS_SLOTS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);

        if (_readbackBuffers[i] != nullptr && _mappedReadback[i] != nullptr)
        {
            const D3D12_RANGE noWrite { 0, 0 };
            _readbackBuffers[i]->Unmap(0, &noWrite);
        }
        _mappedReadback[i] = nullptr;
        SAFE_RELEASE(_readbackBuffers[i]);

        _frameHeaps[i].ReleaseHeaps();
    }
}

bool DlssNr_DetailStats_Dx12::Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* output,
                                     ID3D12Resource* prevOutput, ID3D12Resource* input, ID3D12Resource* prevInput,
                                     ID3D12Resource* proxy, uint32_t width, uint32_t height, float whitePoint,
                                     uint32_t slot)
{
    if (!_init || cmdList == nullptr || output == nullptr || input == nullptr || _grid == nullptr)
        return false;

    slot = slot % DLSSNR_DETAIL_STATS_SLOTS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    ID3D12Resource* srvs[kSrvCount] = { output, prevOutput != nullptr ? prevOutput : output, input,
                                        prevInput != nullptr ? prevInput : input, proxy != nullptr ? proxy : input };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], heap.GetSrvCPU(i), DXGI_FORMAT_UNKNOWN, true);

    CreateUnorderedAccessView(_device, _grid, heap.GetUavCPU(0), 0, true);

    Params p {};
    p.mode = 0;
    p.measureWhitePoint = whitePoint > 1e-6f ? whitePoint : 1.0f;
    p.width = width;
    p.height = height;
    p.shoulderThreshold = 1.0f;
    p.floorThreshold = 0.05f;
    std::memcpy(_mappedConstants[slot], &p, sizeof(p));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch(64, 64, 1);

    // Transition grid from UAV to copy source, copy to readback buffer, then back to UAV
    const auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(_grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->ResourceBarrier(1, &toCopy);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {};
    layout.Footprint.Width = kGridWidth;
    layout.Footprint.Height = kGridHeight;
    layout.Footprint.Depth = 1;
    layout.Footprint.RowPitch = kGridWidth * sizeof(float) * 4;
    layout.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    const CD3DX12_TEXTURE_COPY_LOCATION dst(_readbackBuffers[slot], layout);
    const CD3DX12_TEXTURE_COPY_LOCATION src(_grid, 0);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(_grid, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &toUav);

    return true;
}

bool DlssNr_DetailStats_Dx12::Readback(uint32_t slot, DlssNr::DetailStats::Stats& outStats)
{
    slot = slot % DLSSNR_DETAIL_STATS_SLOTS;
    if (_mappedReadback[slot] == nullptr)
        return false;

    outStats = DlssNr::DetailStats::ReduceGrid(static_cast<const float*>(_mappedReadback[slot]));
    return DlssNr::DetailStats::Finite(outStats);
}
