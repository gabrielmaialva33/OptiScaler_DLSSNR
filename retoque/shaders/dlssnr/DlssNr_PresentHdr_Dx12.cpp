#include "pch.h"

#include "DlssNr_PresentHdr_Dx12.h"
#include <Logger.h>
#include <State.h>

#include <cstring>

// The pass's bytecode, when the build carries it. Its absence is a supported state: the conversion reports itself
// unavailable, an HDR swapchain stays declined, and the menu does not offer it. See dlssnr_present_hdr.hlsl for the
// command that makes it.
#if __has_include("precompile/DlssNr_PresentHdr_Shader.h")
#include "precompile/DlssNr_PresentHdr_Shader.h"
#define DLSSNR_PRESENT_HDR_BYTECODE 1
#else
#define DLSSNR_PRESENT_HDR_BYTECODE 0
#endif

namespace
{
constexpr DXGI_FORMAT kWorkFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// What the device says of one format: readable through an SRV, writable through a typed UAV store.
bool AskDevice(ID3D12Device* device, DXGI_FORMAT format, bool needLoad)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support {};
    support.Format = format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))))
        return false;

    const bool load = (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
    const bool uav = (support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0 &&
                     (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
    return (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0 && uav && (load || !needLoad);
}
} // namespace

bool DlssNr_PresentHdr_Dx12::Available() { return DLSSNR_PRESENT_HDR_BYTECODE != 0; }

DlssNr_PresentHdr_Dx12::DlssNr_PresentHdr_Dx12(std::string name, ID3D12Device* device) : Shader_Dx12(name, device)
{
    if (device == nullptr || !Available())
        return;

    // Two inputs, one output, one constant buffer, no sampler: every read is a Load at the texel's own position.
    if (!SetupRootSignature(device, 2, 1, 1))
    {
        LOG_ERROR("[{}] Failed to setup root signature", _name);
        return;
    }

    // One upload buffer per slot, mapped for life: a single shared buffer would be rewritten by the next record while
    // the GPU may still be reading this one's.
    const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(Constants));
    const auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    for (uint32_t i = 0; i < DLSSNR_PRESENT_HDR_NUM_OF_HEAPS; ++i)
    {
        if (FAILED(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&_constantBuffers[i]))) ||
            _constantBuffers[i] == nullptr)
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
    }

#if DLSSNR_PRESENT_HDR_BYTECODE
    if (!CreateComputePipeline(device, &_pipelineState, DlssNr_PresentHdr_cso, sizeof(DlssNr_PresentHdr_cso), nullptr))
    {
        LOG_ERROR("[{}] Failed to create the compute pipeline", _name);
        return;
    }
#endif

    if (!InitHeaps(device, _frameHeaps, DLSSNR_PRESENT_HDR_NUM_OF_HEAPS))
        return;

    for (uint32_t i = 0; i < DLSSNR_PRESENT_HDR_NUM_OF_HEAPS; ++i)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv {};
        cbv.BufferLocation = _constantBuffers[i]->GetGPUVirtualAddress();
        cbv.SizeInBytes = sizeof(Constants);
        device->CreateConstantBufferView(&cbv, _frameHeaps[i].GetCbvCPU(0));
    }

    // The composition itself writes the work texture through a UAV too; the conversion only adds the store here.
    _workSupported = AskDevice(device, kWorkFormat, true);
    if (!_workSupported)
        LOG_WARN("[{}] the device has no typed UAV store for R16G16B16A16_FLOAT; the HDR conversion stays off", _name);

    _init = true;
}

DlssNr_PresentHdr_Dx12::~DlssNr_PresentHdr_Dx12()
{
    // Only ever deleted once the recordings that used it are done, or at a shutdown that has already proved the same.
    if (State::Instance().isShuttingDown)
        return;

    // Shader_Dx12 releases these only for an object that finished initialising.
    if (!_init)
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
    }

    for (uint32_t i = 0; i < DLSSNR_PRESENT_HDR_NUM_OF_HEAPS; ++i)
    {
        if (_constantBuffers[i] != nullptr && _mappedConstants[i] != nullptr)
            _constantBuffers[i]->Unmap(0, nullptr);
        _mappedConstants[i] = nullptr;
        SAFE_RELEASE(_constantBuffers[i]);
        _frameHeaps[i].ReleaseHeaps();
    }
}

bool DlssNr_PresentHdr_Dx12::Supports(DXGI_FORMAT format)
{
    if (!_init || !_workSupported || _device == nullptr)
        return false;

    // Only the two formats the profiles take are ever asked; a typeless one has no view to give.
    if (format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R16G16B16A16_FLOAT)
        return false;

    for (const FormatAnswer& answer : _answers)
    {
        if (answer.format == format)
            return answer.supported;
    }

    const bool supported = AskDevice(_device, format, true);
    for (FormatAnswer& answer : _answers)
    {
        if (answer.format == DXGI_FORMAT_UNKNOWN)
        {
            answer.format = format;
            answer.supported = supported;
            break;
        }
    }

    if (!supported)
        LOG_WARN("[{}] the device cannot load format {} in a shader or store it through a typed UAV; the HDR "
                 "conversion declines it",
                 _name, (int) format);

    return supported;
}

bool DlssNr_PresentHdr_Dx12::TextureShape(const D3D12_RESOURCE_DESC& d)
{
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width > 0 &&
           d.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && d.Height > 0 &&
           d.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
           d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0;
}

bool DlssNr_PresentHdr_Dx12::ValidResources(ID3D12Resource* original, ID3D12Resource* processed, ID3D12Resource* out,
                                            uint32_t profile, uint32_t mode)
{
    if (!_init || original == nullptr || processed == nullptr || out == nullptr || original == out || processed == out)
        return false;
    if ((profile != DlssNr::PresentHdr::kProfileHdr10 && profile != DlssNr::PresentHdr::kProfileScRgb) ||
        (mode != DlssNr::PresentHdr::kModeDecode && mode != DlssNr::PresentHdr::kModeEncode))
        return false;
    const DXGI_FORMAT format =
        profile == DlssNr::PresentHdr::kProfileHdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : kWorkFormat;
    if (!Supports(format))
        return false;
    const auto src = original->GetDesc();
    const auto work = processed->GetDesc();
    const auto dst = out->GetDesc();
    if (!TextureShape(src) || !TextureShape(work) || !TextureShape(dst) || src.Width != work.Width ||
        src.Height != work.Height || src.Width != dst.Width || src.Height != dst.Height || src.Format != format ||
        (src.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (work.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0 ||
        (dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
        return false;
    if (mode == DlssNr::PresentHdr::kModeDecode
            ? (processed != original || dst.Format != kWorkFormat)
            : (processed == original || work.Format != kWorkFormat || dst.Format != format))
        return false;
    for (auto resource : { original, processed, out })
    {
        ID3D12Device* owner = nullptr;
        const HRESULT result = resource->GetDevice(IID_PPV_ARGS(&owner));
        const bool same = SUCCEEDED(result) && owner == _device;
        if (owner != nullptr)
            owner->Release();
        if (!same)
            return false;
    }
    return true;
}

bool DlssNr_PresentHdr_Dx12::Record(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original,
                                    ID3D12Resource* processed, ID3D12Resource* out, uint32_t profile, uint32_t mode)
{
    if (cmdList == nullptr || !ValidResources(original, processed, out, profile, mode))
        return false;

    if (profile != DlssNr::PresentHdr::kProfileHdr10 && profile != DlssNr::PresentHdr::kProfileScRgb)
        return false;

    const auto desc = out->GetDesc();
    const auto width = static_cast<uint32_t>(desc.Width);
    const auto height = desc.Height;
    if (width == 0 || height == 0)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_PRESENT_HDR_NUM_OF_HEAPS;
    FrameDescriptorHeap& heap = _frameHeaps[slot];

    // Every descriptor in the table is written, the unread one included: the root signature's ranges are static, and
    // D3D12 wants all of them valid at execution whether the shader reads them or not.
    CreateShaderResourceView(_device, original, heap.GetSrvCPU(0));
    CreateShaderResourceView(_device, processed, heap.GetSrvCPU(1));
    CreateUnorderedAccessView(_device, out, heap.GetUavCPU(0), 0);

    Constants constants {};
    constants.Width = width;
    constants.Height = height;
    constants.Profile = profile;
    constants.Mode = mode;
    std::memcpy(_mappedConstants[slot], &constants, sizeof(constants));

    ID3D12DescriptorHeap* heaps[] = { heap.GetHeapCSU() };
    cmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    cmdList->SetComputeRootSignature(_rootSignature);
    cmdList->SetPipelineState(_pipelineState);
    cmdList->SetComputeRootDescriptorTable(0, heap.GetTableGPUStart());
    cmdList->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
    return true;
}

bool DlssNr_PresentHdr_Dx12::Decode(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original, ID3D12Resource* work,
                                    uint32_t profile)
{
    // The decode reads nothing at t1; the original stands in for it so the slot is a valid view.
    return Record(cmdList, original, original, work, profile, DlssNr::PresentHdr::kModeDecode);
}

bool DlssNr_PresentHdr_Dx12::Encode(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* original, ID3D12Resource* work,
                                    ID3D12Resource* out, uint32_t profile)
{
    if (original == nullptr || out == nullptr)
        return false;

    // The encode writes what it read: out must be the original's size and format, or the bypass would hand a code of
    // one format to a store of another.
    const auto originalDesc = original->GetDesc();
    const auto outDesc = out->GetDesc();
    if (originalDesc.Width != outDesc.Width || originalDesc.Height != outDesc.Height ||
        originalDesc.Format != outDesc.Format)
    {
        return false;
    }

    return Record(cmdList, original, work, out, profile, DlssNr::PresentHdr::kModeEncode);
}
