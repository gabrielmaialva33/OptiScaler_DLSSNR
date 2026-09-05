#pragma once
#include <d3d12.h>
namespace ResTrack_Dx12
{
IUnknown* NrRealObject(IUnknown* object);
bool EnableNrSubmissionTracking(ID3D12Device* device);
void* NrQueueImplementation();
} // namespace ResTrack_Dx12
