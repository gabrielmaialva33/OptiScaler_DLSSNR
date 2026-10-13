// Copies of a texture into a buffer the CPU can read, shared by the raw burst (DlssNr_Capture.h) and the
// comparison capture (DlssNr_PairCapture.cpp).
//
// They live in one place because each carries a crash already paid for (TypedForCopy below), and a second copy of
// them is a second place for that fix to be missing (docs/dlssnr/design/comparison-capture.md, section 4.9).
//
// What D3D12 asks of a readback, all of it met here and none of it checked by vkd3d-proton: a buffer on a READBACK
// heap, created in COPY_DEST and never moved out of it; a placed footprint from GetCopyableFootprints, so the row
// pitch is a multiple of 256 bytes and the offset of 512; a fully typed footprint format; the source in COPY_SOURCE
// for the copy and handed back in the state it came in.

#pragma once

#include <windows.h>
#include <d3d12.h>

namespace capture
{
struct Shot
{
    ID3D12Resource* readback = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    unsigned long long bytes = 0;
};

// A copy needs a fully typed format, and the buffer the upscaler writes is occasionally declared
// typeless. A placed footprint carrying a typeless format makes CopyTextureRegion invalid, and an
// invalid copy removes the device -- which is what capturing did: it crashed the game every time
// the output happened to be one of these.
inline DXGI_FORMAT TypedForCopy(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return f;
    }
}

// A readback buffer sized for subresource 0 of a texture described by desc. False, with nothing held, when the
// footprint is empty or the buffer could not be created.
inline bool AllocReadback(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, Shot& shot)
{
    D3D12_RESOURCE_DESC typed = desc;
    typed.Format = TypedForCopy(desc.Format);

    unsigned long long total = 0;
    device->GetCopyableFootprints(&typed, 0, 1, 0, &shot.layout, nullptr, nullptr, &total);
    shot.bytes = total;

    if (total == 0)
        return false;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buf = {};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.Format = DXGI_FORMAT_UNKNOWN;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&shot.readback)));
}

// Records the copy of subresource 0 of src into shot, with src moved to COPY_SOURCE around it and handed back in
// state. Nothing is recorded when shot holds no buffer.
inline void CopyToReadback(ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, D3D12_RESOURCE_STATES state, Shot& shot)
{
    if (shot.readback == nullptr)
        return;

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = src;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    const bool needsTransition = state != D3D12_RESOURCE_STATE_COPY_SOURCE;

    if (needsTransition)
        cmd->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = shot.readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = shot.layout;

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = src;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &source, nullptr);

    if (needsTransition)
    {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = state;
        cmd->ResourceBarrier(1, &b);
    }
}
} // namespace capture
