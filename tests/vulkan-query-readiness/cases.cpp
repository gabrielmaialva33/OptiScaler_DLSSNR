static void Record()
{
    UpscalerTimeVk::UpscaleStart(commandBuffer);
    UpscalerTimeVk::UpscaleEnd(commandBuffer);
}

static void Unchanged()
{
    Check(State::Instance().upscaleTimes == std::deque<double> { 10.0, 20.0, 30.0 },
          "unavailable/error/invalid sample must not modify history");
}

static void Published(double expected)
{
    const auto& times = State::Instance().upscaleTimes;
    Check(times.size() == 3 && times[0] == 20.0 && times[1] == 30.0 && std::abs(times[2] - expected) < 1e-9,
          "publish exactly one valid sample, preserving rolling history");
}

int main(int argc, char** argv)
{
    Check(argc == 2, "one case per process (fresh production static state)");
    const std::string_view name = argv[1];
    if (name == "inactive")
    {
        Record(); // No pool: neither recording nor polling is allowed.
        UpscalerTimeVk::ReadUpscalingTime(device);
        Check(recorded.empty() && polls == 0, "null pool is inactive");
        UpscalerTimeVk::Init(device, physicalDevice);
        UpscalerTimeVk::ReadUpscalingTime(device);
        UpscalerTimeVk::UpscaleStart(commandBuffer);
        UpscalerTimeVk::ReadUpscalingTime(device);
        Check(recorded == std::vector<unsigned> { 0, 1 } && polls == 0, "start alone does not arm a sample");
        Unchanged();
        return 0;
    }

    period = 2.0f;
    UpscalerTimeVk::Init(device, physicalDevice);
    Record();
    Check(recorded == std::vector<unsigned> { 0, 1, 2 }, "reset/start/end recording order");
    if (name == "not-ready-retry" || name == "not-ready-pending" || name == "partial-not-ready")
    {
        // Deliberately plausible garbage on NOT_READY: deterministic even on the broken code.
        // Zeroes isolate premature consumption from the bogus-publication failure.
        replies.push_back({ VK_NOT_READY, name == "not-ready-pending" ? std::array<uint64_t, 2> { 0, 0 }
                                                                      : std::array<uint64_t, 2> { 100, 1000100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        Unchanged();
        if (name == "partial-not-ready")
        {
            // Unavailable queries may leave some or all destination words untouched.
            replies.push_back({ VK_NOT_READY, { 200, 0 }, 1 });
            replies.push_back({ VK_NOT_READY, { 0, 0 }, 0 });
            UpscalerTimeVk::ReadUpscalingTime(device);
            UpscalerTimeVk::ReadUpscalingTime(device);
            Unchanged();
        }
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        Check(polls == (name == "partial-not-ready" ? 4u : 2u), "retry the same pending read until ready");
        Published(3.0);
        UpscalerTimeVk::ReadUpscalingTime(device);
        Published(3.0);
    }
    else if (name == "errors")
    {
        for (const auto error :
             { VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN })
        {
            Record();
            replies.push_back({ error, { 100, 1000100 } });
            UpscalerTimeVk::ReadUpscalingTime(device);
            Unchanged();
            UpscalerTimeVk::ReadUpscalingTime(device); // Error consumes; no endless retry.
        }
        Check(polls == 4, "each failed measurement is consumed once");
        Record();
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        Published(3.0); // A new measurement can still arm after a failure.
    }
    else if (name == "success-once")
    {
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        UpscalerTimeVk::ReadUpscalingTime(device);
        Check(polls == 1, "one poll after success");
        Published(3.0);
    }
    else if (name == "invalid-duration")
    {
        // Preserve the existing elapsed-duration filter; do not add timestamp wrap semantics.
        for (const auto pair : { std::array<uint64_t, 2> { 100, 100 }, { 200, 100 }, { 0, 2500000000ULL } })
        {
            Record();
            replies.push_back({ VK_SUCCESS, pair });
            UpscalerTimeVk::ReadUpscalingTime(device);
            Unchanged();
            UpscalerTimeVk::ReadUpscalingTime(device); // Invalid completed sample is consumed too.
        }
        Check(polls == 3, "completed invalid durations are not retried");
    }
    else if (name == "record-again")
    {
        replies.push_back({ VK_NOT_READY, { 100, 1000100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        Unchanged();
        Record();
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        UpscalerTimeVk::ReadUpscalingTime(device);
        UpscalerTimeVk::ReadUpscalingTime(device);
        Check(recorded == std::vector<unsigned> { 0, 1, 2, 0, 1, 2 } && polls == 2, "reused pair can be polled");
        Published(3.0); // This says nothing about which GPU recording produced the result.
    }
    else
        Check(false, "unknown case");
    Check(replies.empty(), "all scripted replies consumed");
}
