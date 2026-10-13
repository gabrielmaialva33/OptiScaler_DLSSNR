#pragma once

// What is known of each swapchain's colour space, for the D3D12 present route's HDR decision
// (docs/dlssnr/design/hdr-present-conversion.md section 4.1; the D3D11 bridge keeps its own, in PresentHost).
//
// DXGI has no GetColorSpace1, so this is learned from calls: the wrapped swapchain tells it about a creation, a
// successful SetColorSpace1 -- the game's, and the two ForceHDR makes itself, which bypass the wrapper's own -- and a
// resize. State::outputColorSpace stays what it was, the menu's process-wide answer; this one is per swapchain and
// remembers which format a colour space was set for, so a resize to another format is not met with a stale colour
// space (which would decode PQ as sRGB, or the reverse, and compose that over the frame).
//
// Keyed by the window the swapchain presents to. The present route does not see one swapchain object: with frame
// generation the game presents an FG swapchain whose inner swapchain is the wrapped one, and RunPresentPass is entered
// from both. Every layer presents to the same window, and a new swapchain on that window starts its record over. A
// swapchain with no window (composition, CoreWindow) has no record, and its HDR frames are declined.
//
// The book itself is standard library and DXGI types only, so tests/nr-present-host runs it as it is. A resize that
// keeps the format keeps the record: whether DXGI also keeps the colour space across one that changes it is not known
// here (open question 3), and the answer to that is to decline until the colour space is set again.

#include "DlssNr_PresentColour.h"

#include <mutex>
#include <unordered_map>

struct IDXGISwapChain;

namespace DlssNr::SwapchainColour
{
class Book
{
  public:
    // A swapchain was created on this window: whatever was known of an earlier one there is forgotten, and the format's
    // default colour space applies until one is set.
    void Created(const void* window, DXGI_FORMAT format)
    {
        if (window == nullptr)
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        PresentColourRecord& record = _records[window];
        record = {};
        record.known = true;
        record.valid = true;
        record.format = format;
    }

    // A SetColorSpace1 succeeded while the swapchain was in this format.
    void ColourSpaceSet(const void* window, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colourSpace)
    {
        if (window == nullptr)
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        PresentColourRecord& record = _records[window];
        record.known = true;
        record.valid = true;
        record.colourSpaceSet = true;
        record.format = format;
        record.colourSpace = colourSpace;
    }

    // A resize succeeded and left the swapchain in this format. With no colour space set, the default follows the
    // format. With one set, another format leaves it unknown until the next SetColorSpace1. An unknown swapchain stays
    // unknown: whatever was set on it before the book saw it is not something a resize tells.
    void Resized(const void* window, DXGI_FORMAT format)
    {
        if (window == nullptr)
            return;

        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _records.find(window);
        if (found == _records.end())
            return;

        PresentColourRecord& record = found->second;
        if (!record.colourSpaceSet)
            record.format = format;
        else if (format != record.format)
            record.valid = false;
    }

    PresentColourRecord Lookup(const void* window) const
    {
        if (window == nullptr)
            return {};

        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = _records.find(window);
        return found != _records.end() ? found->second : PresentColourRecord {};
    }

  private:
    mutable std::mutex _mutex;
    std::unordered_map<const void*, PresentColourRecord> _records;
};

// The calls the wrapped swapchain makes, one line each (OptiScaler/wrapped/wrapped_swapchain.cpp). Each reads the
// window and the current format from the swapchain itself, and records nothing for a call that failed. Bookkeeping
// only: nothing here changes what the game's call does or returns.
void NoteCreated(IDXGISwapChain* swapchain);
void NoteColourSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colourSpace, HRESULT result);
void NoteResized(IDXGISwapChain* swapchain, HRESULT result);

// What the book knows of the swapchain being presented.
PresentColourRecord Lookup(IDXGISwapChain* swapchain);
} // namespace DlssNr::SwapchainColour
