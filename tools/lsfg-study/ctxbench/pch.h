#pragma once

// Stand-in for OptiScaler's pch.h: just what retoque/lsfg/LsfgFrameGen_Dx12.cpp and LsfgShaderSource.cpp use.

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstring>
#include <format>
#include <string>

inline std::string wstring_to_string(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
