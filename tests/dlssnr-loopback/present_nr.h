// Test-only controlled present host. No OptiScaler hook, upscaler, FG or game metadata.
#pragma once
#include <algorithm>
#include <array>
#include <thread>
#include "hud_fixture.h"
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Common.h"
#include "../../OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h"

namespace PresentNr
{
using ColdNr::Transition;
constexpr auto Srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto Uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

// All objects that can be referenced by a recording live in Host, outside Run's try block.
// An unknown completion ends this diagnostic process BEFORE any COM destructor can run.
struct Host
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 serial = 0;
    unsigned width = 0, height = 0, generation = 0, attempts = 0, successes = 0, presents = 0, drains = 0;
    unsigned controls = 0, captures = 0;
    const char* stage = "setup";
    bool hud = false;
    unsigned uiCorrection = 1;
    int switchTo = -1;
    std::string trial;
    Com<IDXGISwapChain3> swapchain;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    Com<ID3D12DescriptorHeap> cpuHeap, gpuHeap;
    Com<ID3D12Resource> upload, constants, before, after, source, proxy, original, answer, target, depth, motion;
    std::array<Com<ID3D12Resource>, 2> backs;
    Com<ID3D12Fence> gate;
    std::thread gateThread;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT64 bufferBytes = 0;
    UINT stride = 0;
    std::vector<unsigned char> pixels;
    NVSDK_NGX_Parameter* params = nullptr;
    void* feature = nullptr;
    ColdNr::Create create = nullptr;
    ColdNr::Evaluate evaluate = nullptr;
    using Extras = void (*)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned, unsigned,
                            unsigned, unsigned);
    Extras extras = nullptr;
    void (*release)(void*) = nullptr;
    PFN_DestroyParams destroy = nullptr;
    ColdNr::Shutdown shutdown = nullptr;
    int *lastInit = nullptr, *lastCreate = nullptr;
    wchar_t snippetPath[MAX_PATH] {};

    // A single registration and deadline. Old event wakes and removal are not completion.
    void Wait(UINT64 value)
    {
        const auto deadline = GetTickCount64() + 10000;
        bool registered = false;
        for (;;)
        {
            auto done = fence->GetCompletedValue();
            if (done == UINT64_MAX)
            {
                Check(device->GetDeviceRemovedReason(), "present device removal");
                Check(DXGI_ERROR_DEVICE_REMOVED, "present fence removal sentinel");
            }
            if (done >= value)
                return;
            auto now = GetTickCount64();
            if (now >= deadline)
                Check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "present fence original deadline");
            if (!registered)
            {
                Check(fence->SetEventOnCompletion(value, event), "present SetEventOnCompletion");
                registered = true;
            }
            auto wake = WaitForSingleObject(event, static_cast<DWORD>(deadline - now));
            if (wake == WAIT_FAILED)
            {
                const auto error = GetLastError();
                Check(HRESULT_FROM_WIN32(error), "present WAIT_FAILED");
            }
            Require(wake == WAIT_OBJECT_0 || wake == WAIT_TIMEOUT, "unexpected present fence wake");
            // Includes WAIT_TIMEOUT: one last removal/completion read before declaring timeout.
        }
    }

    void Begin()
    {
        Wait(serial);
        Check(alloc->Reset(), "present allocator Reset");
        Check(list->Reset(alloc, nullptr), "present list Reset");
    }

    void Submit(bool wait = true)
    {
        Check(list->Close(), "present list Close");
        ID3D12CommandList* lists[] = { list };
        queue->ExecuteCommandLists(1, lists);
        ++serial; // Pending before Signal; failure must never make this recording look idle.
        Check(queue->Signal(fence, serial), "present queue Signal");
        if (wait)
            Wait(serial);
    }

    ID3D12Resource* Buffer(UINT64 size, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = type;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* resource = nullptr;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                             : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&resource)),
              "present buffer allocation");
        return resource;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned slot, bool visible = false)
    {
        auto handle = (visible ? gpuHeap.p : cpuHeap.p)->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += slot * stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(unsigned slot)
    {
        auto handle = gpuHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += slot * stride;
        return handle;
    }
    void View(unsigned slot, ID3D12Resource* resource, bool uav)
    {
        if (uav)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            view.Format = resource->GetDesc().Format;
            device->CreateUnorderedAccessView(resource, nullptr, &view, Cpu(slot));
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view {};
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Format = resource->GetDesc().Format;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &view, Cpu(slot));
        }
        device->CopyDescriptorsSimple(1, Cpu(slot, true), Cpu(slot), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void Init(IDXGIFactory4* factory, HWND window)
    {
        stage = "cold NGX initialization";
        auto core = ColdNr::Load(L".\\_nvngx.dll");
        auto init = ColdNr::Export<ColdNr::InitExt>(core, "NVSDK_NGX_D3D12_Init_Ext");
        auto caps = ColdNr::Export<ColdNr::GetCaps>(core, "NVSDK_NGX_D3D12_GetCapabilityParameters");
        destroy = ColdNr::Export<PFN_DestroyParams>(core, "NVSDK_NGX_D3D12_DestroyParameters");
        shutdown = ColdNr::Export<ColdNr::Shutdown>(core, "NVSDK_NGX_D3D12_Shutdown1");
        NVSDK_NGX_FeatureCommonInfo info {};
        info.LoggingInfo = { ColdNr::CoreLog, NVSDK_NGX_LOGGING_LEVEL_ON, true };
        // Current production cold SDK fallback, unlike the historical --cold-nr sdk=0 probe.
        ColdNr::NgxResult("core Init_Ext", init(1337, L"", device, NVSDK_NGX_Version_API, &info));
        ColdNr::NgxResult("core capabilities", caps(&params));
        Require(params != nullptr, "null core capabilities");
        auto forwarder = ColdNr::Load(L".\\nvngx.dll_dlssnr.dll");
        Require(*ColdNr::Export<int*>(forwarder, "dlssnr_abi_version") == kDlssNrForwarderAbi,
                "forwarder ABI mismatch");
        ColdNr::Export<void (*)(int)>(forwarder, "dlssnr_set_host_abi")(kDlssNrForwarderAbi);
        auto probe = ColdNr::Export<void (*)(void*, const char*, float, int)>(forwarder, "dlssnr_call_probe_float");
        auto setSlot = ColdNr::Export<void (*)(int)>(forwarder, "dlssnr_call_set_float_slot");
        bool found = false;
        for (int slot : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float value = 0;
            probe(params, "DLSSNR.OptiScalerFloatProbe", 0.375f, slot);
            if (params->Get("DLSSNR.OptiScalerFloatProbe", &value) == NVSDK_NGX_Result_Success && value == 0.375f)
            {
                setSlot(slot);
                found = true;
                break;
            }
        }
        Require(found, "no verified core float setter");
        create = ColdNr::Export<ColdNr::Create>(forwarder, "dlssnr_call_create");
        evaluate = ColdNr::Export<ColdNr::Evaluate>(forwarder, "dlssnr_call_evaluate");
        if (hud)
            extras = ColdNr::Export<Extras>(forwarder, "dlssnr_call_set_extras");
        release = ColdNr::Export<void (*)(void*)>(forwarder, "dlssnr_call_release");
        lastInit = ColdNr::Export<int*>(forwarder, "dlssnr_call_last_init");
        lastCreate = ColdNr::Export<int*>(forwarder, "dlssnr_call_last_create");
        auto snippet = ColdNr::Load(L".\\nvngx_dlssnr.dll");
        const auto length = GetModuleFileNameW(snippet, snippetPath, MAX_PATH);
        Require(length != 0 && length < MAX_PATH, "snippet path length");

        stage = "real SDR swapchain";
        DXGI_SWAP_CHAIN_DESC1 desc {};
        desc.Width = 1280;
        desc.Height = 720;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Com<IDXGISwapChain1> sc;
        Check(factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &sc), "CreateSwapChainForHwnd");
        Check(sc->QueryInterface(IID_PPV_ARGS(&swapchain)), "IDXGISwapChain3");
        Check(swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709), "SDR color space");
        Check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        RECT rect { 0, 0, 1280, 720 };
        Require(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) != 0, "AdjustWindowRect");
        Require(SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0,
                "SetWindowPos");

        stage = "production composition shader";
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
        ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 5 };
        D3D12_ROOT_PARAMETER parameters[2] {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = { 2, ranges };
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 0;
        D3D12_STATIC_SAMPLER_DESC sampler {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rd { 2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        Com<ID3DBlob> blob, error;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "serialize NR root");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
              "NR root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root;
        pd.CS = { DlssNr_cso, sizeof(DlssNr_cso) };
        Check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)), "production NR compute pipeline");
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 16;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap)), "CPU views");
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpuHeap)), "GPU views");
        stride = device->GetDescriptorHandleIncrementSize(hd.Type);
        constants.p = Buffer(2 * sizeof(DlssNrConstants), D3D12_HEAP_TYPE_UPLOAD);
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "resize gate fence");
    }

    // Static synthetic SDR scene: gradients, textured ground, a brick wall, foliage silhouettes,
    // one-pixel detail, saturated patches and a HUD-like reticle. Exact bytes reused every frame.
    // This can reveal corruption and changes to fine detail; it is not a photorealism benchmark.
    void Fixture()
    {
        pixels.resize(static_cast<size_t>(width) * height * 4);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                unsigned r = 45 + 90 * y / height, g = 100 + 80 * y / height, b = 185 + 50 * y / height;
                if (y > height / 2)
                {
                    unsigned noise = ((x * 1103515245u + y * 12345u) >> 24) & 15;
                    r = 70 + noise;
                    g = 90 + noise;
                    b = 48 + noise;
                }
                if (x > width / 4 && x < 3 * width / 4 && y > height / 3 && y < 4 * height / 5)
                {
                    bool mortar = y % 24 < 2 || (x + ((y / 24) % 2) * 24) % 48 < 2;
                    r = mortar ? 55 : 160 + (x + y) % 12;
                    g = mortar ? 50 : 79;
                    b = mortar ? 44 : 48;
                }
                int dx = static_cast<int>(x) - static_cast<int>(width / 7);
                int dy = static_cast<int>(y) - static_cast<int>(height / 2);
                if (dx * dx + dy * dy < static_cast<int>(height * height / 25))
                {
                    r = 25 + (x * 7 + y * 11) % 30;
                    g = 60 + (x * 3 + y * 7) % 40;
                    b = 28;
                }
                if (y < height / 8 && x < width / 2)
                {
                    auto patch = x * 8 / (width / 2);
                    r = (patch & 1) ? 235 : 20;
                    g = (patch & 2) ? 235 : 20;
                    b = (patch & 4) ? 235 : 20;
                }
                if (x > 4 * width / 5 && y > height / 3 && y < 3 * height / 4)
                {
                    r = g = b = ((x + y) & 1) ? 215 : 35;
                }
                if ((x == width / 2 && y > height / 2 - 15 && y < height / 2 + 15) ||
                    (y == height / 2 && x > width / 2 - 15 && x < width / 2 + 15))
                    r = g = b = 255;
                auto index = (static_cast<size_t>(y) * width + x) * 4;
                pixels[index] = static_cast<unsigned char>(r);
                pixels[index + 1] = static_cast<unsigned char>(g);
                pixels[index + 2] = static_cast<unsigned char>(b);
                pixels[index + 3] = 255;
            }
    }

    void UploadFixture()
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead { 0, 0 };
        Check(upload->Map(0, &noRead, &mapped), "fixture Map");
        for (unsigned y = 0; y < height; ++y)
            memcpy(static_cast<unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                   pixels.data() + static_cast<size_t>(y) * width * 4, width * 4);
        upload->Unmap(0, nullptr);
    }

    void Allocate(unsigned w, unsigned h)
    {
        stage = "generation resources and clear-once guides";
        width = w;
        height = h;
        ++generation;
        for (unsigned i = 0; i < backs.size(); ++i)
            Check(swapchain->GetBuffer(i, IID_PPV_ARGS(&backs[i].p)), "GetBuffer");
        source.p = MakeTexture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_COPY_DEST);
        target.p = MakeTexture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, Uav);
        proxy.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        original.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        answer.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        depth.p = MakeTexture(device, w, h, DXGI_FORMAT_R32_FLOAT, true, Uav);
        motion.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, true, Uav);
        const auto desc = source->GetDesc();
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bufferBytes);
        upload.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_UPLOAD);
        before.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_READBACK);
        after.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_READBACK);
        Fixture();
        UploadFixture();
        ID3D12Resource* encode[] = { source, source, source, source, source, proxy, original };
        ID3D12Resource* resolve[] = { proxy, answer, original, motion, proxy, target, target };
        for (unsigned i = 0; i < 7; ++i)
        {
            View(i, encode[i], i >= 5);
            View(7 + i, resolve[i], i >= 5);
        }
        View(14, depth, true);
        View(15, motion, true);
        Begin();
        ID3D12DescriptorHeap* heaps[] = { gpuHeap.p };
        list->SetDescriptorHeaps(1, heaps);
        const float zero[4] {};
        list->ClearUnorderedAccessViewFloat(Gpu(14), Cpu(14), depth, zero, 0, nullptr);
        list->ClearUnorderedAccessViewFloat(Gpu(15), Cpu(15), motion, zero, 0, nullptr);
        Transition(list, depth, Uav, Srv);
        Transition(list, motion, Uav, Srv);
        Submit();
        Say("  generation=%u extent=%ux%u guides cleared once and completed serial=%llu\n", generation, w, h, serial);
        stage = "CreateFeature(18)";
        Begin();
        // Production defaults: intensity/local structure/tone=1, skin follows structure, mask=1.
        // Default 1 matches production; the opt-in HUD experiment varies only this create argument.
        if (hud)
            extras(params, 1, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        feature = create(snippetPath, L"", device, list, params, w, h, 0, 1, 0, 1, 1, 1, 1, uiCorrection);
        Submit();
        ColdNr::NgxResult("snippet Init", static_cast<unsigned>(*lastInit));
        ColdNr::NgxResult("CreateFeature(18)", static_cast<unsigned>(*lastCreate));
        Require(feature != nullptr, "null feature18");
        if (hud)
        {
            unsigned read = 99;
            ColdNr::NgxResult("UICorrection Get after create", params->Get("DLSSNR.UICorrection", &read));
            Require(read == uiCorrection, "create UI parameter did not round-trip");
            Say("  HUD trial=%s create_ui=%u\n", trial.c_str(), read);
        }
    }

    void Shader(unsigned slot)
    {
        ID3D12DescriptorHeap* heaps[] = { gpuHeap.p };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root);
        list->SetPipelineState(pipeline);
        list->SetComputeRootDescriptorTable(0, Gpu(slot * 7));
        list->SetComputeRootConstantBufferView(1, constants->GetGPUVirtualAddress() + slot * sizeof(DlssNrConstants));
        list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    }

    void Readback(ID3D12Resource* back, ID3D12Resource* buffer)
    {
        D3D12_TEXTURE_COPY_LOCATION from {};
        from.pResource = back;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to {};
        to.pResource = buffer;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }

    void Record(unsigned frame)
    {
        stage = "clean source -> copy-out -> encode -> NR -> resolve -> copy-back";
        Begin();
        if (hud)
        {
            // Match production's explicit clearing of absent UI/UIAlpha/Backbuffer inputs.
            extras(params, 1, nullptr, nullptr, nullptr, 0, 0, 0, 0);
            Fixture();
            HudFixture(pixels, width, height, frame);
            UploadFixture(); // Begin has already proved the upload is no longer in use.
            if (switchTo >= 0 && frame >= 8)
            {
                params->Set("DLSSNR.UICorrection", static_cast<unsigned>(switchTo));
                unsigned read = 99;
                ColdNr::NgxResult("UICorrection Get before evaluate", params->Get("DLSSNR.UICorrection", &read));
                Require(read == static_cast<unsigned>(switchTo), "evaluate UI parameter did not round-trip");
            }
        }
        DlssNrConstants c[2] {};
        for (auto& item : c)
        {
            item.Width = width;
            item.Height = height;
            item.Passthrough = 1; // Explicit SDR: no exposure texture, white-point law or tonemap.
            item.WhitePoint = item.ExposurePreMul = 1;
            item.TransferStrength = item.ColourStrength = 1;
            item.MaxRatio = 2;
            item.Transfer = 1;
            item.ApplyModel = frame != 0; // One negative composition control per generation.
        }
        c[0].Mode = DlssNrMode_Encode;
        c[1].Mode = DlssNrMode_Resolve;
        void* mapped = nullptr;
        const D3D12_RANGE noRead { 0, 0 };
        Check(constants->Map(0, &noRead, &mapped), "constants Map");
        memcpy(mapped, c, sizeof(c));
        constants->Unmap(0, nullptr);
        auto back = backs[swapchain->GetCurrentBackBufferIndex()].p;
        // We own every transition. The swapchain buffer rests in PRESENT, never assumed RT.
        Transition(list, back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from {}, to {};
        from.pResource = upload;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprint;
        to.pResource = back;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Transition(list, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Readback(back, before);
        list->CopyResource(source, back); // Full-resolution copy-out, no UAV on swapchain buffers.
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_DEST, Srv);
        Shader(0);
        Transition(list, proxy, Uav, Srv);
        Transition(list, original, Uav, Srv);
        ++attempts;
        auto result = evaluate(list, feature, params, proxy, depth, motion, answer, width, height, width, height, width,
                               height, 0, 0, 0, 0, 0, frame == 0 ? 1 : 0, 1, 0, 1, 1, 1, 1, 1, 1);
        Say("  EvaluateFeature(18) generation=%u frame=%u result=0x%08X\n", generation, frame,
            static_cast<unsigned>(result));
        // A failed evaluate may have recorded GPU work. Submit/drain before diagnosing; never copy back.
        if (result != 1)
        {
            Submit();
            ColdNr::NgxResult("EvaluateFeature(18)", static_cast<unsigned>(result));
        }
        ++successes;
        Transition(list, answer, Uav, Srv);
        Shader(1);
        Transition(list, target, Uav, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(back, target);
        Transition(list, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Readback(back, after);
        Transition(list, back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, Uav);
        Transition(list, proxy, Srv, Uav);
        Transition(list, original, Srv, Uav);
        Transition(list, answer, Srv, Uav);
        Transition(list, source, Srv, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    std::vector<unsigned char> Bytes(ID3D12Resource* buffer)
    {
        Wait(serial);
        void* mapped = nullptr;
        const D3D12_RANGE read { 0, static_cast<SIZE_T>(bufferBytes) };
        Check(buffer->Map(0, &read, &mapped), "readback Map");
        std::vector<unsigned char> result(pixels.size());
        for (unsigned y = 0; y < height; ++y)
            memcpy(result.data() + static_cast<size_t>(y) * width * 4,
                   static_cast<unsigned char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                   width * 4);
        const D3D12_RANGE noWrite { 0, 0 };
        buffer->Unmap(0, &noWrite);
        return result;
    }

    void Save(const std::string& name, const std::vector<unsigned char>& data)
    {
        FILE* file = nullptr;
        Require(fopen_s(&file, name.c_str(), "wb") == 0 && file != nullptr, "open PPM");
        bool ok = fprintf(file, "P6\n%u %u\n255\n", width, height) > 0;
        for (size_t i = 0; i < data.size(); i += 4)
            ok = fwrite(data.data() + i, 1, 3, file) == 3 && ok;
        ok = fclose(file) == 0 && ok;
        Require(ok, "write PPM");
    }

    void Inspect(unsigned frame)
    {
        auto a = Bytes(before), b = Bytes(after);
        Require(a == pixels, "backbuffer source is not the deterministic clean fixture");
        UINT64 delta = 0, changed = 0;
        unsigned maximum = 0;
        for (size_t i = 0; i < a.size(); ++i)
            if (i % 4 != 3)
            {
                unsigned d = static_cast<unsigned>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
                delta += d;
                changed += d != 0;
                maximum = std::max(maximum, d);
            }
        Say("  readback generation=%u frame=%u clean=exact rgb_changed=%llu/%zu MAE=%.6f max=%u\n", generation, frame,
            changed, a.size() / 4 * 3, static_cast<double>(delta) / (a.size() / 4 * 3), maximum);
        if (frame == 0)
        {
            Require(a == b, "ApplyModel=0 control changed source bytes");
            ++controls;
        }
        if (hud || frame == 0 || frame == 1 || frame == 15)
        {
            auto prefix = (hud ? trial : "g" + std::to_string(generation)) + "-f" + std::to_string(frame);
            Save(prefix + "-before.ppm", a);
            Save(prefix + "-after.ppm", b);
            ++captures;
        }
    }

    void Present()
    {
        stage = "completion before flip";
        Wait(serial); // Full recording, including both copies AND the readbacks, precedes this fence.
        const auto hr = swapchain->Present(1, 0);
        if (hr != S_OK)
            Say("  Present returned 0x%08lX (occlusion/status/failure)\n", static_cast<unsigned long>(hr));
        Require(hr == S_OK, "Present did not succeed normally");
        ++presents;
        PumpMessages();
    }

    void ReleaseGeneration()
    {
        Wait(serial); // BEFORE the first destructive operation, including the model's Release.
        if (feature)
        {
            release(feature);
            feature = nullptr;
        }
        for (auto* resource :
             { &upload, &before, &after, &source, &proxy, &original, &answer, &target, &depth, &motion })
        {
            if (*resource)
                (*resource)->Release();
            *resource = nullptr;
        }
        for (auto& back : backs)
        {
            if (back.p)
                back.p->Release();
            back.p = nullptr;
        }
    }

    void Resize(unsigned w, unsigned h)
    {
        stage = "resize entered with proven pending NR/copy-back recording";
        const auto completed = fence->GetCompletedValue();
        Require(completed != UINT64_MAX && completed < serial, "resize experiment did not enter with pending work");
        Say("  resize entry: generation=%u pending=%llu completed=%llu; drain before ANY release\n", generation, serial,
            completed);
        // Deliberately gate the real command queue: a fast GPU cannot make the drain test vacuous.
        // Only this thread releases the gate; it references Host-owned objects until joined.
        gateThread = std::thread(
            [this]()
            {
                Sleep(100);
                const auto hr = gate->Signal(drains + 1);
                if (FAILED(hr))
                {
                    Say("resize gate Signal failed 0x%08lX\n", static_cast<unsigned long>(hr));
                    ExitProcess(1);
                }
            });
        Wait(serial);
        gateThread.join();
        ++drains;
        Inspect(15);
        Present(); // The queued write-back is visibly presented only after proven completion.
        // Fence after Present as well, before releasing swapchain buffer references for resize.
        Check(queue->Signal(fence, ++serial), "post-Present drain Signal");
        Wait(serial);
        ReleaseGeneration();
        stage = "ResizeBuffers after completion";
        Check(swapchain->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
        Check(device->GetDeviceRemovedReason(), "device health after ResizeBuffers");
        Allocate(w, h);
    }
};

static void Run(IDXGIFactory4* factory, HWND window, ID3D12Device* device, ID3D12CommandQueue* queue,
                ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event,
                bool hud = false)
{
    Host host { device, queue, alloc, list, fence, event };
    try
    {
        Say("PRESENT-NR: controlled SDR source, real swapchain, production forwarder + composition bytecode\n");
        Say("  full resolution, one pass, zero guides, no confidence, no upscaler, no FG\n");
        Say(hud ? "  UICorrection A/B: per-trial create value, then optional evaluate-only switch\n"
                : "  UICorrection=1\n");
        host.hud = hud;
        host.Init(factory, window);
        if (hud)
        {
            const char* names[] = { "ui0-r0", "ui1-r0", "ui0-r1", "ui1-r1", "ui0-to1", "ui1-to0" };
            for (unsigned trial = 0; trial < 6; ++trial)
            {
                host.trial = names[trial];
                host.uiCorrection = trial % 2;
                host.switchTo = trial >= 4 ? 1 - static_cast<int>(host.uiCorrection) : -1;
                host.Allocate(1280, 720); // Independent feature + Reset + identical history for every trial.
                for (unsigned frame = 0; frame < 32; ++frame)
                {
                    host.Record(frame);
                    host.Submit();
                    host.Inspect(frame);
                    host.Present();
                }
                Check(queue->Signal(fence, ++host.serial), "HUD post-Present Signal");
                host.ReleaseGeneration();
            }
        }
        else
        {
            host.Allocate(1280, 720);
            for (unsigned generation = 0; generation < 3; ++generation)
            {
                for (unsigned frame = 0; frame < 16; ++frame)
                {
                    host.Record(frame);
                    if (frame == 15 && generation < 2)
                    {
                        Check(queue->Wait(host.gate, host.drains + 1), "enqueue resize gate");
                        host.Submit(false);
                        host.Resize(generation == 0 ? 960 : 1280, generation == 0 ? 540 : 720);
                    }
                    else
                    {
                        host.Submit();
                        host.Inspect(frame);
                        host.Present();
                    }
                }
            }
        }
        // Keep the final frame visible briefly; no additional NR evaluations or resubmissions.
        for (unsigned i = 0; i < 100; ++i)
        {
            PumpMessages();
            Sleep(20);
        }
        host.stage = "teardown after completion";
        Check(queue->Signal(fence, ++host.serial), "final post-Present Signal");
        host.Wait(host.serial);
        host.ReleaseGeneration();
        ColdNr::NgxResult("core DestroyParameters", host.destroy(host.params));
        unsigned remaining = 0;
        ColdNr::NgxResult("core Shutdown1", host.shutdown(device, &remaining));
        Check(device->GetDeviceRemovedReason(), "final device health");
        if (hud)
            Say("HUD-AB PASS: trials=6 attempts=%u successes=%u presents=%u controls=%u capture_pairs=%u\n",
                host.attempts, host.successes, host.presents, host.controls, host.captures);
        else
            Say("PRESENT-NR PASS: attempts=%u successes=%u presents=%u pending_resize_drains=%u controls=%u "
                "capture_pairs=%u\n",
                host.attempts, host.successes, host.presents, host.drains, host.controls, host.captures);
    }
    catch (const std::exception& error)
    {
        Say("PRESENT-NR FAIL: stage=%s: %s; serial=%llu attempts=%u successes=%u presents=%u\n", host.stage,
            error.what(), host.serial, host.attempts, host.successes, host.presents);
        ExitProcess(1); // Never unwind an unproven GPU recording, its thread, or its presenter.
    }
}
} // namespace PresentNr
