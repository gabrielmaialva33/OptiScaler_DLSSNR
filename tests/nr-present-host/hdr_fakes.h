// Fakes for the HDR present conversion's side of the host (docs/dlssnr/design/hdr-present-conversion.md section 5.2),
// appended after the production DlssNr_PresentColour.h, DlssNr_PresentHdr.h and DlssNr_SwapchainColour.h so they can
// use their types. The conversion pass records what it was asked to dispatch and holds each call to the states its
// header documents; the timing sample holds every resolve to an open list.
#pragma once

inline bool g_hdrAvailable = true; // the build carries the conversion's shader
inline bool g_hdrSupports = true;  // the device can load and typed-UAV-store the swapchain's format
inline bool g_hdrResourcesValid = true;
inline unsigned g_hdrPassesBuilt = 0;

struct HdrDispatch
{
    ID3D12Resource* original;
    ID3D12Resource* processed;
    ID3D12Resource* out;
    uint32_t profile;
    uint32_t mode;
};
inline std::vector<HdrDispatch> g_hdrDispatches;

class DlssNr_PresentHdr_Dx12
{
    ID3D12Device* _device;

  public:
    static bool Available() { return g_hdrAvailable; }
    DlssNr_PresentHdr_Dx12(std::string, ID3D12Device* device) : _device(device) { ++g_hdrPassesBuilt; }
    bool BuiltFor(ID3D12Device* device) const { return device == _device; }
    bool IsInit() const { return true; }
    bool Supports(DXGI_FORMAT format)
    {
        return g_hdrSupports && (format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    bool ValidResources(ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, uint32_t, uint32_t)
    {
        return g_hdrResourcesValid;
    }
    bool Decode(ID3D12GraphicsCommandList* list, ID3D12Resource* original, ID3D12Resource* work, uint32_t profile)
    {
        assert(!list->closed);
        assert(original->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(work->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(work->desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        ++list->recorded;
        g_hdrDispatches.push_back({ original, original, work, profile, DlssNr::PresentHdr::kModeDecode });
        return true;
    }
    bool Encode(ID3D12GraphicsCommandList* list, ID3D12Resource* original, ID3D12Resource* work, ID3D12Resource* out,
                uint32_t profile)
    {
        assert(!list->closed);
        assert(original->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(work->state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assert(out->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        assert(out->desc.Format == original->desc.Format && out->desc.Width == original->desc.Width &&
               out->desc.Height == original->desc.Height);
        ++list->recorded;
        g_hdrDispatches.push_back({ original, work, out, profile, DlssNr::PresentHdr::kModeEncode });
        return true;
    }
};

namespace DlssNr
{
// The menu's HDR status, as the host reports it.
struct NotedHdr
{
    PresentColour profile;
    const char* reason;
    bool converted;
};
inline std::vector<NotedHdr> g_notedHdr;
inline void NotePresentHdr(PresentColour profile, const char* reason, bool converted)
{
    g_notedHdr.push_back({ profile, reason, converted });
}

// Production's description of a present frame, the HDR part included (DlssNr_Dx12.cpp).
inline DlssNrFrameInfo PresentFrameDefaults(bool reset, PresentColour colour = PresentColour::Sdr)
{
    DlssNrFrameInfo frame {};
    frame.PresentSource = true;
    frame.ColourIsLinearHdr = false;
    frame.ExtentIsStable = true;
    frame.OutputState = (int) D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    frame.Reset = reset;
    if (IsHdrProfile(colour))
    {
        frame.ColourIsLinearHdr = true;
        frame.PresentWhite = PresentHdr::kSdrWhiteScRgb;
        frame.PresentQuantRel = colour == PresentColour::Hdr10 ? PresentHdr::kPqQuantRel : 0.0f;
    }
    return frame;
}

namespace GpuTiming
{
inline unsigned g_conversionsFinished = 0;
inline unsigned g_conversionsWhole = 0;

class Conversion
{
    ID3D12GraphicsCommandList* _list;
    bool _open = false;
    unsigned _brackets = 0;
    bool _finished = false;

  public:
    Conversion(ID3D12Device*, ID3D12GraphicsCommandList* list, bool, uint32_t) : _list(list) {}
    ~Conversion() { Finish(false); }
    Conversion(const Conversion&) = delete;
    Conversion& operator=(const Conversion&) = delete;
    void Begin()
    {
        assert(!_list->closed && !_open);
        _open = true;
    }
    void End()
    {
        assert(!_list->closed && _open);
        _open = false;
        ++_brackets;
    }
    void Finish(bool success)
    {
        if (_finished)
            return;
        _finished = true;
        assert(!_list->closed && "a timing sample resolved on a closed list");
        ++g_conversionsFinished;
        if (success)
        {
            assert(_brackets == 2 && !_open);
            ++g_conversionsWhole;
        }
    }
};
} // namespace GpuTiming
} // namespace DlssNr
