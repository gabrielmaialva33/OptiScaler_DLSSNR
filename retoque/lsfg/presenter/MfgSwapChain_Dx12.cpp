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
// FidelityFX SDK 2.3.0 Kits/FidelityFX/framegeneration/fsr3/dx12/FrameInterpolationSwapchainDX12.cpp and widened to N
// generated frames per real frame. Function names and order follow the original so the two can be compared
// function by function. What changed, by function:
// - presenterThread, compositeSwapChainFrame: loop over the entry's frames instead of FrameType::Count; the presenter
//   keeps a 10 s present summary (MfgPacing.h PresentWindow) with each frame's latency.
// - interpolationThread: every present of an entry is spaced by the real frame time divided by the frames the entry
//   presents (MfgPacing.h PresentDelta) instead of half of it.
// - dispatchInterpolationCommands: asks the frame generation callback for N frames into N outputs of this present's
//   output set and takes back how many it wrote; a callback that fails has its list closed before it is dropped; GPU
//   timestamps around the interpolation list for the summary.
// - presentInterpolated: the entry carries those frames, then the real one.
// - verifyBackbufferDuplicateResources / verifyInterpolationOutputs: outputs are created per set, on demand, up to N.
// - setFrameGenerationConfig: fed by Lsfg::Presenter with the config FFX's provider would have built; the default
//   present callback is this swapchain's own UI composition, with its objects as the context.
// - shutdown: also releases the UI composition, debug pacing and timing objects this swapchain owns.
// - Removed: the SDK1/SDK1.1.5 ABI types and calls, the FFX COM interfaces, the registered interpolation command
//   list, interpolationOutput(), presentPassthrough (never called), AntiLag 2 marking and the per-buffer
//   IID_IFfxFrameInterpolationSwapChainResourceInfo private data.

#include "pch.h"

#include "MfgSwapChain_Dx12.h"

#include <Logger.h>

#include <dx12/ffx_api_dx12.h>

#include <algorithm>
#include <intrin.h>
#include <tuple>

namespace LsfgMfg
{

namespace
{
constexpr double kSummaryIntervalSec = 10.0;

// FGHooks::HookFGSwapchain detours this class's Release, Present, Present1, ResizeBuffers(1), ResizeTarget,
// Set/GetFullscreenState, GetFullscreenDesc and GetFrameLatencyWaitableObject by code address. FFX's swapchain lives
// in its own DLL; this one shares OptiScaler.dll with other DXGI wrappers, and the linker folds identical functions
// (/OPT:ICF). A pure forwarder folded with another class's would carry the detour to that class too, so the hooked
// forwarders start with a NOP that no other forwarder has.
#define LSFG_MFG_UNFOLDABLE() __nop()

void InitPresentCallback(ffxCallbackDescFrameGenerationPresent& desc,
                         CallbackDescFrameGenerationPresentPremulAlpha& extra, void* device, void* cmdList,
                         bool isGenerated, FfxApiResource swapChainBuffer, FfxApiResource backBuffer,
                         FfxApiResource currentUI, bool usePremulAlpha, uint64_t frameID)
{
    desc = {};
    desc.header.type = FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT;
    desc.commandList = cmdList;
    desc.device = device;
    desc.isGeneratedFrame = isGenerated;
    desc.outputSwapChainBuffer = swapChainBuffer;
    desc.currentBackBuffer = backBuffer;
    desc.currentUI = currentUI;
    desc.frameID = frameID;

    extra = {};
    extra.usePremulAlpha = usePremulAlpha;
    extra.header.pNext = nullptr;
    extra.header.type = LSFG_MFG_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT_PREMUL_ALPHA;

    desc.header.pNext = &extra.header;
}

FfxApiResource ResourceFor(ID3D12Resource* resource, uint32_t state) { return ffxApiGetResourceDX12(resource, state); }

double TicksToMs(int64_t ticks, int64_t frequency) { return double(ticks) * 1000.0 / double(frequency); }

void LogPresentWindow(PresentInfo* presenter, int64_t nowQpc, int64_t qpcFrequency)
{
    auto& w = presenter->window;
    const double seconds = double(nowQpc - presenter->windowStartQpc) / double(qpcFrequency);
    const double p50 = w.Percentile(0.50);
    const double p99 = w.Percentile(0.99);
    const double over = w.intervalsMs.empty() ? 0.0 : 100.0 * double(w.overTarget) / double(w.intervalsMs.size());

    LOG_INFO("LSFG presenter: last {:.1f} s: {} real + {} generated presented ({:.2f} per real; entries by generated "
             "count 0:{} 1:{} 2:{} 3:{}), {:.1f} fps presented over {:.1f} fps real, present interval p50 {:.2f} ms "
             "p99 {:.2f} ms (target {:.2f} ms), {:.1f}% over 1.5x target",
             seconds, w.real, w.generated, w.GeneratedPerReal(), w.entriesByGenerated[0], w.entriesByGenerated[1],
             w.entriesByGenerated[2], w.entriesByGenerated[3], double(w.real + w.generated) / seconds,
             double(w.real) / seconds, p50, p99, w.MeanTargetMs(), over);

    // Latency: on the GPU, generated frame k done after its real frame's first interpolation list started (it can be
    // composited from then on); on the CPU, each frame presented after the game handed its frame over.
    std::string ready, shown;
    for (uint32_t k = 0; k < kMaxGeneratedFrames; ++k)
    {
        if (!w.readyMs[k].empty())
            ready += std::format("{}#{} {:.2f}/{:.2f} ms", ready.empty() ? "" : ", ", k + 1,
                                 PresentWindow::PercentileOf(w.readyMs[k], 0.50),
                                 PresentWindow::PercentileOf(w.readyMs[k], 0.99));
        if (!w.shownGeneratedMs[k].empty())
            shown += std::format("{}#{} {:.2f} ms", shown.empty() ? "" : ", ", k + 1,
                                 PresentWindow::PercentileOf(w.shownGeneratedMs[k], 0.50));
    }
    if (!w.shownRealMs.empty())
        shown += std::format("{}real {:.2f} ms", shown.empty() ? "" : ", ",
                             PresentWindow::PercentileOf(w.shownRealMs, 0.50));

    LOG_INFO("LSFG presenter: last {:.1f} s: {:.2f} interpolation list(s) per real frame; generated frame done after "
             "its interpolation started (GPU p50/p99): {}; presented after the game's Present (p50): {}",
             seconds, w.ListsPerEntry(), ready.empty() ? std::string("no GPU timings") : ready,
             shown.empty() ? std::string("none") : shown);

    w = {};
    presenter->windowStartQpc = nowQpc;
}
} // namespace

HRESULT compositeSwapChainFrame(PresentInfo* presenter, PacingData* pacingEntry, uint32_t frameIndex)
{
    const FrameInfo& frameInfo = pacingEntry->frames[frameIndex];

    if (frameInfo.generated)
        presenter->presentQueue->Wait(presenter->interpolationFence, frameInfo.interpolationCompletedFenceValue);

    if (pacingEntry->drawDebugPacingLines)
    {
        if (auto gpuCommands = presenter->commandPool.get(presenter->presentQueue, L"compositeSwapChainFrame"))
        {
            uint32_t currBackbufferIndex = presenter->swapChain->GetCurrentBackBufferIndex();
            ID3D12Resource* swapchainBackbuffer = nullptr;
            presenter->swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer));

            ffxCallbackDescFrameGenerationPresent desc;
            CallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
            InitPresentCallback(desc, premulAlpha, presenter->device, gpuCommands->reset(), frameInfo.generated,
                                ResourceFor(swapchainBackbuffer, FFX_API_RESOURCE_STATE_PRESENT), frameInfo.resource,
                                pacingEntry->uiSurface, pacingEntry->usePremulAlphaComposite,
                                pacingEntry->currentFrameID);

            DebugPacing(&desc, presenter->debugPacing);

            gpuCommands->execute(true);

            SafeRelease(swapchainBackbuffer);
        }
    }

    if (auto gpuCommands = presenter->commandPool.get(presenter->presentQueue, L"compositeSwapChainFrame"))
    {
        uint32_t currBackbufferIndex = presenter->swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* swapchainBackbuffer = nullptr;
        presenter->swapChain->GetBuffer(currBackbufferIndex, IID_PPV_ARGS(&swapchainBackbuffer));

        ffxCallbackDescFrameGenerationPresent desc;
        CallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
        InitPresentCallback(desc, premulAlpha, presenter->device, gpuCommands->reset(), frameInfo.generated,
                            ResourceFor(swapchainBackbuffer, FFX_API_RESOURCE_STATE_PRESENT), frameInfo.resource,
                            pacingEntry->uiSurface, pacingEntry->usePremulAlphaComposite, pacingEntry->currentFrameID);

        if (pacingEntry->presentCallback != nullptr)
            pacingEntry->presentCallback(&desc, pacingEntry->presentCallbackContext);

        gpuCommands->execute(true);

        SafeRelease(swapchainBackbuffer);
    }

    presenter->presentQueue->Signal(presenter->compositionFenceGPU, frameInfo.presentIndex);
    presenter->compositionFenceCPU->Signal(frameInfo.presentIndex);

    return S_OK;
}

void presentToSwapChain(PresentInfo* presenter, PacingData* pacingEntry, uint32_t frameIndex)
{
    const FrameInfo& frameInfo = pacingEntry->frames[frameIndex];

    const UINT uSyncInterval = pacingEntry->vsync ? 1 : 0;
    const bool bExclusiveFullscreen = isExclusiveFullscreen(presenter->swapChain);
    const bool bSetAllowTearingFlag = pacingEntry->tearingSupported && !bExclusiveFullscreen && (0 == uSyncInterval);
    const UINT uFlags = bSetAllowTearingFlag * DXGI_PRESENT_ALLOW_TEARING;

    presenter->swapChain->Present(uSyncInterval, uFlags);

    // tick frames sent for presentation
    presenter->presentQueue->Signal(presenter->presentFence, frameInfo.presentIndex);
}

DWORD WINAPI presenterThread(LPVOID param)
{
    PresentInfo* presenter = static_cast<PresentInfo*>(param);

    if (presenter)
    {
        UINT64 numFramesSentForPresentation = 0;
        int64_t qpcFrequency = 0;

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFrequency = freq.QuadPart;

        presenter->previousPresentQpc = 0;

        while (!presenter->shutdown)
        {
            WaitForSingleObject(presenter->pacerEvent, INFINITE);

            if (!presenter->shutdown)
            {
                EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                PacingData entry = presenter->scheduledPresents;
                presenter->scheduledPresents.invalidate();

                LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

                if (entry.numFramesToPresent > 0)
                {
                    // we might have dropped entries so have to update here, oterwise we might deadlock
                    presenter->presentQueue->Signal(presenter->presentFence, entry.numFramesSentForPresentationBase);
                    presenter->presentQueue->Wait(presenter->interpolationFence,
                                                  entry.interpolationCompletedFenceValue);

                    // Pure spin unless the hybrid spin is allowed and the system timer is known (FFX asks per frame).
                    const UINT timerResolution =
                        presenter->allowHybridSpin ? queryTimerResolution() : UNKNOWN_TIMER_RESOLUTION;

                    presenter->window.AddEntry(entry.generatedFrames);

                    for (uint32_t frameIndex = 0; frameIndex < entry.frameCount; frameIndex++)
                    {
                        const FrameInfo& frameInfo = entry.frames[frameIndex];
                        if (frameInfo.doPresent)
                        {
                            compositeSwapChainFrame(presenter, &entry, frameIndex);

                            // signal replacement buffer availability
                            if (frameInfo.presentIndex == entry.replacementBufferFenceSignal)
                            {
                                presenter->presentQueue->Signal(presenter->replacementBufferFence,
                                                                entry.replacementBufferFenceSignal);
                            }

                            // pacing without composition
                            waitForFenceValue(presenter->compositionFenceGPU, frameInfo.presentIndex);
                            uint64_t targetQpc = presenter->previousPresentQpc + frameInfo.presentQpcDelta;
                            waitForPerformanceCount(targetQpc, qpcFrequency, timerResolution,
                                                    presenter->hybridSpinTime);

                            int64_t currentPresentQPC;
                            QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentPresentQPC));
                            presenter->previousPresentQpc = currentPresentQPC;

                            presentToSwapChain(presenter, &entry, frameIndex);

                            // The summary: QPC between consecutive presents against this frame's pacing target.
                            if (presenter->windowStartQpc == 0)
                                presenter->windowStartQpc = currentPresentQPC;

                            const double intervalMs =
                                presenter->lastStatsPresentQpc != 0
                                    ? TicksToMs(currentPresentQPC - presenter->lastStatsPresentQpc, qpcFrequency)
                                    : -1.0;
                            presenter->lastStatsPresentQpc = currentPresentQPC;
                            presenter->window.AddPresent(frameInfo.generated, intervalMs,
                                                         TicksToMs(int64_t(frameInfo.presentQpcDelta), qpcFrequency));
                            if (entry.handoffQpc != 0)
                                presenter->window.AddShown(
                                    frameInfo.generated, frameIndex,
                                    TicksToMs(currentPresentQPC - entry.handoffQpc, qpcFrequency));
                        }
                    }

                    // The entry's last generated frame has been composited, and its composition waited for the list
                    // that resolved the entry's timestamps: they have landed.
                    presenter->window.AddLists(entry.interpolationLists);
                    double readyMs[kMaxGeneratedFrames] {};
                    if (presenter->timingData != nullptr && entry.timingSlot < kTimingSlots &&
                        ReadyTimes(presenter->timingData + entry.timingSlot * kTimestampsPerEntry,
                                   entry.interpolationLists, entry.generatedFrames, entry.timestampFrequency,
                                   presenter->lastTimingBegin, readyMs))
                    {
                        for (uint32_t k = 0; k < entry.generatedFrames; k++)
                            presenter->window.AddReady(k, readyMs[k]);
                    }

                    numFramesSentForPresentation = entry.numFramesSentForPresentationBase + entry.numFramesToPresent;

                    int64_t nowQpc;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&nowQpc));
                    if (presenter->windowStartQpc != 0 &&
                        double(nowQpc - presenter->windowStartQpc) >= kSummaryIntervalSec * double(qpcFrequency))
                    {
                        LogPresentWindow(presenter, nowQpc, qpcFrequency);
                    }
                }
            }
        }

        waitForFenceValue(presenter->presentFence, numFramesSentForPresentation);
    }

    return 0;
}

DWORD WINAPI interpolationThread(LPVOID param)
{
    PresentInfo* presenter = static_cast<PresentInfo*>(param);

    if (presenter)
    {
        HANDLE presenterThreadHandle = CreateThread(nullptr, 0, presenterThread, param, 0, nullptr);

        if (presenterThreadHandle != 0)
        {
            SetThreadPriority(presenterThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(presenterThreadHandle, L"LSFG MFG Presenter Thread");

            SimpleMovingAverage<10, double> frameTime {};

            int64_t previousQpc = 0;
            int64_t previousDelta = 0;
            int64_t qpcFrequency;
            QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&qpcFrequency));

            while (!presenter->shutdown)
            {
                WaitForSingleObject(presenter->presentEvent, INFINITE);

                if (!presenter->shutdown)
                {
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);

                    PacingData entry = presenter->scheduledInterpolations;
                    presenter->scheduledInterpolations.invalidate();

                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

                    int64_t preWaitQPC = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&preWaitQPC));
                    int64_t previousPresentQPC = presenter->previousPresentQpc;
                    int64_t targetDelta = (previousPresentQPC + previousDelta) - preWaitQPC;

                    // The fence of the entry's generated frames; none to wait for when it has none (FFX's
                    // frames[Interpolated_1] is empty then).
                    const UINT64 interpolationValue =
                        entry.generatedFrames > 0 ? entry.frames[0].interpolationCompletedFenceValue : 0;

                    // Risk of late wake if overthreading. If allowed, use WaitForSingleObject to wait for
                    // interpolationFence if the target is more than 2ms later.
                    if (previousPresentQPC && (targetDelta * 1000000) / qpcFrequency > 2000)
                    {
                        waitForFenceValue(presenter->interpolationFence, interpolationValue, INFINITE, nullptr,
                                          presenter->allowWaitForSingleObjectOnFence);
                    }
                    else
                    {
                        // spin to wait for interpolationFence if the target is less than 2ms.
                        waitForFenceValue(presenter->interpolationFence, interpolationValue, INFINITE, nullptr, false);
                    }

                    SetEvent(presenter->interpolationEvent);

                    int64_t currentQpc = 0;
                    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&currentQpc));

                    const double deltaQpc = double(currentQpc - previousQpc) * (previousQpc > 0);
                    previousQpc = currentQpc;

                    // reset pacing averaging if delta > 10 fps,
                    const float fTimeoutInSeconds = 0.1f;
                    double deltaQpcResetThreashold = double(qpcFrequency * fTimeoutInSeconds);
                    if ((deltaQpc > deltaQpcResetThreashold) || presenter->resetTimer)
                    {
                        frameTime.reset();
                    }
                    else
                    {
                        frameTime.update(deltaQpc);
                    }

                    // set presentation time: the real frame time split over the frames this entry presents, reduced
                    // by the variance and a safety margin so we don't lock on a framerate lower than necessary
                    const int64_t qpcSafetyMargin = int64_t(qpcFrequency * presenter->safetyMarginInSec);
                    const int64_t deltaToUse =
                        PresentDelta(frameTime.getAverage(), frameTime.getVariance(), entry.numFramesToPresent,
                                     presenter->varianceFactor, qpcSafetyMargin);
                    for (uint32_t i = 0; i < entry.frameCount; i++)
                        entry.frames[i].presentQpcDelta = deltaToUse;
                    previousDelta = deltaToUse;

                    // schedule presents
                    EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
                    presenter->scheduledPresents = entry;
                    LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);
                    SetEvent(presenter->pacerEvent);
                }
            }

            // signal event to allow thread to finish
            SetEvent(presenter->pacerEvent);
            WaitForSingleObject(presenterThreadHandle, INFINITE);
            SafeCloseHandle(presenterThreadHandle);
        }
    }

    return 0;
}

DWORD WINAPI waitableSignalThread(LPVOID param)
{
    PresentInfo* presenter = static_cast<PresentInfo*>(param);
    presenter->shutdownWaitableSignalThread = false;

    while (!presenter->shutdownWaitableSignalThread)
    {
        SignalObjectAndWait(presenter->waitableObjectSemaphoreHandle, presenter->frameDoneEventHandle, INFINITE, false);
    }

    // this code only is reached when swapchain gets destroyed
    // make sure semaphore is not blocking the game
    ReleaseSemaphore(presenter->waitableObjectSemaphoreHandle, kMaxBufferCount, NULL);

    return 0;
}

HANDLE CreateReplaceableObjectHandle(PresentInfo* presenter, UINT latency, UINT bufferCount)
{
    // Create internal semaphore and worker thread for frame latency waitable object.
    // The semaphore will be duplicated when returned to the application via GetFrameLatencyWaitableObject().
    presenter->frameDoneEventHandle = CreateEvent(0, false, TRUE, nullptr);
    presenter->waitableObjectSemaphoreHandle = CreateSemaphore(0, latency, bufferCount, nullptr);

    if (presenter->waitableThreadHandle == NULL)
    {
        presenter->waitableThreadHandle =
            CreateThread(nullptr, 0, &waitableSignalThread, reinterpret_cast<void*>(presenter), 0, nullptr);

        if (presenter->waitableThreadHandle != 0)
        {
            SetThreadPriority(presenter->waitableThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(presenter->waitableThreadHandle, L"LSFG MFG WaitableObject tracking Thread");
        }
    }

    return presenter->waitableObjectSemaphoreHandle;
}

void ReleaseReplaceableObjectHandle(PresentInfo* presenter)
{
    // Shutdown the internal waitable object infrastructure.
    // Note: The application is responsible for closing its duplicated handle(s) obtained
    // via GetFrameLatencyWaitableObject(). The handle duplication ensures independent
    // lifetime management - the semaphore object persists until both SDK and app close their handles.
    if (presenter->waitableThreadHandle != NULL)
    {
        // prepare present CPU thread for shutdown
        presenter->shutdownWaitableSignalThread = true;
        // signal event to allow thread to finish
        SetEvent(presenter->frameDoneEventHandle);
        // wait for thread function to finish
        WaitForSingleObject(presenter->waitableThreadHandle, INFINITE);
        // close thread
        SafeCloseHandle(presenter->waitableThreadHandle);
    }

    // close internal event & semaphore handles
    SafeCloseHandle(presenter->waitableObjectSemaphoreHandle);
    SafeCloseHandle(presenter->frameDoneEventHandle);
}

void UpdateFrameLatencyWaitableObject(PresentInfo* presenter, INT delta)
{
    if (presenter->waitableObjectSemaphoreHandle == 0)
        return;

    // adjust semaphorecount to reflect new gameMaximumFrameLatency
    if (delta > 0)
    {
        ReleaseSemaphore(presenter->waitableObjectSemaphoreHandle, delta, NULL);
    }

    for (INT i = 0; i < -delta; ++i)
    {
        WaitForSingleObject(presenter->waitableObjectSemaphoreHandle, INFINITE);
    }
}

void SignalFrameLatencyWaitableObject(PresentInfo* presenter, ID3D12Fence* fence, UINT64 framesSentForPresentation)
{
    if (presenter->frameDoneEventHandle)
    {
        fence->SetEventOnCompletion(framesSentForPresentation, presenter->frameDoneEventHandle);
    }
}

bool MfgSwapChainDX12::verifyBackbufferDuplicateResources()
{
    HRESULT hr = S_OK;

    ID3D12Device* device = nullptr;
    ID3D12Resource* buffer = nullptr;
    if (SUCCEEDED(real()->GetBuffer(0, IID_PPV_ARGS(&buffer))))
    {
        if (SUCCEEDED(buffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            auto bufferDesc = buffer->GetDesc();
            D3D12_CLEAR_VALUE clearValue { bufferDesc.Format, 0.f, 0.f, 0.f, 1.f };

            D3D12_HEAP_PROPERTIES heapProperties {};
            D3D12_HEAP_FLAGS heapFlags;
            buffer->GetHeapProperties(&heapProperties, &heapFlags);

            heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
            heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

            for (size_t i = 0; i < gameBufferCount; i++)
            {
                if (replacementSwapBuffers[i].resource == nullptr)
                {
                    // create game render output resource
                    if (FAILED(device->CreateCommittedResource(&heapProperties, heapFlags, &bufferDesc,
                                                               D3D12_RESOURCE_STATE_PRESENT, &clearValue,
                                                               IID_PPV_ARGS(&replacementSwapBuffers[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
                        totalUsageInBytes += resourceSize;
                        replacementSwapBuffers[i].resource->SetName(L"LSFG MFG Replacement BackBuffer");
                    }
                }
            }

            SafeRelease(device);
        }

        SafeRelease(realBackBuffer0);
        realBackBuffer0 = buffer;
    }

    return SUCCEEDED(hr);
}

// FFX creates its two interpolation outputs with the replacement buffers. Here a set holds up to N, created when a
// present first asks for that many: in the real backbuffer's description with ALLOW_UNORDERED_ACCESS, in
// UNORDERED_ACCESS. Nothing is released before destroyReplacementResources, which waits for the GPU first.
bool MfgSwapChainDX12::verifyInterpolationOutputs(uint32_t set, uint32_t count)
{
    if (realBackBuffer0 == nullptr && !verifyBackbufferDuplicateResources())
        return false;
    if (realBackBuffer0 == nullptr)
        return false;

    ID3D12Device* device = nullptr;
    if (FAILED(realBackBuffer0->GetDevice(IID_PPV_ARGS(&device))))
        return false;

    auto bufferDesc = realBackBuffer0->GetDesc();
    bufferDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE clearValue { bufferDesc.Format, 0.f, 0.f, 0.f, 1.f };

    D3D12_HEAP_PROPERTIES heapProperties {};
    D3D12_HEAP_FLAGS heapFlags;
    realBackBuffer0->GetHeapProperties(&heapProperties, &heapFlags);

    heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
    heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

    bool ok = true;
    for (uint32_t k = 0; k < count && k < kMaxGeneratedFrames; k++)
    {
        ID3D12Resource*& output = outputSets[set].resource[k];
        if (output != nullptr)
            continue;

        if (FAILED(device->CreateCommittedResource(&heapProperties, heapFlags, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &clearValue,
                                                   IID_PPV_ARGS(&output))))
        {
            output = nullptr;
            ok = false;
            break;
        }

        totalUsageInBytes += GetResourceGpuMemorySize(output);
        output->SetName(L"LSFG MFG Generated Frame Output");
    }

    SafeRelease(device);
    return ok;
}

HRESULT MfgSwapChainDX12::init(HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* queue,
                               IDXGIFactory2* dxgiFactory)
{
    if (desc == nullptr || queue == nullptr || dxgiFactory == nullptr)
        return E_INVALIDARG;

    // store values we modify, to return when application asks for info
    gameBufferCount = std::clamp<UINT>(desc->BufferCount, 1, kMaxBufferCount);
    gameFlags = desc->Flags;
    gameSwapEffect = desc->SwapEffect;

    // set default ui composition / frame interpolation present function
    presentCallback = UiComposition;
    presentCallbackContext = &presentInfo.uiComposition;

    HRESULT hr = E_FAIL;

    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&presentInfo.device))))
    {
        presentInfo.gameQueue = queue;

        InitializeCriticalSection(&criticalSection);
        InitializeCriticalSection(&criticalSectionUpdateConfig);
        InitializeCriticalSection(&presentInfo.criticalSectionScheduledFrame);
        presentInfo.presentEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
        presentInfo.interpolationEvent = CreateEvent(NULL, FALSE, TRUE, nullptr);
        presentInfo.pacerEvent = CreateEvent(NULL, FALSE, FALSE, nullptr);
        tearingSupported = isTearingSupported(dxgiFactory);

        // Create presentation queue
        D3D12_COMMAND_QUEUE_DESC presentQueueDesc = queue->GetDesc();
        presentQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        presentQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        presentQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        presentQueueDesc.NodeMask = 0;
        if (FAILED(presentInfo.device->CreateCommandQueue(&presentQueueDesc, IID_PPV_ARGS(&presentInfo.presentQueue))))
            return E_FAIL;
        presentInfo.presentQueue->SetName(L"LSFG MFG PresentQueue");

        // Setup pass-through swapchain default state is disabled/passthrough
        IDXGISwapChain1* pSwapChain1 = nullptr;

        DXGI_SWAP_CHAIN_DESC1 realDesc = getInterpolationEnabledSwapChainDescription(desc);
        hr = dxgiFactory->CreateSwapChainForHwnd(presentInfo.presentQueue, hWnd, &realDesc, fullscreenDesc, nullptr,
                                                 &pSwapChain1);
        if (SUCCEEDED(hr))
        {
            hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&presentInfo.swapChain));
            SafeRelease(pSwapChain1);
            if (FAILED(hr))
                return hr;
        }
        else
        {
            return hr;
        }

        // init min and lax luminance according to monitor metadata
        // in case app doesn't set it through SetHDRMetadata
        getMonitorLuminanceRange(presentInfo.swapChain, &minLuminance, &maxLuminance);

        presentInfo.device->CreateFence(gameFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.gameFence));
        presentInfo.gameFence->SetName(L"LSFG MFG GameFence");

        presentInfo.device->CreateFence(interpolationFenceValue, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.interpolationFence));
        presentInfo.interpolationFence->SetName(L"LSFG MFG InterpolationFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.presentFence));
        presentInfo.presentFence->SetName(L"LSFG MFG PresentFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.replacementBufferFence));
        presentInfo.replacementBufferFence->SetName(L"LSFG MFG ReplacementBufferFence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.compositionFenceGPU));
        presentInfo.compositionFenceGPU->SetName(L"LSFG MFG CompositionFence GPU");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(&presentInfo.compositionFenceCPU));
        presentInfo.compositionFenceCPU->SetName(L"LSFG MFG CompositionFence CPU");

        createTiming();

        if ((desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ==
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        {
            replacementFrameLatencyWaitableObjectHandle =
                CreateReplaceableObjectHandle(&presentInfo, gameMaximumFrameLatency, gameBufferCount);
        }

        // Create interpolation queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        queueDesc.NodeMask = 0;
        presentInfo.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&presentInfo.asyncComputeQueue));
        if (presentInfo.asyncComputeQueue != nullptr)
            presentInfo.asyncComputeQueue->SetName(L"LSFG MFG AsyncComputeQueue");

        // Default to dispatch interpolation workloads on the game queue
        presentInfo.interpolationQueue = presentInfo.gameQueue;
    }

    return hr;
}

MfgSwapChainDX12::MfgSwapChainDX12() {}

MfgSwapChainDX12::~MfgSwapChainDX12() { shutdown(); }

UINT MfgSwapChainDX12::getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags)
{
    UINT flags = nonAdjustedFlags;

    // The DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT flag changes the D3D runtime behavior for fences
    // We will make our own waitable object for the app to wait on, but we need to keep the flag
    flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (tearingSupported)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    return flags;
}

DXGI_SWAP_CHAIN_DESC1
MfgSwapChainDX12::getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc)
{
    DXGI_SWAP_CHAIN_DESC1 fiDesc = *nonAdjustedDesc;

    // adjust swap chain descriptor to fit FI requirements
    fiDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    fiDesc.BufferCount = 3;
    fiDesc.Flags = getInterpolationEnabledSwapChainFlags(fiDesc.Flags);

    return fiDesc;
}

IDXGISwapChain4* MfgSwapChainDX12::real() { return presentInfo.swapChain; }

// The timestamp query heap and its readback buffer (PresentInfo::timingQueries). A failure only costs the summary's
// latency figures.
void MfgSwapChainDX12::createTiming()
{
    D3D12_QUERY_HEAP_DESC queryDesc {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = kTimingSlots * kTimestampsPerEntry;

    D3D12_HEAP_PROPERTIES readbackHeap {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(uint64_t) * queryDesc.Count;
    bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    void* mapped = nullptr;
    if (FAILED(presentInfo.device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&presentInfo.timingQueries))) ||
        FAILED(presentInfo.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&presentInfo.timingReadback))) ||
        FAILED(presentInfo.timingReadback->Map(0, nullptr, &mapped)))
    {
        LOG_WARN("LSFG presenter: no GPU timestamps for the interpolation lists; the summary has no latency figures");
        SafeRelease(presentInfo.timingReadback);
        SafeRelease(presentInfo.timingQueries);
        return;
    }

    presentInfo.timingReadback->SetName(L"LSFG MFG Interpolation Timestamps");
    presentInfo.timingData = static_cast<const uint64_t*>(mapped);
}

void MfgSwapChainDX12::releaseTiming()
{
    if (presentInfo.timingReadback != nullptr && presentInfo.timingData != nullptr)
    {
        D3D12_RANGE nothingWritten {};
        presentInfo.timingReadback->Unmap(0, &nothingWritten);
    }
    presentInfo.timingData = nullptr;
    SafeRelease(presentInfo.timingReadback);
    SafeRelease(presentInfo.timingQueries);
}

HRESULT MfgSwapChainDX12::shutdown()
{
    // m_pDevice will be nullptr already shutdown
    if (presentInfo.device)
    {
        destroyReplacementResources();

        EnterCriticalSection(&criticalSection);
        killPresenterThread();
        LeaveCriticalSection(&criticalSection);

        SafeCloseHandle(presentInfo.presentEvent);
        SafeCloseHandle(presentInfo.interpolationEvent);
        SafeCloseHandle(presentInfo.pacerEvent);

        // if we failed initialization, we may not have an interpolation queue or fence
        if (presentInfo.interpolationQueue)
        {
            if (presentInfo.interpolationFence)
            {
                presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
                waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE,
                                  presentInfo.waitCallback);
            }
        }

        // Both threads have exited and every present has been seen through the present fence
        // (destroyReplacementResources waited for it): nothing on the GPU uses these any more.
        presentInfo.uiComposition.release();
        presentInfo.debugPacing.release();
        releaseTiming();

        SafeRelease(presentInfo.asyncComputeQueue);
        SafeRelease(presentInfo.presentQueue);

        SafeRelease(presentInfo.interpolationFence);
        SafeRelease(presentInfo.presentFence);
        SafeRelease(presentInfo.replacementBufferFence);
        SafeRelease(presentInfo.compositionFenceGPU);
        SafeRelease(presentInfo.compositionFenceCPU);

        std::ignore = SafeRelease(presentInfo.swapChain);

        if (presentInfo.gameFence)
        {
            waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
        }
        SafeRelease(presentInfo.gameFence);

        DeleteCriticalSection(&criticalSection);
        DeleteCriticalSection(&criticalSectionUpdateConfig);
        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        ReleaseReplaceableObjectHandle(&presentInfo);
        replacementFrameLatencyWaitableObjectHandle = 0;

        std::ignore = SafeRelease(presentInfo.device);
    }

    return S_OK;
}

bool MfgSwapChainDX12::killPresenterThread()
{
    if (interpolationThreadHandle != NULL)
    {
        // prepare present CPU thread for shutdown
        presentInfo.shutdown = true;

        // signal event to allow thread to finish
        SetEvent(presentInfo.presentEvent);
        WaitForSingleObject(interpolationThreadHandle, INFINITE);
        SafeCloseHandle(interpolationThreadHandle);
    }

    return interpolationThreadHandle == nullptr;
}

bool MfgSwapChainDX12::spawnPresenterThread()
{
    if (interpolationThreadHandle == NULL)
    {
        presentInfo.shutdown = false;
        interpolationThreadHandle =
            CreateThread(nullptr, 0, &interpolationThread, reinterpret_cast<void*>(&presentInfo), 0, nullptr);

        if (interpolationThreadHandle != 0)
        {
            SetThreadPriority(interpolationThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(interpolationThreadHandle, L"LSFG MFG Interpolation Thread");
        }

        SetEvent(presentInfo.interpolationEvent);
    }

    return interpolationThreadHandle != NULL;
}

void MfgSwapChainDX12::setFrameGenerationConfig(const FrameGenerationConfig* config)
{
    if (config != nullptr)
        applyFrameGenerationConfig(config, false);
}

void MfgSwapChainDX12::applyFrameGenerationConfig(const FrameGenerationConfig* config, bool applyChangesNow)
{
    EnterCriticalSection(&criticalSectionUpdateConfig);

    // FFX: called with its own copy from ::Present to apply the changes; externally, the new config is stored and
    // applied at once only if it changes something that needs the threads or the queue to change.
    const FrameGenerationConfig cfg = applyChangesNow ? nextFrameGenerationConfig : *config;

    FfxApiPresentCallbackFunc inputPresentCallback =
        (nullptr != cfg.presentCallback) ? cfg.presentCallback : UiComposition;
    void* inputPresentCallbackCtx =
        (nullptr != cfg.presentCallback) ? cfg.presentCallbackContext : &presentInfo.uiComposition;
    ID3D12CommandQueue* inputInterpolationQueue = (cfg.allowAsyncWorkloads && presentInfo.asyncComputeQueue != nullptr)
                                                      ? presentInfo.asyncComputeQueue
                                                      : presentInfo.gameQueue;

    // if this is called externally just copy the new config to the internal copy to avoid potentially stalling on
    // criticalSection
    if (!applyChangesNow)
    {
        nextFrameGenerationConfig = cfg;

        // in case of actual reconfiguration: apply the changes immediately
        if (presentInfo.interpolationQueue != inputInterpolationQueue ||
            interpolationEnabled != cfg.frameGenerationEnabled || presentCallback != inputPresentCallback ||
            presentCallbackContext != inputPresentCallbackCtx ||
            frameGenerationCallback != cfg.frameGenerationCallback ||
            frameGenerationCallbackContext != cfg.frameGenerationCallbackContext ||
            drawDebugPacingLines != cfg.drawDebugPacingLines)
        {
            applyChangesNow = true;
        }
    }

    if (applyChangesNow)
    {
        EnterCriticalSection(&criticalSection);

        currentFrameID = cfg.frameID;
        presentInterpolatedOnly = cfg.onlyPresentInterpolated;
        interpolationRect = cfg.interpolationRect;
        drawDebugPacingLines = cfg.drawDebugPacingLines;

        if (presentInfo.interpolationQueue != inputInterpolationQueue)
        {
            waitForPresents();

            // change interpolation queue and reset fence value
            presentInfo.interpolationQueue = inputInterpolationQueue;
            interpolationFenceValue = 0;
            presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, interpolationFenceValue);
        }

        if (interpolationEnabled != cfg.frameGenerationEnabled || presentCallback != inputPresentCallback ||
            frameGenerationCallback != cfg.frameGenerationCallback || configFlags != cfg.flags ||
            presentCallbackContext != inputPresentCallbackCtx ||
            frameGenerationCallbackContext != cfg.frameGenerationCallbackContext)
        {
            waitForPresents();
            presentCallback = inputPresentCallback;
            presentCallbackContext = inputPresentCallbackCtx;
            frameGenerationCallback = cfg.frameGenerationCallback;
            configFlags = cfg.flags;
            frameGenerationCallbackContext = cfg.frameGenerationCallbackContext;

            // handle interpolation mode change
            if (interpolationEnabled != cfg.frameGenerationEnabled)
            {
                interpolationEnabled = cfg.frameGenerationEnabled;
                if (interpolationEnabled)
                {
                    frameInterpolationResetCondition = true;
                    nextPresentWaitValue = framesSentForPresentation;

                    spawnPresenterThread();
                }
                else
                {
                    killPresenterThread();
                }
            }
        }
        LeaveCriticalSection(&criticalSection);
    }

    LeaveCriticalSection(&criticalSectionUpdateConfig);
}

void MfgSwapChainDX12::setGeneratedFrames(uint32_t generatedFrames)
{
    generatedFramesWanted = std::clamp<uint32_t>(generatedFrames, 1, kMaxGeneratedFrames);
}

bool MfgSwapChainDX12::destroyReplacementResources()
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&criticalSection);

    waitForPresents();

    const bool recreatePresenterThread = interpolationThreadHandle != nullptr;
    if (recreatePresenterThread)
    {
        killPresenterThread();
    }

    {
        for (size_t i = 0; i < _countof(replacementSwapBuffers); i++)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
            totalUsageInBytes -= resourceSize;
            replacementSwapBuffers[i].destroy();
        }

        SafeRelease(realBackBuffer0);

        for (auto& set : outputSets)
        {
            for (auto& output : set.resource)
            {
                totalUsageInBytes -= GetResourceGpuMemorySize(output);
                SafeRelease(output);
            }
            set.availabilityFenceValue = 0;
        }

        if (uiReplacementBuffer.resource != nullptr)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
        }

        uiReplacementBuffer.destroy();
    }

    // reset counters used in buffer management
    framesSentForPresentation = 0;
    nextPresentWaitValue = 0;
    replacementSwapBufferIndex = 0;
    outputSetIndex = 0;
    presentCount = 0;
    interpolationFenceValue = 0;
    gameFenceValue = 0;

    // if we didn't init correctly, some parameters may not exist
    if (presentInfo.gameFence)
    {
        presentInfo.gameFence->Signal(gameFenceValue);
    }

    if (presentInfo.interpolationFence)
    {
        presentInfo.interpolationFence->Signal(interpolationFenceValue);
    }

    if (presentInfo.presentFence)
    {
        presentInfo.presentFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.replacementBufferFence)
    {
        presentInfo.replacementBufferFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceGPU)
    {
        presentInfo.compositionFenceGPU->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceCPU)
    {
        presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);
    }

    frameInterpolationResetCondition = true;

    if (recreatePresenterThread)
    {
        spawnPresenterThread();
    }

    LeaveCriticalSection(&criticalSection);

    return SUCCEEDED(hr);
}

bool MfgSwapChainDX12::waitForPresents()
{
    // wait for interpolation to finish
    waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.presentFence, framesSentForPresentation, INFINITE, presentInfo.waitCallback);

    return true;
}

// IUnknown
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr)
        return E_POINTER;

    const GUID guidReplacements[] = { IID_IUnknown,        IID_IDXGIObject,     IID_IDXGIDeviceSubObject,
                                      IID_IDXGISwapChain,  IID_IDXGISwapChain1, IID_IDXGISwapChain2,
                                      IID_IDXGISwapChain3, IID_IDXGISwapChain4, __uuidof(MfgSwapChainDX12) };

    for (auto guid : guidReplacements)
    {
        if (IsEqualGUID(riid, guid) == TRUE)
        {
            AddRef();
            *ppvObject = this;
            return S_OK;
        }
    }

    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE MfgSwapChainDX12::AddRef() { return InterlockedIncrement(&refCount); }

ULONG STDMETHODCALLTYPE MfgSwapChainDX12::Release()
{
    LSFG_MFG_UNFOLDABLE();
    ULONG ref = InterlockedDecrement(&refCount);
    if (ref != 0)
    {
        return ref;
    }

    delete this;

    return 0;
}

// IDXGIObject
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return real()->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return real()->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return real()->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetParent(REFIID riid, void** ppParent)
{
    return real()->GetParent(riid, ppParent);
}

// IDXGIDeviceSubObject
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetDevice(REFIID riid, void** ppDevice)
{
    return real()->GetDevice(riid, ppDevice);
}

void MfgSwapChainDX12::registerUiResource(FfxApiResource uiResource, uint32_t flags)
{
    EnterCriticalSection(&criticalSection);

    presentInfo.currentUiSurface = uiResource;
    presentInfo.uiCompositionFlags = flags;
    if (nullptr == uiResource.resource)
        presentInfo.uiCompositionFlags &= ~FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;

    LeaveCriticalSection(&criticalSection);
}

void MfgSwapChainDX12::setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc)
{
    presentInfo.waitCallback = waitCallbackFunc;
}

void MfgSwapChainDX12::setFramePacingTuning(const FfxApiSwapchainFramePacingTuning* framePacingTuning)
{
    if (framePacingTuning == nullptr)
        return;

    presentInfo.safetyMarginInSec = static_cast<double>(framePacingTuning->safetyMarginInMs) / 1000.0;
    presentInfo.varianceFactor = static_cast<double>(framePacingTuning->varianceFactor);
    presentInfo.allowHybridSpin = framePacingTuning->allowHybridSpin;
    presentInfo.hybridSpinTime = framePacingTuning->hybridSpinTime;
    presentInfo.allowWaitForSingleObjectOnFence = framePacingTuning->allowWaitForSingleObjectOnFence;
}

void MfgSwapChainDX12::presentWithUiComposition(UINT SyncInterval, UINT Flags)
{
    ID3D12Resource* dx12SwapchainBuffer = nullptr;
    UINT currentBackBufferIndex = presentInfo.swapChain->GetCurrentBackBufferIndex();
    presentInfo.swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&dx12SwapchainBuffer));

    if (auto uiCompositionList = presentInfo.commandPool.get(presentInfo.presentQueue, L"uiCompositionList()"))
    {
        auto list = uiCompositionList->reset();

        FfxApiResource currentUI;
        if (presentInfo.uiCompositionFlags &
            FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)
        {
            currentUI = ResourceFor(uiReplacementBuffer.resource, presentInfo.currentUiSurface.state);
        }
        else
        {
            currentUI = presentInfo.currentUiSurface;
        }

        ffxCallbackDescFrameGenerationPresent desc;
        CallbackDescFrameGenerationPresentPremulAlpha premulAlpha;
        InitPresentCallback(
            desc, premulAlpha, presentInfo.device, list, false,
            ResourceFor(dx12SwapchainBuffer, FFX_API_RESOURCE_STATE_PRESENT),
            ResourceFor(replacementSwapBuffers[replacementSwapBufferIndex].resource, FFX_API_RESOURCE_STATE_PRESENT),
            currentUI, (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA) != 0,
            currentFrameID);

        presentCallback(&desc, presentCallbackContext);

        uiCompositionList->execute(true);
    }

    presentInfo.presentQueue->Signal(presentInfo.replacementBufferFence, ++framesSentForPresentation);
    presentInfo.presentQueue->Signal(presentInfo.compositionFenceGPU, framesSentForPresentation);
    presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);

    presentInfo.swapChain->Present(SyncInterval, Flags);

    presentInfo.presentQueue->Signal(presentInfo.presentFence, framesSentForPresentation);
    presentInfo.gameQueue->Wait(presentInfo.presentFence, framesSentForPresentation);

    SafeRelease(dx12SwapchainBuffer);
}

InterpolationResult MfgSwapChainDX12::dispatchInterpolationCommands()
{
    InterpolationResult result;

    const UINT currentBackBufferIndex = GetCurrentBackBufferIndex();
    FfxApiResource backbuffer =
        ResourceFor(replacementSwapBuffers[currentBackBufferIndex].resource, FFX_API_RESOURCE_STATE_PRESENT);

    result.realFrame = backbuffer;

    const uint32_t wanted = std::clamp<uint32_t>(generatedFramesWanted.load(), 1, kMaxGeneratedFrames);
    OutputSet& set = outputSets[outputSetIndex];

    if (!verifyInterpolationOutputs(outputSetIndex, wanted))
        return result; // no outputs this present: the real frame alone

    // interpolation queue must wait for output resources to become available
    if (set.availabilityFenceValue != 0)
        presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, set.availabilityFenceValue);

    Dx12Commands* interpolationCommandList =
        presentInfo.commandPool.get(presentInfo.interpolationQueue, L"getInterpolationCommandList()");
    if (interpolationCommandList == nullptr)
        return result;

    auto dx12CommandList = interpolationCommandList->reset();

    // GPU timestamps for the summary: the start of the interpolation, then the end of each list.
    const bool timed = presentInfo.timingData != nullptr;
    const uint32_t timingSlot = timed ? uint32_t(timedEntries % kTimingSlots) : kNoTimingSlot;
    const UINT timingBase = timed ? timingSlot * kTimestampsPerEntry : 0;
    if (timed)
        dx12CommandList->EndQuery(presentInfo.timingQueries, D3D12_QUERY_TYPE_TIMESTAMP, timingBase);

    ffxDispatchDescFrameGeneration desc {};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    desc.header.pNext = nullptr;
    desc.commandList = dx12CommandList;
    for (uint32_t k = 0; k < wanted; k++)
        desc.outputs[k] = ResourceFor(set.resource[k], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.presentColor = backbuffer;
    desc.reset = frameInterpolationResetCondition;
    desc.numGeneratedFrames = wanted;
    desc.backbufferTransferFunction = backBufferTransferFunction;
    desc.minMaxLuminance[0] = minLuminance;
    desc.minMaxLuminance[1] = maxLuminance;
    desc.generationRect = interpolationRect;
    desc.frameID = currentFrameID;

    uint32_t produced = 0;
    if (frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_API_RETURN_OK)
    {
        if (timed)
        {
            dx12CommandList->EndQuery(presentInfo.timingQueries, D3D12_QUERY_TYPE_TIMESTAMP, timingBase + 1);
            dx12CommandList->ResolveQueryData(presentInfo.timingQueries, D3D12_QUERY_TYPE_TIMESTAMP, timingBase, 2,
                                              presentInfo.timingReadback, UINT64(timingBase) * sizeof(uint64_t));
            timedEntries++;

            if (timestampQueue != presentInfo.interpolationQueue)
            {
                timestampQueue = presentInfo.interpolationQueue;
                if (FAILED(timestampQueue->GetTimestampFrequency(&timestampFrequency)))
                    timestampFrequency = 0;
            }
            result.timingSlot = timingSlot;
            result.timestampFrequency = timestampFrequency;
        }

        interpolationCommandList->execute(true);

        presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
        result.listFence[0] = interpolationFenceValue;
        result.lists = 1;

        // The callback says how many it wrote (FSR-FG's Lsfg::TakesFrame lowers it to 1 when FFX, which writes
        // outputs[0] only, interpolates instead).
        produced = std::min(desc.numGeneratedFrames, wanted);
    }
    else
    {
        // FFX drops it with the list still open, which the next Reset of its allocator would trip over.
        interpolationCommandList->drop(true);
    }

    // reset condition if at least one frame was interpolated
    if (produced > 0)
    {
        frameInterpolationResetCondition = false;
        for (uint32_t k = 0; k < produced; k++)
            result.frames[k] = desc.outputs[k];
    }

    result.produced = produced;
    return result;
}

void MfgSwapChainDX12::presentInterpolated(UINT SyncInterval, UINT)
{
    const bool bVsync = SyncInterval > 0;

    int64_t handoffQpc = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&handoffQpc));

    // interpolation needs to wait for the game queue
    presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
    presentInfo.interpolationQueue->Wait(presentInfo.gameFence, gameFenceValue);

    const InterpolationResult interpolation = dispatchInterpolationCommands();
    const uint32_t produced = interpolation.produced;

    EnterCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    PacingData entry {};
    entry.presentCallback = presentCallback;
    entry.presentCallbackContext = presentCallbackContext;
    entry.drawDebugPacingLines = drawDebugPacingLines;

    if (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)
    {
        entry.uiSurface = ResourceFor(uiReplacementBuffer.resource, presentInfo.currentUiSurface.state);
    }
    else
    {
        entry.uiSurface = presentInfo.currentUiSurface;
    }
    entry.vsync = bVsync;
    entry.tearingSupported = tearingSupported;
    entry.numFramesSentForPresentationBase = framesSentForPresentation;
    entry.interpolationCompletedFenceValue = interpolationFenceValue;
    entry.usePremulAlphaComposite =
        (presentInfo.uiCompositionFlags & FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA) != 0;
    entry.currentFrameID = currentFrameID;
    entry.interpolationLists = interpolation.lists;
    entry.timingSlot = interpolation.timingSlot;
    entry.timestampFrequency = interpolation.timestampFrequency;
    entry.handoffQpc = handoffQpc;

    // generated, in display order
    uint32_t frameCount = 0;
    for (uint32_t k = 0; k < produced; k++)
    {
        FrameInfo& fi = entry.frames[frameCount++];
        fi.doPresent = true;
        fi.generated = true;
        fi.resource = interpolation.frames[k];
        fi.interpolationCompletedFenceValue = interpolation.listFence[ListForFrame(k, interpolation.lists)];
        fi.presentIndex = ++framesSentForPresentation;
    }

    // real
    if (!presentInterpolatedOnly && interpolation.realFrame.resource != nullptr)
    {
        FrameInfo& fi = entry.frames[frameCount++];
        fi.doPresent = true;
        fi.generated = false;
        fi.resource = interpolation.realFrame;
        fi.presentIndex = ++framesSentForPresentation;
    }

    entry.generatedFrames = produced;
    entry.frameCount = frameCount;
    entry.replacementBufferFenceSignal = framesSentForPresentation;
    entry.numFramesToPresent = UINT32(framesSentForPresentation - entry.numFramesSentForPresentationBase);

    outputSets[outputSetIndex].availabilityFenceValue =
        SetAvailableAfter(entry.numFramesSentForPresentationBase, produced);

    presentInfo.resetTimer = frameInterpolationResetCondition;
    presentInfo.scheduledInterpolations = entry;
    LeaveCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    // Set event to kick off async CPU present thread
    SetEvent(presentInfo.presentEvent);

    // hold the replacement object back until previous frame or interpolated is presented
    nextPresentWaitValue = entry.numFramesSentForPresentationBase;
}

bool MfgSwapChainDX12::verifyUiDuplicateResource()
{
    HRESULT hr = S_OK;

    ID3D12Device* device = nullptr;
    ID3D12Resource* uiResource = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);

    if ((0 == (presentInfo.uiCompositionFlags &
               FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING)) ||
        (uiResource == nullptr))
    {
        if (nullptr != uiReplacementBuffer.resource)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
            waitForFenceValue(presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE,
                              presentInfo.waitCallback);
            SafeRelease(uiReplacementBuffer.resource);
            uiReplacementBuffer = {};
        }
    }
    else
    {
        auto uiResourceDesc = uiResource->GetDesc();

        if (uiReplacementBuffer.resource != nullptr)
        {
            auto internalDesc = uiReplacementBuffer.resource->GetDesc();

            if (uiResourceDesc.Format != internalDesc.Format || uiResourceDesc.Width != internalDesc.Width ||
                uiResourceDesc.Height != internalDesc.Height)
            {
                waitForFenceValue(presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE,
                                  presentInfo.waitCallback);
                SafeRelease(uiReplacementBuffer.resource);
            }
        }

        if (uiReplacementBuffer.resource == nullptr)
        {
            if (SUCCEEDED(uiResource->GetDevice(IID_PPV_ARGS(&device))))
            {
                D3D12_HEAP_PROPERTIES heapProperties {};
                D3D12_HEAP_FLAGS heapFlags;
                uiResource->GetHeapProperties(&heapProperties, &heapFlags);

                heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
                heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

                // create game render output resource
                if (FAILED(device->CreateCommittedResource(&heapProperties, heapFlags, &uiResourceDesc,
                                                           ToD3D12State(presentInfo.currentUiSurface.state), nullptr,
                                                           IID_PPV_ARGS(&uiReplacementBuffer.resource))))
                {
                    hr |= E_FAIL;
                }
                else
                {
                    uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
                    totalUsageInBytes += resourceSize;
                    uiReplacementBuffer.resource->SetName(L"LSFG MFG Internal Ui Resource");
                }

                SafeRelease(device);
            }
        }
    }

    return SUCCEEDED(hr);
}

void MfgSwapChainDX12::copyUiResource()
{
    auto copyList = presentInfo.commandPool.get(presentInfo.gameQueue, L"uiResourceCopyList");
    if (copyList == nullptr)
        return;

    auto dx12List = copyList->reset();

    ID3D12Resource* dx12ResourceSrc = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);
    ID3D12Resource* dx12ResourceDst = uiReplacementBuffer.resource;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = dx12ResourceSrc;
    barriers[0].Transition.StateBefore = ToD3D12State(presentInfo.currentUiSurface.state);
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = dx12ResourceDst;
    barriers[1].Transition.StateBefore = ToD3D12State(presentInfo.currentUiSurface.state);
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    dx12List->ResourceBarrier(_countof(barriers), barriers);

    dx12List->CopyResource(dx12ResourceDst, dx12ResourceSrc);

    for (size_t i = 0; i < _countof(barriers); ++i)
    {
        D3D12_RESOURCE_STATES tmpStateBefore = barriers[i].Transition.StateBefore;
        barriers[i].Transition.StateBefore = barriers[i].Transition.StateAfter;
        barriers[i].Transition.StateAfter = tmpStateBefore;
    }

    dx12List->ResourceBarrier(_countof(barriers), barriers);

    copyList->execute(true);

    presentInfo.currentUiSurface.resource = nullptr;
}

// IDXGISwapChain1
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::Present(UINT SyncInterval, UINT Flags)
{
    const UINT64 previousFramesSentForPresentation = framesSentForPresentation;

    if (Flags & DXGI_PRESENT_TEST)
    {
        return presentInfo.swapChain->Present(SyncInterval, Flags);
    }

    applyFrameGenerationConfig(&nextFrameGenerationConfig, true);

    EnterCriticalSection(&criticalSection);

    const UINT currentBackBufferIndex = GetCurrentBackBufferIndex();

    // determine what present path to execute
    const bool runInterpolation = interpolationEnabled && frameGenerationCallback != nullptr;

    // Ensure presenter thread has signaled before applying any wait to the game queue
    waitForFenceValue(presentInfo.compositionFenceCPU, previousFramesSentForPresentation);
    if (previousFramesSentForPresentation != 0)
        presentInfo.gameQueue->Wait(presentInfo.compositionFenceGPU, previousFramesSentForPresentation);

    // Verify integrity of internal Ui resource
    if (verifyUiDuplicateResource())
    {
        if ((presentInfo.uiCompositionFlags &
             FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING) &&
            (presentInfo.currentUiSurface.resource != nullptr))
        {
            copyUiResource();
        }
    }

    previousFrameWasInterpolated = runInterpolation;
    if (runInterpolation)
    {
        WaitForSingleObject(presentInfo.interpolationEvent, INFINITE);

        presentInterpolated(SyncInterval, Flags);

        SignalFrameLatencyWaitableObject(&presentInfo, presentInfo.compositionFenceGPU, framesSentForPresentation);
    }
    else
    {
        // if no interpolation, then we copied directly to the swapchain. Render UI, present and be done
        presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
        presentInfo.presentQueue->Wait(presentInfo.gameFence, gameFenceValue);

        presentWithUiComposition(SyncInterval, Flags);

        SignalFrameLatencyWaitableObject(&presentInfo, presentInfo.presentFence, framesSentForPresentation);
    }

    replacementSwapBuffers[currentBackBufferIndex].availabilityFenceValue = framesSentForPresentation;

    presentCount++;
    outputSetIndex = OutputSetFor(presentCount);

    // update active backbuffer and block when no buffer is available
    replacementSwapBufferIndex = int(presentCount % gameBufferCount);

    LeaveCriticalSection(&criticalSection);

    waitForFenceValue(presentInfo.replacementBufferFence,
                      replacementSwapBuffers[replacementSwapBufferIndex].availabilityFenceValue, INFINITE,
                      presentInfo.waitCallback);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    if (ppSurface == nullptr)
        return E_POINTER;

    EnterCriticalSection(&criticalSection);

    HRESULT hr = E_FAIL;

    if (Buffer >= gameBufferCount)
    {
        hr = DXGI_ERROR_INVALID_CALL;
    }
    else if (riid == IID_ID3D12Resource || riid == IID_ID3D12Resource1 || riid == IID_ID3D12Resource2)
    {
        if (verifyBackbufferDuplicateResources() && replacementSwapBuffers[Buffer].resource != nullptr)
        {
            ID3D12Resource* pBuffer = replacementSwapBuffers[Buffer].resource;

            pBuffer->AddRef();
            *ppSurface = pBuffer;

            hr = S_OK;
        }
    }

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LSFG_MFG_UNFOLDABLE();
    return real()->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    LSFG_MFG_UNFOLDABLE();
    return real()->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = real()->GetDesc(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        // update values we changed
        pDesc->BufferCount = gameBufferCount;
        pDesc->Flags = gameFlags;
        pDesc->SwapEffect = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                          DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    destroyReplacementResources();

    EnterCriticalSection(&criticalSection);

    const UINT fiAdjustedFlags = getInterpolationEnabledSwapChainFlags(SwapChainFlags);

    // update params expected by the application
    if (BufferCount > 0)
    {
        gameBufferCount = std::min<UINT>(BufferCount, kMaxBufferCount);
    }
    gameFlags = SwapChainFlags;

    HRESULT hr = real()->ResizeBuffers(0 /* preserve count */, Width, Height, NewFormat, fiAdjustedFlags);

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    LSFG_MFG_UNFOLDABLE();
    return real()->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetContainingOutput(IDXGIOutput** ppOutput)
{
    HRESULT hr = DXGI_ERROR_INVALID_CALL;

    if (ppOutput)
    {
        *ppOutput = getMostRelevantOutputFromSwapChain(real());
        if (*ppOutput)
        {
            hr = S_OK;
        }
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return real()->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetLastPresentCount(UINT* pLastPresentCount)
{
    return real()->GetLastPresentCount(pLastPresentCount);
}

// IDXGISwapChain1
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = real()->GetDesc1(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        // update values we changed
        pDesc->BufferCount = gameBufferCount;
        pDesc->Flags = gameFlags;
        pDesc->SwapEffect = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    LSFG_MFG_UNFOLDABLE();
    return real()->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetHwnd(HWND* pHwnd) { return real()->GetHwnd(pHwnd); }

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return real()->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::Present1(UINT SyncInterval, UINT PresentFlags,
                                                     const DXGI_PRESENT_PARAMETERS*)
{
    LSFG_MFG_UNFOLDABLE();
    return Present(SyncInterval, PresentFlags);
}

BOOL STDMETHODCALLTYPE MfgSwapChainDX12::IsTemporaryMonoSupported(void) { return real()->IsTemporaryMonoSupported(); }

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return real()->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return real()->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return real()->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return real()->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return real()->GetRotation(pRotation);
}

// IDXGISwapChain2
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetSourceSize(UINT Width, UINT Height)
{
    return real()->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return real()->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetMaximumFrameLatency(UINT MaxLatency)
{
    INT newMaxLatency = INT(std::min(MaxLatency, gameBufferCount));
    UpdateFrameLatencyWaitableObject(&presentInfo, newMaxLatency - INT(gameMaximumFrameLatency));
    gameMaximumFrameLatency = UINT(newMaxLatency);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (pMaxLatency)
    {
        *pMaxLatency = gameMaximumFrameLatency;
    }

    return pMaxLatency ? S_OK : DXGI_ERROR_INVALID_CALL;
}

HANDLE STDMETHODCALLTYPE MfgSwapChainDX12::GetFrameLatencyWaitableObject(void)
{
    // Duplicate the internal semaphore handle for the application (matches DXGI behavior).
    // This creates an independent handle with separate reference counting, allowing the SDK
    // and application to manage handle lifetimes independently without race conditions during shutdown.
    // Note: The application must close this handle when finished. If called multiple times,
    // each returned handle is independent and must be closed separately by the application.
    HANDLE gameWaitableHandle = nullptr;
    if (replacementFrameLatencyWaitableObjectHandle != 0)
    {
        DuplicateHandle(GetCurrentProcess(), replacementFrameLatencyWaitableObjectHandle, GetCurrentProcess(),
                        &gameWaitableHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }

    return gameWaitableHandle;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->GetMatrixTransform(pMatrix);
}

// IDXGISwapChain3
UINT STDMETHODCALLTYPE MfgSwapChainDX12::GetCurrentBackBufferIndex(void)
{
    EnterCriticalSection(&criticalSection);

    UINT result = (UINT) replacementSwapBufferIndex;

    LeaveCriticalSection(&criticalSection);

    return result;
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                   UINT* pColorSpaceSupport)
{
    return real()->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
        break;
    default:
        break;
    }

    return real()->SetColorSpace1(ColorSpace);
}

// FFX asserts and returns S_OK without resizing. The real swapchain presents from this swapchain's own present queue,
// so the per-buffer queues and node masks do not apply to it: resize as ResizeBuffers does.
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                           DXGI_FORMAT Format, UINT SwapChainFlags, const UINT*,
                                                           IUnknown* const*)
{
    return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);
}

// IDXGISwapChain4
HRESULT STDMETHODCALLTYPE MfgSwapChainDX12::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (Size > 0 && pMetaData != nullptr && Type == DXGI_HDR_METADATA_TYPE_HDR10)
    {
        auto* HDR10MetaData = static_cast<DXGI_HDR_METADATA_HDR10*>(pMetaData);
        minLuminance = HDR10MetaData->MinMasteringLuminance / 10000.0f;
        maxLuminance = float(HDR10MetaData->MaxMasteringLuminance);
    }

    return real()->SetHDRMetaData(Type, Size, pMetaData);
}

} // namespace LsfgMfg
