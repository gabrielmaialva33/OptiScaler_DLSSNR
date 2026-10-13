#pragma once

#include <chrono>
#include <cstdint>

namespace DlssNr::Detail
{
// Dispatch calls are not frames: a title may evaluate multiple times on one open command list.
// A pre-upscale feature created this frame must wait for an observed present before evaluation.
class CreationFrameGate
{
    uint64_t _createdAt = 0;
    bool _pending = false;

  public:
    void Created(uint64_t frame)
    {
        _createdAt = frame;
        _pending = true;
    }
    bool Ready(uint64_t frame)
    {
        if (!_pending)
            return true;
        if (frame == 0 || frame <= _createdAt)
            return false;
        _pending = false;
        return true;
    }
};

// Require a stable contract for half a second before allocating or rebuilding. A continuously
// changing DRS subrect leaves the game's original color selected instead of rebuilding every frame.
class StableExtent
{
    uint32_t _width = 0, _height = 0, _format = 0;
    std::chrono::steady_clock::time_point _since {};

  public:
    bool Observe(uint32_t width, uint32_t height, std::chrono::steady_clock::time_point now, uint32_t format = 0)
    {
        if (width != _width || height != _height || format != _format)
        {
            _width = width;
            _height = height;
            _format = format;
            _since = now;
        }
        return width != 0 && height != 0 && now - _since >= std::chrono::milliseconds(500);
    }
    void Reset() { _width = _height = 0; }
};

// NGX implementations may keep typed and void* entries separately, or alias them. Preserve both
// readable slots. Conflicting originals are ambiguous and must not be replaced by a guessed slot.
template <typename Parameters, typename Resource> class ColorBinding
{
    Parameters* _params = nullptr;
    const char* _key = nullptr;
    Resource* _typed = nullptr;
    void* _untyped = nullptr;
    bool _haveTyped = false, _haveUntyped = false, _swapped = false;

  public:
    ColorBinding() = default;
    ColorBinding(const ColorBinding&) = delete;
    ColorBinding& operator=(const ColorBinding&) = delete;
    ~ColorBinding() { Restore(); }

    template <typename Result> Resource* Read(Parameters* params, const char* key, Result success)
    {
        _params = params;
        _key = key;
        _haveTyped = params->Get(key, &_typed) == success;
        _haveUntyped = params->Get(key, &_untyped) == success;
        if (!_haveTyped)
            _typed = nullptr;
        if (!_haveUntyped)
            _untyped = nullptr;
        if (_typed && _untyped && _typed != _untyped)
            return nullptr;
        return _typed ? _typed : static_cast<Resource*>(_untyped);
    }

    void Swap(Resource* replacement)
    {
        if (_haveTyped)
            _params->Set(_key, replacement);
        if (_haveUntyped)
            _params->Set(_key, static_cast<void*>(replacement));
        _swapped = true;
    }

    void Restore()
    {
        if (!_swapped)
            return;
        if (_haveTyped)
            _params->Set(_key, _typed);
        if (_haveUntyped)
            _params->Set(_key, _untyped);
        _swapped = false;
    }
};
} // namespace DlssNr::Detail
