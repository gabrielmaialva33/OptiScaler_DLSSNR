#pragma once

#include <sl_consts.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>

// Own only camera values. No chained SDK pointers or game-owned memory crosses the input/present threads.
template <size_t Count> class DlssgGameCamera
{
    struct Entry
    {
        std::optional<uint64_t> frame;
        std::optional<sl::Constants> camera;
    };
    std::array<Entry, Count> _entries {};
    std::mutex _mutex;

    static bool Finite(float value) { return std::isfinite(value) && value != sl::INVALID_FLOAT; }

    static bool Finite(const sl::float3& value) { return Finite(value.x) && Finite(value.y) && Finite(value.z); }

    static std::array<float, 16> Scalars(const sl::float4x4& matrix)
    {
        std::array<float, 16> result;
        static_assert(sizeof(result) == sizeof(matrix));
        std::memcpy(result.data(), &matrix, sizeof(matrix));
        return result;
    }

    static bool Matrix(const sl::float4x4& matrix, bool rejectIdentity = false)
    {
        const auto values = Scalars(matrix);
        bool nonzero = false, identity = true;
        for (size_t i = 0; i < values.size(); i++)
        {
            if (!Finite(values[i]))
                return false;
            nonzero |= values[i] != 0.0f;
            identity &= values[i] == (i % 5 == 0 ? 1.0f : 0.0f);
        }
        return nonzero && !(rejectIdentity && identity);
    }

    static bool InversePair(const sl::float4x4& matrix, const sl::float4x4& inverse)
    {
        const auto a = Scalars(matrix), b = Scalars(inverse);
        for (size_t row = 0; row < 4; row++)
        {
            for (size_t col = 0; col < 4; col++)
            {
                double value = 0;
                for (size_t k = 0; k < 4; k++)
                    value += static_cast<double>(a[row * 4 + k]) * b[k * 4 + col];
                if (!std::isfinite(value) || std::abs(value - (row == col ? 1.0 : 0.0)) > 0.01)
                    return false;
            }
        }
        return true;
    }

    static bool Camera(const sl::Constants& data)
    {
        return (data.orthographicProjection == sl::Boolean::eFalse ||
                data.orthographicProjection == sl::Boolean::eTrue) &&
               Matrix(data.cameraViewToClip, true) && Matrix(data.clipToCameraView) && Matrix(data.clipToPrevClip) &&
               Matrix(data.prevClipToClip) && InversePair(data.cameraViewToClip, data.clipToCameraView) &&
               InversePair(data.clipToPrevClip, data.prevClipToClip);
    }

    static void CopyCamera(sl::Constants& output, const sl::Constants& data)
    {
        output.cameraViewToClip = data.cameraViewToClip;
        output.clipToCameraView = data.clipToCameraView;
        output.clipToPrevClip = data.clipToPrevClip;
        output.prevClipToClip = data.prevClipToClip;
        output.orthographicProjection = data.orthographicProjection;

        if (Matrix(data.clipToLensClip))
            output.clipToLensClip = data.clipToLensClip;
        if (Finite(data.cameraPinholeOffset.x) && Finite(data.cameraPinholeOffset.y))
            output.cameraPinholeOffset = data.cameraPinholeOffset;
        if (Finite(data.cameraPos) && Finite(data.cameraUp) && Finite(data.cameraRight) && Finite(data.cameraFwd))
        {
            output.cameraPos = data.cameraPos;
            output.cameraUp = data.cameraUp;
            output.cameraRight = data.cameraRight;
            output.cameraFwd = data.cameraFwd;
        }
        if (Finite(data.cameraNear) && data.cameraNear > 0)
            output.cameraNear = data.cameraNear;
        if (Finite(data.cameraFar) && data.cameraFar > 0)
            output.cameraFar = data.cameraFar;
        if (Finite(data.cameraFOV) && data.cameraFOV > 0 && data.cameraFOV < 3.141593f)
            output.cameraFOV = data.cameraFOV;
        if (Finite(data.cameraAspectRatio) && data.cameraAspectRatio > 0)
            output.cameraAspectRatio = data.cameraAspectRatio;
    }

  public:
    void BeginFrame(uint64_t frame)
    {
        std::scoped_lock lock(_mutex);
        _entries[frame % Count] = { frame, std::nullopt };
    }

    void Store(uint64_t frame, const sl::Constants& data, bool allow)
    {
        std::scoped_lock lock(_mutex);
        auto& entry = _entries[frame % Count];
        if (entry.frame != frame)
            return;

        entry.camera.reset();
        if (allow && Camera(data))
        {
            entry.camera.emplace();
            CopyCamera(*entry.camera, data);
        }
    }

    bool Apply(uint64_t frame, sl::Constants& output)
    {
        std::scoped_lock lock(_mutex);
        const auto& entry = _entries[frame % Count];
        if (entry.frame != frame || !entry.camera)
            return false;
        CopyCamera(output, *entry.camera);
        return true;
    }

    void Clear()
    {
        std::scoped_lock lock(_mutex);
        _entries = {};
    }
};
