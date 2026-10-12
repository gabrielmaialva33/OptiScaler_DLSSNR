static void Record(GpuTime_Vk& timer)
{
    timer.Start(commandBuffer);
    timer.End(commandBuffer);
}

// The ring starts at pair 0 and Start advances before recording, so the first three recordings land on
// pairs 1, 2, 0, and a read looks at the pair after the current one: the oldest. One recording is
// therefore read two recordings later, and the first poll after three recordings targets pair 1.
static void PrimeOldestPair(GpuTime_Vk& timer)
{
    Record(timer);
    Check(!timer.ReadGpuTime().has_value() && polled.empty(), "after one recording the oldest pair is not armed");
    Record(timer);
    Check(!timer.ReadGpuTime().has_value() && polled.empty(), "after two recordings the oldest pair is not armed");
    Record(timer);
}

static void Near(std::optional<double> value, double expected, const char* message)
{
    Check(value.has_value() && std::abs(value.value() - expected) < 1e-9, message);
}

int main(int argc, char** argv)
{
    Check(argc == 2, "one case per process (fresh fake state)");
    const std::string_view name = argv[1];

    if (name == "inactive")
    {
        // No device: no pool, no recording, no polling.
        {
            GpuTime_Vk timer(VK_NULL_HANDLE, physicalDevice);
            for (int i = 0; i < 4; ++i)
                Record(timer);
            Check(!timer.ReadGpuTime().has_value(), "null device publishes nothing");
        }
        // A failed create keeps nothing, not even the handle the driver wrote.
        createFails = true;
        {
            GpuTime_Vk timer(device, physicalDevice);
            for (int i = 0; i < 4; ++i)
                Record(timer);
            Check(!timer.ReadGpuTime().has_value(), "failed create publishes nothing");
        }
        Check(creates == 1 && destroys == 0, "a pool that was never created is never destroyed");
        // The scoped helper does nothing without a timer or a command buffer.
        createFails = false;
        GpuTime_Vk timer(device, physicalDevice);
        {
            ScopedGpuTime_Vk noTimer(nullptr, commandBuffer);
            ScopedGpuTime_Vk noList(&timer, 0);
        }
        Check(recorded.empty() && polled.empty(), "inactive timers record and poll nothing");
        return 0;
    }

    period = 2.0f;
    GpuTime_Vk timer(device, physicalDevice);

    if (name == "start-alone")
    {
        // Start without End records the reset and the start, and never arms a read.
        for (int i = 0; i < 4; ++i)
            timer.Start(commandBuffer);
        Check(!timer.ReadGpuTime().has_value() && polled.empty(), "start alone does not arm a sample");
        Check(recorded.size() == 8 && recorded[0] == Command { 'R', 2 } && recorded[1] == Command { 'T', 2 },
              "reset then start timestamp of the advanced pair");
    }
    else if (name == "ring")
    {
        {
            ScopedGpuTime_Vk scoped(&timer, commandBuffer); // the form the shaders use
        }
        Record(timer);
        Record(timer);
        Check(recorded == std::vector<Command> { { 'R', 2 }, { 'T', 2 }, { 'T', 3 }, { 'R', 4 }, { 'T', 4 },
                                                 { 'T', 5 }, { 'R', 0 }, { 'T', 0 }, { 'T', 1 } },
              "per pair: reset, start, end; pairs advance 1, 2, 0");
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        Near(timer.ReadGpuTime(), 3.0, "the oldest pair is read, scaled by timestampPeriod");
        Check(polled == std::vector<uint32_t> { 2 }, "the first poll targets pair 1");
    }
    else if (name == "not-ready-retry")
    {
        PrimeOldestPair(timer);
        // Plausible words on NOT_READY: deterministic even against a guard-less read.
        replies.push_back({ VK_NOT_READY, { 100, 1000100 } });
        Check(!timer.ReadGpuTime().has_value(), "NOT_READY publishes nothing");
        // Unavailable queries may leave some or all destination words untouched.
        replies.push_back({ VK_NOT_READY, { 200, 0 }, 1 });
        replies.push_back({ VK_NOT_READY, { 0, 0 }, 0 });
        Check(!timer.ReadGpuTime().has_value() && !timer.ReadGpuTime().has_value(),
              "partial or no writes publish nothing");
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        Near(timer.ReadGpuTime(), 3.0, "the retained pair publishes once it is ready");
        Check(polled == std::vector<uint32_t> { 2, 2, 2, 2 }, "the same pair is polled until ready");
        Check(!timer.ReadGpuTime().has_value() && polled.size() == 4, "a published pair is consumed");
    }
    else if (name == "errors")
    {
        PrimeOldestPair(timer);
        for (const auto error :
             { VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN })
        {
            replies.push_back({ error, { 100, 1000100 } });
            Check(!timer.ReadGpuTime().has_value(), "a query error publishes nothing");
        }
        // Upstream keeps the pair armed on any non-success: it is read again until it succeeds or the ring
        // records over it.
        Check(polled.size() == 4, "an errored pair stays armed");
        replies.push_back({ VK_SUCCESS, { 100, 1500100 } });
        Near(timer.ReadGpuTime(), 3.0, "a later success still publishes");
    }
    else if (name == "reversed")
    {
        PrimeOldestPair(timer);
        replies.push_back({ VK_SUCCESS, { 200, 100 } });
        Check(!timer.ReadGpuTime().has_value(), "an end before its start publishes nothing");
        Check(!timer.ReadGpuTime().has_value() && polled.size() == 1, "and is consumed, not retried");
    }
    else if (name == "lifetime")
    {
        // Destroyed with the timer, once, except at process shutdown, where it is abandoned.
        {
            GpuTime_Vk other(device, physicalDevice);
        }
        Check(destroys == 1, "destroyed with its timer");
        State::Instance().isShuttingDown = true;
        {
            GpuTime_Vk other(device, physicalDevice);
        }
        Check(destroys == 1, "abandoned at shutdown");
        State::Instance().isShuttingDown = false;
    }
    else
        Check(false, "unknown case");

    Check(replies.empty(), "all scripted replies consumed");
}
