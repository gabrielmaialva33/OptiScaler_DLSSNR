static int failures = 0;
void check(bool passed, const char* message)
{
    if (!passed)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

void resizeCases(const std::function<void()>& resize)
{
    auto& state = State::Instance();
    auto& enabled = Config::Instance()->FGUseMutexForSwapchain.value;
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        FG first, second;
        state.currentFG = &first;
        state.activeFgOutput = FGOutput::FSRFG;
        enabled = scenario != 2;
        if (scenario == 1)
            second.Mutex.lock(3);
        if (scenario == 2)
            first.Mutex.lock(3);
        operation = [&]
        {
            check(first.Mutex.getOwner() == 3, "resize keeps original FG locked throughout device work");
            if (scenario == 0)
                enabled = false;
            if (scenario == 1)
                state.currentFG = &second;
            if (scenario == 2)
                enabled = true;
            if (scenario == 3)
                state.currentFG = nullptr;
            if (scenario == 4)
                state.activeFgOutput = FGOutput::DLSSG;
            if (scenario == 5)
                throw std::runtime_error("resize failure");
            return -1;
        };
        try
        {
            resize();
        }
        catch (const std::runtime_error&)
        {
        }
        check(first.Mutex.getOwner() == (scenario == 2 ? 3u : 0u),
              "resize releases only its acquired mutex after config/FG/output/exception change");
        check(second.Mutex.getOwner() == (scenario == 1 ? 3u : 0u), "resize must leave replacement FG mutex untouched");
        if (second.Mutex.getOwner() == 3)
            second.Mutex.unlockThis(3);
        // Permit independent regression cases to continue on the unfixed baseline.
        if (first.Mutex.getOwner() == 3)
            first.Mutex.unlockThis(3);
    }
    state.currentFG = nullptr;
    enabled = true;
    operation = [] { return 0; };
    resize();
}

template <typename T> void releaseCases()
{
    auto& enabled = Config::Instance()->FGUseMutexForSwapchain.value;
    for (int scenario = 0; scenario < 4; ++scenario)
    {
        T fg;
        enabled = scenario != 1;
        if (scenario == 1)
            fg.Mutex.lock(1);
        cleanup = [&]
        {
            check(fg.Mutex.getOwner() == 1, "release keeps FG locked throughout cleanup");
            if (scenario == 0)
                enabled = false;
            if (scenario == 1)
                enabled = true;
            if (scenario == 2)
            {
                check(fg.ReleaseSwapchain(fg._hwnd), "recursive release is acknowledged");
                check(fg.Mutex.getOwner() == 1, "recursive release must leave caller's mutex held");
            }
            if (scenario == 3)
                throw std::runtime_error("cleanup failure");
        };
        try
        {
            check(fg.ReleaseSwapchain(fg._hwnd), "matching release succeeds");
        }
        catch (const std::runtime_error&)
        {
        }
        check(fg.Mutex.getOwner() == (scenario == 1 ? 1u : 0u),
              "release pairs only its acquired mutex after config change or exception");
        if (fg.Mutex.getOwner() == 1)
            fg.Mutex.unlockThis(1);
    }
    T fg;
    enabled = true;
    cleanup = [] { assert(false); };
    check(!fg.ReleaseSwapchain(nullptr), "foreign window must be refused without cleanup");
    check(fg.Mutex.getOwner() == 0, "foreign window does not acquire a mutex");
}

int main(int argc, char** argv)
{
    assert(argc == 2);
    if (std::strcmp(argv[1], "resize") == 0)
    {
        resizeCases([] { ResizeBuffers(); });
        IUnknown* queues[] = { nullptr };
        resizeCases([&] { ResizeBuffers1(queues); });
        State::Instance().currentFG = nullptr;
        ResizeBuffers1(nullptr);
        check(_device == nullptr, "missing present queues leave device untouched");
    }
    if (std::strcmp(argv[1], "release") == 0)
    {
        releaseCases<FSRFG_Dx12>();
        releaseCases<DLSSG_Dx12>();
        releaseCases<XeFG_Dx12>();
    }
    if (std::strcmp(argv[1], "dispatch") == 0)
    {
        FSRFG_Dx12 fg;
        Config::Instance()->FGUseMutexForSwapchain.value = true;
        fg.Mutex.lock(1);
        check(fg.DispatchTail(), "successful dispatch remains successful");
        check(fg.Mutex.getOwner() == 1, "dispatch must not unlock swapchain release's mutex");
        if (fg.Mutex.getOwner() == 1)
            fg.Mutex.unlockThis(1);
    }
    if (failures)
        return 1;
    std::puts("FG lifecycle lock transactions passed");
}
