// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// Modified for Retoque's LSFG study (branch study/lsfg-d3d12, tools/lsfg-study/MFG_PRESENTER.md): copied from
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12_Helpers.h into
// namespace LsfgMfg, without the FFX backend headers; the command pool waits for a free entry instead of asserting.

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <limits>

namespace LsfgMfg
{

typedef int32_t FfxErrorCode;
typedef FfxErrorCode (*FfxWaitCallbackFunc)(wchar_t* fenceName, uint64_t fenceValueToWaitFor);

constexpr UINT UNKNOWN_TIMER_RESOLUTION = 0; // Timer resolution is not known.

IDXGIFactory* getDXGIFactoryFromSwapChain(IDXGISwapChain* swapChain);
bool isExclusiveFullscreen(IDXGISwapChain* swapChain);
void waitForPerformanceCount(const int64_t targetCount, const int64_t frequency, const UINT timerResolution,
                             const UINT spinTime);
bool waitForFenceValue(ID3D12Fence* fence, UINT64 value, DWORD dwMilliseconds = INFINITE,
                       FfxWaitCallbackFunc waitCallback = nullptr, const bool waitForSingleObjectOnFence = false);
bool isTearingSupported(IDXGIFactory* dxgiFactory);
bool getMonitorLuminanceRange(IDXGISwapChain* swapChain, float* outMinLuminance, float* outMaxLuminance);
IDXGIOutput6* getMostRelevantOutputFromSwapChain(IDXGISwapChain* swapChain);
uint64_t GetResourceGpuMemorySize(ID3D12Resource* resource);

// The finest system timer period for the pacer's hybrid spin, or UNKNOWN_TIMER_RESOLUTION. FFX links winmm.lib;
// OptiScaler links no winmm.lib and may itself be loaded as winmm.dll, so the three entry points are resolved from
// System32 at first use.
UINT queryTimerResolution();

// Safe release for interfaces
template <class Interface> inline UINT SafeRelease(Interface*& pInterfaceToRelease)
{
    UINT refCount = std::numeric_limits<UINT>::max();
    if (pInterfaceToRelease != nullptr)
    {
        refCount = pInterfaceToRelease->Release();

        pInterfaceToRelease = nullptr;
    }

    return refCount;
}

inline void SafeCloseHandle(HANDLE& handle)
{
    if (handle)
    {
        CloseHandle(handle);
        handle = 0;
    }
}

// fix up format in case resource passed for SRV cannot be mapped
inline DXGI_FORMAT convertFormatSrv(DXGI_FORMAT format)
{
    switch (format)
    {
    // Handle Depth
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

    // Handle TYPELESS format for color: assume FLOAT for 16 and 32 bit channels, else UNORM
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    default:
        return format;
    }
}

class Dx12Commands
{
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 availableFenceValue = 0;

  public:
    void release()
    {
        SafeRelease(allocator);
        SafeRelease(list);
        SafeRelease(fence);
    }

  public:
    ~Dx12Commands() { release(); }

    bool initiated() { return allocator != nullptr; }

    bool verify(ID3D12CommandQueue* pQueue)
    {
        HRESULT hr = initiated() ? S_OK : E_FAIL;

        if (FAILED(hr))
        {
            ID3D12Device* device = nullptr;
            if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&device))))
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();
                if (SUCCEEDED(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator))))
                {
                    allocator->SetName(L"Dx12CommandPool::Allocator");
                    if (SUCCEEDED(device->CreateCommandList(queueDesc.NodeMask, queueDesc.Type, allocator, nullptr,
                                                            IID_PPV_ARGS(&list))))
                    {
                        list->SetName(L"Dx12CommandPool::Commandlist");
                        list->Close();

                        if (SUCCEEDED(
                                device->CreateFence(availableFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                        {
                            hr = S_OK;
                        }
                    }
                }

                SafeRelease(device);
            }
        }

        if (FAILED(hr))
        {
            release();
        }

        return SUCCEEDED(hr);
    }

    void occupy(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        availableFenceValue++;
        queue = pQueue;
        allocator->SetName(name);
        list->SetName(name);
        fence->SetName(name);
    }

    ID3D12GraphicsCommandList* reset()
    {
        if (SUCCEEDED(allocator->Reset()))
        {
            if (SUCCEEDED(list->Reset(allocator, nullptr)))
            {
            }
        }

        return list;
    }

    ID3D12GraphicsCommandList* getList() { return list; }

    void execute(bool listIsOpen = false)
    {
        if (listIsOpen)
        {
            list->Close();
        }

        ID3D12CommandList* pListsToExec[] = { list };
        queue->ExecuteCommandLists(_countof(pListsToExec), pListsToExec);
        queue->Signal(fence, availableFenceValue);
    }

    void drop(bool listIsOpen = false)
    {
        if (listIsOpen)
        {
            getList()->Close();
        }
        queue->Signal(fence, availableFenceValue);
    }

    bool available() { return fence->GetCompletedValue() >= availableFenceValue; }
};

// FFX's pool holds 32 lists per queue type and asserts when none is free. With N generated frames a real frame takes
// up to N interpolation lists (one per generated frame, MFG_PRESENTER.md 4.2), up to N + 1 composition lists and as
// many debug pacing lists, and the DIRECT bucket is shared by the game queue and the present queue (and the
// interpolation lists without FGAsync), so this one holds 64 and waits for the GPU to free one (every list taken is
// executed or dropped, which signals its fence). Each entry has its own allocator, reset only once its fence says the
// GPU is past it: an allocator per list in flight. After kWaitMs with none free, null: callers skip their recording
// but keep every fence they signal.
template <size_t Capacity> class Dx12CommandPool
{
  private:
    static constexpr DWORD kWaitMs = 2000;

    CRITICAL_SECTION criticalSection {};
    Dx12Commands buffer[4 /* D3D12_COMMAND_LIST_TYPE_COPY == 3 */][Capacity] = {};

    Dx12Commands* tryGet(D3D12_COMMAND_LIST_TYPE type, ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        EnterCriticalSection(&criticalSection);

        Dx12Commands* pCommands = nullptr;
        for (size_t idx = 0; idx < Capacity && (pCommands == nullptr); idx++)
        {
            auto& cmds = buffer[type][idx];
            if (cmds.verify(pQueue) && cmds.available())
            {
                pCommands = &cmds;
            }
        }

        if (pCommands != nullptr)
            pCommands->occupy(pQueue, name);

        LeaveCriticalSection(&criticalSection);

        return pCommands;
    }

  public:
    Dx12CommandPool() { InitializeCriticalSection(&criticalSection); }

    ~Dx12CommandPool()
    {
        EnterCriticalSection(&criticalSection);

        for (size_t type = 0; type < 4; type++)
        {
            for (size_t idx = 0; idx < Capacity; idx++)
            {
                auto& cmds = buffer[type][idx];
                while (cmds.initiated() && !cmds.available())
                {
                    // wait for list to be idling
                }
                cmds.release();
            }
        }

        LeaveCriticalSection(&criticalSection);

        DeleteCriticalSection(&criticalSection);
    }

    Dx12Commands* get(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        const D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();
        const ULONGLONG start = GetTickCount64();

        for (;;)
        {
            if (auto pCommands = tryGet(queueDesc.Type, pQueue, name))
                return pCommands;

            if (GetTickCount64() - start > kWaitMs)
                return nullptr;

            Sleep(0);
        }
    }
};

template <const int Size, typename Type = double> struct SimpleMovingAverage
{
    Type history[Size] = {};
    unsigned int idx = 0;
    unsigned int updateCount = 0;

    Type getAverage()
    {
        if (updateCount < Size)
            return 0.0;

        Type average = 0.f;
        unsigned int iterations = (updateCount >= Size) ? Size : updateCount;

        if (iterations > 0)
        {
            for (size_t i = 0; i < iterations; i++)
            {
                average += history[i];
            }
            average /= iterations;
        }

        return average;
    }

    Type getVariance()
    {
        if (updateCount < Size)
            return 0.0;

        Type average = getAverage();
        Type variance = 0.f;
        unsigned int iterations = (updateCount >= Size) ? Size : updateCount;

        if (iterations > 0)
        {
            for (size_t i = 0; i < iterations; i++)
            {
                variance += (history[i] - average) * (history[i] - average);
            }
            variance /= iterations;
        }

        return sqrt(variance);
    }

    void reset()
    {
        updateCount = 0;
        idx = 0;
    }

    void update(Type newValue)
    {
        history[idx] = newValue;
        idx = (idx + 1) % Size;
        updateCount++;
    }
};

} // namespace LsfgMfg
