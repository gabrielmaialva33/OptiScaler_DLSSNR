#include "pch.h"
#include "DlssNr_DirectRuntime.h"
#include "DlssNr_PeScan.h"

#include <Logger.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Mirrors dlssnr_forwarder.cpp: the parameter-block setters, the snippet's D3D12 entry points, the fault
// containment and the five calls the D3D12 host makes. What differs is only where the calls come from
// (this module, not a DLL named to pass the caller check) and the caller-path adapter that makes that
// acceptable to the model. See DlssNr_DirectRuntime.h. tests/nr-model-loader holds this file to the
// forwarder: every parameter write of the five calls, name, value and order; the Guarded model calls;
// the setters; and FaultFilter.

namespace
{

// ---------------------------------------------------------------------------------------------------
// The capability block's setters, driven through its vtable exactly as the forwarder drives them.
// ---------------------------------------------------------------------------------------------------

constexpr int VT_SET_ULL = 0;
int g_floatSlot = 1; // set by the host once it has round-tripped a value (SetFloatSlot)
constexpr int VT_SET_UINT = 3;
constexpr int VT_GET_ULL = VT_SET_ULL + 8;

using PFN_SetULL = void(__thiscall*)(void*, const char*, unsigned long long);
using PFN_SetFloat = void(__thiscall*)(void*, const char*, float);
using PFN_SetUInt = void(__thiscall*)(void*, const char*, unsigned int);
using PFN_GetULL = int(__thiscall*)(void*, const char*, unsigned long long*);
using PFN_GetFloat = int(__thiscall*)(void*, const char*, float*);

void setUInt(void* params, const char* name, unsigned int v)
{
    void** vt = *reinterpret_cast<void***>(params);
    reinterpret_cast<PFN_SetUInt>(vt[VT_SET_UINT])(params, name, v);
}

void setFloat(void* params, const char* name, float v)
{
    void** vt = *reinterpret_cast<void***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[g_floatSlot])(params, name, v);
}

void setResource(void* params, const char* name, ID3D12Resource* v)
{
    void** vt = *reinterpret_cast<void***>(params);
    reinterpret_cast<PFN_SetULL>(vt[VT_SET_ULL])(params, name, (unsigned long long) v);
}

// ---------------------------------------------------------------------------------------------------
// The snippet.
// ---------------------------------------------------------------------------------------------------

using PFN_NrInitExt = int(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using PFN_NrCreate = int(__cdecl*)(ID3D12GraphicsCommandList*, int, const void*, void**);
using PFN_NrEvaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using PFN_NrRelease = int(__cdecl*)(void*);
using PFN_NrPopulate = int(__cdecl*)(void*);
using PFN_NrRatioCallback = int(__cdecl*)(void*);

struct Snippet
{
    HMODULE module = nullptr;
    PFN_NrInitExt init = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    bool adapted = false;
    bool initialised = false;
    bool loadFailed = false;
};

Snippet g_snip;
std::mutex g_loadMutex;
const char* g_loadError = nullptr;

int g_lastInit = 0;
int g_lastCreate = 0;
int g_lastRatioResult = 0;
int g_lastRatioStage = 0;

// ---------------------------------------------------------------------------------------------------
// The caller-path adapter.
//
// The model asks GetModuleFileNameW/A about the module that owns its return address. Inside one of the
// calls below that is this module, and the answer it is given is the forwarder's own path -- this
// module's folder plus nvngx.dll_dlssnr.dll -- so the model sees exactly what it saw through the
// forwarder, directory included. Scoped per thread and per call: outside a call, or for any other
// module, the query goes to whatever the import slot held before (the system export, or another
// loader's or overlay's wrapper), which is what wilsjo2's v0.8.3 changed from demanding the pristine
// export and why it survives a runtime already wrapped by someone else.
// ---------------------------------------------------------------------------------------------------

thread_local HMODULE t_callerAlias = nullptr;

std::atomic<decltype(&GetModuleFileNameW)> g_previousW { nullptr };
std::atomic<decltype(&GetModuleFileNameA)> g_previousA { nullptr };

wchar_t g_aliasW[MAX_PATH] {};
char g_aliasA[MAX_PATH] {};

HMODULE ThisModule()
{
    static HMODULE module = []
    {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ThisModule), &h);
        return h;
    }();
    return module;
}

template <typename Char> DWORD CopyAlias(const Char* alias, Char* out, DWORD capacity)
{
    size_t length = 0;
    while (alias[length] != 0)
        ++length;

    if (capacity == 0)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    // GetModuleFileName's own contract: a buffer too small gets capacity-1 characters and a NUL, returns
    // capacity, and sets ERROR_INSUFFICIENT_BUFFER.
    const size_t copied = length < capacity ? length : capacity - 1;
    for (size_t i = 0; i < copied; ++i)
        out[i] = alias[i];
    out[copied] = 0;

    if (length >= capacity)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return capacity;
    }

    return static_cast<DWORD>(copied);
}

DWORD WINAPI CallerPathW(HMODULE queried, LPWSTR path, DWORD capacity)
{
    if (t_callerAlias != nullptr && queried == t_callerAlias)
        return CopyAlias(g_aliasW, path, capacity);

    const auto previous = g_previousW.load();
    return previous != nullptr ? previous(queried, path, capacity) : GetModuleFileNameW(queried, path, capacity);
}

DWORD WINAPI CallerPathA(HMODULE queried, LPSTR path, DWORD capacity)
{
    if (t_callerAlias != nullptr && queried == t_callerAlias)
        return CopyAlias(g_aliasA, path, capacity);

    const auto previous = g_previousA.load();
    return previous != nullptr ? previous(queried, path, capacity) : GetModuleFileNameA(queried, path, capacity);
}

bool ReplaceSlot(void** slot, void* expected, void* replacement)
{
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
    {
        LOG_ERROR("DLSS-NR direct: could not make a caller-path import writable at {} (error {})", (void*) slot,
                  GetLastError());
        return false;
    }

    const bool replaced = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), protection, &ignored);

    if (!replaced)
        LOG_ERROR("DLSS-NR direct: a caller-path import at {} changed while it was being adapted", (void*) slot);

    return replaced;
}

// Points every GetModuleFileNameW/A slot the model imports at the adapter. Idempotent: slots already
// adapted are left as they are. Refuses an import table that does not parse, and slots of one API that
// disagree about their target -- one adapter cannot preserve two different chains.
bool AdaptCallerPath(HMODULE model)
{
    const auto* base = reinterpret_cast<const uint8_t*>(model);
    const uint32_t size = DlssNr::PeScan::ImageSize(base, 4096);

    std::vector<DlssNr::PeScan::ImportSlot> slots;
    if (size == 0 || !DlssNr::PeScan::FindCallerPathImports(base, size, slots))
    {
        g_loadError = "the model's import table could not be read";
        return false;
    }

    if (slots.empty())
    {
        // Nothing to adapt means the check is not the one this was written against. Calling on would
        // fail it with PlatformError at best, so say so instead.
        g_loadError = "the model imports no GetModuleFileNameW/A to adapt";
        return false;
    }

    void* ourW = reinterpret_cast<void*>(&CallerPathW);
    void* ourA = reinterpret_cast<void*>(&CallerPathA);
    void* previousW = nullptr;
    void* previousA = nullptr;

    for (const auto& slot : slots)
    {
        void* current = *reinterpret_cast<void* const*>(base + slot.slotOffset);
        void* ours = slot.wide ? ourW : ourA;
        void*& previous = slot.wide ? previousW : previousA;

        if (current == ours)
            continue;

        if (current == nullptr || (previous != nullptr && previous != current))
        {
            g_loadError = "the model's caller-path imports point at different targets";
            return false;
        }

        previous = current;
    }

    // The slots are about to point into this module and nothing restores them -- the model is never
    // unloaded, as with the forwarder. wilsjo2's runtime restores them at teardown instead. Pinned, so
    // this module cannot be unloaded from under a model that still calls into it.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&CallerPathW), &pinned))
    {
        g_loadError = "OptiScaler could not pin itself for the model's imports";
        return false;
    }

    if (previousW != nullptr)
        g_previousW = reinterpret_cast<decltype(&GetModuleFileNameW)>(previousW);
    if (previousA != nullptr)
        g_previousA = reinterpret_cast<decltype(&GetModuleFileNameA)>(previousA);

    size_t adapted = 0;
    for (const auto& slot : slots)
    {
        void** address = reinterpret_cast<void**>(const_cast<uint8_t*>(base) + slot.slotOffset);
        void* ours = slot.wide ? ourW : ourA;
        void* previous = slot.wide ? previousW : previousA;

        if (*address == ours)
            continue;

        if (!ReplaceSlot(address, previous, ours))
        {
            g_loadError = "a caller-path import of the model could not be adapted";
            return false;
        }

        ++adapted;
    }

    LOG_INFO("DLSS-NR direct: {} caller-path import slot(s) of the model adapted ({} already), answering {} for "
             "OptiScaler's own module during its calls",
             adapted, slots.size() - adapted, wstring_to_string(g_aliasW));
    return true;
}

bool LoadSnippet(const wchar_t* path)
{
    std::lock_guard lock(g_loadMutex);

    if (g_snip.module != nullptr)
        return g_snip.create != nullptr && g_snip.adapted;

    if (g_snip.loadFailed || path == nullptr)
        return false;

    // The alias is the path the forwarder would have had beside OptiScaler.
    wchar_t folder[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(ThisModule(), folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        g_snip.loadFailed = true;
        g_loadError = "OptiScaler could not read its own path";
        return false;
    }

    std::wstring alias(folder, length);
    alias = alias.substr(0, alias.find_last_of(L"\\/") + 1) + L"nvngx.dll_dlssnr.dll";
    wcsncpy_s(g_aliasW, alias.c_str(), _TRUNCATE);
    WideCharToMultiByte(CP_ACP, 0, g_aliasW, -1, g_aliasA, MAX_PATH, nullptr, nullptr);

    g_snip.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (g_snip.module == nullptr)
    {
        g_snip.loadFailed = true;
        g_loadError = "nvngx_dlssnr.dll would not load";
        LOG_ERROR("DLSS-NR direct: {} would not load (error {})", wstring_to_string(path), GetLastError());
        return false;
    }

    g_snip.init = (PFN_NrInitExt) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_Init_Ext");
    g_snip.create = (PFN_NrCreate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_CreateFeature");
    g_snip.evaluate = (PFN_NrEvaluate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_EvaluateFeature");
    g_snip.release = (PFN_NrRelease) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_ReleaseFeature");

    if (g_snip.create == nullptr || g_snip.evaluate == nullptr)
    {
        g_snip.loadFailed = true;
        g_loadError = "nvngx_dlssnr.dll is missing its D3D12 entry points";
        LOG_ERROR("DLSS-NR direct: {}", g_loadError);
        return false;
    }

    g_snip.adapted = AdaptCallerPath(g_snip.module);

    if (!g_snip.adapted)
    {
        g_snip.loadFailed = true;
        LOG_ERROR("DLSS-NR direct: {}; the forwarder (ModelLoader=forwarder) does not need this", g_loadError);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------------------------------
// Fault containment, as the forwarder's: one fault is recorded, then nothing enters the model again.
// ---------------------------------------------------------------------------------------------------

constexpr int kFaulted = static_cast<int>(0xBAD0FA17u);

struct FaultState
{
    volatile long faulted = 0;
    unsigned long code = 0;
    void* address = nullptr;
};

FaultState g_fault;

int FaultFilter(unsigned long code, EXCEPTION_POINTERS* info)
{
    if ((code & 0xC0000000ul) != 0xC0000000ul && code != 0xE06D7363ul)
        return EXCEPTION_CONTINUE_SEARCH;

    if (InterlockedCompareExchange(&g_fault.faulted, 1, 0) == 0)
    {
        g_fault.code = code;
        g_fault.address =
            info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionAddress : nullptr;
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

// The call into the model sits inside __try in this module, so the return address the model resolves is
// this module's and the call is never a tail call. The caller alias is set for exactly its duration.
template <typename Fn, typename... Args> int Guarded(Fn fn, Args... args)
{
    if (g_fault.faulted != 0)
        return kFaulted;

    const HMODULE previousAlias = t_callerAlias;
    t_callerAlias = ThisModule();

    int result = kFaulted;

    __try
    {
        result = fn(args...);
    }
    __except (FaultFilter(GetExceptionCode(), GetExceptionInformation()))
    {
        result = kFaulted;
    }

    t_callerAlias = previousAlias;
    return result;
}

} // namespace

namespace DlssNr::DirectRuntime
{

void SetFloatSlot(int slot)
{
    if (slot >= 0 && slot < 8)
        g_floatSlot = slot;
}

void ProbeFloat(void* params, const char* name, float value, int slot)
{
    if (!params || slot < 0 || slot >= 8)
        return;
    void** vt = *reinterpret_cast<void***>(params);
    reinterpret_cast<PFN_SetFloat>(vt[slot])(params, name, value);
}

int* LastInit() { return &g_lastInit; }
int* LastCreate() { return &g_lastCreate; }
const int* LastRatioStage() { return &g_lastRatioStage; }
const char* LoadError() { return g_loadError; }

int FaultState(unsigned long* code, void** address)
{
    if (g_fault.faulted == 0)
        return 0;
    if (code != nullptr)
        *code = g_fault.code;
    if (address != nullptr)
        *address = g_fault.address;
    return 1;
}

int QueryScalingRatio(const wchar_t* snippetPath, void* capabilityParams, unsigned int perfQuality, float* outRatio)
{
    g_lastRatioStage = 0;

    if (!LoadSnippet(snippetPath) || !capabilityParams || !outRatio)
        return 0;

    g_lastRatioStage = 1;

    auto populate = (PFN_NrPopulate) GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_PopulateParameters_Impl");

    if (populate != nullptr)
    {
        volatile int populated = Guarded(populate, capabilityParams);
        (void) populated;
        g_lastRatioStage = 2;
    }

    void** vt = *reinterpret_cast<void***>(capabilityParams);
    unsigned long long raw = 0;

    if (reinterpret_cast<PFN_GetULL>(vt[VT_GET_ULL])(capabilityParams, "DLSSNRComputeScalingRatioCallback", &raw) !=
            1 ||
        raw == 0)
        return 0;

    g_lastRatioStage = 3;

    setUInt(capabilityParams, "PerfQualityValue", perfQuality);
    setFloat(capabilityParams, "DLSSNR.ScalingRatio", -1.0f);

    volatile int result = Guarded(reinterpret_cast<PFN_NrRatioCallback>((void*) raw), capabilityParams);
    g_lastRatioResult = (int) result;

    if (result != 1)
        return -1;

    g_lastRatioStage = 4;

    float ratio = -1.0f;

    if (reinterpret_cast<PFN_GetFloat>(vt[g_floatSlot + 8])(capabilityParams, "DLSSNR.ScalingRatio", &ratio) != 1)
        return -1;

    g_lastRatioStage = 5;
    *outRatio = ratio;
    return 1;
}

void* Create(const wchar_t* snippetPath, const wchar_t* dataPath, ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
             void* capabilityParams, unsigned int width, unsigned int height, int preset, float intensity, int style,
             float localStructure, float localTone, float skinStructure, int useAutoMask, int uiCorrection)
{
    if (!LoadSnippet(snippetPath) || !capabilityParams)
        return nullptr;

    if (!g_snip.initialised && g_snip.init)
    {
        // The forwarder's application id, so the driver sees the same caller whichever loader runs.
        g_lastInit = Guarded(g_snip.init, 0x24480451ull, dataPath, device, 0x0000015, capabilityParams);
        g_snip.initialised = (g_lastInit == 1);
        if (!g_snip.initialised)
            return nullptr;
    }

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "CreationNodeMask", 1);
    setUInt(capabilityParams, "VisibilityNodeMask", 1);
    setUInt(capabilityParams, "DLSSNR.Hint.Render.Preset", (unsigned int) preset);
    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);
    setUInt(capabilityParams, "DLSSNR.UICorrection", (unsigned int) uiCorrection);

    void* handle = nullptr;
    g_lastCreate = Guarded(g_snip.create, cmd, 18, capabilityParams, &handle);
    return g_lastCreate == kFaulted ? nullptr : handle;
}

int Evaluate(ID3D12GraphicsCommandList* cmd, void* feature, void* capabilityParams, ID3D12Resource* color,
             ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, unsigned int width,
             unsigned int height, unsigned int guideWidth, unsigned int guideHeight, unsigned int motionWidth,
             unsigned int motionHeight, unsigned int depthBaseX, unsigned int depthBaseY, unsigned int motionBaseX,
             unsigned int motionBaseY, int depthInverted, int reset, float intensity, int style, float localStructure,
             float localTone, float skinStructure, int useAutoMask, float mvScaleX, float mvScaleY)
{
    // No ABI handshake: this and the host are one binary, so their argument lists cannot drift apart.
    if (!feature || !capabilityParams || !g_snip.evaluate)
        return 0;

    setResource(capabilityParams, "DLSSNR.Color", color);
    setResource(capabilityParams, "DLSSNR.Depth", depth);
    setResource(capabilityParams, "DLSSNR.MVec", motion);
    setResource(capabilityParams, "DLSSNR.Output", output);

    setUInt(capabilityParams, "DLSSNR.Enabled", 1);
    setUInt(capabilityParams, "DLSSNR.Width", width);
    setUInt(capabilityParams, "DLSSNR.Height", height);
    setUInt(capabilityParams, "DLSSNR.DepthInverted", (unsigned int) depthInverted);
    setUInt(capabilityParams, "DLSSNR.Reset", (unsigned int) reset);

    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.ColorSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectWidth", width);
    setUInt(capabilityParams, "DLSSNR.OutputSubrectHeight", height);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseX", depthBaseX);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectBaseY", depthBaseY);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectWidth", guideWidth);
    setUInt(capabilityParams, "DLSSNR.DepthSubrectHeight", guideHeight);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseX", motionBaseX);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectBaseY", motionBaseY);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectWidth", motionWidth);
    setUInt(capabilityParams, "DLSSNR.MVecSubrectHeight", motionHeight);

    setFloat(capabilityParams, "DLSSNR.MVecScaleX", mvScaleX);
    setFloat(capabilityParams, "DLSSNR.MVecScaleY", mvScaleY);

    setFloat(capabilityParams, "DLSSNR.Intensity", intensity);
    setUInt(capabilityParams, "DLSSNR.Style", (unsigned int) style);
    setFloat(capabilityParams, "DLSSNR.LocalStructureStrength", localStructure);
    setFloat(capabilityParams, "DLSSNR.LocalToneStrength", localTone);
    setFloat(capabilityParams, "DLSSNR.SkinStructureStrength", skinStructure);
    setUInt(capabilityParams, "DLSSNR.UseAutoMask", (unsigned int) useAutoMask);

    // Assigned, not returned: a tail call would leave this module's frame and the alias behind.
    volatile int result = Guarded(g_snip.evaluate, cmd, feature, capabilityParams, nullptr);
    return result;
}

void SetExtras(void* capabilityParams, float globalTone, ID3D12Resource* ui, ID3D12Resource* uiAlpha,
               ID3D12Resource* backbuffer, unsigned int uiWidth, unsigned int uiHeight, unsigned int bbWidth,
               unsigned int bbHeight)
{
    if (!capabilityParams)
        return;

    // Not written, as in the forwarder: the model has no such parameter.
    (void) globalTone;

    setResource(capabilityParams, "DLSSNR.UI", ui);
    setResource(capabilityParams, "DLSSNR.UIAlpha", uiAlpha);
    setResource(capabilityParams, "DLSSNR.Backbuffer", backbuffer);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UISubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UISubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectWidth", uiWidth);
    setUInt(capabilityParams, "DLSSNR.UIAlphaSubrectHeight", uiHeight);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseX", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectBaseY", 0);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectWidth", bbWidth);
    setUInt(capabilityParams, "DLSSNR.BackbufferSubrectHeight", bbHeight);
}

void Release(void* feature)
{
    if (feature && g_snip.release)
    {
        volatile int result = Guarded(g_snip.release, feature);
        (void) result;
    }
}

} // namespace DlssNr::DirectRuntime
