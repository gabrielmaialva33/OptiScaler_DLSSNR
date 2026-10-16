// Test-only deterministic graphics input for the production D3D12 NGX exports.
#pragma once
#include <array>

namespace MovingScene
{
constexpr unsigned Width = 640, Height = 360, Warmup = 64, Count = 80;
constexpr auto Srv = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

struct Host
{
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* alloc;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    HANDLE event;
    UINT64 serial = 0, hz = 0;
    Ngx ngx;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    Com<ID3D12Resource> colour, depth, motion, output, zbuffer, vertices, constants, timing;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    Com<ID3D12DescriptorHeap> rtv, dsv;
    Com<ID3D12QueryHeap> queries;
    struct Capture
    {
        Com<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        unsigned pixelBytes = 0;
    };
    std::array<Capture, 4> captures;
    FILE* manifest = nullptr;
    unsigned evaluations = 0;
    const char* stage = "setup";

    void Begin()
    {
        Check(alloc->Reset(), "moving allocator Reset");
        Check(list->Reset(alloc, nullptr), "moving list Reset");
    }
    void Submit() { ColdNr::Submit(device, queue, list, fence, event, serial); }

    void Buffer(Com<ID3D12Resource>& resource, UINT64 bytes, D3D12_HEAP_TYPE heapType)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = heapType;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                              heapType == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                                 : D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&resource)),
              "moving buffer");
    }
    void Texture(Com<ID3D12Resource>& resource, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                 D3D12_RESOURCE_STATES state)
    {
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = Width;
        desc.Height = Height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        Check(
            device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
            "moving texture");
    }
    template <class T> void Upload(ID3D12Resource* buffer, const T* data, size_t count)
    {
        void* mapped = nullptr;
        const D3D12_RANGE noRead { 0, 0 };
        Check(buffer->Map(0, &noRead, &mapped), "moving upload Map");
        memcpy(mapped, data, sizeof(T) * count);
        buffer->Unmap(0, nullptr);
    }
    void Init(const char* dllPath)
    {
        ngx = LoadNgx(dllPath);
        ColdNr::NgxResult("moving production Init", ngx.Init(0x1337, L".", device, nullptr, NVSDK_NGX_Version_API));
        ColdNr::NgxResult("moving AllocateParameters", ngx.AllocateParameters(&params));
        params->Set(NVSDK_NGX_Parameter_Width, Width);
        params->Set(NVSDK_NGX_Parameter_Height, Height);
        params->Set(NVSDK_NGX_Parameter_OutWidth, Width);
        params->Set(NVSDK_NGX_Parameter_OutHeight, Height);
        params->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_DLAA);
        params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                    NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure);
        params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        Begin();
        const auto created = ngx.CreateFeature(list, NVSDK_NGX_Feature_SuperSampling, params, &feature);
        Submit(); // Even a failed feature create can record work.
        ColdNr::NgxResult("moving production CreateFeature", created);
        Require(feature != nullptr, "moving CreateFeature returned no handle");

        Texture(colour, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, Srv);
        Texture(motion, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, Srv);
        Texture(depth, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, Srv);
        Texture(output, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Texture(zbuffer, DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 3;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)), "moving RTV heap");
        auto handle = rtv->GetCPUDescriptorHandleForHeapStart();
        for (auto* resource : { colour.p, motion.p, depth.p })
        {
            device->CreateRenderTargetView(resource, nullptr, handle);
            handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv)), "moving DSV heap");
        device->CreateDepthStencilView(zbuffer, nullptr, dsv->GetCPUDescriptorHandleForHeapStart());
        D3D12_ROOT_PARAMETER parameter {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rd {};
        rd.NumParameters = 1;
        rd.pParameters = &parameter;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        Com<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors),
              "moving root serialize");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                          IID_PPV_ARGS(&root)),
              "moving root signature");
        Com<ID3DBlob> vs, ps;
        const auto compile = [&](const char* entry, const char* target, Com<ID3DBlob>& code)
        {
            const auto hr =
                D3DCompileFromFile(L"scene.hlsl", nullptr, nullptr, entry, target,
                                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr) && errors.p)
                Say("scene compile: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            Check(hr, "moving scene shader compile");
        };
        compile("VSMain", "vs_5_0", vs);
        compile("PSMain", "ps_5_0", ps);
        const D3D12_INPUT_ELEMENT_DESC input[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd {};
        pd.pRootSignature = root;
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout = { input, 2 };
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.DepthStencilState.StencilReadMask = pd.DepthStencilState.StencilWriteMask = 255;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 3;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        pd.RTVFormats[2] = DXGI_FORMAT_R32_FLOAT;
        for (auto& target : pd.BlendState.RenderTarget)
        {
            target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
            target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
            target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)), "moving graphics PSO");
        struct Vertex
        {
            float p[3], n[3];
        };
        std::vector<Vertex> geometry;
        // Each face has a constant normal; none of the analytic motion depends on the shading.
        for (unsigned axis = 0; axis < 3; ++axis)
            for (int sign : { -1, 1 })
            {
                Vertex corners[4] {};
                for (unsigned i = 0; i < 4; ++i)
                {
                    corners[i].p[axis] = static_cast<float>(sign);
                    corners[i].p[(axis + 1) % 3] = (i & 1) ? 1.f : -1.f;
                    corners[i].p[(axis + 2) % 3] = (i & 2) ? 1.f : -1.f;
                    corners[i].n[axis] = static_cast<float>(sign);
                }
                for (unsigned i : { 0u, 1u, 2u, 2u, 1u, 3u })
                    geometry.push_back(corners[i]);
            }
        Buffer(vertices, sizeof(Vertex) * geometry.size(), D3D12_HEAP_TYPE_UPLOAD);
        Upload(vertices, geometry.data(), geometry.size());
        Buffer(constants, sizeof(FrameConstants), D3D12_HEAP_TYPE_UPLOAD);
        D3D12_QUERY_HEAP_DESC qd {};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2;
        Check(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "moving timestamp heap");
        Buffer(timing, sizeof(UINT64) * 2, D3D12_HEAP_TYPE_READBACK);
        Check(queue->GetTimestampFrequency(&hz), "moving timestamp frequency");
        Require(hz != 0, "zero GPU timestamp frequency");
        unsigned slot = 0;
        for (auto* resource : { colour.p, motion.p, depth.p, output.p })
        {
            auto& c = captures[slot++];
            auto desc = resource->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &c.footprint, nullptr, nullptr, &c.bytes);
            c.pixelBytes = resource == depth.p || resource == motion.p ? 4 : 8;
            Buffer(c.buffer, c.bytes, D3D12_HEAP_TYPE_READBACK);
        }
        Require(fopen_s(&manifest, "moving.json", "w") == 0 && manifest, "moving manifest open");
        fprintf(manifest, "{\"width\":%u,\"height\":%u,\"frames\":%u,\"measured\":%u,\"samples\":[\n", Width, Height,
                Count, Count - Warmup);
    }

    void Copy(unsigned slot, ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
    {
        auto& c = captures[slot];
        ColdNr::Transition(list, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
        src.pResource = resource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = c.buffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = c.footprint;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        ColdNr::Transition(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    void Save(unsigned slot, unsigned frame, const char* suffix)
    {
        auto& c = captures[slot];
        void* mapped = nullptr;
        const D3D12_RANGE read { 0, static_cast<SIZE_T>(c.bytes) };
        Check(c.buffer->Map(0, &read, &mapped), "moving readback Map");
        FILE* file = nullptr;
        const auto path = "f" + std::to_string(frame) + "." + suffix;
        Require(fopen_s(&file, path.c_str(), "wb") == 0 && file, "moving readback open");
        bool ok = true;
        for (unsigned y = 0; y < Height; ++y)
            ok = fwrite(static_cast<char*>(mapped) + c.footprint.Offset + y * c.footprint.Footprint.RowPitch,
                        c.pixelBytes, Width, file) == Width &&
                 ok;
        const D3D12_RANGE noWrite { 0, 0 };
        c.buffer->Unmap(0, &noWrite);
        ok = fclose(file) == 0 && ok;
        Require(ok, "moving readback write");
    }
    void Matrix(const XMMATRIX& matrix)
    {
        XMFLOAT4X4 data;
        XMStoreFloat4x4(&data, matrix);
        const float* values = &data._11;
        fprintf(manifest, "[");
        for (unsigned i = 0; i < 16; ++i)
            fprintf(manifest, "%s%.9g", i ? "," : "", values[i]);
        fprintf(manifest, "]");
    }
    void Frame(unsigned frame)
    {
        stage = "render/evaluate/readback";
        const auto current = PoseForFrame(frame, static_cast<float>(Width) / Height);
        const auto previous = PoseForFrame(frame ? frame - 1 : 0, static_cast<float>(Width) / Height);
        const auto jitter = JitterForFrame(frame);
        const auto jitterPrev = JitterForFrame(frame ? frame - 1 : 0);
        FrameConstants c {};
        XMStoreFloat4x4(&c.viewProj, current.view * current.proj);
        XMStoreFloat4x4(&c.viewProjPrev, previous.view * previous.proj);
        XMStoreFloat4x4(&c.model, current.model);
        XMStoreFloat4x4(&c.modelPrev, previous.model);
        c.jitter[0] = 2 * jitter.x / Width;
        c.jitter[1] = -2 * jitter.y / Height;
        c.jitterPrev[0] = 2 * jitterPrev.x / Width;
        c.jitterPrev[1] = -2 * jitterPrev.y / Height;
        c.renderExtent[0] = Width;
        c.renderExtent[1] = Height;
        c.frameIndex = static_cast<float>(frame);
        Upload(constants, &c, 1);
        Begin();
        for (auto* resource : { colour.p, motion.p, depth.p })
            ColdNr::Transition(list, resource, Srv, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto first = rtv->GetCPUDescriptorHandleForHeapStart();
        auto handle = first;
        const float clearColour[] = { .03f, .04f, .06f, 1.f }, zero[4] {}, farDepth[] = { 1, 1, 1, 1 };
        list->ClearRenderTargetView(handle, clearColour, 0, nullptr);
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        list->ClearRenderTargetView(handle, zero, 0, nullptr);
        handle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        list->ClearRenderTargetView(handle, farDepth, 0, nullptr);
        const auto z = dsv->GetCPUDescriptorHandleForHeapStart();
        list->ClearDepthStencilView(z, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        list->OMSetRenderTargets(3, &first, TRUE, &z);
        list->SetGraphicsRootSignature(root);
        list->SetPipelineState(pipeline);
        list->SetGraphicsRootConstantBufferView(0, constants->GetGPUVirtualAddress());
        const D3D12_VIEWPORT viewport { 0, 0, static_cast<float>(Width), static_cast<float>(Height), 0, 1 };
        const D3D12_RECT scissor { 0, 0, Width, Height };
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        const D3D12_VERTEX_BUFFER_VIEW vertexView { vertices->GetGPUVirtualAddress(), 36 * 24, 24 };
        list->IASetVertexBuffers(0, 1, &vertexView);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(36, 1, 0, 0);
        for (auto* resource : { colour.p, motion.p, depth.p })
            ColdNr::Transition(list, resource, D3D12_RESOURCE_STATE_RENDER_TARGET, Srv);
        params->Set(NVSDK_NGX_Parameter_Color, colour.p);
        params->Set(NVSDK_NGX_Parameter_Output, output.p);
        params->Set(NVSDK_NGX_Parameter_Depth, depth.p);
        params->Set(NVSDK_NGX_Parameter_MotionVectors, motion.p);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jitter.x);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jitter.y);
        params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.f); // Shader already writes pixels, not UV units.
        params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.f);
        params->Set(NVSDK_NGX_Parameter_Reset, frame == 0 ? 1 : 0);
        params->Set(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 1000.f / 120.f);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, Width);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, Height);
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        const auto result = ngx.EvaluateFeature(list, feature, params, nullptr);
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timing, 0);
        if (frame >= Warmup)
        {
            Copy(0, colour, Srv);
            Copy(1, motion, Srv);
            Copy(2, depth, Srv);
            Copy(3, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        Submit();
        ColdNr::NgxResult("moving production EvaluateFeature", result);
        ++evaluations;
        Check(device->GetDeviceRemovedReason(), "moving device health");
        if (frame >= Warmup)
        {
            UINT64* ticks = nullptr;
            const D3D12_RANGE read { 0, sizeof(UINT64) * 2 };
            Check(timing->Map(0, &read, reinterpret_cast<void**>(&ticks)), "moving timing Map");
            Require(ticks[1] > ticks[0], "moving timestamps are zero or reversed");
            const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000. / static_cast<double>(hz);
            const D3D12_RANGE noWrite { 0, 0 };
            timing->Unmap(0, &noWrite);
            fprintf(manifest,
                    "%s{\"frame\":%u,\"gpu_ms\":%.9g,\"jitter\":[%.9g,%.9g],\"inverse\":", frame == Warmup ? "" : ",\n",
                    frame, ms, jitter.x, jitter.y);
            Matrix(XMMatrixInverse(nullptr, current.model * current.view * current.proj));
            fprintf(manifest, ",\"previous\":");
            Matrix(previous.model * previous.view * previous.proj);
            fprintf(manifest, "}");
            fflush(manifest);
            Save(0, frame, "source");
            Save(1, frame, "motion");
            Save(2, frame, "depth");
            Save(3, frame, "output");
            Say("MOVING frame=%u gpu_ms=%.6f\n", frame, ms);
        }
        else
            Sleep(20); // Allow the production 500ms settling gates to expire during warmup only.
        PumpMessages();
    }
    void Finish()
    {
        stage = "teardown after fence";
        ColdNr::NgxResult("moving ReleaseFeature", ngx.ReleaseFeature(feature));
        feature = nullptr;
        ColdNr::NgxResult("moving DestroyParameters", ngx.DestroyParameters(params));
        params = nullptr;
        // The legacy sweep found Shutdown1 can fault in the driver. Preserve that finding here.
        ColdNr::NgxResult("moving Shutdown", ngx.Shutdown(device));
        Require(fprintf(manifest, "\n]}\n") > 0 && fclose(manifest) == 0, "moving manifest close");
        manifest = nullptr;
        Say("MOVING-SCENE PASS: evaluations=%u captures=%u\n", evaluations, Count - Warmup);
    }
};

static void Run(const char* dllPath, ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12CommandAllocator* alloc,
                ID3D12GraphicsCommandList* list, ID3D12Fence* fence, HANDLE event)
{
    // Keep all GPU-owned objects outside the try block. A failed/unknown fence cannot unwind them.
    Host host { device, queue, alloc, list, fence, event };
    try
    {
        host.Init(dllPath);
        for (unsigned frame = 0; frame < Count; ++frame)
            host.Frame(frame);
        host.Finish();
    }
    catch (const std::exception& e)
    {
        Say("MOVING-SCENE FAIL stage=%s evaluations=%u serial=%llu: %s\n", host.stage, host.evaluations, host.serial,
            e.what());
        ExitProcess(1);
    }
}
} // namespace MovingScene
