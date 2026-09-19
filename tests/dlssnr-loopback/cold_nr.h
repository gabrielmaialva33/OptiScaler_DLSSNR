// Test-only cold-start driver. Included after the harness's D3D12/logging helpers.
// No OptiScaler Init export, DLSS/RR feature, composition shader or present host is involved.
#pragma once

namespace ColdNr
{
using InitExt = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
                                     const NVSDK_NGX_FeatureCommonInfo*);
using GetCaps = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
// Private core ABI, as used by NVNGXProxy::ShutdownDx12Device (not the public SDK typedef).
using Shutdown = NVSDK_NGX_Result (*)(ID3D12Device*, unsigned int*);
using Create = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*, unsigned,
                          unsigned, int, float, int, float, float, float, int, int);
using Evaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                         ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                         unsigned, unsigned, unsigned, int, int, float, int, float, float, float, int, float, float);

template <typename T> T Export(HMODULE module, const char* name)
{
    auto address = GetProcAddress(module, name);
    if (!address)
    {
        Say("  missing export %s: Win32=%lu\n", name, GetLastError());
        throw std::runtime_error("required export unavailable");
    }
    return reinterpret_cast<T>(address);
}

static HMODULE Load(const wchar_t* path)
{
    wchar_t full[MAX_PATH] {};
    const auto length = GetFullPathNameW(path, MAX_PATH, full, nullptr);
    Require(length != 0 && length < MAX_PATH, "DLL path cannot be resolved");
    auto module = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        Say("  LoadLibraryExW(%ls): Win32=%lu\n", path, GetLastError());
        throw std::runtime_error("required runtime DLL unavailable");
    }
    wchar_t actual[MAX_PATH] {};
    GetModuleFileNameW(module, actual, MAX_PATH);
    Say("  loaded %ls\n", actual);
    return module;
}

static void NgxResult(const char* call, unsigned result)
{
    Say("  %s -> 0x%08X (%s)\n", call, result, result == 1 ? "success" : "failure");
    Require(result == 1, call);
}

static void NVSDK_CONV CoreLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature source)
{
    // Say's FILE streams are already open here; CRT stream writes are synchronized.
    Say("  NGX[%u]: %s\n", static_cast<unsigned>(source), message);
}

static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}

static void Submit(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, ID3D12Fence* fence,
                   HANDLE event, UINT64& serial)
{
    Check(list->Close(), "cold command list Close");
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    Check(queue->Signal(fence, ++serial), "cold queue Signal");
    const auto deadline = GetTickCount64() + 10000;
    for (;;)
    {
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            Say("  device removed: 0x%08lX\n", static_cast<unsigned long>(device->GetDeviceRemovedReason()));
            throw std::runtime_error("cold fence reports device removal");
        }
        if (completed >= serial)
            return;
        Check(fence->SetEventOnCompletion(serial, event), "cold fence SetEventOnCompletion");
        const auto now = GetTickCount64();
        if (now >= deadline)
            Check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "cold fence deadline");
        const auto wait = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
        if (wait == WAIT_FAILED)
        {
            const auto error = GetLastError();
            Say("  WaitForSingleObject failed: Win32=%lu\n", error);
            throw std::runtime_error("cold fence WAIT_FAILED");
        }
        // Even a successful wake does not prove this serial completed. Reread above, retaining
        // the original deadline. Timeout also gets one final completion/removal read.
    }
}

static void Run(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* alloc,
                ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event, unsigned width = 640,
                unsigned height = 360, int uiCorrection = 0, unsigned coreSdk = 0)
{
    // The size is a parameter because it turned out to be a variable. The production present host
    // reached CreateFeature(18) in Divinity: Original Sin 2 at 3440x1440 and was refused with
    // FAIL_PlatformError, on the same metadata this mode proves accepted at 640x360. Whether the
    // refusal is about the resolution is a question only a run at that resolution answers.
    constexpr unsigned iterations = 48;
    unsigned attempts = 0, successes = 0, completedEvaluations = 0;
    bool coreReady = false, featureReady = false;
    const char* stage = "load core";
    // Outside the try: on failure ExitProcess must happen BEFORE their destructors run.
    Com<ID3D12DescriptorHeap> cpuHeap, gpuHeap;
    try
    {
        Say("COLD-NR: standalone core + production forwarder; no DLSS or RR creation\n");
        Say("  fixture=%ux%u iterations=%u; no confidence resource or parameter\n", width, height, iterations);
        // State.h cold metadata at bd6d66d3, passed by NVNGXProxy::InitDx12 as it stood then.
        //
        // The SDK version is a parameter because production stopped agreeing with this default.
        // NVNGXProxy::SdkVersion now substitutes NVSDK_NGX_Version_API (0x15) when State's version is
        // zero -- that change was made precisely because zero is not a version -- so a cold NR run in
        // a title with no game NGX init initialises the core with 0x15 while this mode kept proving
        // 0. There is still no retry with a different ID or path that could hide a refusal.
        auto core = Load(L".\\_nvngx.dll");
        auto init = Export<InitExt>(core, "NVSDK_NGX_D3D12_Init_Ext");
        auto caps = Export<GetCaps>(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
        auto destroy = Export<PFN_DestroyParams>(core, "NVSDK_NGX_D3D12_DestroyParameters");
        auto shutdown = Export<Shutdown>(core, "NVSDK_NGX_D3D12_Shutdown1");
        NVSDK_NGX_FeatureCommonInfo info {};
        info.LoggingInfo = { CoreLog, NVSDK_NGX_LOGGING_LEVEL_ON, true };
        stage = "core Init_Ext";
        Say("  calling core Init_Ext: appId=1337 (decimal), dataPath=empty, sdk=0x%X, searchPaths=0\n", coreSdk);
        NgxResult(stage, init(1337, L"", device, static_cast<NVSDK_NGX_Version>(coreSdk), &info));
        coreReady = true;
        stage = "core GetCapabilityParameters";
        NVSDK_NGX_Parameter* params = nullptr;
        NgxResult(stage, caps(&params));
        Require(params != nullptr, "GetCapabilityParameters returned a null block");

        stage = "forwarder ABI and float setter";
        auto forwarder = Load(L".\\nvngx.dll_dlssnr.dll");
        Require(*Export<int*>(forwarder, "dlssnr_abi_version") == 2, "forwarder ABI must be 2");
        Export<void (*)(int)>(forwarder, "dlssnr_set_host_abi")(2);
        auto probe = Export<void (*)(void*, const char*, float, int)>(forwarder, "dlssnr_call_probe_float");
        auto setSlot = Export<void (*)(int)>(forwarder, "dlssnr_call_set_float_slot");
        auto create = Export<Create>(forwarder, "dlssnr_call_create");
        auto evaluate = Export<Evaluate>(forwarder, "dlssnr_call_evaluate");
        auto release = Export<void (*)(void*)>(forwarder, "dlssnr_call_release");
        auto lastInit = Export<int*>(forwarder, "dlssnr_call_last_init");
        auto lastCreate = Export<int*>(forwarder, "dlssnr_call_last_create");
        bool foundSlot = false;
        for (int slot : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float value = 0;
            probe(params, "DLSSNR.OptiScalerFloatProbe", 0.375f, slot);
            if (params->Get("DLSSNR.OptiScalerFloatProbe", &value) == NVSDK_NGX_Result_Success && value == 0.375f)
            {
                Say("  float setter round-trip: slot=%d\n", slot);
                setSlot(slot);
                foundSlot = true;
                break;
            }
        }
        Require(foundSlot, "no verified float setter in core capability block");

        stage = "allocate and clear guides";
        // Two copies of each descriptor: a CPU-only heap for Clear's CPU handle, and the
        // bound shader-visible heap for its GPU handle. All views explicitly typed.
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap)), "cold CPU descriptor heap");
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuHeap)), "cold GPU descriptor heap");
        const auto stride = device->GetDescriptorHandleIncrementSize(hd.Type);
        Frame f;
        f.depth =
            MakeTexture(device, width, height, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        f.motion =
            MakeTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        f.colour = MakeTexture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        f.output = MakeTexture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource* inputs[] = { f.depth, f.motion, f.colour };
        D3D12_CPU_DESCRIPTOR_HANDLE cpu[3], visibleCpu[3];
        D3D12_GPU_DESCRIPTOR_HANDLE gpu[3];
        for (unsigned i = 0; i < 3; ++i)
        {
            cpu[i] = cpuHeap->GetCPUDescriptorHandleForHeapStart();
            visibleCpu[i] = gpuHeap->GetCPUDescriptorHandleForHeapStart();
            gpu[i] = gpuHeap->GetGPUDescriptorHandleForHeapStart();
            cpu[i].ptr += i * stride;
            visibleCpu[i].ptr += i * stride;
            gpu[i].ptr += i * stride;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            view.Format = inputs[i]->GetDesc().Format;
            device->CreateUnorderedAccessView(inputs[i], nullptr, &view, cpu[i]);
            device->CopyDescriptorsSimple(1, visibleCpu[i], cpu[i], hd.Type);
        }
        UINT64 serial = 0;
        auto begin = [&]()
        {
            Check(alloc->Reset(), "cold allocator Reset");
            Check(list->Reset(alloc, nullptr), "cold list Reset");
            ID3D12DescriptorHeap* heaps[] = { gpuHeap.p };
            list->SetDescriptorHeaps(1, heaps);
        };
        const auto srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        begin();
        const float zero[4] {};
        for (unsigned i = 0; i < 2; ++i)
        {
            list->ClearUnorderedAccessViewFloat(gpu[i], cpu[i], inputs[i], zero, 0, nullptr);
            Transition(list, inputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
        }
        Submit(device, queue, list, fence, event, serial);
        Say("  guides cleared ONCE to zero and fence-complete (serial=%llu)\n", serial);

        stage = "load snippet";
        // Resolve loading/export failures here: the production forwarder exposes the last NGX
        // results, but loadSnippet failures have no NGX result to report. Do not call these raw
        // exports from the EXE (the model requires the forwarder as its caller).
        auto snippet = Load(L".\\nvngx_dlssnr.dll");
        for (const char* name : { "NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_CreateFeature",
                                  "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_ReleaseFeature" })
            Export<FARPROC>(snippet, name);
        wchar_t snippetPath[MAX_PATH] {};
        const auto pathLength = GetModuleFileNameW(snippet, snippetPath, MAX_PATH);
        Require(pathLength != 0 && pathLength < MAX_PATH, "snippet module path cannot be resolved");
        // Same reporter, same order, same prefix as production writes. The point is a diff.
        DlssNr::Identity::ReportAll([](void*, const char* line) { Say("%s\n", line); }, nullptr, "harness", device,
                                    queue);

        stage = "snippet Init_Ext / CreateFeature(18)";
        begin();
        // The last argument is UI correction. This mode has always passed 0; production passes 1, and
        // that is the one create argument the two disagree on once the data path is accounted for
        // (it is only ever written by a game's own NGX init, so in a title with none it is empty on
        // both sides). Parameterised to find out whether it is what feature 18 is refusing.
        Say("  calling production dlssnr_call_create (feature 18 only), uiCorrection=%d\n", uiCorrection);
        auto feature =
            create(snippetPath, L"", device, list, params, width, height, 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1,
                   uiCorrection);
        Say("  snippet Init_Ext -> 0x%08X; CreateFeature(18) -> 0x%08X; handle=%p\n", static_cast<unsigned>(*lastInit),
            static_cast<unsigned>(*lastCreate), feature);
        // Create can record work even on refusal; finish the recording before interpreting it.
        Submit(device, queue, list, fence, event, serial);
        NgxResult("snippet Init_Ext", static_cast<unsigned>(*lastInit));
        NgxResult("snippet CreateFeature(18)", static_cast<unsigned>(*lastCreate));
        Require(feature != nullptr, "CreateFeature(18) succeeded with a null handle");
        featureReady = true;

        stage = "EvaluateFeature(18)";
        for (unsigned i = 0; i < iterations; ++i)
        {
            begin();
            if (i != 0)
                Transition(list, f.colour, srv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // A known, exactly representable colour changes with the iteration. It is written
            // from scratch, never copied from model output. This is not a quality fixture.
            const float colour[] = { 0.125f + (i % 4) * 0.125f, 0.25f, 0.5f, 1.0f };
            list->ClearUnorderedAccessViewFloat(gpu[2], cpu[2], f.colour, colour, 0, nullptr);
            Transition(list, f.colour, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, srv);
            ++attempts;
            Say("  calling EvaluateFeature(18) iteration=%u reset=%u\n", attempts, i == 0 ? 1u : 0u);
            const auto result =
                evaluate(list, feature, params, f.colour, f.depth, f.motion, f.output, width, height, width, height,
                         width, height, 0, 0, 0, 0, 0, i == 0 ? 1 : 0, 0.5f, 0, 0.5f, 0.5f, 0.5f, 1, 1.0f, 1.0f);
            Say("  EvaluateFeature(18)[%u] -> 0x%08X\n", attempts, static_cast<unsigned>(result));
            successes += result == 1;
            // Order output writes between evaluations; never feed this output to the next call.
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = f.output;
            list->ResourceBarrier(1, &b);
            Submit(device, queue, list, fence, event, serial);
            ++completedEvaluations;
            NgxResult("EvaluateFeature(18)", static_cast<unsigned>(result));
            PumpMessages();
        }
        stage = "teardown after proven completion";
        release(feature); // Production forwarder returns void; no release result is exposed.
        f.Release();
        NgxResult("core DestroyParameters", destroy(params));
        unsigned remaining = 0;
        NgxResult("core Shutdown1", shutdown(device, &remaining));
        Say("COLD-NR PASS: core_init=1 feature18=1 attempts=%u successes=%u gpu_completed=%u\n", attempts, successes,
            completedEvaluations);
    }
    catch (const std::exception& e)
    {
        Say("COLD-NR FAIL: stage=%s: %s; core_init=%u feature18=%u attempts=%u successes=%u gpu_completed=%u\n", stage,
            e.what(), coreReady ? 1u : 0u, featureReady ? 1u : 0u, attempts, successes, completedEvaluations);
        // A diagnostic process must not unwind the outer device/allocator wrappers after an
        // unproven submission. End this process, leaving runtime reclamation to process teardown.
        ExitProcess(1);
    }
}
} // namespace ColdNr

static void RunColdNr(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* alloc,
                      ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event, unsigned width = 640,
                      unsigned height = 360, int uiCorrection = 0, unsigned coreSdk = 0)
{
    ColdNr::Run(device, queue, alloc, list, fence, event, width, height, uiCorrection, coreSdk);
}
