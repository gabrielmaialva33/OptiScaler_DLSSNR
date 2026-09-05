#pragma once
#include <cstdio>
#include <format>
#include <string_view>
template <class... Args> void TimingTestLog(std::string_view format, Args&&... args)
{
    const auto line = std::vformat(format, std::make_format_args(args...));
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}
#define LOG_INFO(...) TimingTestLog(__VA_ARGS__)
#define LOG_WARN(...) TimingTestLog(__VA_ARGS__)
#define LOG_ERROR(...) TimingTestLog(__VA_ARGS__)
#define LOG_DEBUG(...) TimingTestLog(__VA_ARGS__)
