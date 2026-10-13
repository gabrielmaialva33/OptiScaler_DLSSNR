#include "pch.h"

#include "DlssNr_Pulse_Dx12.h"
#include <Logger.h>
#include <State.h>

#include <cstring>

// The pass's bytecode, when the build carries it. Its absence is a supported state: the metric reports itself
// unavailable and the menu does not offer it. See dlssnr_pulse.hlsl for the command that makes it.
#if __has_include("precompile/DlssNr_Pulse_Shader.h")
#include "precompile/DlssNr_Pulse_Shader.h"
#define DLSSNR_PULSE_BYTECODE 1
#else
#define DLSSNR_PULSE_BYTECODE 0
#endif

namespace
{
void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, from, to);
    cmdList->ResourceBarrier(1, &barrier);
}

// One row of the grid in the readback: kGrid texels of four floats is 256 bytes, which is also the pitch D3D12
// requires a placed footprint's rows to be a multiple of.
constexpr UINT kRowPitch = DlssNr::Pulse::kGrid * sizeof(float) * 4;
static_assert(kRowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0, "the grid's rows must meet the copy pitch");
constexpr size_t kGridBytes = (size_t) kRowPitch * DlssNr::Pulse::kGrid;
} // namespace

bool DlssNr_Pulse_Dx12::Available() { return DLSSNR_PULSE_BYTECODE != 0; }

DlssNr_Pulse_Dx12::DlssNr_Pulse_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr || !Available())
        return;

    // Two inputs, one output, one constant buffer, no sampler: every read is a Load.
    if (!SetupRootSignature(device, kSrvCount, kUavCount, 1))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    const auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Params));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(kGridBytes);

    for (uint32_t i = 0; i < DLSSNR_PULSE_SLOTS; ++i)
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

        const D3D12_RANGE readRange { 0, kGridBytes };
        if (FAILED(_readbackBuffers[i]->Map(0, &readRange, &_mappedReadback[i])) || _mappedReadback[i] == nullptr)
        {
            LOG_ERROR("[{}] Failed to map readback buffer {}", _name, i);
            return;
        }
    }

#if DLSSNR_PULSE_BYTECODE
    if (!CreateComputePipeline(device, &_pipelineState, DlssNr_Pulse_cso, sizeof(DlssNr_Pulse_cso), nullptr))
    {
        LOG_ERROR("[{}] Failed to create compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(device, _frameHeaps, DLSSNR_PULSE_SLOTS))
        return;

    const auto gridDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, kGrid, kGrid, 1, 1, 1, 0,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gridDesc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&_grid))) ||
        _grid == nullptr)
    {
        LOG_ERROR("[{}] Failed to create the grid", _name);
        return;
    }
    _grid->SetName(L"DLSS-NR pulse grid");

    for (uint32_t i = 0; i < DLSSNR_PULSE_SLOTS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Params);
        device->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    _init = true;
}

DlssNr_Pulse_Dx12::~DlssNr_Pulse_Dx12()
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

    SAFE_RELEASE(_grid);

    for (uint32_t i = 0; i < DLSSNR_PULSE_SLOTS; ++i)
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

bool DlssNr_Pulse_Dx12::Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* output, ID3D12Resource* input,
                               uint32_t width, uint32_t height, uint32_t slot)
{
    if (!_init || cmdList == nullptr || output == nullptr || input == nullptr || _grid == nullptr || width == 0 ||
        height == 0)
    {
        return false;
    }

    slot %= DLSSNR_PULSE_SLOTS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    CreateShaderResourceView(_device, output, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, input, heap.GetSrvCPU(1));
    CreateUnorderedAccessView(_device, _grid, heap.GetUavCPU(0), 0);

    Params p {};
    p.width = width;
    p.height = height;
    std::memcpy(_mappedConstants[slot], &p, sizeof(p));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch(kGrid, kGrid, 1);

    // The transition doubles as the wait for the writes before the copy reads them.
    Transition(cmdList, _grid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout {};
    layout.Footprint.Width = kGrid;
    layout.Footprint.Height = kGrid;
    layout.Footprint.Depth = 1;
    layout.Footprint.RowPitch = kRowPitch;
    layout.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;

    const CD3DX12_TEXTURE_COPY_LOCATION dst(_readbackBuffers[slot], layout);
    const CD3DX12_TEXTURE_COPY_LOCATION src(_grid, 0);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    Transition(cmdList, _grid, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

double DlssNr_Pulse_Dx12::Light(uint32_t slot) const
{
    slot %= DLSSNR_PULSE_SLOTS;
    if (_mappedReadback[slot] == nullptr)
        return NAN;
    return DlssNr::Pulse::EditLight(static_cast<const float*>(_mappedReadback[slot]));
}
