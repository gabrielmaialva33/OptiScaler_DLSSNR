// Test-only controlled present host. No OptiScaler hook, upscaler, FG or game metadata.
#pragma once
#include <algorithm>
#include <array>
#include <thread>
#include <map>
#include <tuple>
#include <cmath>
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
    bool composition = false;
    float transferStrength = 1, colourStrength = 1;
    unsigned reversibleMode = 0;
    unsigned uiCorrection = 1;
    // Baked in at create time. Every trial releases and recreates the model, so varying it per trial
    // is honest; varying it between evaluations of one feature would not be.
    unsigned style = 0;
    // Frames per generation. Sixteen is enough to show transport and a stable cost, and may be too
    // few for the model's temporal state to settle -- ngxGym runs 1800 for its colour scenarios.
    // A comparison that depends on settled history has to say which number it used.
    unsigned framesPerGeneration = 16;
    // Linear, non-passthrough input. The SDR fixture reaches the encode through the swapchain buffer,
    // which is eight bits and tone-mapped, so Passthrough must be 1 and the proxy modes never run.
    // In this mode a linear copy of the same scene is uploaded straight into `source` instead, and the
    // backbuffer still carries the tone-mapped one for presentation and for the before image.
    bool linearInput = false;
    std::vector<float> linearPixels;
    // First linear ApplyModel=0 frame PER EXTENT. The sweep runs 1280x720, 960x540 and 1280x720 in
    // every trial, so one global control compares images of different sizes -- which is exactly the
    // mistake this key exists to prevent.
    std::map<std::pair<unsigned, unsigned>, std::vector<unsigned char>> linearControl;
    Com<ID3D12Resource> linearUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT linearFootprint {};
    UINT64 linearBytes = 0;
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

    // GPU timing. Four timestamps bracket the same spans production reports, so a number from here
    // and a number from a game's log mean the same thing: model is the NGX evaluate alone, outside is
    // our encode and resolve. The harness's own fixture upload and readbacks are test scaffolding and
    // are deliberately outside both. Reading is trivial here only because every Submit already waits.
    //
    // LOCK THE GPU CLOCKS BEFORE BELIEVING ANY NUMBER FROM HERE. This harness runs sixteen frames,
    // pauses, recreates and repeats: a low duty cycle that leaves the GPU dropping to its idle power
    // state between trials, so the clock is ramping through most of the samples. Unlocked, three
    // points with IDENTICAL settings measured 2.84, 14.09 and 15.63 ms; locked, the same three
    // measured 3.615, 3.553 and 3.561. A game does not have this problem because its load is
    // sustained, which is why the in-game numbers were stable and these were not.
    //
    //     sudo nvidia-smi -lgc 2100,2100 && sudo nvidia-smi -lmc 10501   # before
    //     sudo nvidia-smi -rgc && sudo nvidia-smi -rmc                   # after
    //
    // The composition sweep carries "mode0", "composed", "repeat" and "style0" as the SAME
    // configuration for exactly this reason. Read those four first. If they disagree, nothing else in
    // the table means anything, and that is a fact about the machine rather than about the settings.
    //
    // A SECOND LIMIT, structural rather than statistical: this fixture is SDR and runs with
    // Passthrough=1, and the encode returns before any ReversibleMode branch in that case
    // (dlssnr.hlsl:658-662, whose own comment says the branch is "reached only when the frame is not
    // passthrough"). So ReversibleMode 0, 1 and 3 are IDENTICAL here by construction -- their effect
    // lives in the encode -- while 2 and 4 differ only because theirs lives in the resolve. Measuring
    // the proxy modes needs a linear, non-passthrough fixture that this harness does not have. Any
    // parameter whose effect is in the encode is invisible to this sweep; do not read its flat
    // numbers as "the setting does nothing".
    Com<ID3D12QueryHeap> timestamps;
    Com<ID3D12Resource> timings;
    UINT64 gpuHz = 0;
    bool timed = false; // Set per recording; a torn recording (failed evaluate, resize) reports nothing.
    struct Sample
    {
        std::string trial;
        unsigned generation, frame, width, height;
        double modelMs, outsideMs;
    };
    std::vector<Sample> samples;
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

        D3D12_QUERY_HEAP_DESC qd {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 4;
        Check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&timestamps)), "timestamp query heap");
        timings.p = Buffer(4 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
        // A DIRECT queue's own frequency. Do not reuse one read from another queue.
        Check(queue->GetTimestampFrequency(&gpuHz), "GetTimestampFrequency");
    }

    // Static synthetic SDR scene: gradients, textured ground, a brick wall, foliage silhouettes,
    // one-pixel detail, saturated patches and a HUD-like reticle. Exact bytes reused every frame.
    // This can reveal corruption and changes to fine detail; it is not a photorealism benchmark.
    // The same scene as Fixture(), in linear light, with WhitePoint at 1.0 so display white is 1.0 and
    // the bright patches sit above it. Those highlights are the entire reason this mode exists: the
    // proxy modes differ in how much gradation they preserve above white, and an SDR fixture has none
    // to preserve. This is a procedural scene, not a photometric reference -- it can show that the
    // modes differ and by how much, and cannot say which looks better.
    void FixtureLinear()
    {
        linearPixels.resize(static_cast<size_t>(width) * height * 4);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                const size_t index = (static_cast<size_t>(y) * width + x) * 4;
                const size_t sdr = (static_cast<size_t>(y) * width + x) * 4;
                // Start from the tone-mapped scene decoded back to linear, so both fixtures show the
                // same picture, then push the emissive regions above white.
                const auto decode = [](unsigned char v)
                {
                    const float c = v / 255.0f;
                    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
                };
                float r = decode(pixels[sdr]), g = decode(pixels[sdr + 1]), b = decode(pixels[sdr + 2]);
                // Sky gradient carries a mild overbright; the saturated bar strip and a lamp disc go
                // well past white, which is where the knee and the two reversible curves disagree.
                const float sky = 1.0f + 0.6f * (1.0f - static_cast<float>(y) / height);
                if (y < height / 8 && x < width / 2)
                {
                    r *= 3.5f;
                    g *= 3.5f;
                    b *= 3.5f;
                }
                else if (y < height / 3)
                {
                    r *= sky;
                    g *= sky;
                    b *= sky;
                }
                const float dx = static_cast<float>(x) - width * 0.82f, dy = static_cast<float>(y) - height * 0.22f;
                if (dx * dx + dy * dy < (width * 0.035f) * (width * 0.035f))
                {
                    r += 8.0f;
                    g += 7.4f;
                    b += 5.2f;
                }
                linearPixels[index] = r;
                linearPixels[index + 1] = g;
                linearPixels[index + 2] = b;
                linearPixels[index + 3] = 1.0f;
            }
    }

    void UploadLinear()
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead { 0, 0 };
        Check(linearUpload->Map(0, &noRead, &mapped), "linear upload Map");
        for (unsigned y = 0; y < height; ++y)
            memcpy(static_cast<unsigned char*>(mapped) + linearFootprint.Offset +
                       y * linearFootprint.Footprint.RowPitch,
                   linearPixels.data() + static_cast<size_t>(y) * width * 4,
                   static_cast<size_t>(width) * 4 * sizeof(float));
        linearUpload->Unmap(0, nullptr);
    }

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
        source.p = MakeTexture(device, w, h, linearInput ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
                               false, D3D12_RESOURCE_STATE_COPY_DEST);
        target.p = MakeTexture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, Uav);
        proxy.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        original.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        answer.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, Uav);
        depth.p = MakeTexture(device, w, h, DXGI_FORMAT_R32_FLOAT, true, Uav);
        motion.p = MakeTexture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, true, Uav);
        // This footprint describes the EIGHT-BIT backbuffer upload and the readbacks, so it must be
        // taken from that format and not from `source`, which the linear mode widens to four floats.
        // Deriving it from `source` silently repitched every row of the fixture upload.
        auto desc = source->GetDesc();
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bufferBytes);
        upload.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_UPLOAD);
        before.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_READBACK);
        after.p = Buffer(bufferBytes, D3D12_HEAP_TYPE_READBACK);
        Fixture();
        UploadFixture();
        if (linearInput)
        {
            // Derived from `pixels`, so it has to come after Fixture() has filled it for this extent.
            const auto linearDesc = source->GetDesc();
            device->GetCopyableFootprints(&linearDesc, 0, 1, 0, &linearFootprint, nullptr, nullptr, &linearBytes);
            linearUpload.p = Buffer(linearBytes, D3D12_HEAP_TYPE_UPLOAD);
            FixtureLinear();
            UploadLinear();
        }
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
        // SkinStructure is -1, not 1. Minus one means "follow local structure", which is the model's
        // own default; one is an explicit strength. Passing 1 here made every measurement taken with
        // this harness a measurement of a configuration no user runs. Found by reading NIGos/ngxGym's
        // scenario files (MIT), which set NRSkinStructure=-1 throughout.
        feature = create(snippetPath, L"", device, list, params, w, h, 0, 1, static_cast<int>(style), 1, 1, -1, 1,
                         uiCorrection);
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
            // Passthrough 1 is the explicit SDR contract: no exposure texture, white-point law or
            // tonemap. Clearing it is the whole point of the linear mode -- it is what lets the encode
            // reach its proxy branches at all.
            item.Passthrough = linearInput ? 0 : 1;
            item.WhitePoint = item.ExposurePreMul = 1;
            item.TransferStrength = transferStrength;
            item.ColourStrength = colourStrength;
            item.ReversibleMode = reversibleMode;
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
        if (linearInput)
        {
            // The encode reads linear light that never passed through the eight-bit backbuffer. The
            // backbuffer still received the tone-mapped fixture above, so presentation and the before
            // image are unchanged and every existing comparison still means what it meant.
            D3D12_TEXTURE_COPY_LOCATION lin {}, linTo {};
            lin.pResource = linearUpload;
            lin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            lin.PlacedFootprint = linearFootprint;
            linTo.pResource = source;
            linTo.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&linTo, 0, 0, 0, &lin, nullptr);
        }
        else
            list->CopyResource(source, back); // Full-resolution copy-out, no UAV on swapchain buffers.
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_DEST, Srv);
        timed = false;
        list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        Shader(0);
        Transition(list, proxy, Uav, Srv);
        Transition(list, original, Uav, Srv);
        list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
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
        list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        Transition(list, answer, Uav, Srv);
        Shader(1);
        list->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        list->ResolveQueryData(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, timings, 0);
        timed = true;
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

    // Called only after a Submit that waited. Four resolved timestamps on one queue, so the
    // differences are directly comparable; no cross-queue arithmetic is attempted.
    void ReadTiming(unsigned frame)
    {
        if (!timed || gpuHz == 0)
            return;
        void* mapped = nullptr;
        const D3D12_RANGE read { 0, 4 * sizeof(UINT64) };
        Check(timings->Map(0, &read, &mapped), "timings Map");
        UINT64 t[4] {};
        memcpy(t, mapped, sizeof(t));
        const D3D12_RANGE noWrite { 0, 0 };
        timings->Unmap(0, &noWrite);
        // A non-monotonic quadruple is a broken sample, not a fast one. Drop it rather than report it.
        if (!(t[0] <= t[1] && t[1] <= t[2] && t[2] <= t[3]))
        {
            Say("  timing sample dropped (non-monotonic) generation=%u frame=%u\n", generation, frame);
            return;
        }
        const auto ms = [&](UINT64 a, UINT64 b) { return (b - a) * 1000.0 / static_cast<double>(gpuHz); };
        samples.push_back({ trial, generation, frame, width, height, ms(t[1], t[2]), ms(t[0], t[1]) + ms(t[2], t[3]) });
    }

    void ReportTiming()
    {
        if (samples.empty())
        {
            Say("TIMING: no samples\n");
            return;
        }
        // Group by trial AND model extent. A median across configurations, or across resolutions,
        // is not a measurement of either.
        std::vector<std::tuple<std::string, unsigned, unsigned>> groups;
        for (const auto& s : samples)
        {
            auto key = std::make_tuple(s.trial, s.width, s.height);
            if (std::find(groups.begin(), groups.end(), key) == groups.end())
                groups.push_back(key);
        }
        for (auto& [trialName, w, h] : groups)
        {
            std::vector<double> model, outside;
            for (const auto& s : samples)
                if (s.trial == trialName && s.width == w && s.height == h)
                {
                    model.push_back(s.modelMs);
                    outside.push_back(s.outsideMs);
                }
            std::sort(model.begin(), model.end());
            std::sort(outside.begin(), outside.end());
            const auto pick = [](std::vector<double>& v, double q) { return v[static_cast<size_t>(v.size() * q)]; };
            Say("TIMING trial=%s model=%ux%u n=%zu model_ms p10=%.3f median=%.3f p90=%.3f "
                "outside_model_ms median=%.3f\n",
                trialName.empty() ? "-" : trialName.c_str(), w, h, model.size(), pick(model, 0.1),
                model[model.size() / 2], pick(model, 0.9), outside[outside.size() / 2]);
        }
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
            if (linearInput)
            {
                // The SDR control asserts the output equals the backbuffer, which only holds while the
                // pipeline reads the backbuffer. Here it reads a linear source instead, so an
                // ApplyModel=0 frame is that linear light passed through and written to eight bits --
                // legitimately not the tone-mapped fixture. The invariant that still bites is
                // determinism: the clean frame must not depend on which proxy mode is selected, since
                // the model's edit is the only thing a mode is allowed to change.
                auto& control = linearControl[{ width, height }];
                if (control.empty())
                    control = b;
                else
                {
                    // RGB only, matching the MAE loop above, which skips alpha on purpose. The encode
                    // writes opaque alpha for every mode but 0 (dlssnr.hlsl: alpha = gReversibleMode
                    // != 0 ? 1.0 : source.a), so an alpha difference here is the documented behaviour
                    // rather than a broken control. It is reported instead of asserted, so that
                    // hiding it is a choice on the record.
                    size_t rgb = 0, alpha = 0;
                    for (size_t i = 0; i < b.size(); ++i)
                        (i % 4 == 3 ? alpha : rgb) += (b[i] != control[i]);
                    if (alpha != 0)
                        Say("  linear control: %zu alpha bytes differ from the first trial (expected for "
                            "ReversibleMode != 0)\n",
                            alpha);
                    if (rgb != 0)
                    {
                        // Save both sides before failing. A control that fails without leaving the two
                        // images behind cannot be diagnosed without another full run.
                        Save(trial + "-control-observed.ppm", b);
                        Save("linear-control-expected.ppm", control);
                        Say("  linear control: %zu RGB bytes differ; both images saved\n", rgb);
                    }
                    Require(rgb == 0, "ApplyModel=0 control differs in RGB between linear trials");
                }
            }
            else
                Require(a == b, "ApplyModel=0 control changed source bytes");
            ++controls;
        }
        // First, second and last. At the default sixteen the last is frame 15, so the names the
        // analyzer anchors on are unchanged; a longer run moves that name and needs the analyzer told.
        if (hud || frame == 0 || frame == 1 || frame == framesPerGeneration - 1)
        {
            auto prefix = (hud ? trial : (composition ? trial + "-" : "") + "g" + std::to_string(generation)) + "-f" +
                          std::to_string(frame);
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
    void OriginalSequence()
    {
        Allocate(1280, 720);
        for (unsigned extentStep = 0; extentStep < 3; ++extentStep)
        {
            for (unsigned frame = 0; frame < framesPerGeneration; ++frame)
            {
                Record(frame);
                if (frame == framesPerGeneration - 1 && extentStep < 2)
                {
                    Check(queue->Wait(gate, drains + 1), "enqueue resize gate");
                    Submit(false);
                    Resize(extentStep == 0 ? 960 : 1280, extentStep == 0 ? 540 : 720);
                }
                else
                {
                    Submit();
                    ReadTiming(frame);
                    Inspect(frame);
                    Present();
                }
            }
        }
    }
};

static void Run(IDXGIFactory4* factory, HWND window, ID3D12Device* device, ID3D12CommandQueue* queue,
                ID3D12CommandAllocator* alloc, ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event,
                bool hud = false, bool composition = false)
{
    Host host { device, queue, alloc, list, fence, event };
    try
    {
        Say("PRESENT-NR: controlled SDR source, real swapchain, production forwarder + composition bytecode\n");
        Say("  full resolution, one pass, zero guides, no confidence, no upscaler, no FG\n");
        Say(hud ? "  UICorrection A/B: per-trial create value, then optional evaluate-only switch\n"
                : "  UICorrection=1\n");
        host.hud = hud;
        host.composition = composition;
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
        else if (composition)
        {
            struct Point
            {
                const char* name;
                unsigned mode;
                float transfer;
                float colour;
                unsigned style;
            };
            const Point points[] = { { "mode0", 0, 1, 1, 0 },   { "mode1", 1, 1, 1, 0 },    { "mode3", 3, 1, 1, 0 },
                                     { "mode4", 4, 1, 1, 0 },   { "composed", 0, 1, 1, 0 }, { "direct", 2, 1, 1, 0 },
                                     { "t0", 0, 0, 1, 0 },      { "t25", 0, .25f, 1, 0 },   { "t50", 0, .5f, 1, 0 },
                                     { "t75", 0, .75f, 1, 0 },  { "c0", 0, 1, 0, 0 },       { "c25", 0, 1, .25f, 0 },
                                     { "c50", 0, 1, .5f, 0 },   { "c75", 0, 1, .75f, 0 },   { "c125", 0, 1, 1.25f, 0 },
                                     { "c150", 0, 1, 1.5f, 0 }, { "repeat", 0, 1, 1, 0 },   { "style0", 0, 1, 1, 0 },
                                     { "style1", 0, 1, 1, 1 },  { "style2", 0, 1, 1, 2 } };
            // The four proxy modes again, this time with a linear non-passthrough source so the encode
            // actually reaches them. lin0 repeats lin0b as the control for this half of the table.
            const Point linearPoints[] = { { "lin0", 0, 1, 1, 0 },
                                           { "lin1", 1, 1, 1, 0 },
                                           { "lin3", 3, 1, 1, 0 },
                                           { "lin4", 4, 1, 1, 0 },
                                           { "lin0b", 0, 1, 1, 0 } };
            for (const auto& point : points)
            {
                host.trial = point.name;
                host.reversibleMode = point.mode;
                host.transferStrength = point.transfer;
                host.colourStrength = point.colour;
                host.style = point.style;
                host.generation = 0;
                Say("COMPOSITION-POINT name=%s mode=%u transfer=%.2f colour=%.2f style=%u\n", point.name, point.mode,
                    point.transfer, point.colour, point.style);
                host.OriginalSequence();
                Check(queue->Signal(fence, ++host.serial), "composition post-Present Signal");
                host.ReleaseGeneration();
            }
            host.linearInput = true;
            for (const auto& point : linearPoints)
            {
                host.trial = point.name;
                host.reversibleMode = point.mode;
                host.transferStrength = point.transfer;
                host.colourStrength = point.colour;
                host.style = point.style;
                host.generation = 0;
                Say("COMPOSITION-POINT name=%s mode=%u transfer=%.2f colour=%.2f style=%u\n", point.name, point.mode,
                    point.transfer, point.colour, point.style);
                host.OriginalSequence();
                Check(queue->Signal(fence, ++host.serial), "linear composition post-Present Signal");
                host.ReleaseGeneration();
            }
            host.linearInput = false;
        }
        else
        {
            host.OriginalSequence();
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
        else if (composition)
            Say("COMPOSITION-AB PASS: trials=25 attempts=%u successes=%u presents=%u pending_resize_drains=%u "
                "controls=%u capture_pairs=%u\n",
                host.attempts, host.successes, host.presents, host.drains, host.controls, host.captures);
        else
            Say("PRESENT-NR PASS: attempts=%u successes=%u presents=%u pending_resize_drains=%u controls=%u "
                "capture_pairs=%u\n",
                host.attempts, host.successes, host.presents, host.drains, host.controls, host.captures);
        host.ReportTiming();
    }
    catch (const std::exception& error)
    {
        Say("PRESENT-NR FAIL: stage=%s: %s; serial=%llu attempts=%u successes=%u presents=%u\n", host.stage,
            error.what(), host.serial, host.attempts, host.successes, host.presents);
        ExitProcess(1); // Never unwind an unproven GPU recording, its thread, or its presenter.
    }
}
} // namespace PresentNr
