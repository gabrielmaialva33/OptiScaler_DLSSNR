#pragma once
#include <d3dcompiler.h>
#define SAFE_RELEASE(p)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((p) != nullptr)                                                                                            \
        {                                                                                                              \
            (p)->Release();                                                                                            \
            (p) = nullptr;                                                                                             \
        }                                                                                                              \
    } while (false)

namespace Util
{
inline HRESULT GetDeviceRemovedReason(ID3D12Device* device) { return device->GetDeviceRemovedReason(); }
} // namespace Util
