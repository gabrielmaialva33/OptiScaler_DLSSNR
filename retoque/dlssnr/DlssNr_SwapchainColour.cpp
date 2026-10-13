#include "pch.h"

#include "DlssNr_SwapchainColour.h"

namespace DlssNr::SwapchainColour
{
namespace
{
// Never destroyed: a static destructor would run at process exit while a present thread may still read it.
Book& TheBook()
{
    static Book* book = new Book();
    return *book;
}

// The window and the format of the buffers the swapchain has now, from one GetDesc. The wrapped swapchain and, with
// frame generation, the FG swapchain the game holds around it present to one window, so either finds the same record.
// A swapchain with no window (composition, CoreWindow) gets no record.
bool Describe(IDXGISwapChain* swapchain, const void*& window, DXGI_FORMAT& format)
{
    window = nullptr;
    format = DXGI_FORMAT_UNKNOWN;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (swapchain == nullptr || FAILED(swapchain->GetDesc(&desc)))
        return false;

    window = desc.OutputWindow;
    format = desc.BufferDesc.Format;
    return window != nullptr;
}
} // namespace

void NoteCreated(IDXGISwapChain* swapchain)
{
    const void* window = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (Describe(swapchain, window, format))
        TheBook().Created(window, format);
}

void NoteColourSpace(IDXGISwapChain* swapchain, DXGI_COLOR_SPACE_TYPE colourSpace, HRESULT result)
{
    if (FAILED(result))
        return;

    const void* window = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (Describe(swapchain, window, format))
        TheBook().ColourSpaceSet(window, format, colourSpace);
}

void NoteResized(IDXGISwapChain* swapchain, HRESULT result)
{
    if (FAILED(result))
        return;

    const void* window = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (Describe(swapchain, window, format))
        TheBook().Resized(window, format);
}

PresentColourRecord Lookup(IDXGISwapChain* swapchain)
{
    const void* window = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (!Describe(swapchain, window, format))
        return {};

    return TheBook().Lookup(window);
}
} // namespace DlssNr::SwapchainColour
