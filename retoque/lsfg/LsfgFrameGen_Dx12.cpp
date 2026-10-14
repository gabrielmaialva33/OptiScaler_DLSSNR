#include "pch.h"

#include "LsfgFrameGen_Dx12.h"

#include <lsfg/LsfgDxil.h>
#include <lsfg/LsfgShaderSource.h>

#include <algorithm>
#include <cfloat>

using Microsoft::WRL::ComPtr;

namespace Lsfg
{
namespace
{
#ifdef LSFG_WAVE_TIMING
void StudyTimestamp(ID3D12GraphicsCommandList* list)
{
    if (Study::waveQueries != nullptr)
        list->EndQuery(Study::waveQueries, D3D12_QUERY_TYPE_TIMESTAMP, Study::waveQueryNext++);
}
#else
void StudyTimestamp(ID3D12GraphicsCommandList*) {}
#endif

// Every shader the graph dispatches, as base resource ids: mipmaps, generate, the 24 core shaders.
// generate_16bit (2) is byte-identical HLSL to generate_8bit (1) once converted (OPTI_INTEGRATION.md 6):
// precision is the bound view's, so the graph only ever uses 1.
constexpr uint32_t kBaseRids[] = { 0,  1,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                   14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };

// numthreads of each base shader, a cheap check that a file is the module it claims to be.
uint32_t ExpectedGroupSize(uint32_t base)
{
    switch (base)
    {
    case 0:  // mipmaps
    case 26: // beta4
        return 32;
    case 1:
    case 2: // generate
        return 16;
    default:
        return 8;
    }
}

DXGI_FORMAT ToDxgi(ImageFormat f, DXGI_FORMAT display)
{
    switch (f)
    {
    case ImageFormat::R8:
        return DXGI_FORMAT_R8_UNORM;
    case ImageFormat::Rgba8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case ImageFormat::Rgba16F:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case ImageFormat::Display:
        return display;
    }
    return DXGI_FORMAT_UNKNOWN;
}

// The three samplers lsfg-vk creates (pipeline.cpp), by the register space the converter gave them:
// space 1 border transparent black, space 2 border opaque white, space 3 clamp to edge. All bilinear.
bool StaticSamplerFor(uint32_t space, D3D12_STATIC_SAMPLER_DESC& s)
{
    if (space < 1 || space > 3)
        return false;

    s = {};
    s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    const auto address = space == 3 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    s.AddressU = s.AddressV = s.AddressW = address;
    s.MipLODBias = 0.0f;
    s.MaxAnisotropy = 1;
    s.ComparisonFunc = space == 3 ? D3D12_COMPARISON_FUNC_ALWAYS : D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = space == 2 ? D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE : D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    s.MinLOD = 0.0f;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = 0;
    s.RegisterSpace = space;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return true;
}

bool BuildProgram(ID3D12Device* device, uint32_t rid, const std::vector<uint8_t>& blob,
                  ProgramSet_Dx12::Program& program, std::string& error)
{
    Dxil::Reflection reflection;
    if (!Dxil::Parse(blob.data(), blob.size(), reflection, error))
        return false;

    if (!reflection.hasKinds)
    {
        error = "PSV0 records carry no resource kinds (compile with a newer dxc)";
        return false;
    }

    const uint32_t base = BaseShader(rid);
    const uint32_t group = ExpectedGroupSize(base);
    if (reflection.hasNumThreads &&
        (reflection.numThreads[0] != group || reflection.numThreads[1] != group || reflection.numThreads[2] != 1))
    {
        error = std::format("numthreads {}x{}x{}, expected {}x{}x1: not the module this id names",
                            reflection.numThreads[0], reflection.numThreads[1], reflection.numThreads[2], group, group);
        return false;
    }

    const CbufferUse use = CbuffersOf(base);
    uint32_t cbvs = 0;
    for (const auto& b : reflection.bindings)
        cbvs += b.cls == Dxil::ResClass::Cbv ? 1 : 0;

    if (cbvs != uint32_t(use.cb) + uint32_t(use.pc))
    {
        error =
            std::format("{} constant buffers where the graph expects {}", cbvs, uint32_t(use.cb) + uint32_t(use.pc));
        return false;
    }

    std::vector<D3D12_ROOT_PARAMETER> roots;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    roots.reserve(reflection.bindings.size());
    ranges.reserve(reflection.bindings.size()); // stable addresses for the tables' range pointers
    uint32_t cost = 0;
    bool seenFrameValues = false, seenPushValues = false;

    for (const auto& b : reflection.bindings)
    {
        ProgramSet_Dx12::Param param;
        D3D12_ROOT_PARAMETER root {};
        root.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        switch (b.cls)
        {
        case Dxil::ResClass::Sampler:
        {
            D3D12_STATIC_SAMPLER_DESC s;
            if (b.count != 1 || b.lower != 0 || !StaticSamplerFor(b.space, s))
            {
                error = std::format("sampler at s{} space{} is not one of the three the graph knows", b.lower, b.space);
                return false;
            }
            samplers.push_back(s);
            continue; // static: no root parameter
        }

        case Dxil::ResClass::Cbv:
        {
            if (b.space != 0 || b.count != 1 || b.lower > 1)
            {
                error = std::format("constant buffer at b{} space{}", b.lower, b.space);
                return false;
            }

            // With both cbuffers, cb is b0 (explicit) and the push constants b1; with one, it is at b0.
            const bool isFrameValues = use.cb && (b.lower == 0);
            if ((isFrameValues && seenFrameValues) || (!isFrameValues && seenPushValues) ||
                (!isFrameValues && b.lower != (use.cb ? 1u : 0u)))
            {
                error = std::format("constant buffer at b{} does not fit the graph's layout", b.lower);
                return false;
            }
            (isFrameValues ? seenFrameValues : seenPushValues) = true;
            param.kind =
                isFrameValues ? ProgramSet_Dx12::ParamKind::FrameValues : ProgramSet_Dx12::ParamKind::PushValues;
            root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            root.Constants.ShaderRegister = b.lower;
            root.Constants.RegisterSpace = 0;
            root.Constants.Num32BitValues = isFrameValues ? kCbWords : kPcWords;
            cost += root.Constants.Num32BitValues;
            break;
        }

        case Dxil::ResClass::Srv:
        case Dxil::ResClass::Uav:
        {
            if (b.lower != 0 || b.count == 0 || b.count > 7 ||
                (b.kind != Dxil::kKindTexture2D && b.kind != Dxil::kKindTexture2DArray))
            {
                error = std::format("binding space{} (t/u{}, {} descriptors, kind {}) is not a 2D texture array of "
                                    "at most 7 at register 0",
                                    b.space, b.lower, b.count, b.kind);
                return false;
            }

            param.kind = ProgramSet_Dx12::ParamKind::Table;
            param.uav = b.cls == Dxil::ResClass::Uav;
            param.space = b.space;
            param.count = b.count;
            param.array = b.kind == Dxil::kKindTexture2DArray;

            D3D12_DESCRIPTOR_RANGE range {};
            range.RangeType = param.uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = b.count;
            range.BaseShaderRegister = 0;
            range.RegisterSpace = b.space;
            range.OffsetInDescriptorsFromTableStart = 0;
            ranges.push_back(range);

            root.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            root.DescriptorTable.NumDescriptorRanges = 1;
            root.DescriptorTable.pDescriptorRanges = &ranges.back();
            cost += 1;
            break;
        }

        default:
            error = std::format("unsupported resource class at space{} (raw or structured buffers)", b.space);
            return false;
        }

        roots.push_back(root);
        program.params.push_back(param);
    }

    if (cost > 64)
    {
        error = "root signature above 64 DWORDs";
        return false;
    }

    D3D12_ROOT_SIGNATURE_DESC rs {};
    rs.NumParameters = UINT(roots.size());
    rs.pParameters = roots.data();
    rs.NumStaticSamplers = UINT(samplers.size());
    rs.pStaticSamplers = samplers.data();
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned {};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    versioned.Desc_1_0 = rs;

    ComPtr<ID3DBlob> encoded;
    ComPtr<ID3DBlob> message;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &encoded, &message);
    if (FAILED(hr))
    {
        error = std::format("root signature serialization failed ({:X}){}{}", (UINT) hr, message ? ": " : "",
                            message ? static_cast<const char*>(message->GetBufferPointer()) : "");
        return false;
    }

    hr = device->CreateRootSignature(0, encoded->GetBufferPointer(), encoded->GetBufferSize(),
                                     IID_PPV_ARGS(&program.root));
    if (FAILED(hr))
    {
        error = std::format("CreateRootSignature failed ({:X})", (UINT) hr);
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = program.root.Get();
    pso.CS = { blob.data(), blob.size() };
    hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&program.pso));
    if (FAILED(hr))
    {
        error = std::format("CreateComputePipelineState failed ({:X})", (UINT) hr);
        return false;
    }

    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------

std::shared_ptr<ProgramSet_Dx12> ProgramSet_Dx12::Build(ID3D12Device* device, bool performance, bool fp16,
                                                        IShaderSource& source, std::string& error)
{
    if (device == nullptr)
    {
        error = "no device";
        return nullptr;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm { D3D_SHADER_MODEL_6_0 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_0)
    {
        error = "the device has no shader model 6.0";
        return nullptr;
    }

    auto set = std::shared_ptr<ProgramSet_Dx12>(new ProgramSet_Dx12());
    set->_device = device;
    set->_performance = performance;
    set->_fp16 = fp16;

    std::vector<uint8_t> blob;
    for (uint32_t base : kBaseRids)
    {
        const uint32_t rid = VariantRid(base, performance, fp16);
        std::string why;

        if (!source.LoadShader(rid, blob, why))
        {
            error = std::format("shader {}: {}", rid, why);
            return nullptr;
        }

        Program program;
        if (!BuildProgram(device, rid, blob, program, why))
        {
            error = std::format("shader {}: {}", rid, why);
            return nullptr;
        }

        set->_programs.emplace(rid, std::move(program));
    }

    return set;
}

const ProgramSet_Dx12::Program* ProgramSet_Dx12::Find(uint32_t rid) const
{
    auto it = _programs.find(rid);
    return it == _programs.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<Context_Dx12> Context_Dx12::Build(std::shared_ptr<ProgramSet_Dx12> programs, const ContextKey& key,
                                                  std::string& error)
{
    if (programs == nullptr || programs->Device() != key.device || programs->Performance() != key.performance ||
        programs->Fp16() != key.fp16)
    {
        error = "program set does not match the context";
        return nullptr;
    }

    auto context = std::unique_ptr<Context_Dx12>(new Context_Dx12());
    context->_programs = std::move(programs);
    context->_key = key;

    if (!context->_Create(error))
        return nullptr;

    return context;
}

Context_Dx12::~Context_Dx12()
{
    if (_readback != nullptr && _readbackData != nullptr)
    {
        D3D12_RANGE nothingWritten {};
        _readback->Unmap(0, &nothingWritten);
    }
}

bool Context_Dx12::_Create(std::string& error)
{
    ID3D12Device* device = _key.device;

    if (!BuildGraph(_key.width, _key.height, _key.flow, _key.performance, _key.fp16, _graph, error))
        return false;

    // Formats: typed UAV store and sampling for every format the graph writes and reads.
    for (DXGI_FORMAT format :
         { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, _key.format })
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        {
            error = std::format("format {} lacks sampling or typed UAV stores on this device", (UINT) format);
            return false;
        }
    }

    // Textures. The source array is only ever copied into and sampled, so it carries no UAV flag.
    _textures.resize(_graph.images.size());
    uint32_t clears = 0;
    for (size_t i = 0; i < _graph.images.size(); ++i)
    {
        auto& t = _textures[i];
        t.shape = _graph.images[i];
        t.format = ToDxgi(t.shape.format, _key.format);
        const bool isSource = i == _graph.source;
        const bool isOutput = i == _graph.output;
        t.clear = !isSource && !isOutput; // written before read every frame: no history to zero
        t.state = isSource ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (t.clear)
            t.clearSlot = clears++;

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = t.shape.width;
        desc.Height = t.shape.height;
        desc.DepthOrArraySize = UINT16(t.shape.layers);
        desc.MipLevels = 1;
        desc.Format = t.format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = isSource ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, t.state, nullptr,
                                                     IID_PPV_ARGS(&t.resource));
        if (FAILED(hr))
        {
            error = std::format("texture {} ({}x{}x{}, format {}) allocation failed ({:X})", i, t.shape.width,
                                t.shape.height, t.shape.layers, (UINT) t.format, (UINT) hr);
            return false;
        }

        t.resource->SetName(std::format(L"LSFG image {}", i).c_str());
        _vramBytes += device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    }

    // Passes: resolve programs and count descriptors.
    auto resolve = [&](const std::vector<GraphPass>& graphPasses, std::vector<Pass>& passes)
    {
        for (const auto& gp : graphPasses)
        {
            Pass p;
            p.graph = &gp;
            p.program = _programs->Find(gp.rid);
            if (p.program == nullptr)
            {
                error = std::format("no program for shader {}", gp.rid);
                return false;
            }
            p.tables.resize(p.program->params.size());
            for (const auto& param : p.program->params)
                _descriptorCount += param.kind == ProgramSet_Dx12::ParamKind::Table ? param.count : 0;
            passes.push_back(std::move(p));
        }
        return true;
    };

    if (!resolve(_graph.pre, _pre) || !resolve(_graph.main, _main))
        return false;

    const uint32_t tableDescriptors = _descriptorCount;
    _descriptorCount += clears + kOutputRing;
    _ringBase = tableDescriptors + clears;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = _descriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_heap))))
    {
        error = std::format("shader-visible heap of {} descriptors failed", _descriptorCount);
        return false;
    }
    _heap->SetName(L"LSFG descriptors");

    heapDesc.NumDescriptors = std::max(clears, 1u);
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_clearHeap))))
    {
        error = "CPU clear heap failed";
        return false;
    }

    _descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto cpuBase = _heap->GetCPUDescriptorHandleForHeapStart();
    const auto gpuBase = _heap->GetGPUDescriptorHandleForHeapStart();
    auto cpuAt = [&](uint32_t slot)
    {
        auto h = cpuBase;
        h.ptr += SIZE_T(slot) * _descriptorSize;
        return h;
    };
    auto gpuAt = [&](uint32_t slot)
    {
        auto h = gpuBase;
        h.ptr += UINT64(slot) * _descriptorSize;
        return h;
    };

    // A view of `shape` (a texture, or a typed null one when `resource` is null) as the binding declares it.
    auto view =
        [&](ID3D12Resource* resource, const Texture& shape, bool uav, bool array, D3D12_CPU_DESCRIPTOR_HANDLE where)
    {
        if (uav)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.Format = shape.format;
            if (array)
            {
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.ArraySize = shape.shape.layers;
            }
            else
            {
                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            }
            device->CreateUnorderedAccessView(resource, nullptr, &v, where);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = shape.format;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (array)
            {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                v.Texture2DArray.MipLevels = 1;
                v.Texture2DArray.ArraySize = shape.shape.layers;
            }
            else
            {
                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Texture2D.MipLevels = 1;
            }
            device->CreateShaderResourceView(resource, &v, where);
        }
    };

    // Immutable, full-length tables per pass: the elements this dispatch reads (or writes) are live views,
    // every other element a typed null view of the same shape, so no image is ever bound as SRV and UAV at once.
    uint32_t next = 0;
    auto tables = [&](std::vector<Pass>& passes)
    {
        for (auto& p : passes)
        {
            for (size_t i = 0; i < p.program->params.size(); ++i)
            {
                const auto& param = p.program->params[i];
                if (param.kind != ProgramSet_Dx12::ParamKind::Table)
                    continue;

                auto poolIt = _graph.pools.find(param.space);
                if (poolIt == _graph.pools.end() || poolIt->second.size() != param.count)
                {
                    error =
                        std::format("shader {} binds {} descriptors in space {}, the graph has {}", p.graph->rid,
                                    param.count, param.space, poolIt == _graph.pools.end() ? 0 : poolIt->second.size());
                    return false;
                }

                const auto& active = param.uav ? p.graph->writes : p.graph->reads;
                const uint32_t start = next;
                next += param.count;
                p.tables[i] = gpuAt(start);
                if (param.uav && param.space == kOutputBinding)
                    p.outputParam = int(i);

                for (uint32_t j = 0; j < param.count; ++j)
                {
                    const uint32_t image = poolIt->second[j];
                    const Texture& t = _textures[image];

                    if (!param.array && t.shape.layers != 1)
                    {
                        error = std::format("space {} declares Texture2D over a {}-layer image", param.space,
                                            t.shape.layers);
                        return false;
                    }

                    const bool enabled = std::find(active.begin(), active.end(), image) != active.end();
                    view(enabled ? t.resource.Get() : nullptr, t, param.uav, param.array, cpuAt(start + j));
                }
            }
        }
        return true;
    };

    if (!tables(_pre) || !tables(_main))
        return false;

    if (next != tableDescriptors)
    {
        error = "descriptor count drift";
        return false;
    }

    // The clears: a whole-resource UAV in both heaps for every texture zeroed on first use. Clear slot n is
    // descriptor tableDescriptors + n of the shader-visible heap and descriptor n of the CPU heap.
    _tableDescriptors = tableDescriptors;
    const auto clearCpuBase = _clearHeap->GetCPUDescriptorHandleForHeapStart();
    for (auto& t : _textures)
    {
        if (!t.clear)
            continue;
        auto cpu = clearCpuBase;
        cpu.ptr += SIZE_T(t.clearSlot) * _descriptorSize;
        const bool array = t.shape.layers > 1;
        view(t.resource.Get(), t, true, array, cpuAt(tableDescriptors + t.clearSlot));
        view(t.resource.Get(), t, true, array, cpu);
    }

    // GPU timing (optional: a failure here only costs the timing line).
    {
        ComPtr<ID3D12CommandQueue> probe;
        // The frequency of a direct queue on this device: FFX's list may run on its async compute queue, whose
        // frequency is the same on the GPUs this was measured on but is not guaranteed to be.
        D3D12_COMMAND_QUEUE_DESC qd {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&probe))))
            probe->GetTimestampFrequency(&_timestampFrequency);

        D3D12_QUERY_HEAP_DESC qh {};
        qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qh.Count = kTimingSlots * 2;

        D3D12_HEAP_PROPERTIES rb {};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(uint64_t) * kTimingSlots * 2;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        void* mapped = nullptr;
        if (_timestampFrequency == 0 || FAILED(device->CreateQueryHeap(&qh, IID_PPV_ARGS(&_queries))) ||
            FAILED(device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&_readback))) ||
            FAILED(_readback->Map(0, nullptr, &mapped)))
        {
            _queries.Reset();
            _readback.Reset();
            _timestampFrequency = 0;
        }
        else
        {
            _readbackData = static_cast<const uint64_t*>(mapped);
        }
    }

    // The pass schedules, and a check that every generate pass found its output parameter (the direct output
    // needs it; a module whose output is not where the graph says is refused here, not mis-bound later).
    _wavesPre = ScheduleWaves(_graph, true, false);
    _wavesPreMain = ScheduleWaves(_graph, true, true);
    _wavesMain = ScheduleWaves(_graph, false, true);
    for (const auto& p : _main)
    {
        const bool writesOutput =
            std::find(p.graph->writes.begin(), p.graph->writes.end(), _graph.output) != p.graph->writes.end();
        if (writesOutput != (p.outputParam >= 0))
        {
            error = std::format("shader {}: its output binding is not where the graph says", p.graph->rid);
            return false;
        }
    }

    _barriers.reserve(64);
    return true;
}

void Context_Dx12::_Transition(std::vector<D3D12_RESOURCE_BARRIER>& barriers, Texture& t, D3D12_RESOURCE_STATES next)
{
    if (t.state == next)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.resource.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = t.state;
    b.Transition.StateAfter = next;
    barriers.push_back(b);
    t.state = next;
    t.dirty = false; // a transition orders every earlier access, the UAV writes included
}

void Context_Dx12::_UavBarrier(std::vector<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    barriers.push_back(b);
}

void Context_Dx12::_Flush(ID3D12GraphicsCommandList* list)
{
    if (_barriers.empty())
        return;
    list->ResourceBarrier(UINT(_barriers.size()), _barriers.data());
    _barriers.clear();
}

void Context_Dx12::_Dispatch(ID3D12GraphicsCommandList* list, const Pass& p, const uint32_t* frameWords,
                             D3D12_GPU_DESCRIPTOR_HANDLE outputTable)
{
    // Passes of one shader often follow each other inside a wave (the seven alpha levels): same root signature
    // and pipeline, so neither is set again. Every root argument is still set for every dispatch.
    if (p.program->pso.Get() != _boundPso)
    {
        list->SetComputeRootSignature(p.program->root.Get());
        list->SetPipelineState(p.program->pso.Get());
        _boundPso = p.program->pso.Get();
    }

    for (UINT i = 0; i < UINT(p.program->params.size()); ++i)
    {
        switch (p.program->params[i].kind)
        {
        case ProgramSet_Dx12::ParamKind::Table:
            list->SetComputeRootDescriptorTable(i, (int(i) == p.outputParam && outputTable.ptr != 0) ? outputTable
                                                                                                     : p.tables[i]);
            break;
        case ProgramSet_Dx12::ParamKind::FrameValues:
            list->SetComputeRoot32BitConstants(i, kCbWords, frameWords, 0);
            break;
        case ProgramSet_Dx12::ParamKind::PushValues:
        {
            const uint32_t push[kPcWords] { p.graph->special, p.graph->index, 0, 0 };
            list->SetComputeRoot32BitConstants(i, kPcWords, push, 0);
            break;
        }
        }
    }

    list->Dispatch(p.graph->groupsX, p.graph->groupsY, 1);
}

void Context_Dx12::_ExecutePerPass(ID3D12GraphicsCommandList* list, std::vector<Pass>& passes,
                                   const FrameConstants& frame)
{
    uint32_t frameWords[kCbWords] {};
    std::memcpy(frameWords, &frame, sizeof(frame));

    for (auto& p : passes)
    {
        for (auto r : p.graph->reads)
            _Transition(_barriers, _textures[r], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (auto w : p.graph->writes)
        {
            auto& t = _textures[w];
            if (t.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                _Transition(_barriers, t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            else if (t.dirty)
            {
                // A write still unordered: the first-use clear, or a frame recorded with the waves that left it.
                _UavBarrier(_barriers, t.resource.Get());
                t.dirty = false;
            }
        }
        _Flush(list);

        _Dispatch(list, p, frameWords, {});

        // As the harness: every write is followed by a UAV barrier.
        for (auto w : p.graph->writes)
        {
            _UavBarrier(_barriers, _textures[w].resource.Get());
            _textures[w].dirty = false;
        }
        _Flush(list);
    }
}

void Context_Dx12::_ExecuteWaves(ID3D12GraphicsCommandList* list, const Waves& waves, const FrameConstants& pre,
                                 const FrameConstants& main, DirectTarget* target)
{
    uint32_t preWords[kCbWords] {};
    uint32_t mainWords[kCbWords] {};
    std::memcpy(preWords, &pre, sizeof(pre));
    std::memcpy(mainWords, &main, sizeof(main));

    for (const auto& wave : waves)
    {
        // One batch for the whole wave: what it reads becomes a shader resource (ordering the writes of earlier
        // waves), what it writes becomes a UAV, or gets a UAV barrier when it already is one and an earlier write
        // is still unordered. Nothing in a wave reads what another pass of the wave writes (ScheduleWaves).
        for (const auto& step : wave)
        {
            const Pass& p = step.main ? _main[step.index] : _pre[step.index];
            for (auto r : p.graph->reads)
                _Transition(_barriers, _textures[r], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (auto w : p.graph->writes)
            {
                if (target != nullptr && w == _graph.output)
                {
                    if (target->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                    {
                        // Last written by an earlier list and read since on FFX's present queue, both behind FFX's
                        // fences; the barrier costs nothing inside this batch.
                        _UavBarrier(_barriers, target->resource);
                    }
                    else
                    {
                        D3D12_RESOURCE_BARRIER b {};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = target->resource;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        b.Transition.StateBefore = target->state;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        _barriers.push_back(b);
                    }
                    continue;
                }

                auto& t = _textures[w];
                if (t.state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
                {
                    _Transition(_barriers, t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                }
                else if (t.dirty)
                {
                    _UavBarrier(_barriers, t.resource.Get());
                    t.dirty = false;
                }
            }
        }
        _Flush(list);

        for (const auto& step : wave)
        {
            const Pass& p = step.main ? _main[step.index] : _pre[step.index];
            const bool direct = target != nullptr && p.outputParam >= 0;
            _Dispatch(list, p, step.main ? mainWords : preWords,
                      direct ? target->table : D3D12_GPU_DESCRIPTOR_HANDLE {});
            for (auto w : p.graph->writes)
            {
                if (!(target != nullptr && w == _graph.output))
                    _textures[w].dirty = true;
            }
        }
        StudyTimestamp(list);
    }
}

bool Context_Dx12::_BindDirect(ID3D12Resource* target, D3D12_RESOURCE_STATES state, DirectTarget& out)
{
    const auto d = target->GetDesc();
    uint32_t viewFormat = 0;

    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1)
        _routeReason = "it is not a single-sample, single-slice 2D texture";
    else if (d.Width != _key.width || d.Height != _key.height)
        _routeReason = "its size is not the frame's";
    else if (!(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        _routeReason = "it was created without ALLOW_UNORDERED_ACCESS";
    else if ((viewFormat = DirectOutputFormat((uint32_t) d.Format, (uint32_t) _key.format)) == 0)
        _routeReason = "its format takes no UAV of the working format (an _SRGB format)";
    else
        _routeReason = "";

    if (*_routeReason != '\0')
    {
        _route = OutputRoute::Copy;
        return false;
    }

    // The working format was checked for typed UAV stores when the context was built (LSFG's own output needs
    // them too), and it is the view's format here.
    const uint32_t slot = _ringBase + uint32_t(_ringNext++ % kOutputRing);
    auto cpu = _heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(slot) * _descriptorSize;
    auto gpu = _heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(slot) * _descriptorSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
    v.Format = (DXGI_FORMAT) viewFormat;
    v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipSlice = 0;
    _key.device->CreateUnorderedAccessView(target, nullptr, &v, cpu);

    out.resource = target;
    out.state = state;
    out.table = gpu;
    _route = OutputRoute::Direct;
    return true;
}

void Context_Dx12::_CopyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* target, D3D12_RESOURCE_STATES targetState)
{
    auto& output = _textures[_graph.output];

    _Transition(_barriers, output, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (targetState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = targetState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        _barriers.push_back(b);
    }
    _Flush(list);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = target;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = output.resource.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Left pending: issued with the next batch, or by Record's last flush.
    if (targetState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = target;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = targetState;
        _barriers.push_back(b);
    }
}

void Context_Dx12::_ClearHistory(ID3D12GraphicsCommandList* list)
{
    const float zero[4] {};
    const auto clearCpuBase = _clearHeap->GetCPUDescriptorHandleForHeapStart();
    const auto gpuBase = _heap->GetGPUDescriptorHandleForHeapStart();

    for (auto& t : _textures)
    {
        if (t.clear)
            _Transition(_barriers, t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    _Flush(list);

    for (auto& t : _textures)
    {
        if (!t.clear)
            continue;
        auto gpu = gpuBase;
        gpu.ptr += UINT64(_tableDescriptors + t.clearSlot) * _descriptorSize;
        auto cpu = clearCpuBase;
        cpu.ptr += SIZE_T(t.clearSlot) * _descriptorSize;
        list->ClearUnorderedAccessViewFloat(gpu, cpu, t.resource.Get(), zero, 0, nullptr);
        t.dirty = true;
    }

    _recorded = true;
}

void Context_Dx12::_Ingest(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor,
                           D3D12_RESOURCE_STATES presentState)
{
    auto& source = _textures[_graph.source];
    _Transition(_barriers, source, D3D12_RESOURCE_STATE_COPY_DEST);
    if (presentState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = presentColor;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = presentState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        _barriers.push_back(b);
    }
    _Flush(list);

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = source.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = _history.SourceSlot(); // mip 0 of array slice `slot`
    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = presentColor;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_BOX box { 0, 0, 0, _key.width, _key.height, 1 };
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    if (presentState != D3D12_RESOURCE_STATE_COPY_SOURCE)
    {
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = presentColor;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = presentState;
        _barriers.push_back(b);
    }
}

bool Context_Dx12::RecordGenerated(ID3D12GraphicsCommandList* list, ID3D12Resource* presentColor,
                                   D3D12_RESOURCE_STATES presentState, uint32_t index, uint32_t count,
                                   ID3D12Resource* output, D3D12_RESOURCE_STATES outputState,
                                   const FrameConstants& frame, Schedule schedule)
{
    if (list == nullptr || output == nullptr || count == 0 || index >= count)
        return false;

    const bool perPass = schedule == Schedule::PerPass;

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    _boundPso = nullptr;
    _barriers.clear();

    if (!_recorded)
    {
        _ClearHistory(list);
    }

    if (index == 0)
    {
        if (presentColor == nullptr)
            return false;
        _Ingest(list, presentColor, presentState);
    }

    FrameConstants f = frame;
    f.timestamp = TimestampFor(index + 1, count);

    if (perPass)
    {
        _Flush(list);
        if (index == 0)
        {
            _ExecutePerPass(list, _pre, frame);
        }
        _ExecutePerPass(list, _main, f);
        _CopyOut(list, output, outputState);
        _Flush(list);
    }
    else
    {
        DirectTarget direct;
        const bool isDirect = _BindDirect(output, outputState, direct);
        _ExecuteWaves(list, index == 0 ? _wavesPreMain : _wavesMain, frame, f, isDirect ? &direct : nullptr);

        if (!isDirect)
        {
            _CopyOut(list, output, outputState);
        }
        else if (direct.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            _UavBarrier(_barriers, direct.resource);
        }
        else
        {
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = direct.resource;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = direct.state;
            _barriers.push_back(b);
        }
    }
    _Flush(list);
    return true;
}

void Context_Dx12::Record(ID3D12GraphicsCommandList* list, const FrameIo& io, const FrameConstants& frame,
                          Schedule schedule)
{
    const bool timed = _queries != nullptr;
    const uint32_t slot = uint32_t(_timedFrames % kTimingSlots);
    const bool perPass = schedule == Schedule::PerPass;

    ID3D12DescriptorHeap* heaps[] = { _heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    _boundPso = nullptr; // the list is FFX's: whatever it had bound is not ours
    _barriers.clear();

    if (timed)
        list->EndQuery(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);

    // First use: the feature history (and every other internal image) starts at zero, as in lsfg-vk.
    if (!_recorded)
    {
        _ClearHistory(list);
    }

    // Ingest: FFX's backbuffer into this base frame's source slot.
    _Ingest(list, io.presentColor, io.presentState);

    StudyTimestamp(list);

    uint32_t generate = std::min<uint32_t>(io.generate, uint32_t(io.outputs.size()));
    for (uint32_t k = 0; k < generate; ++k)
    {
        if (io.outputs[k] == nullptr)
        {
            generate = k;
            break;
        }
    }

    if (perPass)
    {
        // The harness's schedule, for the study's A/B timing: barriers around every dispatch, the copy-out.
        _Flush(list);
        _ExecutePerPass(list, _pre, frame);
        for (uint32_t k = 0; k < generate; ++k)
        {
            FrameConstants f = frame;
            f.timestamp = TimestampFor(k + 1, io.generatedFrames);
            _ExecutePerPass(list, _main, f);
            _CopyOut(list, io.outputs[k], io.outputStates[k]);
            _Flush(list);
        }
    }
    else if (generate == 0)
    {
        _ExecuteWaves(list, _wavesPre, frame, frame, nullptr);
    }
    else
    {
        for (uint32_t k = 0; k < generate; ++k)
        {
            FrameConstants f = frame;
            f.timestamp = TimestampFor(k + 1, io.generatedFrames);

            DirectTarget direct;
            const bool isDirect = _BindDirect(io.outputs[k], io.outputStates[k], direct);
            _ExecuteWaves(list, k == 0 ? _wavesPreMain : _wavesMain, frame, f, isDirect ? &direct : nullptr);

            if (!isDirect)
            {
                _CopyOut(list, io.outputs[k], io.outputStates[k]);
            }
            else if (direct.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                // FFX's state stays; order the writes before whatever reads them next on this queue.
                _UavBarrier(_barriers, direct.resource);
            }
            else
            {
                D3D12_RESOURCE_BARRIER b {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = direct.resource;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                b.Transition.StateAfter = direct.state;
                _barriers.push_back(b);
            }
        }
    }
    _Flush(list);
    StudyTimestamp(list);

    if (timed)
    {
        list->EndQuery(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        list->ResolveQueryData(_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, _readback.Get(),
                               UINT64(slot) * 2 * sizeof(uint64_t));
        _slotPerPass[slot] = perPass;
        ++_timedFrames;
    }
}

void Context_Dx12::CollectGpuTimes(std::vector<double>& out, std::vector<double>* perPass)
{
    if (_readbackData == nullptr || _timestampFrequency == 0 || _timedFrames <= kTimingLag)
        return;

    // Frame n resolves into slot n % kTimingSlots. It is read once it is kTimingLag frames old (FFX keeps far
    // fewer in flight) and only while frame n + kTimingSlots, which reuses the slot, cannot have run yet.
    const uint64_t newest = _timedFrames - 1 - kTimingLag;
    const uint64_t oldest = _timedFrames > kTimingSlots ? _timedFrames - kTimingSlots : 0;
    if (_collectedFrames < oldest)
        _collectedFrames = oldest;

    for (; _collectedFrames <= newest; ++_collectedFrames)
    {
        const uint32_t slot = uint32_t(_collectedFrames % kTimingSlots);
        const uint64_t begin = _readbackData[slot * 2];
        const uint64_t end = _readbackData[slot * 2 + 1];

        // A slot whose resolve had not landed still holds the pair of the frame kTimingSlots earlier: its
        // begin is older than the last one taken, so it is dropped rather than counted twice.
        if (end > begin && begin > _lastCollectedBegin)
        {
            _lastCollectedBegin = begin;
            const double ms = double(end - begin) * 1000.0 / double(_timestampFrequency);
            if (ms < 1000.0)
                (_slotPerPass[slot] && perPass != nullptr ? *perPass : out).push_back(ms);
        }
    }
}

} // namespace Lsfg
