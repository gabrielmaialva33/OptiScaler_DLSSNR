// Standalone LSFG 3.1 SDR study and GPU benchmark. Shader binaries are user-supplied.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& message)
{
    fprintf(stderr, "FAIL: %s\n", message.c_str());
    fflush(nullptr);
    // Completion may be unknown. Do not unwind objects still used by the GPU.
    ExitProcess(1);
}
void check(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (0x%08lx)", what, hr);
        fail(buf);
    }
}
void require(bool yes, const char* message)
{
    if (!yes)
        fail(message);
}
struct Pixels
{
    UINT w, h;
    std::vector<unsigned char> data;
};
ComPtr<IWICImagingFactory> wic;
Pixels readPNG(const fs::path& path)
{
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    check(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder),
          "PNG decode");
    check(decoder->GetFrame(0, &frame), "PNG frame");
    Pixels p {};
    check(frame->GetSize(&p.w, &p.h), "PNG dimensions");
    require(p.w >= 64 && p.h >= 64 && p.w <= 16384 && p.h <= 16384, "input extent must be 64..16384 on both axes");
    check(wic->CreateFormatConverter(&converter), "PNG converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom),
          "RGBA8 conversion");
    p.data.resize(size_t(p.w) * p.h * 4);
    check(converter->CopyPixels(nullptr, p.w * 4, UINT(p.data.size()), p.data.data()), "PNG pixels");
    return p;
}
void writePNG(const fs::path& path, const Pixels& p)
{
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    check(wic->CreateStream(&stream), "PNG stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "PNG output");
    check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG initialize");
    check(encoder->CreateNewFrame(&frame, nullptr), "PNG new frame");
    check(frame->Initialize(nullptr), "PNG frame init");
    check(frame->SetSize(p.w, p.h), "PNG size");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    check(frame->SetPixelFormat(&format), "PNG format");
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICFormatConverter> converter;
    check(wic->CreateBitmapFromMemory(p.w, p.h, GUID_WICPixelFormat32bppRGBA, p.w * 4, UINT(p.data.size()),
                                      const_cast<BYTE*>(p.data.data()), &bitmap),
          "PNG source bitmap");
    check(wic->CreateFormatConverter(&converter), "PNG output converter");
    check(converter->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom),
          "PNG output conversion");
    check(frame->WriteSource(converter.Get(), nullptr), "PNG write");
    check(frame->Commit(), "PNG frame commit");
    check(encoder->Commit(), "PNG commit");
}
struct Texture
{
    ComPtr<ID3D12Resource> resource;
    UINT w, h, layers;
    DXGI_FORMAT format;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};
struct Binding
{
    D3D12_SHADER_INPUT_BIND_DESC desc {};
    bool push = false;
    UINT words = 0;
};
struct Shader
{
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    std::vector<Binding> bindings;
};
struct Pass
{
    UINT rid, index, special, gx, gy;
    std::vector<Texture*> reads, writes;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> tables;
};
struct Uniform
{
    float timestamp;
    UINT source, color, hdr;
    float invScale = 1, uiThreshold = .5f;
};
static_assert(sizeof(Uniform) == 24);

class Harness
{
  public:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12DescriptorHeap> heap, samplers, clearHeap;
    ComPtr<ID3D12Resource> uniforms;
    ComPtr<IDxcUtils> utils;
    DxcCreateInstanceProc dxc = nullptr;
    HANDLE event = nullptr;
    UINT increment = 0, samplerIncrement = 0, used = 0;
    UINT64 serial = 0;
    UINT width, height;
    bool half, performance;
    float flow;
    fs::path shaders;
    std::map<UINT, Shader> programs;
    std::vector<std::unique_ptr<Texture>> textures;
    std::map<UINT, std::vector<Texture*>> pools;
    std::vector<Pass> pre, main;
    Texture* source = nullptr;
    Texture* output = nullptr;

    Harness(UINT w, UINT h, bool fp16, bool perf, float scale, const fs::path& dir, bool debug, bool gbv)
        : width(w), height(h), half(fp16), performance(perf), flow(scale), shaders(dir)
    {
        if (debug || gbv)
        {
            ComPtr<ID3D12Debug> layer;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)), "D3D12 debug layer requested but unavailable");
            layer->EnableDebugLayer();
            if (gbv)
            {
                ComPtr<ID3D12Debug1> d;
                check(layer.As(&d), "GPU validation interface");
                d->SetEnableGPUBasedValidation(TRUE);
            }
        }
        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
        ComPtr<IDXGIAdapter1> adapter;
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
              "GPU adapter");
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        printf("GPU: %ls\n", desc.Description);
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12 device");
        device.As(&info);
        D3D12_FEATURE_DATA_SHADER_MODEL sm { D3D_SHADER_MODEL_6_0 };
        check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)), "SM6 support");
        require(sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0, "shader model 6.0 required");
        D3D12_COMMAND_QUEUE_DESC q {};
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        check(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
            "list");
        check(list->Close(), "initial close");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "fence event");
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.NumDescriptors = 8192;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "resource heap");
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&clearHeap)), "CPU clear heap");
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&samplers)), "sampler heap");
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        samplerIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for (UINT i = 0; i < 3; ++i)
        {
            D3D12_SAMPLER_DESC s {};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW =
                i == 2 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            s.ComparisonFunc = i == 2 ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_NEVER;
            s.MaxAnisotropy = 1;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            if (i == 1)
                std::fill(std::begin(s.BorderColor), std::end(s.BorderColor), 1.f);
            auto cpu = samplers->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(i) * samplerIncrement;
            device->CreateSampler(&s, cpu);
        }
        uniforms = buffer(256, D3D12_HEAP_TYPE_UPLOAD);
        HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
        require(dll != nullptr, "dxcompiler.dll missing");
        dxc = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
        require(dxc != nullptr, "DxcCreateInstance missing");
        check(dxc(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DXC utils");
        graph();
        for (auto& p : pre)
            tables(p);
        for (auto& p : main)
            tables(p);
        printf("Graph: %zu textures, %zu pre + %zu main dispatches, %u descriptors, %s variants\n", textures.size(),
               pre.size(), main.size(), used, half ? "FP16" : "FP32");
        begin();
        for (auto& t : textures)
            clear(*t);
        submit();
    }
    ~Harness()
    {
        if (event)
            CloseHandle(event);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot, bool clearing = false)
    {
        auto h = (clearing ? clearHeap : heap)->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(slot) * increment;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot)
    {
        auto h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(slot) * increment;
        return h;
    }
    UINT reserve(UINT n)
    {
        require(used + n <= 8192, "descriptor heap exhausted");
        UINT first = used;
        used += n;
        return first;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = type;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                              type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                             : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&r)),
              "buffer allocation");
        return r;
    }
    Texture* texture(UINT w, UINT h, UINT layers, DXGI_FORMAT format)
    {
        require(w > 0 && h > 0, "zero image extent");
        D3D12_FEATURE_DATA_FORMAT_SUPPORT f { format };
        check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof(f)), "format support");
        require((f.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) && (f.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) &&
                    (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
                "texture sampling/typed UAV store format unsupported");
        auto t = std::make_unique<Texture>();
        t->w = w;
        t->h = h;
        t->layers = layers;
        t->format = format;
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = UINT16(layers);
        d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t->state, nullptr,
                                              IID_PPV_ARGS(&t->resource)),
              "texture allocation");
        auto ptr = t.get();
        textures.push_back(std::move(t));
        return ptr;
    }
    void transition(Texture& t, D3D12_RESOURCE_STATES next)
    {
        if (t.state == next)
            return;
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { t.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.state, next };
        list->ResourceBarrier(1, &b);
        t.state = next;
    }
    void uavBarrier(Texture& t)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = t.resource.Get();
        list->ResourceBarrier(1, &b);
    }
    void begin()
    {
        check(allocator->Reset(), "allocator reset");
        check(list->Reset(allocator.Get(), nullptr), "list reset");
        ID3D12DescriptorHeap* heaps[] { heap.Get(), samplers.Get() };
        list->SetDescriptorHeaps(2, heaps);
    }
    void messages()
    {
        if (!info)
            return;
        bool errors = false;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<BYTE> bytes(size);
            auto m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            check(info->GetMessage(i, m, &size), "debug message");
            fprintf(stderr, "D3D12[%u]: %s\n", m->Severity, m->pDescription);
            errors |= m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
        }
        info->ClearStoredMessages();
        require(!errors, "D3D12 validation errors");
    }
    void submit()
    {
        check(list->Close(), "list close");
        ID3D12CommandList* cmds[] { list.Get() };
        queue->ExecuteCommandLists(1, cmds);
        check(queue->Signal(fence.Get(), ++serial), "queue signal");
        check(fence->SetEventOnCompletion(serial, event), "fence event");
        require(WaitForSingleObject(event, 120000) == WAIT_OBJECT_0, "GPU fence timeout");
        check(device->GetDeviceRemovedReason(), "device removed");
        messages();
    }
    void view(Texture* t, const D3D12_SHADER_INPUT_BIND_DESC& b, UINT slot, bool clearing = false,
              Texture* shape = nullptr)
    {
        auto model = t ? t : shape;
        require(model != nullptr, "missing descriptor shape");
        const bool array = b.Dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY;
        require(array || (b.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && model->layers == 1),
                "shader/view texture dimension mismatch");
        if (b.Type == D3D_SIT_UAV_RWTYPED)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.Format = model->format;
            v.ViewDimension = array ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY : D3D12_UAV_DIMENSION_TEXTURE2D;
            if (array)
                v.Texture2DArray.ArraySize = model->layers;
            device->CreateUnorderedAccessView(t ? t->resource.Get() : nullptr, nullptr, &v, cpu(slot, clearing));
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = model->format;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.ViewDimension = array ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
            if (array)
            {
                v.Texture2DArray.ArraySize = model->layers;
                v.Texture2DArray.MipLevels = 1;
            }
            else
                v.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(t ? t->resource.Get() : nullptr, &v, cpu(slot));
        }
    }
    void clear(Texture& t)
    {
        UINT slot = reserve(1);
        D3D12_SHADER_INPUT_BIND_DESC b {};
        b.Type = D3D_SIT_UAV_RWTYPED;
        b.Dimension = t.layers == 1 ? D3D_SRV_DIMENSION_TEXTURE2D : D3D_SRV_DIMENSION_TEXTURE2DARRAY;
        view(&t, b, slot);
        view(&t, b, slot, true);
        transition(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float zero[4] {};
        list->ClearUnorderedAccessViewFloat(gpu(slot), cpu(slot, true), t.resource.Get(), zero, 0, nullptr);
        uavBarrier(t);
    }
    Shader& shader(UINT rid)
    {
        if (programs.contains(rid))
            return programs.at(rid);
        printf("Loading shader %u\n", rid);
        fflush(stdout);
        ComPtr<IDxcBlobEncoding> blob;
        check(utils->LoadFile((shaders / (std::to_wstring(rid) + L".cso")).c_str(), nullptr, &blob), "load DXIL");
        ComPtr<IDxcContainerReflection> container;
        check(dxc(CLSID_DxcContainerReflection, IID_PPV_ARGS(&container)), "DXIL container");
        check(container->Load(blob.Get()), "DXIL container load");
        UINT32 part = 0;
        check(container->FindFirstPartKind(0x4c495844, &part), "DXIL part");
        ComPtr<ID3D12ShaderReflection> reflection;
        check(container->GetPartReflection(part, IID_PPV_ARGS(&reflection)), "DXIL reflection");
        D3D12_SHADER_DESC sd {};
        check(reflection->GetDesc(&sd), "shader description");
        Shader s;
        s.bindings.reserve(sd.BoundResources);
        std::vector<D3D12_ROOT_PARAMETER> roots(sd.BoundResources);
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges(sd.BoundResources);
        UINT cost = 0;
        for (UINT i = 0; i < sd.BoundResources; ++i)
        {
            Binding b;
            check(reflection->GetResourceBindingDesc(i, &b.desc), "shader binding");
            b.push = std::string(b.desc.Name) == "type_PushConstant_PC";
            auto& r = roots[i];
            r.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            if (b.push)
            {
                D3D12_SHADER_BUFFER_DESC cb {};
                check(reflection->GetConstantBufferByName(b.desc.Name)->GetDesc(&cb), "push constant size");
                b.words = cb.Size / 4;
                require(b.words >= 2 && b.words <= 4, "unexpected push constant layout");
                r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                r.Constants = { b.desc.BindPoint, b.desc.Space, b.words };
                cost += b.words;
            }
            else
            {
                auto& d = ranges[i];
                d.NumDescriptors = b.desc.BindCount;
                d.BaseShaderRegister = b.desc.BindPoint;
                d.RegisterSpace = b.desc.Space;
                require(d.NumDescriptors > 0 && d.NumDescriptors <= 7, "unexpected descriptor count");
                require(d.BaseShaderRegister == 0, "expected register zero in each binding space");
                switch (b.desc.Type)
                {
                case D3D_SIT_CBUFFER:
                    d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
                    require(b.desc.Space == 0, "unknown cbuffer");
                    break;
                case D3D_SIT_SAMPLER:
                    d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                    break;
                case D3D_SIT_TEXTURE:
                    d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    break;
                case D3D_SIT_UAV_RWTYPED:
                    d.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                    break;
                default:
                    fail("unsupported shader binding class");
                }
                r.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                r.DescriptorTable = { 1, &d };
                ++cost;
            }
            b.desc.Name = nullptr;
            s.bindings.push_back(b);
        }
        require(cost <= 64, "root signature exceeds 64 DWORDs");
        D3D12_ROOT_SIGNATURE_DESC rs {};
        rs.NumParameters = UINT(roots.size());
        rs.pParameters = roots.data();
        ComPtr<ID3DBlob> encoded, error;
        HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &encoded, &error);
        if (FAILED(hr) && error)
            fprintf(stderr, "%s\n", static_cast<char*>(error->GetBufferPointer()));
        check(hr, "root serialization");
        check(device->CreateRootSignature(0, encoded->GetBufferPointer(), encoded->GetBufferSize(),
                                          IID_PPV_ARGS(&s.root)),
              "root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = s.root.Get();
        pd.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
        check(device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&s.pso)), "compute PSO");
        return programs.emplace(rid, std::move(s)).first->second;
    }
    void tables(Pass& pass)
    {
        auto& s = shader(pass.rid);
        for (const auto& b : s.bindings)
        {
            if (b.push)
            {
                pass.tables.push_back({});
                continue;
            }
            if (b.desc.Type == D3D_SIT_SAMPLER)
            {
                require(b.desc.Space >= 1 && b.desc.Space <= 3, "unknown sampler");
                auto h = samplers->GetGPUDescriptorHandleForHeapStart();
                h.ptr += UINT64(b.desc.Space - 1) * samplerIncrement;
                pass.tables.push_back(h);
                continue;
            }
            UINT start = reserve(b.desc.BindCount);
            pass.tables.push_back(gpu(start));
            if (b.desc.Type == D3D_SIT_CBUFFER)
            {
                D3D12_CONSTANT_BUFFER_VIEW_DESC cb { uniforms->GetGPUVirtualAddress(), 256 };
                device->CreateConstantBufferView(&cb, cpu(start));
                continue;
            }
            const auto& pool = pools.at(b.desc.Space);
            require(pool.size() == b.desc.BindCount, "reflected descriptor array length differs from graph");
            const auto& active = b.desc.Type == D3D_SIT_TEXTURE ? pass.reads : pass.writes;
            for (UINT j = 0; j < pool.size(); ++j)
            {
                bool enabled = std::find(active.begin(), active.end(), pool[j]) != active.end();
                view(enabled ? pool[j] : nullptr, b.desc, start + j, false, pool[j]);
            }
        }
    }
    Texture* add(UINT binding, UINT w, UINT h, UINT layers, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        auto t = texture(w, h, layers, format);
        pools[binding].push_back(t);
        if (binding != 55)
            pools[binding + 1].push_back(t);
        return t;
    }
    void pass(std::vector<Pass>& dst, UINT rid, UINT i, UINT special, UINT w, UINT h, UINT group,
              std::vector<Texture*> reads, std::vector<Texture*> writes)
    {
        for (auto r : reads)
            require(std::find(writes.begin(), writes.end(), r) == writes.end(), "pass SRV/UAV overlap");
        dst.push_back({ rid + (rid >= 3 ? (half ? 48 : 0) + (performance ? 24 : 0) : 0),
                        i,
                        special,
                        (w + group - 1) / group,
                        (h + group - 1) / group,
                        std::move(reads),
                        std::move(writes),
                        {} });
    }
    void graph()
    {
        const UINT fw = UINT(float(width) * flow), fh = UINT(float(height) * flow);
        const UINT mul = performance ? 1 : 2;
        require(fw >= 64 && fh >= 64, "flow-scaled input extent must be at least 64 on both axes");
        source = texture(width, height, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
        pools[4] = { source };
        for (UINT i = 0; i < 7; ++i)
            add(5, std::max(fw >> i, 1u), std::max(fh >> i, 1u), 1, DXGI_FORMAT_R8_UNORM);
        pass(pre, 0, 0, 0, fw, fh, 64, { source }, pools[5]);
        std::array<Texture*, 7> alpha {}, gamma {};
        std::array<Texture*, 3> delta {}, epsilon {};
        for (UINT i = 0; i < 7; ++i)
        {
            UINT w = ((fw >> (6 - i)) + 1) / 2, h = ((fh >> (6 - i)) + 1) / 2;
            auto a = add(7, w, h, mul);
            auto b = add(9, w, h, mul);
            pass(pre, 13, i, 0, w, h, 8, { pools[5][6 - i] }, { a });
            pass(pre, 14, i, 0, w, h, 8, { a }, { b });
            w = (w + 1) / 2;
            h = (h + 1) / 2;
            auto c = add(11, w, h, 2 * mul);
            alpha[i] = add(13, w, h, 6 * mul);
            pass(pre, 15, i, 0, w, h, 8, { b }, { c });
            pass(pre, 16, i, 0, w, h, 8, { c }, { alpha[i] });
        }
        UINT bw = alpha[6]->w, bh = alpha[6]->h;
        Texture* prev = alpha[6];
        for (UINT i = 0; i < 4; ++i)
        {
            auto t = add(15 + 2 * i, bw, bh, 2);
            pass(pre, 22 + i, 0, 0, bw, bh, 8, { prev }, { t });
            prev = t;
        }
        for (UINT i = 0; i < 6; ++i)
            add(23, std::max(bw >> i, 1u), std::max(bh >> i, 1u), 1, DXGI_FORMAT_R8_UNORM);
        pass(pre, 26, 0, 0, bw, bh, 32, { prev }, pools[23]);
        for (UINT i = 0; i < 7; ++i)
        {
            UINT w = alpha[i]->w, h = alpha[i]->h;
            auto chain = [&](UINT firstRid, UINT binding, UINT index, UINT layers0, UINT layersN,
                             std::vector<Texture*> reads0, std::vector<Texture*> reads4, UINT special)
            {
                Texture* t = nullptr;
                for (UINT j = 0; j < 5; ++j)
                {
                    auto next = add(binding + 2 * j, w, h,
                                    j == 4   ? 1
                                    : j == 0 ? layers0
                                             : layersN,
                                    j == 4 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
                    auto reads = j == 0 ? reads0 : std::vector<Texture*> { t };
                    if (j == 4)
                        reads.insert(reads.end(), reads4.begin(), reads4.end());
                    pass(main, firstRid + j, index, (j == 0 || j == 4) ? special : 0, w, h, 8, reads, { next });
                    t = next;
                }
                return t;
            };
            std::vector<Texture*> gr { alpha[i] }, gr4 { pools[23][i == 0 ? 5 : 6 - i] };
            if (i)
            {
                gr.push_back(gamma[i - 1]);
                gr4.push_back(gamma[i - 1]);
            }
            gamma[i] = chain(3, 25, i, 3, 2 * mul, gr, gr4, i == 0);
            if (i >= 4)
            {
                UINT j = i - 4;
                std::vector<Texture*> dr { alpha[i] }, dr4 { pools[23][6 - i] };
                if (j)
                {
                    dr.push_back(delta[j - 1]);
                    dr4.push_back(delta[j - 1]);
                }
                delta[j] = chain(8, 35, j, 3, 2 * mul, dr, dr4, j == 0);
                std::vector<Texture*> er { alpha[i], gamma[i - 1] }, er4;
                if (j)
                {
                    er.push_back(delta[j - 1]);
                    er4.push_back(epsilon[j - 1]);
                }
                epsilon[j] = chain(17, 45, j, mul, mul, er, er4, j == 0);
            }
        }
        output = add(55, width, height, 1);
        pass(main, 1, 0, 0, width, height, 16, { source, gamma[6], delta[2], epsilon[2] }, { output });
        require(pre.size() == 34 && main.size() == 66, "graph pass count drift");
    }
    void constants(UINT frame, float timestamp)
    {
        Uniform cb {};
        cb.source = frame;
        cb.timestamp = timestamp;
        cb.invScale = 1.f / flow;
        void* mapped = nullptr;
        D3D12_RANGE noRead {};
        check(uniforms->Map(0, &noRead, &mapped), "uniform map");
        memset(mapped, 0, 256);
        memcpy(mapped, &cb, sizeof(cb));
        uniforms->Unmap(0, nullptr);
    }
    void execute(std::vector<Pass>& passes, ID3D12QueryHeap* queries = nullptr, UINT queryBase = 0)
    {
        for (auto& p : passes)
        {
            if (queries)
                list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, queryBase++);
            auto& s = programs.at(p.rid);
            for (auto t : p.reads)
                transition(*t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (auto t : p.writes)
                transition(*t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetComputeRootSignature(s.root.Get());
            list->SetPipelineState(s.pso.Get());
            for (UINT i = 0; i < s.bindings.size(); ++i)
            {
                auto& b = s.bindings[i];
                if (b.push)
                {
                    UINT pc[4] { p.special, p.index, 0, 0 };
                    list->SetComputeRoot32BitConstants(i, b.words, pc, 0);
                }
                else
                    list->SetComputeRootDescriptorTable(i, p.tables[i]);
            }
            list->Dispatch(p.gx, p.gy, 1);
            for (auto t : p.writes)
                uavBarrier(*t);
            if (queries)
                list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, queryBase++);
        }
    }
    static std::pair<double, double> statistics(std::vector<double> values)
    {
        require(!values.empty(), "empty timing sample");
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        const double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) * .5;
        return { median, values[size_t(std::ceil(.95 * double(n))) - 1] };
    }
    void benchmark(UINT warmup, UINT iterations, const fs::path& out)
    {
        require(iterations >= 2 && iterations <= 100000 && warmup >= 3 && warmup <= 100000,
                "timing requires 2..100000 iterations and 3..100000 warm-up iterations");
        UINT64 frequency = 0;
        check(queue->GetTimestampFrequency(&frequency), "timestamp frequency");
        require(frequency > 0, "zero timestamp frequency");
        constexpr UINT count = 204; // total, main phase, and two timestamps for each of 100 passes
        D3D12_QUERY_HEAP_DESC qd {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = count;
        ComPtr<ID3D12QueryHeap> queries;
        check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "timestamp heap");
        auto results = buffer(count * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
        std::array<std::vector<double>, 103> samples; // total, main, pre, each pass
        std::vector<double> coarseTotal, coarseMain;
        std::array<double, 2> wall {};
        std::ofstream raw(out / "timing-samples.csv");
        raw << "sweep,sample,total_ms,main_ms,pre_ms";
        for (UINT i = 0; i < 100; ++i)
            raw << ",pass_" << i << "_ms";
        raw << "\n" << std::setprecision(9);
        UINT frame = 0;
        for (UINT sweep = 0; sweep < 2; ++sweep)
        {
            const bool detailed = sweep == 1;
            std::chrono::steady_clock::time_point start;
            for (UINT iteration = 0; iteration < warmup + iterations; ++iteration, ++frame)
            {
                if (iteration == warmup)
                    start = std::chrono::steady_clock::now();
                // The source stays zero (no CPU uploads); feature history still rotates per iteration.
                constants(frame, .5f);
                begin();
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                execute(pre, detailed ? queries.Get() : nullptr, 4);
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
                execute(main, detailed ? queries.Get() : nullptr, 4 + 2 * UINT(pre.size()));
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
                list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
                list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, detailed ? count : 4,
                                       results.Get(), 0);
                submit(); // fence protects query reuse, readback and the next constant update
                UINT64* ticks = nullptr;
                D3D12_RANGE range { 0, count * sizeof(UINT64) };
                check(results->Map(0, &range, reinterpret_cast<void**>(&ticks)), "timestamp map");
                auto ms = [&](UINT a, UINT b)
                {
                    require(ticks[b] >= ticks[a], "non-monotonic GPU timestamps");
                    return double(ticks[b] - ticks[a]) * 1000. / double(frequency);
                };
                if (iteration >= warmup)
                {
                    const double total = ms(0, 1), generation = ms(2, 3), preprocessing = ms(0, 2);
                    raw << (detailed ? "passes" : "total") << ',' << iteration - warmup << ',' << total << ','
                        << generation << ',' << preprocessing;
                    if (detailed)
                    {
                        samples[0].push_back(total);
                        samples[1].push_back(generation);
                        samples[2].push_back(preprocessing);
                        for (UINT i = 0; i < 100; ++i)
                        {
                            double t = ms(4 + 2 * i, 5 + 2 * i);
                            samples[3 + i].push_back(t);
                            raw << ',' << t;
                        }
                    }
                    else
                    {
                        coarseTotal.push_back(total);
                        coarseMain.push_back(generation);
                    }
                    raw << '\n';
                }
                D3D12_RANGE noWrite {};
                results->Unmap(0, &noWrite);
            }
            wall[sweep] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() /
                          iterations;
        }
        std::ofstream json(out / "timing.json");
        require(bool(json) && bool(raw), "timing report file failure");
        auto emit = [&](const char* name, const std::vector<double>& v)
        {
            auto [median, p95] = statistics(v);
            json << "\"" << name << "\":{\"median_ms\":" << median << ",\"p95_ms\":" << p95 << "}";
        };
        json << std::setprecision(9) << "{\"width\":" << width << ",\"height\":" << height
             << ",\"performance\":" << (performance ? "true" : "false")
             << ",\"fp16_variant\":" << (half ? "true" : "false") << ",\"flow_scale\":" << flow
             << ",\"multiplier\":2,\"warmup\":" << warmup << ",\"iterations\":" << iterations
             << ",\"frequency_hz\":" << frequency << ",\"wall_mean_ms\":" << wall[0] << ',';
        emit("total", coarseTotal);
        json << ',';
        emit("generation", coarseMain);
        json << ',';
        emit("instrumented_total", samples[0]);
        json << ',';
        emit("instrumented_generation", samples[1]);
        json << ',';
        emit("instrumented_preprocessing", samples[2]);
        json << ",\"passes\":[";
        std::ofstream csv(out / "timing-passes.csv");
        csv << "phase,pass,rid,level,median_ms,p95_ms\n" << std::setprecision(9);
        const char* names[] { "mipmaps",  "generate", "generate_hdr", "gamma0",   "gamma1",   "gamma2",   "gamma3",
                              "gamma4",   "delta0",   "delta1",       "delta2",   "delta3",   "delta4",   "alpha0",
                              "alpha1",   "alpha2",   "alpha3",       "epsilon0", "epsilon1", "epsilon2", "epsilon3",
                              "epsilon4", "beta0",    "beta1",        "beta2",    "beta3",    "beta4" };
        for (UINT i = 0; i < 100; ++i)
        {
            const bool isPre = i < pre.size();
            const auto& p = isPre ? pre[i] : main[i - UINT(pre.size())];
            UINT base = p.rid < 3 ? p.rid : p.rid - (half ? 48 : 0) - (performance ? 24 : 0);
            auto [median, p95] = statistics(samples[i + 3]);
            if (i)
                json << ',';
            json << "{\"phase\":\"" << (isPre ? "pre" : "main") << "\",\"name\":\"" << names[base]
                 << "\",\"rid\":" << p.rid << ",\"level\":" << p.index << ",\"median_ms\":" << median
                 << ",\"p95_ms\":" << p95 << '}';
            csv << (isPre ? "pre" : "main") << ',' << names[base] << ',' << p.rid << ',' << p.index << ',' << median
                << ',' << p95 << '\n';
        }
        json << "]}\n";
        auto [median, p95] = statistics(coarseTotal);
        printf("GPU total: median %.6f ms, p95 %.6f ms; wall mean %.6f ms (%u samples after %u warm-up)\n", median, p95,
               wall[0], iterations, warmup);
        printf("Per-pass timings: timing-passes.csv; raw samples: timing-samples.csv; summary: timing.json\n");
    }
    void upload(const Pixels& p, UINT frame)
    {
        auto d = source->resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&d, frame % 2, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        BYTE* mapped = nullptr;
        D3D12_RANGE noRead {};
        check(upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "upload map");
        for (UINT y = 0; y < height; ++y)
            memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                   p.data.data() + size_t(y) * width * 4, size_t(width) * 4);
        upload->Unmap(0, nullptr);
        begin();
        transition(*source, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprint;
        dst.pResource = source->resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = frame % 2;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        submit();
    }
    Pixels generate(UINT frame, UINT index, UINT multiplier)
    {
        constants(frame, float(index + 1) / float(multiplier));
        begin();
        execute(main);
        transition(*output, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto d = output->resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&d, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto readback = buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = output->resource.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        submit();
        Pixels p { width, height, std::vector<BYTE>(size_t(width) * height * 4) };
        BYTE* mapped = nullptr;
        D3D12_RANGE range { 0, SIZE_T(bytes) };
        check(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "readback map");
        for (UINT y = 0; y < height; ++y)
            memcpy(p.data.data() + size_t(y) * width * 4,
                   mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(width) * 4);
        D3D12_RANGE noWrite {};
        readback->Unmap(0, &noWrite);
        return p;
    }
};
int wmain(int argc, wchar_t** argv)
{
    try
    {
        fs::path shaders, out;
        std::vector<fs::path> paths;
        UINT multiplier = 2;
        bool half = false, debug = false, gbv = false, perf = false, timing = false;
        float flow = 1.f;
        UINT width = 1920, height = 1080, iterations = 400, warmup = 100;
        for (int i = 1; i < argc; ++i)
        {
            std::wstring a = argv[i];
            if (a == L"--fp16")
                half = true;
            else if (a == L"--performance")
                perf = true;
            else if (a == L"--benchmark")
                timing = true;
            else if (a == L"--debug")
                debug = true;
            else if (a == L"--gpu-validation")
                gbv = true;
            else if (a == L"--shaders" || a == L"--out" || a == L"--multiplier" || a == L"--flow-scale" ||
                     a == L"--width" || a == L"--height" || a == L"--iterations" || a == L"--warmup")
            {
                require(i + 1 < argc, "option requires a value");
                auto value = argv[++i];
                if (a == L"--shaders")
                    shaders = value;
                else if (a == L"--out")
                    out = value;
                else if (a == L"--multiplier")
                    multiplier = std::stoul(value);
                else if (a == L"--flow-scale")
                    flow = std::stof(value);
                else if (a == L"--width")
                    width = std::stoul(value);
                else if (a == L"--height")
                    height = std::stoul(value);
                else if (a == L"--iterations")
                    iterations = std::stoul(value);
                else
                    warmup = std::stoul(value);
            }
            else
            {
                require(!a.starts_with(L"--"), "unknown option");
                paths.emplace_back(a);
            }
        }
        require(!shaders.empty() && !out.empty() && (multiplier >= 2 && multiplier <= 4),
                "usage: lsfg-harness --shaders DIR --out DIR [--multiplier 2|3|4] [--fp16] [--debug] "
                "[--gpu-validation] FOLDER | PREV.png CURRENT.png");
        require(std::isfinite(flow) && flow >= .25f && flow <= 1.f, "flow scale must be .25..1");
        if (timing)
        {
            require(paths.empty() && multiplier == 2, "benchmark accepts no PNG inputs and requires multiplier 2");
            require(width >= 64 && height >= 64 && width <= 16384 && height <= 16384, "invalid benchmark extent");
            fs::create_directories(out);
            Harness h(width, height, half, perf, flow, shaders, debug, gbv);
            h.benchmark(warmup, iterations, out);
            return 0;
        }
        if (paths.size() == 1 && fs::is_directory(paths[0]))
        {
            auto folder = paths[0];
            paths.clear();
            for (const auto& entry : fs::directory_iterator(folder))
                if (entry.path().extension() == L".png")
                    paths.push_back(entry.path());
            std::sort(paths.begin(), paths.end(),
                      [](const auto& a, const auto& b)
                      {
                          auto sa = a.stem().wstring(), sb = b.stem().wstring();
                          require(sa.find_first_not_of(L"0123456789") == std::wstring::npos &&
                                      sb.find_first_not_of(L"0123456789") == std::wstring::npos,
                                  "folder PNGs must have numbered stems");
                          return std::stoull(sa) < std::stoull(sb);
                      });
        }
        require(paths.size() >= 2, "at least two frames required");
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM init");
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
              "WIC factory");
        fs::create_directories(out);
        auto first = readPNG(paths[0]);
        Harness h(first.w, first.h, half, perf, flow, shaders, debug, gbv);
        for (UINT f = 0; f < paths.size(); ++f)
        {
            auto p = f == 0 ? first : readPNG(paths[f]);
            require(p.w == first.w && p.h == first.h, "frame dimensions differ");
            h.upload(p, f);
            h.constants(f, 0.f);
            h.begin();
            h.execute(h.pre);
            h.submit();
            for (UINT j = 0; j < multiplier - 1; ++j)
            {
                auto result = h.generate(f, j, multiplier);
                // Keep frame zero for diagnosing the explicitly zero-initialized history.
                auto name = std::to_wstring(f) + L"_" + std::to_wstring(j + 1) + L".png";
                writePNG(out / name, result);
                printf("frame=%u generated=%u/%u -> %ls\n", f, j + 1, multiplier, name.c_str());
                fflush(stdout);
            }
        }
        printf("PASS: completed %zu input frames\n", paths.size());
        return 0;
    }
    catch (const std::exception& e)
    {
        fail(e.what());
    }
}
