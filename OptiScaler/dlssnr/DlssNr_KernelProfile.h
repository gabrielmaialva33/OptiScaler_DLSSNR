#pragma once

// Kernel-variant census and per-group GPU timing of NVIDIA's DLSS-NR model from NvAPI launches.
// Ported from janblade (https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass, commit db065ec2, GPL-3.0).
//
// The DLSS-NR model launches every kernel through NvAPI_D3D12_LaunchCuKernelChain on the command list.
// When enabled via [DlssNr] KernelProfile, timestamps are sampled across 3 of every 240 evaluations
// and resolved to readback buffers without blocking the CPU (using DlssNr::Submission::TimingCertificate).

#ifdef _WIN32
#include <d3d12.h>
#include <nvapi.h>
#include "DlssNr_Submission.h"
#include <Config.h>
#include <Logger.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace DlssNr::KernelProfile
{
constexpr const char* kGroupNames[] = {
    "pre_block",          "post_block",     "swin_1h_32", "swin_2h_64",        "swin_4h_128",
    "swin_8h_256",        "split_swin_16h", "vit_1d",     "cg2r_post_process", "cg2r_copy",
    "dec_input_upsample", "cb_clear",       "other"
};
constexpr unsigned kGroupCount = static_cast<unsigned>(sizeof(kGroupNames) / sizeof(kGroupNames[0]));
constexpr unsigned kOtherGroupIndex = kGroupCount - 1;

struct Info
{
    unsigned char group = kOtherGroupIndex;
    bool fp8 = false;
    std::string name;
};

inline Info Classify(const char* rawName)
{
    Info info;
    if (rawName == nullptr || rawName[0] == '\0')
    {
        info.name = "unknown";
        return info;
    }

    info.name = rawName;
    info.fp8 = (info.name.find("_fp8") != std::string::npos);

    struct Rule
    {
        const char* pattern;
        unsigned char group;
    };

    // Specific prefixes precede generic ones (e.g. pre_block and split_swin before swin)
    static constexpr Rule kRules[] = {
        { "pre_block", 0 },    { "post_block", 1 },        { "split_swin", 6 }, { "cc_vit_1d", 7 },
        { "vit_1d", 7 },       { "cg2r_post_process", 8 }, { "cg2r_copy", 9 },  { "dec_input_upsample", 10 },
        { "cc_cb_clear", 11 }, { "cb_clear", 11 },         { "swin_1h_32", 2 }, { "swin_2h_64", 3 },
        { "swin_4h_128", 4 },  { "swin_8h_256", 5 }
    };

    for (const auto& r : kRules)
    {
        if (info.name.find(r.pattern) != std::string::npos)
        {
            info.group = r.group;
            return info;
        }
    }

    info.group = kOtherGroupIndex;
    return info;
}

struct GroupStats
{
    double meanMs = 0.0;
    double p95Ms = 0.0;
    double sharePercent = 0.0;
    unsigned kernelCount = 0;
};

struct AggregateReport
{
    uint64_t evaluationsSampled = 0;
    double totalMeanMs = 0.0;
    double totalP95Ms = 0.0;
    unsigned fp8Count = 0;
    unsigned fp16Count = 0;
    std::string fp8SampleName;
    std::string fp16SampleName;
    GroupStats groups[kGroupCount] {};

    std::string Format() const
    {
        std::string s = std::format("DLSS-NR kernel profile (sampled {} evals): total {:.2f} ms GPU (p95 {:.2f} ms), "
                                    "{} kernels ({} fp8, {} fp16). e.g. fp8: {} | plain: {} | ",
                                    evaluationsSampled, totalMeanMs, totalP95Ms, fp8Count + fp16Count, fp8Count,
                                    fp16Count, fp8SampleName.empty() ? "none" : fp8SampleName,
                                    fp16SampleName.empty() ? "none" : fp16SampleName);

        std::vector<unsigned> order(kGroupCount);
        for (unsigned g = 0; g < kGroupCount; ++g)
            order[g] = g;
        std::sort(order.begin(), order.end(),
                  [&](unsigned a, unsigned b) { return groups[a].meanMs > groups[b].meanMs; });

        for (unsigned g : order)
        {
            if (groups[g].kernelCount == 0 && groups[g].meanMs <= 0.0)
                continue;
            s += std::format("{}: {:.2f} ms [p95 {:.2f} ms] ({:.1f}%, x{})  ", kGroupNames[g], groups[g].meanMs,
                             groups[g].p95Ms, groups[g].sharePercent, groups[g].kernelCount);
        }

        s += "(approximate: chained kernels overlap)";
        return s;
    }
};

// Pure CPU stats aggregator over collected raw evaluation timings.
class MetricsAggregator
{
    struct Entry
    {
        double totalMs = 0.0;
        double groupMs[kGroupCount] {};
        unsigned groupKernels[kGroupCount] {};
        unsigned fp8 = 0;
        unsigned plain = 0;
        std::string fp8Example;
        std::string plainExample;
    };

    std::vector<Entry> _entries;

  public:
    void Reset() { _entries.clear(); }

    void Add(double totalMs, const double* groupMs, const unsigned* groupKernels, unsigned fp8, unsigned plain,
             const std::string& fp8Ex, const std::string& plainEx)
    {
        Entry e;
        e.totalMs = totalMs;
        for (unsigned g = 0; g < kGroupCount; ++g)
        {
            e.groupMs[g] = groupMs != nullptr ? groupMs[g] : 0.0;
            e.groupKernels[g] = groupKernels != nullptr ? groupKernels[g] : 0;
        }
        e.fp8 = fp8;
        e.plain = plain;
        e.fp8Example = fp8Ex;
        e.plainExample = plainEx;
        _entries.push_back(std::move(e));
    }

    size_t Count() const { return _entries.size(); }

    AggregateReport BuildReport() const
    {
        AggregateReport rep {};
        if (_entries.empty())
            return rep;

        rep.evaluationsSampled = _entries.size();
        const double invN = 1.0 / static_cast<double>(_entries.size());
        std::vector<double> totals;
        totals.reserve(_entries.size());

        std::vector<std::vector<double>> perGroup(kGroupCount);
        for (auto& pg : perGroup)
            pg.reserve(_entries.size());

        double sumFp8 = 0.0;
        double sumPlain = 0.0;
        double sumGroupKernels[kGroupCount] {};

        for (const auto& e : _entries)
        {
            totals.push_back(e.totalMs);
            sumFp8 += e.fp8;
            sumPlain += e.plain;
            if (rep.fp8SampleName.empty() && !e.fp8Example.empty())
                rep.fp8SampleName = e.fp8Example;
            if (rep.fp16SampleName.empty() && !e.plainExample.empty())
                rep.fp16SampleName = e.plainExample;

            for (unsigned g = 0; g < kGroupCount; ++g)
            {
                perGroup[g].push_back(e.groupMs[g]);
                sumGroupKernels[g] += e.groupKernels[g];
            }
        }

        // Kernel counts per evaluate (average across the window)
        rep.fp8Count = static_cast<unsigned>(std::round(sumFp8 * invN));
        rep.fp16Count = static_cast<unsigned>(std::round(sumPlain * invN));

        const auto calcStats = [](std::vector<double>& v) -> std::pair<double, double>
        {
            if (v.empty())
                return { 0.0, 0.0 };
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (double x : v)
                sum += x;
            double mean = sum / v.size();
            size_t p95Idx = static_cast<size_t>(std::ceil(0.95 * v.size())) - 1;
            p95Idx = std::min(p95Idx, v.size() - 1);
            return { mean, v[p95Idx] };
        };

        const auto [totMean, totP95] = calcStats(totals);
        rep.totalMeanMs = totMean;
        rep.totalP95Ms = totP95;

        for (unsigned g = 0; g < kGroupCount; ++g)
        {
            const auto [gMean, gP95] = calcStats(perGroup[g]);
            rep.groups[g].meanMs = gMean;
            rep.groups[g].p95Ms = gP95;
            rep.groups[g].kernelCount = static_cast<unsigned>(std::round(sumGroupKernels[g] * invN));
            rep.groups[g].sharePercent = totMean > 1e-6 ? (gMean / totMean) * 100.0 : 0.0;
        }

        return rep;
    }
};

#ifdef _WIN32
// D3D12 Profiler managing query heaps and non-blocking readbacks.
class Profiler
{
  public:
    static Profiler& Instance()
    {
        // Deliberately leaked raw allocation to prevent destruction order crashes during process exit
        static Profiler* p = new Profiler();
        return *p;
    }

    void RegisterFunction(void* handle, const char* name)
    {
        if (handle == nullptr || name == nullptr)
            return;
        std::lock_guard lock(_mutex);
        _fnInfo[handle] = Classify(name);
    }

    void UnregisterFunction(void* handle)
    {
        if (handle == nullptr)
            return;
        std::lock_guard lock(_mutex);
        _fnInfo.erase(handle);
    }

    void Begin(ID3D12GraphicsCommandList* cmd, bool enabled)
    {
        if (!enabled || cmd == nullptr)
            return;

        std::lock_guard lock(_mutex);
        ++_evaluation;
        CollectLocked();
        _recordingSlot = -1;

        // Sample 3 consecutive evaluations out of every 240
        if ((_evaluation % 240) >= 3)
            return;

        ID3D12Device* device = nullptr;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
            return;

        int chosenSlot = -1;
        for (size_t i = 0; i < kSlots; ++i)
        {
            if (!_slots[i].pending)
            {
                chosenSlot = static_cast<int>(i);
                break;
            }
        }
        if (chosenSlot < 0)
        {
            device->Release();
            return;
        }

        auto& s = _slots[chosenSlot];
        if (!InitSlotLocked(s, device))
        {
            device->Release();
            return;
        }
        device->Release();

        // Independent from NR resource-retirement mode; refusal drops measurement only.
        if (!DlssNr::Submission::Track(cmd, s.usage))
        {
            s.usage = {};
            return;
        }

        s.cmd = cmd;
        s.queries = 0;
        s.fp8Kernels = 0;
        s.plainKernels = 0;
        s.recs.clear();
        s.fp8Example.clear();
        s.plainExample.clear();

        // Initial timestamp at query 0
        cmd->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, s.queries++);
        _recordingSlot = chosenSlot;
    }

    void Launched(ID3D12GraphicsCommandList* cmd, const void* const* functionHandles, uint32_t count)
    {
        if (cmd == nullptr || functionHandles == nullptr || count == 0)
            return;

        std::lock_guard lock(_mutex);
        if (_recordingSlot < 0 || _slots[_recordingSlot].cmd != cmd)
            return;

        auto& s = _slots[_recordingSlot];
        if (s.queries >= kCap)
            return;

        Rec rec {};
        rec.timeGroup = kOtherGroupIndex;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto it = _fnInfo.find(functionHandles[i]);
            const Info info = it != _fnInfo.end() ? it->second : Classify("unknown");
            rec.kernels[info.group]++;
            if (i == 0)
                rec.timeGroup = info.group;

            if (info.fp8)
            {
                s.fp8Kernels++;
                if (s.fp8Example.empty())
                    s.fp8Example = info.name;
            }
            else
            {
                s.plainKernels++;
                if (s.plainExample.empty())
                    s.plainExample = info.name;
            }
        }

        s.recs.push_back(std::move(rec));
        cmd->EndQuery(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, s.queries++);
    }

    void End(ID3D12GraphicsCommandList* cmd)
    {
        if (cmd == nullptr)
            return;

        std::lock_guard lock(_mutex);
        if (_recordingSlot < 0 || _slots[_recordingSlot].cmd != cmd)
            return;

        auto& s = _slots[_recordingSlot];
        _recordingSlot = -1;

        if (s.queries <= 1)
        {
            static bool s_loggedNoLaunches = false;
            if (!s_loggedNoLaunches)
            {
                LOG_WARN(
                    "DLSS-NR kernel profile: no NvAPI CUDA launches intercepted on command list; profiling inactive "
                    "(wrapper not queried by model or kernel launches occur on another command list)");
                s_loggedNoLaunches = true;
            }
            s.pending = false;
            return;
        }

        cmd->ResolveQueryData(s.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, s.queries, s.readback, 0);
        s.pending = true;
    }

    std::vector<std::string> TakeReports()
    {
        std::lock_guard lock(_mutex);
        std::vector<std::string> res;
        res.swap(_reports);
        return res;
    }

  private:
    static constexpr size_t kSlots = 6;
    static constexpr uint32_t kCap = 1024;

    struct Rec
    {
        unsigned char timeGroup = kOtherGroupIndex;
        unsigned kernels[kGroupCount] {};
    };

    struct Slot
    {
        ID3D12QueryHeap* heap = nullptr;
        ID3D12Resource* readback = nullptr;
        ID3D12Device* device = nullptr;
        ID3D12GraphicsCommandList* cmd = nullptr;
        DlssNr::Submission::Usage usage {};
        std::vector<Rec> recs;
        uint32_t queries = 0;
        unsigned fp8Kernels = 0;
        unsigned plainKernels = 0;
        bool pending = false;
        std::string fp8Example;
        std::string plainExample;
    };

    Profiler() = default;

    bool InitSlotLocked(Slot& s, ID3D12Device* device)
    {
        if (s.device != device)
        {
            if (s.heap != nullptr)
            {
                s.heap->Release();
                s.heap = nullptr;
            }
            if (s.readback != nullptr)
            {
                s.readback->Release();
                s.readback = nullptr;
            }
            s.device = device;
        }

        if (s.heap != nullptr && s.readback != nullptr)
            return true;

        D3D12_QUERY_HEAP_DESC qDesc {};
        qDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qDesc.Count = kCap;
        qDesc.NodeMask = 0;

        if (FAILED(device->CreateQueryHeap(&qDesc, IID_PPV_ARGS(&s.heap))) || s.heap == nullptr)
            return false;

        D3D12_HEAP_PROPERTIES rbHeap {};
        rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bDesc {};
        bDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bDesc.Width = static_cast<UINT64>(kCap) * sizeof(UINT64);
        bDesc.Height = 1;
        bDesc.DepthOrArraySize = 1;
        bDesc.MipLevels = 1;
        bDesc.SampleDesc.Count = 1;
        bDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (FAILED(device->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &bDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&s.readback))) ||
            s.readback == nullptr)
        {
            if (s.heap != nullptr)
            {
                s.heap->Release();
                s.heap = nullptr;
            }
            return false;
        }

        return true;
    }

    void CollectLocked()
    {
        for (auto& s : _slots)
        {
            if (!s.pending)
                continue;

            const auto certificate = DlssNr::Submission::TimingCertificate(s.usage);
            if (!certificate.terminal)
                continue;

            if (!certificate.accepted || certificate.frequency == 0)
            {
                s.pending = false;
                continue;
            }

            void* mapped = nullptr;
            const D3D12_RANGE range { 0, static_cast<SIZE_T>(s.queries) * sizeof(UINT64) };
            if (FAILED(s.readback->Map(0, &range, &mapped)) || mapped == nullptr)
            {
                s.pending = false;
                continue;
            }

            const UINT64* data = static_cast<const UINT64*>(mapped);
            std::vector<UINT64> ticks(data, data + s.queries);
            const D3D12_RANGE noWrite { 0, 0 };
            s.readback->Unmap(0, &noWrite);
            s.pending = false;

            ReportLocked(s, ticks, certificate.frequency);
        }
    }

    void ReportLocked(const Slot& s, const std::vector<UINT64>& ticks, UINT64 frequency)
    {
        if (ticks.size() < 2 || ticks.front() == 0 || ticks.back() < ticks.front() || s.recs.size() + 1 != ticks.size())
            return;

        double groupMs[kGroupCount] {};
        unsigned groupKernels[kGroupCount] {};

        for (size_t i = 0; i < s.recs.size(); ++i)
        {
            const double ms = ticks[i + 1] >= ticks[i] ? (static_cast<double>(ticks[i + 1] - ticks[i]) * 1000.0) /
                                                             static_cast<double>(frequency)
                                                       : 0.0;
            groupMs[s.recs[i].timeGroup] += ms;
            for (unsigned g = 0; g < kGroupCount; ++g)
                groupKernels[g] += s.recs[i].kernels[g];
        }

        const double totalMs =
            static_cast<double>(ticks.back() - ticks.front()) * 1000.0 / static_cast<double>(frequency);
        _aggregator.Add(totalMs, groupMs, groupKernels, s.fp8Kernels, s.plainKernels, s.fp8Example, s.plainExample);

        // One report line per 3-sample window, then reset aggregator
        if (_aggregator.Count() >= 3)
        {
            const auto rep = _aggregator.BuildReport();
            _reports.push_back(rep.Format());
            _aggregator.Reset();
        }
    }

    std::mutex _mutex;
    std::unordered_map<const void*, Info> _fnInfo;
    Slot _slots[kSlots];
    int _recordingSlot = -1;
    uint64_t _evaluation = 0;
    MetricsAggregator _aggregator;
    std::vector<std::string> _reports;
};

namespace Detail
{
static decltype(&NvAPI_D3D12_CreateCuFunction) o_CreateCuFunction = nullptr;
static decltype(&NvAPI_D3D12_DestroyCuFunction) o_DestroyCuFunction = nullptr;
static decltype(&NvAPI_D3D12_LaunchCuKernelChain) o_LaunchCuKernelChain = nullptr;

inline NvAPI_Status __cdecl hkNvAPI_D3D12_CreateCuFunction(ID3D12Device* pDevice, NVDX_ObjectHandle hModule,
                                                           const char* functionName, NVDX_ObjectHandle* pFunction)
{
    if (!o_CreateCuFunction)
        return NVAPI_ERROR;
    const auto status = o_CreateCuFunction(pDevice, hModule, functionName, pFunction);
    if (status == NVAPI_OK && pFunction && *pFunction && functionName)
        Profiler::Instance().RegisterFunction(*pFunction, functionName);
    return status;
}

inline NvAPI_Status __cdecl hkNvAPI_D3D12_DestroyCuFunction(ID3D12Device* pDevice, NVDX_ObjectHandle hFunction)
{
    if (hFunction)
        Profiler::Instance().UnregisterFunction(hFunction);
    return o_DestroyCuFunction ? o_DestroyCuFunction(pDevice, hFunction) : NVAPI_ERROR;
}

inline NvAPI_Status __cdecl hkNvAPI_D3D12_LaunchCuKernelChain(ID3D12GraphicsCommandList* pCommandList,
                                                              const NVAPI_CU_KERNEL_LAUNCH_PARAMS* pKernels,
                                                              NvU32 numKernels)
{
    if (!o_LaunchCuKernelChain)
        return NVAPI_ERROR;
    const auto status = o_LaunchCuKernelChain(pCommandList, pKernels, numKernels);
    if (status == NVAPI_OK && pCommandList && pKernels && numKernels > 0)
    {
        std::vector<const void*> handles(numKernels);
        for (NvU32 i = 0; i < numKernels; ++i)
            handles[i] = pKernels[i].hFunction;
        Profiler::Instance().Launched(pCommandList, handles.data(), numKernels);
    }
    return status;
}
} // namespace Detail

inline void* WrapNvapi(uint32_t interfaceId, void* functionPointer)
{
    if (!functionPointer)
        return nullptr;

    if (!Config::Instance()->DlssNrKernelProfile.value_or_default())
        return functionPointer;

    if (interfaceId == 0xe2436e22) // NvAPI_D3D12_CreateCuFunction
    {
        Detail::o_CreateCuFunction = reinterpret_cast<decltype(&NvAPI_D3D12_CreateCuFunction)>(functionPointer);
        return reinterpret_cast<void*>(&Detail::hkNvAPI_D3D12_CreateCuFunction);
    }
    if (interfaceId == 0xdf295ea6) // NvAPI_D3D12_DestroyCuFunction
    {
        Detail::o_DestroyCuFunction = reinterpret_cast<decltype(&NvAPI_D3D12_DestroyCuFunction)>(functionPointer);
        return reinterpret_cast<void*>(&Detail::hkNvAPI_D3D12_DestroyCuFunction);
    }
    if (interfaceId == 0x24973538) // NvAPI_D3D12_LaunchCuKernelChain
    {
        Detail::o_LaunchCuKernelChain = reinterpret_cast<decltype(&NvAPI_D3D12_LaunchCuKernelChain)>(functionPointer);
        return reinterpret_cast<void*>(&Detail::hkNvAPI_D3D12_LaunchCuKernelChain);
    }

    return functionPointer;
}
#endif

} // namespace DlssNr::KernelProfile
