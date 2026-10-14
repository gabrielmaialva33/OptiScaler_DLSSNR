// LSFG study: drives Lsfg::Context_Dx12 (retoque/lsfg/LsfgFrameGen_Dx12.cpp) the way Lsfg::FfxDispatch does inside
// FSR-FG's callback, outside any game: a presented frame in PRESENT state copied in, FFX's two alternating outputs
// in UNORDERED_ACCESS written, the context's own GPU timestamps read. Two jobs:
// - identity: the bytes of every generated frame of a moving sequence, as FNV-1a hashes, so two builds or two
//   schedules can be compared byte for byte;
// - timing: the context's own GPU time per generated frame (what the in-game summary line reports), at whatever
//   clocks a busy GPU runs at, for each schedule.
// Built against the worktree's retoque/ or against a snapshot of an older revision (-DLSFG_BASELINE: a Record
// without the schedule argument). Shaders are the user's converted DXIL; nothing derived from LSFG is in git.
#include "pch.h"

#include <lsfg/LsfgFrameGen_Dx12.h>
#include <lsfg/LsfgShaderSource.h>

#include <dxgi1_6.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
[[noreturn]] void Fail(const std::string& message)
{
    fprintf(stderr, "FAIL: %s\n", message.c_str());
    fflush(nullptr);
    ExitProcess(1); // completion may be unknown: never unwind objects the GPU may use
}

void Check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Fail(std::format("{} ({:08X})", what, (unsigned) hr));
}

struct Pixels
{
    UINT w = 0, h = 0;
    std::vector<uint8_t> data; // RGBA8
};

ComPtr<IWICImagingFactory> g_wic;

Pixels ReadPng(const fs::path& path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    Check(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder),
          "PNG decode");
    Check(decoder->GetFrame(0, &frame), "PNG frame");
    Pixels p;
    Check(frame->GetSize(&p.w, &p.h), "PNG size");
    Check(g_wic->CreateFormatConverter(&converter), "PNG converter");
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom),
          "RGBA8 conversion");
    p.data.resize(size_t(p.w) * p.h * 4);
    Check(converter->CopyPixels(nullptr, p.w * 4, UINT(p.data.size()), p.data.data()), "PNG pixels");
    return p;
}

void WritePng(const fs::path& path, const Pixels& p)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(g_wic->CreateStream(&stream), "PNG stream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG output");
    Check(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialize");
    Check(encoder->CreateNewFrame(&frame, nullptr), "PNG new frame");
    Check(frame->Initialize(nullptr), "PNG frame init");
    Check(frame->SetSize(p.w, p.h), "PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    Check(frame->SetPixelFormat(&format), "PNG format");
    // The encoder may have picked another layout: convert to whatever it asked for, as the harness does.
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    Check(g_wic->CreateBitmapFromMemory(p.w, p.h, GUID_WICPixelFormat32bppRGBA, p.w * 4, UINT(p.data.size()),
                                        const_cast<BYTE*>(p.data.data()), &bitmap),
          "PNG source bitmap");
    Check(g_wic->CreateFormatConverter(&converter), "PNG output converter");
    Check(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
          "PNG output conversion");
    Check(frame->WriteSource(converter.Get(), nullptr), "PNG write");
    Check(frame->Commit(), "PNG frame commit");
    Check(encoder->Commit(), "PNG commit");
}

uint64_t Fnv1a(const uint8_t* data, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; ++i)
        h = (h ^ data[i]) * 0x100000001b3ull;
    return h;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)),
          "texture");
    return r;
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap { type };
    D3D12_RESOURCE_DESC d {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}

struct Gpu
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;

    void Begin()
    {
        Check(allocator->Reset(), "allocator reset");
        Check(list->Reset(allocator.Get(), nullptr), "list reset");
    }

    void SubmitAndWait()
    {
        Check(list->Close(), "list close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        if (fence->GetCompletedValue() < value)
        {
            Check(fence->SetEventOnCompletion(value, event), "fence event");
            WaitForSingleObject(event, INFINITE);
        }
        Check(device->GetDeviceRemovedReason(), "device removed");
    }
};

void Stats(const char* label, std::vector<double> v)
{
    if (v.empty())
    {
        printf("%s: no samples\n", label);
        return;
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v)
        sum += x;
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(double(v.size()) * q))]; };
    printf("%s: n %zu p50 %.4f p95 %.4f p99 %.4f mean %.4f min %.4f ms\n", label, v.size(), at(0.5), at(0.95), at(0.99),
           sum / double(v.size()), v.front());
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    fs::path shaders, out, input;
    UINT width = 1691, height = 1312, frames = 16, iterations = 600, warmup = 100;
    float flow = 1.0f;
    bool performance = false, fp16 = false, outputCopy = false, bgra = false, waveTiming = false;
    std::wstring schedule = L"waves";
    int dump = -1;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring
        {
            if (i + 1 >= argc)
                Fail("missing value");
            return argv[++i];
        };
        if (a == L"--shaders")
            shaders = next();
        else if (a == L"--out")
            out = next();
        else if (a == L"--input")
            input = next();
        else if (a == L"--width")
            width = UINT(std::stoul(next()));
        else if (a == L"--height")
            height = UINT(std::stoul(next()));
        else if (a == L"--frames")
            frames = UINT(std::stoul(next()));
        else if (a == L"--iterations")
            iterations = UINT(std::stoul(next()));
        else if (a == L"--warmup")
            warmup = UINT(std::stoul(next()));
        else if (a == L"--flow")
            flow = std::stof(next());
        else if (a == L"--performance")
            performance = true;
        else if (a == L"--fp16")
            fp16 = true;
        else if (a == L"--output-copy")
            outputCopy = true;
        else if (a == L"--bgra")
            bgra = true;
        else if (a == L"--wave-timing")
            waveTiming = true;
        else if (a == L"--schedule")
            schedule = next();
        else if (a == L"--dump")
            dump = std::stoi(next());
        else
            Fail("unknown argument");
    }
    if (shaders.empty() || input.empty())
        Fail("--shaders and --input are required");

    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM");
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)), "WIC");

    Gpu gpu;
    {
        ComPtr<IDXGIFactory6> factory;
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
              "adapter");
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        printf("GPU: %ls\n", desc.Description);
        Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device)), "device");
        D3D12_COMMAND_QUEUE_DESC q {};
        Check(gpu.device->CreateCommandQueue(&q, IID_PPV_ARGS(&gpu.queue)), "queue");
        Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator)),
              "allocator");
        Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(&gpu.list)),
              "list");
        Check(gpu.list->Close(), "list close");
        Check(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence)), "fence");
        gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    ID3D12Device* device = gpu.device.Get();

    const DXGI_FORMAT format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    // The context, as Lsfg.cpp builds it.
    std::string error;
    auto source = Lsfg::MakeDirectoryShaderSource(shaders.wstring());
    auto programs = Lsfg::ProgramSet_Dx12::Build(device, performance, fp16, *source, error);
    if (programs == nullptr)
        Fail("programs: " + error);
    Lsfg::ContextKey key;
    key.device = device;
    key.width = width;
    key.height = height;
    key.format = format;
    key.flow = flow;
    key.performance = performance;
    key.fp16 = fp16;
    auto context = Lsfg::Context_Dx12::Build(programs, key, error);
    if (context == nullptr)
        Fail("context: " + error);
    printf("context %ux%u flow %.2f %s %s, %u images, %u descriptors, %.1f MiB\n", width, height, flow,
           performance ? "performance" : "quality", fp16 ? "fp16" : "fp32", context->ImageCount(),
           context->DescriptorCount(), double(context->VramBytes()) / 1048576.0);
#ifndef LSFG_BASELINE
    printf("waves: %u (pre-pass alone), %u (pre-pass and a generated frame)\n", context->WaveCount(false),
           context->WaveCount(true));
#endif

    // A moving sequence made from the input picture: frame n is the picture tiled over the frame, shifted by
    // (7n, 3n) px. Uploaded once into one presented texture per frame.
    const Pixels picture = ReadPng(input);
    const UINT kinds = std::max(frames, 1u);
    std::vector<ComPtr<ID3D12Resource>> presented(kinds);
    {
        D3D12_RESOURCE_DESC td =
            Texture(device, width, height, format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON)->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto upload = Buffer(device, total * kinds, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* mapped = nullptr;
        Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload map");

        gpu.Begin();
        for (UINT n = 0; n < kinds; ++n)
        {
            presented[n] = Texture(device, width, height, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
            uint8_t* base = mapped + total * n;
            for (UINT y = 0; y < height; ++y)
            {
                uint8_t* row = base + size_t(y) * fp.Footprint.RowPitch;
                const UINT sy = (y + 3 * n) % picture.h;
                for (UINT x = 0; x < width; ++x)
                {
                    const UINT sx = (x + 7 * n) % picture.w;
                    const uint8_t* px = &picture.data[(size_t(sy) * picture.w + sx) * 4];
                    row[x * 4 + 0] = bgra ? px[2] : px[0];
                    row[x * 4 + 1] = px[1];
                    row[x * 4 + 2] = bgra ? px[0] : px[2];
                    row[x * 4 + 3] = 255;
                }
            }
            D3D12_TEXTURE_COPY_LOCATION dst { presented[n].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            D3D12_TEXTURE_COPY_LOCATION src { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            src.PlacedFootprint = fp;
            src.PlacedFootprint.Offset = total * n;
            gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            auto b = Transition(presented[n].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            gpu.list->ResourceBarrier(1, &b);
        }
        gpu.SubmitAndWait();
    }

    // FFX's two interpolation outputs: UAV-capable and handed over in UNORDERED_ACCESS, or with --output-copy
    // without the UAV flag (the copy route), handed over in COMMON.
    const auto outFlags = outputCopy
                              ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                              : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto outState = outputCopy ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> outputs[2] = { Texture(device, width, height, format, outFlags, outState),
                                          Texture(device, width, height, format, outFlags, outState) };

    D3D12_RESOURCE_DESC od = outputs[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ofp {};
    UINT64 outTotal = 0;
    device->GetCopyableFootprints(&od, 0, 1, 0, &ofp, nullptr, nullptr, &outTotal);
    auto readback = Buffer(device, outTotal, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

#ifdef LSFG_WAVE_TIMING
    // --wave-timing: a timestamp before Record and the context's own after the ingest, every wave and the end.
    constexpr UINT kWaveQueries = 64;
    ComPtr<ID3D12QueryHeap> waveHeap;
    ComPtr<ID3D12Resource> waveReadback;
    std::vector<std::vector<double>> waveUs; // per interval: 0 ingest, 1..N waves, N+1 the last flush
    UINT64 tsFrequency = 0;
    if (waveTiming)
    {
        D3D12_QUERY_HEAP_DESC qh { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kWaveQueries, 0 };
        Check(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&waveHeap)), "wave query heap");
        waveReadback = Buffer(device, kWaveQueries * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        Check(gpu.queue->GetTimestampFrequency(&tsFrequency), "timestamp frequency");
    }
#else
    if (waveTiming)
        Fail("--wave-timing needs a build with LSFG_WAVE_TIMING");
#endif

    uint64_t frameId = 100;
    auto runFrame = [&](UINT n, bool perPass, bool read, uint64_t* hash, Pixels* pixels)
    {
        auto& history = context->GetHistory();
        const bool reset = !history.Follows(frameId);

        Lsfg::FrameIo io;
        io.presentColor = presented[n % kinds].Get();
        io.presentState = D3D12_RESOURCE_STATE_COMMON;
        io.generatedFrames = 1;
        io.generate = history.Generates(reset) ? 1 : 0;
        io.outputs[0] = outputs[frameId % 2].Get();
        io.outputStates[0] = outState;

        Lsfg::FrameConstants f;
        f.sourceIndex = history.iteration;
        f.resolutionInvScale = 1.0f / flow;

        gpu.Begin();
#ifdef LSFG_WAVE_TIMING
        const bool timeWaves = waveTiming && !read && io.generate > 0 && !perPass;
        if (timeWaves)
        {
            gpu.list->EndQuery(waveHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            Lsfg::Study::waveQueries = waveHeap.Get();
            Lsfg::Study::waveQueryNext = 1;
        }
#endif
#ifdef LSFG_BASELINE
        (void) perPass;
        context->Record(gpu.list.Get(), io, f);
#else
        context->Record(gpu.list.Get(), io, f, perPass ? Lsfg::Schedule::PerPass : Lsfg::Schedule::Waves);
#endif
#ifdef LSFG_WAVE_TIMING
        UINT waveCount = 0;
        if (timeWaves)
        {
            waveCount = Lsfg::Study::waveQueryNext;
            Lsfg::Study::waveQueries = nullptr;
            gpu.list->ResolveQueryData(waveHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, waveCount, waveReadback.Get(), 0);
        }
#endif
        history.Advance(reset, frameId);
        const uint32_t generated = io.generate;

        if (read && generated > 0)
        {
            auto b = Transition(io.outputs[0], outState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            gpu.list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst { readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            dst.PlacedFootprint = ofp;
            D3D12_TEXTURE_COPY_LOCATION src { io.outputs[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b = Transition(io.outputs[0], D3D12_RESOURCE_STATE_COPY_SOURCE, outState);
            gpu.list->ResourceBarrier(1, &b);
        }
        gpu.SubmitAndWait();
        ++frameId;

#ifdef LSFG_WAVE_TIMING
        if (waveCount > 1)
        {
            const uint64_t* t = nullptr;
            D3D12_RANGE range { 0, SIZE_T(waveCount) * 8 };
            Check(waveReadback->Map(0, &range, (void**) &t), "wave readback map");
            if (waveUs.size() < waveCount - 1)
                waveUs.resize(waveCount - 1);
            for (UINT q = 1; q < waveCount; ++q)
                waveUs[q - 1].push_back(double(t[q] - t[q - 1]) * 1e6 / double(tsFrequency));
            D3D12_RANGE none {};
            waveReadback->Unmap(0, &none);
        }
#endif

        if (read && generated > 0)
        {
            const uint8_t* data = nullptr;
            D3D12_RANGE range { 0, SIZE_T(outTotal) };
            Check(readback->Map(0, &range, (void**) &data), "readback map");
            uint64_t h = 0xcbf29ce484222325ull;
            if (pixels != nullptr)
            {
                pixels->w = width;
                pixels->h = height;
                pixels->data.resize(size_t(width) * height * 4);
            }
            for (UINT y = 0; y < height; ++y)
            {
                const uint8_t* row = data + size_t(y) * ofp.Footprint.RowPitch;
                h = Fnv1a(row, size_t(width) * 4, h);
                if (pixels != nullptr)
                {
                    uint8_t* dst = &pixels->data[size_t(y) * width * 4];
                    std::memcpy(dst, row, size_t(width) * 4);
                    if (bgra)
                        for (UINT x = 0; x < width; ++x)
                            std::swap(dst[x * 4 + 0], dst[x * 4 + 2]);
                }
            }
            D3D12_RANGE none {};
            readback->Unmap(0, &none);
            *hash = h;
        }
        return generated;
    };

    // Identity: the sequence once, every generated frame hashed. --schedule ab alternates the two schedules
    // frame by frame, which must not change a byte either.
    {
        for (UINT n = 0; n < frames; ++n)
        {
            const bool perPass = schedule == L"perpass" || (schedule == L"ab" && (n & 1));
            uint64_t hash = 0;
            Pixels pixels;
            const bool keep = int(n) == dump && !out.empty();
            const uint32_t generated = runFrame(n, perPass, true, &hash, keep ? &pixels : nullptr);
            if (generated > 0)
                printf("frame %u %s hash %016llx\n", n, perPass ? "perpass" : "waves", (unsigned long long) hash);
            else
                printf("frame %u held\n", n);
            if (keep && generated > 0)
                WritePng(out / std::format("ctx_{}.png", n), pixels);
        }
#ifndef LSFG_BASELINE
        const char* route = context->LastOutputRoute() == Lsfg::OutputRoute::Direct ? "direct"
                            : context->LastOutputRoute() == Lsfg::OutputRoute::Copy ? "copy"
                                                                                    : "none";
        printf("output route: %s %s\n", route, context->OutputRouteReason());
#endif
    }

    // Timing: the context's own timestamps, per schedule.
    if (iterations > 0)
    {
        std::vector<double> waves, perPass, discard;
        for (UINT i = 0; i < warmup + iterations; ++i)
        {
            const bool pp = schedule == L"perpass" || (schedule == L"ab" && (i & 1));
            runFrame(i, pp, false, nullptr, nullptr);
            if (i < warmup)
                context->CollectGpuTimes(discard
#ifndef LSFG_BASELINE
                                         ,
                                         &discard
#endif
                );
            else
                context->CollectGpuTimes(waves
#ifndef LSFG_BASELINE
                                         ,
                                         &perPass
#endif
                );
        }
#ifdef LSFG_WAVE_TIMING
        if (!waveUs.empty())
        {
            // What each interval holds: the pre-pass and first generated frame's waves, as Record runs them.
            Lsfg::Graph g;
            std::string why;
            Lsfg::BuildGraph(width, height, flow, performance, fp16, g, why);
            const auto schedule = Lsfg::ScheduleWaves(g, true, true);
            double total = 0;
            for (size_t i = 0; i < waveUs.size(); ++i)
            {
                auto v = waveUs[i];
                std::sort(v.begin(), v.end());
                const double median = v[v.size() / 2];
                total += median;
                std::string what;
                if (i == 0)
                    what = "ingest (heap, timestamp, copy-in)";
                else if (i <= schedule.size())
                {
                    uint64_t groups = 0;
                    for (const auto& step : schedule[i - 1])
                    {
                        const auto& p = step.main ? g.main[step.index] : g.pre[step.index];
                        what += std::format("{}{}:{} ", step.main ? "m" : "p", p.rid, p.index);
                        groups += uint64_t(p.groupsX) * p.groupsY;
                    }
                    what += std::format("({} groups)", groups);
                }
                else
                    what = "end (last barriers)";
                printf("interval %2zu median %7.2f us  %s\n", i, median, what.c_str());
            }
            printf("sum of interval medians %.2f us over %zu frames\n", total, waveUs[0].size());
        }
#endif
#ifdef LSFG_BASELINE
        Stats("GPU baseline", waves);
#else
        if (!waves.empty())
            Stats("GPU waves", waves);
        if (!perPass.empty())
            Stats("GPU perpass", perPass);
#endif
    }

    printf("done\n");
    fflush(nullptr);
    return 0;
}
