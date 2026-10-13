#pragma once

// The least of OptiScaler's Config.h, SysUtils.h and OptiTypes.h that retoque/dlssnr/DlssNr_Dx11Upscaler.h reads:
// the three keys it consults, the Upscaler enum, and the two log macros, which record instead of printing. run.py
// holds the enum and the keys' declared defaults to the real ones.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class Upscaler
{
    XeSS,
    XeSS_on12,
    FSR21,
    FSR21_on12,
    FSR22,
    FSR22_on12,
    FSR31,
    FFX,
    FFX_on12,
    DLSS,
    DLSS_on12,
    DLSSD,
    Reset
};

// CustomOptional as far as the header uses it: has_value() says whether the key is set, value_or_default() what it
// means. Unset is "auto" in the ini.
template <class T> class FakeOptional : public std::optional<T>
{
  public:
    explicit FakeOptional(T defaultValue) : _default(defaultValue) {}
    using std::optional<T>::operator=;
    T value_or_default() const { return this->has_value() ? this->value() : _default; }

  private:
    T _default;
};

class Config
{
  public:
    FakeOptional<bool> DlssNrEnabled { false };
    FakeOptional<uint32_t> DlssNrHookMethod { 1 };
    FakeOptional<Upscaler> Dx11Upscaler { Upscaler::FSR22 };

    static Config* Instance()
    {
        static Config instance;
        return &instance;
    }
};

// Every line the header logs, as "LEVEL text" with the one {} filled in.
inline std::vector<std::string> g_fakeLog;

inline void FakeLog(const char* level, const char* format, const char* argument)
{
    std::string line = format;
    const auto at = line.find("{}");
    if (at != std::string::npos)
        line.replace(at, 2, argument);
    g_fakeLog.push_back(std::string(level) + " " + line);
}

#define LOG_INFO(msg, arg) FakeLog("INFO", msg, arg)
#define LOG_WARN(msg, arg) FakeLog("WARN", msg, arg)
