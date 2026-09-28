#pragma once
// Test-only stand-in for OptiScaler/Logger.h. Production uses spdlog's "{}" formatting; std::format
// accepts the same strings for the argument kinds the estimator logs.
#include <cstdio>
#include <format>
#include <string_view>

template <class... Args> void SynthMotionTestLog(const char* level, std::string_view format, Args&&... args)
{
    const auto line = std::vformat(format, std::make_format_args(args...));
    std::printf("[%s] %s\n", level, line.c_str());
    std::fflush(stdout);
}

#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) SynthMotionTestLog("D", __VA_ARGS__)
#define LOG_INFO(...) SynthMotionTestLog("I", __VA_ARGS__)
#define LOG_WARN(...) SynthMotionTestLog("W", __VA_ARGS__)
#define LOG_ERROR(...) SynthMotionTestLog("E", __VA_ARGS__)
#define LOG_FUNC() ((void) 0)
#define LOG_FUNC_RESULT(...) ((void) 0)
