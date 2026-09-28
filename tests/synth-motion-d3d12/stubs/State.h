#pragma once
// Test-only stand-in for OptiScaler/State.h: the two scoped markers the estimator sets around its own
// resource creation. In production they keep the DLSS-NR exposure scan and the heap tracker away
// from OptiScaler's internal resources; this harness has neither, so they only count.
inline int g_internalResourceScopes = 0;
inline int g_skipHeapCaptureScopes = 0;

class ScopedInternalResourceCreation
{
  public:
    ScopedInternalResourceCreation() { ++g_internalResourceScopes; }
    ~ScopedInternalResourceCreation() { --g_internalResourceScopes; }
    static bool Active() { return g_internalResourceScopes > 0; }
};

class ScopedSkipHeapCapture
{
  public:
    ScopedSkipHeapCapture() { ++g_skipHeapCaptureScopes; }
    ~ScopedSkipHeapCapture() { --g_skipHeapCaptureScopes; }
};
