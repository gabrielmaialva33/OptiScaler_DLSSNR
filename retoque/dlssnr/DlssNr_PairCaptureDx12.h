// Same-frame comparison capture, the D3D12 side the pass calls (docs/dlssnr/design/comparison-capture.md).
//
// Two calls inside DlssNr_Dx12::Dispatch, both under the pass's lock on the render thread:
//
//   Poll()         at the top, before anything can return: watches a recorded capture's ticket and hands it to the
//                  writer thread once the GPU is proven past the copies. One relaxed load while idle.
//   AtRecordSite() where the pass has written the frame: when a capture was asked for, records the two copies on
//                  the list that ran the pass. Wanted() is the one relaxed load that guards it, so the caller only
//                  builds the frame's description when a capture will use it.
//
// The menu's side (RequestPairCapture, GetPairCaptureStatus) is in DlssNrFeature_Dx12.h.

#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace DlssNr::PairCapture
{
// What the pass knows about the frame at the record site, for the provenance record. Literals and text the pass
// already builds; nothing here is, or may become, a path.
struct FrameFacts
{
    const char* route = "unknown";   // after-upscale, after-rr, before-upscale, d3d12-present, d3d11-bridge
    const char* surface = "unknown"; // upscaler-output, render-input, present-final
    uint32_t hookMethod = 0;
    bool held = false;
    bool carried = false;
    bool linearHdr = false;
    bool proxy = false;
    uint32_t compare = 0;
    uint32_t debugView = 0;
    float whitePoint = 0.0f;
    uint64_t frame = 0;   // the pass's own dispatch counter
    uint64_t present = 0; // the present counter (State::frameCount)
    std::string settings; // TimingContract: every setting that shapes the picture
};

void Poll();

// Whether a capture is waiting for a frame. When true, the caller fills FrameFacts and calls AtRecordSite.
bool Wanted();

// original is the frame the pass read (hdrCopy), processed the frame it wrote (the target, after the stabilizer).
// Each is handed back in the state it arrived in.
void AtRecordSite(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device, ID3D12Resource* original,
                  D3D12_RESOURCE_STATES originalState, ID3D12Resource* processed, D3D12_RESOURCE_STATES processedState,
                  const FrameFacts& facts);

// The record site threw (describing the frame allocates): withdraws the request this frame took, or the one still
// waiting, with a reason, so the button does not stay "waiting", and logs what (the exception's text, or null).
// Nothing is freed.
void AbandonAfterError(const char* what);

// From DlssNr::Shutdown: lets go of the readbacks proven done; the rest are kept, never freed under the GPU.
void Shutdown();
} // namespace DlssNr::PairCapture
