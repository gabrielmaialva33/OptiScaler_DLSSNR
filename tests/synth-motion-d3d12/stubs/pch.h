#pragma once
// Test-only stand-in for OptiScaler/pch.h: the Windows and D3D12 headers the estimator needs, and
// nothing that drags in the rest of the DLL (spdlog, Config, State, hooks).
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Logger.h"
