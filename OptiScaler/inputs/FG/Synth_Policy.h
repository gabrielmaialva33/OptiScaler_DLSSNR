#pragma once

// What synthesized frame generation does with a base frame, decided from what the motion field and
// the frame clock say. Pure logic with no D3D in it, so the host tests drive it directly
// (tests/fg-synth-policy). Design: dlssnr/design/synthesized-frame-generation.md, "Motion into FG, and
// emulator behaviour".
//
// - Fast-motion response: FG is fed with Reset, which makes FSR's interpolation copy the frame instead
//   of interpolating (AMD AFMF's Fast Motion Response), when
//   - the motion is incoherent: more than a set share of neighbouring samples disagree while the median
//     exceeds kLargeMotionOfWidth of the image (the rule; "Coherence, not speed" in the design note), or
//   - the median exceeds a hard cap in pixels, however coherent.
//   Each Reset also restarts FSR's own optical flow and its game-vector arbitration for about ten
//   frames, so a uniform pan, which interpolates correctly at any speed, is left alone.
// - Low-fps floor: below a base rate, FG is not fed at all.
// - Duplicate presents: detected from all-zero motion samples interleaved with moving ones, reported
//   once, never acted on (collapsing them would need a same-frame CPU wait on the GPU).
//
// Motion samples arrive a few frames late (read back from the GPU); the frame clock does not.

#include "Synth_MotionStats.h"

#include <cstdint>

class SynthFgPolicy
{
  public:
    enum class Action : uint8_t
    {
        Generate, // feed FG normally
        Reset,    // feed FG with Reset: the frame is repeated, not interpolated
        Skip,     // do not feed FG this frame
    };

    // 0 turns each off.
    // - capPx: median displacement, in display pixels per base frame, above which frames are repeated
    //   whatever the coherence ([FrameGen] SynthesizedFastMotion).
    // - minFps: base rate below which FG is not fed ([FrameGen] SynthesizedMinFps).
    // - incoherence: share of neighbouring samples that disagree, 0..1, above which frames are repeated
    //   while the median exceeds kLargeMotionOfWidth ([FrameGen] SynthesizedIncoherence).
    void Configure(float capPx, float minFps, float incoherence)
    {
        // A response held under other thresholds is decided afresh by the next sample; one switched off
        // stops at once.
        if (capPx != _capPx || incoherence != _incoherence)
        {
            _fast = false;
            _calmSamples = 0;
        }

        if (minFps <= 0.0f)
            _belowFloor = false;

        _capPx = capPx;
        _minFps = minFps;
        _incoherence = incoherence;
    }

    // One base frame presented, at nowMs on a monotonic clock.
    void ObserveBaseFrame(double nowMs)
    {
        if (_lastFrameMs > 0.0)
        {
            const double interval = nowMs - _lastFrameMs;

            // A gap this long is a pause (a menu, a load), not a frame rate; it restarts the average.
            if (interval > 0.0 && interval < kPauseMs)
                _intervalMs = _intervalMs > 0.0 ? _intervalMs + (interval - _intervalMs) * kIntervalAlpha : interval;
            else
                _intervalMs = 0.0;
        }

        _lastFrameMs = nowMs;

        if (_minFps <= 0.0f || _intervalMs <= 0.0)
        {
            _belowFloor = false;
            return;
        }

        const double fps = 1000.0 / _intervalMs;

        if (_belowFloor)
            _belowFloor = fps < _minFps * kFloorResume;
        else
            _belowFloor = fps < _minFps;
    }

    // One motion sample: the statistics of the sampled rows (SynthMotionStats::Measure), and whether the
    // estimator reported a scene cut.
    void ObserveMotion(const SynthMotionStats::Stats& stats, bool sceneCut)
    {
        if (sceneCut)
            _cutPending = true;

        const bool incoherenceOn = _incoherence > 0.0f;
        const bool capOn = _capPx > 0.0f;

        if (incoherenceOn || capOn)
        {
            // Each trigger holds until its levels fall below kFastRelease of the thresholds; two calm
            // samples in a row, below every level held, end the response.
            const bool incoherent =
                incoherenceOn && stats.incoherent > _incoherence && stats.medianOfWidth > kLargeMotionOfWidth;
            const bool overCap = capOn && stats.medianPx > _capPx;
            const bool incoherenceCalm = !incoherenceOn || stats.incoherent < _incoherence * kFastRelease ||
                                         stats.medianOfWidth < kLargeMotionOfWidth * kFastRelease;
            const bool capCalm = !capOn || stats.medianPx < _capPx * kFastRelease;

            if (incoherent || overCap)
            {
                _fast = true;
                _calmSamples = 0;
            }
            else if (_fast && incoherenceCalm && capCalm)
            {
                if (++_calmSamples >= kCalmSamplesToRelease)
                    _fast = false;
            }
            else
            {
                _calmSamples = 0;
            }
        }

        _ObserveCadence(stats.allZero);
    }

    // The decision for the frame about to be fed. A pending scene cut is spent here.
    Action Decide()
    {
        if (_belowFloor)
            return Action::Skip;

        if (_cutPending)
        {
            _cutPending = false;
            return Action::Reset;
        }

        return _fast ? Action::Reset : Action::Generate;
    }

    // True exactly once, when an interleaved pattern of identical and moving frames is established.
    bool TakeDuplicateAdvice()
    {
        if (!_duplicatePattern || _duplicateAdvised)
            return false;

        _duplicateAdvised = true;
        return true;
    }

    bool Fast() const { return _fast; }
    bool FastMotionConfigured() const { return _capPx > 0.0f || _incoherence > 0.0f; }
    bool BelowFloor() const { return _belowFloor; }
    double BaseFps() const { return _intervalMs > 0.0 ? 1000.0 / _intervalMs : 0.0; }
    uint32_t IdenticalInWindow() const { return _identical; }
    uint32_t AlternationsInWindow() const { return _alternations; }

    static constexpr double kPauseMs = 500.0;
    static constexpr double kIntervalAlpha = 0.1;
    static constexpr float kFloorResume = 1.15f;
    static constexpr float kFastRelease = 0.75f;
    // The incoherence rule's size gate: the median above 1% of the image width per base frame (19 px at
    // 1920, 34 px at 3440). At the midpoint frame a wrong vector misplaces content by at most half the
    // local disagreement, which the motion bounds; below this that halo costs less than the ten degraded
    // frames a Reset starts in FSR. To be revisited from the logged p50.
    static constexpr float kLargeMotionOfWidth = 0.01f;
    static constexpr uint32_t kCalmSamplesToRelease = 2;
    static constexpr uint32_t kWindow = 60;
    static constexpr uint32_t kMinIdentical = kWindow / 4;
    static constexpr uint32_t kMinAlternations = 20;

  private:
    void _ObserveCadence(bool allZero)
    {
        // A ring of the last kWindow samples; identical counts and alternations are kept incrementally.
        if (_filled == kWindow)
        {
            const bool oldest = _ring[_head];
            const bool second = _ring[(_head + 1) % kWindow];
            _identical -= oldest ? 1 : 0;
            _alternations -= oldest != second ? 1 : 0;
        }
        else
        {
            ++_filled;
        }

        if (_filled > 1)
        {
            const bool previous = _ring[(_head + kWindow - 1) % kWindow];
            _alternations += previous != allZero ? 1 : 0;
        }

        _ring[_head] = allZero;
        _identical += allZero ? 1 : 0;
        _head = (_head + 1) % kWindow;

        // Every sample identical is a paused or static game, not a cadence: it needs moving frames too.
        _duplicatePattern = _filled == kWindow && _identical >= kMinIdentical && _identical < kWindow &&
                            _alternations >= kMinAlternations;
    }

    float _capPx = 0.0f;
    float _minFps = 0.0f;
    float _incoherence = 0.0f;

    double _lastFrameMs = 0.0;
    double _intervalMs = 0.0;
    bool _belowFloor = false;

    bool _fast = false;
    uint32_t _calmSamples = 0;
    bool _cutPending = false;

    bool _ring[kWindow] {};
    uint32_t _head = 0;
    uint32_t _filled = 0;
    uint32_t _identical = 0;
    uint32_t _alternations = 0;
    bool _duplicatePattern = false;
    bool _duplicateAdvised = false;
};
