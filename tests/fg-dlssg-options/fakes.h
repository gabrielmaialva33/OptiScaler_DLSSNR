#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#define LOG_WARN(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)

template <typename T> struct Setting
{
    T value {};
    T value_or_default() const { return value; }
    Setting& operator=(T next)
    {
        value = next;
        return *this;
    }
};

struct Config
{
    Setting<int> FGDLSSGInterpolationCount { 1 };
    Setting<bool> FGDLSSGForceDMFG { false };
    Setting<float> FGDLSSGFramerateTargetDMFG { 120.0f };
    Setting<bool> FGDLSSGReflexBoost { false };
    int FGInterpolationCount(int count) { return count; }
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};

struct MenuOverlayBase
{
    static inline bool visible = false;
    static bool IsVisible() { return visible; }
};

struct ReflexHooks
{
    static inline bool markers = false;
    static inline int frameCount = -1;
    static bool gameIsSendingMarkers() { return markers; }
    static void setDlssgFrameCount(int count) { frameCount = count; }
};

struct ScopedOptiScalerReflex
{
};

struct StreamlineHooks
{
    static inline unsigned int dlssgGeneration = 0;
    static inline unsigned int reflexGeneration = 0;
    static unsigned int dlssgOptionsGeneration() { return dlssgGeneration; }
    static unsigned int reflexOptionsGeneration() { return reflexGeneration; }
    static void applyMenuDlssgInterlock(sl::DLSSGOptions& options, bool dlssgPotentiallyActive);
};

// Only the SDK boundary is fake: retain arguments and return a controllable driver result.
struct StreamlineProxy
{
    static inline std::vector<sl::DLSSGOptions> dlssgCalls;
    static inline std::vector<sl::ReflexOptions> reflexCalls;
    static inline sl::Result dlssgResult = sl::Result::eOk;
    static inline sl::Result reflexResult = sl::Result::eOk;
    static sl::Result setDlssg(const sl::ViewportHandle&, const sl::DLSSGOptions& options)
    {
        dlssgCalls.push_back(options);
        return dlssgResult;
    }
    static sl::Result setReflex(const sl::ReflexOptions& options)
    {
        reflexCalls.push_back(options);
        return reflexResult;
    }
    static PFun_slDLSSGSetOptions* DLSSGSetOptions() { return setDlssg; }
    static PFun_slReflexSetOptions* ReflexSetOptions() { return setReflex; }
};

class DLSSG_Common
{
    // @cache-members@
    sl::ViewportHandle _viewport { 0 };
    int _maxInterpolationCount = 3;
    int _framesToInterpolate = 1;

  public:
    bool SetOptions(bool gameMarkersReachStreamline);
};
