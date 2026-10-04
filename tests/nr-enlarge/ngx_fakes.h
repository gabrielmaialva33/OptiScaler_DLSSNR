// A recording stand-in for the NGX driver core: a parameter table that keeps every Set with its type, and the
// five entry points Transfer 2's private DLSS calls, logging each call in order. Shared by adapter.cpp (the
// NGX half alone) and host.cpp (the pass's Transfer 2 section around it).
#pragma once

#include <dlssnr/DlssNr_PrivateSr.h>

#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// What a D3D12 resource, list and device are to these suites: identities with a little state. host.cpp adds the
// resource states and shapes it checks.
struct ID3D12Resource
{
    int format = 0;
    unsigned width = 0, height = 0;
    unsigned releases = 0;
    std::string name;
    void Release() { ++releases; }
};
struct ID3D12GraphicsCommandList
{
    int id = 0;
};
struct ID3D12Device
{
    int id = 0;
};

namespace Ngx
{
using Value =
    std::variant<unsigned long long, float, double, unsigned int, int, ID3D11Resource*, ID3D12Resource*, void*>;

// Everything written, keyed by name, with the overload it came through: a uint where NVIDIA's helper writes an
// int is a different slot on a real table, so the type is part of what is checked.
struct Table final : NVSDK_NGX_Parameter
{
    std::map<std::string, Value> values;
    bool destroyed = false;

    void Put(const char* name, Value value)
    {
        assert(!destroyed);
        values[name] = value;
    }
    void Set(const char* n, unsigned long long v) override { Put(n, v); }
    void Set(const char* n, float v) override { Put(n, v); }
    void Set(const char* n, double v) override { Put(n, v); }
    void Set(const char* n, unsigned int v) override { Put(n, v); }
    void Set(const char* n, int v) override { Put(n, v); }
    void Set(const char* n, ID3D11Resource* v) override { Put(n, v); }
    void Set(const char* n, ID3D12Resource* v) override { Put(n, v); }
    void Set(const char* n, void* v) override { Put(n, v); }
    NVSDK_NGX_Result Get(const char*, unsigned long long*) const override
    {
        return NVSDK_NGX_Result_FAIL_NotImplemented;
    }
    NVSDK_NGX_Result Get(const char*, float*) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, double*) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, unsigned int*) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, int*) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, ID3D11Resource**) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, ID3D12Resource**) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    NVSDK_NGX_Result Get(const char*, void**) const override { return NVSDK_NGX_Result_FAIL_NotImplemented; }
    void Reset() override { values.clear(); }

    template <class T> T At(const char* name) const
    {
        const auto found = values.find(name);
        if (found == values.end())
        {
            std::fprintf(stderr, "parameter %s was never written\n", name);
            assert(false);
        }
        const T* value = std::get_if<T>(&found->second);
        if (value == nullptr)
        {
            std::fprintf(stderr, "parameter %s was written through another overload (index %zu)\n", name,
                         found->second.index());
            assert(false);
        }
        return *value;
    }
};

// The calls, in order, and what each was handed.
struct Call
{
    std::string what;
    ID3D12GraphicsCommandList* list = nullptr;
    const NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Parameter* table = nullptr;
    NVSDK_NGX_Feature feature = NVSDK_NGX_Feature_Reserved0;
    bool callback = false;
};

struct Driver
{
    std::vector<Call> calls;
    std::vector<std::unique_ptr<Table>> tables; // owned here, so an abandoned one is not a leak to the sanitizer
    std::vector<std::unique_ptr<NVSDK_NGX_Handle>> handles;
    std::vector<const NVSDK_NGX_Handle*> released;

    NVSDK_NGX_Result allocateResult = NVSDK_NGX_Result_Success;
    bool allocateNull = false;
    NVSDK_NGX_Result createResult = NVSDK_NGX_Result_Success;
    bool createNullHandle = false;
    NVSDK_NGX_Result evaluateResult = NVSDK_NGX_Result_Success;

    // Run by EvaluateFeature and ReleaseFeature before they answer, so a suite can look at the world as the
    // call sees it (resource states, the retirement list).
    void (*onEvaluate)(const Table&) = nullptr;
    void (*onRelease)(const NVSDK_NGX_Handle*) = nullptr;

    unsigned Count(const std::string& what) const
    {
        unsigned n = 0;
        for (const auto& c : calls)
            n += c.what == what;
        return n;
    }
    const Table& LastTable() const
    {
        assert(!tables.empty());
        return *tables.back();
    }
};

inline Driver driver;

inline NVSDK_NGX_Result Allocate(NVSDK_NGX_Parameter** out)
{
    driver.calls.push_back({ "allocate" });
    if (driver.allocateResult != NVSDK_NGX_Result_Success)
        return driver.allocateResult;
    if (driver.allocateNull)
    {
        *out = nullptr;
        return NVSDK_NGX_Result_Success;
    }
    driver.tables.push_back(std::make_unique<Table>());
    *out = driver.tables.back().get();
    return NVSDK_NGX_Result_Success;
}

inline NVSDK_NGX_Result Destroy(NVSDK_NGX_Parameter* table)
{
    auto* t = static_cast<Table*>(table);
    assert(t != nullptr && !t->destroyed);
    t->destroyed = true;
    Call c { "destroy" };
    c.table = table;
    driver.calls.push_back(c);
    return NVSDK_NGX_Result_Success;
}

inline NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* table,
                               NVSDK_NGX_Handle** out)
{
    assert(table != nullptr && !static_cast<Table*>(table)->destroyed);
    Call c { "create", list };
    c.table = table;
    c.feature = feature;
    driver.calls.push_back(c);
    if (driver.createResult != NVSDK_NGX_Result_Success)
        return driver.createResult;
    if (driver.createNullHandle)
    {
        *out = nullptr;
        return NVSDK_NGX_Result_Success;
    }
    driver.handles.push_back(std::make_unique<NVSDK_NGX_Handle>());
    driver.handles.back()->Id = static_cast<unsigned>(driver.handles.size());
    *out = driver.handles.back().get();
    return NVSDK_NGX_Result_Success;
}

inline NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                                 const NVSDK_NGX_Parameter* table, PFN_NVSDK_NGX_ProgressCallback callback)
{
    assert(handle != nullptr && table != nullptr && !static_cast<const Table*>(table)->destroyed);
    for (const auto* gone : driver.released)
        assert(gone != handle);
    Call c { "evaluate", list, handle, table };
    c.callback = callback != nullptr;
    driver.calls.push_back(c);
    if (driver.onEvaluate)
        driver.onEvaluate(*static_cast<const Table*>(table));
    return driver.evaluateResult;
}

inline NVSDK_NGX_Result Release(NVSDK_NGX_Handle* handle)
{
    assert(handle != nullptr);
    for (const auto* gone : driver.released)
        assert(gone != handle);
    Call c { "release" };
    c.handle = handle;
    driver.calls.push_back(c);
    if (driver.onRelease)
        driver.onRelease(handle);
    driver.released.push_back(handle);
    return NVSDK_NGX_Result_Success;
}

inline DlssNr::PrivateSr::Api FullApi()
{
    DlssNr::PrivateSr::Api api;
    api.allocate = &Allocate;
    api.destroy = &Destroy;
    api.create = &Create;
    api.evaluate = &Evaluate;
    api.release = &Release;
    return api;
}

inline void Clear() { driver = Driver {}; }
} // namespace Ngx
