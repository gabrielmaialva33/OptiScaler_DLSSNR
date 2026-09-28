#pragma once

#include <d3d12.h>

// The model reached from inside OptiScaler.dll, without nvngx.dll_dlssnr.dll. [DlssNr] ModelLoader=direct.
//
// Why the forwarder exists at all: the model turns its caller's return address into a module
// (RtlPcToFileHeader), that module into a path (GetModuleFileNameW/A), and refuses a path without
// "nvngx.dll" in it. The forwarder passes by being a DLL named nvngx.dll_dlssnr.dll that makes the
// calls. This passes the same check from here: the model's own GetModuleFileNameW/A import slots are
// pointed at an adapter that, only on a thread inside one of the calls below and only when asked about
// OptiScaler's own module, answers with the path the forwarder would have had -- OptiScaler's folder +
// nvngx.dll_dlssnr.dll. Every other query goes to whatever the slot held before, including another
// loader's or overlay's wrapper. Nothing on disk changes and no process-wide API is hooked.
//
// The technique -- name-resolved caller-path imports, chaining the existing target instead of demanding
// the pristine export -- is wilsjo2's (OptiScaler-DLSSNR-PreSR-Multipass, releases v0.8.1-nr-direct-runtime
// and v0.8.3, GPL-3). The calls themselves mirror dlssnr_forwarder.cpp exactly: same application id,
// same capability block, same parameter writes in the same order (tests/nr-model-loader holds the two
// to that), same fault containment.
//
// D3D12 only, which covers the after-upscale pass, the D3D12 present pass and the D3D11 bridge host.
// Native Vulkan and the native D3D11 probe keep the forwarder whatever ModelLoader says.
//
// Each function has the signature of the forwarder export it replaces, so the host fills the same
// function pointers from here instead of from GetProcAddress.
namespace DlssNr::DirectRuntime
{
int QueryScalingRatio(const wchar_t* snippetPath, void* capabilityParams, unsigned int perfQuality, float* outRatio);
const int* LastRatioStage();

void* Create(const wchar_t* snippetPath, const wchar_t* dataPath, ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
             void* capabilityParams, unsigned int width, unsigned int height, int preset, float intensity, int style,
             float localStructure, float localTone, float skinStructure, int useAutoMask, int uiCorrection);

int Evaluate(ID3D12GraphicsCommandList* cmd, void* feature, void* capabilityParams, ID3D12Resource* color,
             ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, unsigned int width,
             unsigned int height, unsigned int guideWidth, unsigned int guideHeight, unsigned int motionWidth,
             unsigned int motionHeight, unsigned int depthBaseX, unsigned int depthBaseY, unsigned int motionBaseX,
             unsigned int motionBaseY, int depthInverted, int reset, float intensity, int style, float localStructure,
             float localTone, float skinStructure, int useAutoMask, float mvScaleX, float mvScaleY);

void SetExtras(void* capabilityParams, float globalTone, ID3D12Resource* ui, ID3D12Resource* uiAlpha,
               ID3D12Resource* backbuffer, unsigned int uiWidth, unsigned int uiHeight, unsigned int bbWidth,
               unsigned int bbHeight);

void Release(void* feature);

void SetFloatSlot(int slot);
void ProbeFloat(void* params, const char* name, float value, int slot);

int* LastInit();
int* LastCreate();

// Sticky for the process, like dlssnr_fault_state: once a call has faulted, nothing enters the model again.
int FaultState(unsigned long* code, void** address);

// Why the last load failed, for the host's reason string; nullptr when it did not.
const char* LoadError();
} // namespace DlssNr::DirectRuntime
