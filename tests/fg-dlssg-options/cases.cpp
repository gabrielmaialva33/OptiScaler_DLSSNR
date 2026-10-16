static void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

static void unchanged()
{
    DLSSG_Common output;
    require(output.SetOptions(true), "first frame must dispatch");
    require(output.SetOptions(true), "unchanged frame must dispatch");
    require(StreamlineProxy::dlssgCalls.size() == 1, "identical successful DLSSG options were resent");
    require(StreamlineProxy::reflexCalls.size() == 1, "identical successful Reflex options were resent");
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eOn, "DLSSG was not enabled");
    require(StreamlineProxy::reflexCalls.back().mode == sl::ReflexMode::eLowLatency,
            "default Reflex mode must keep low latency enabled");
}

static void retry(bool dlssg, bool invalidateGeneration)
{
    DLSSG_Common output;
    if (invalidateGeneration)
    {
        output.SetOptions(true);
        if (dlssg)
            ++StreamlineHooks::dlssgGeneration;
        else
            ++StreamlineHooks::reflexGeneration;
    }
    auto& result = dlssg ? StreamlineProxy::dlssgResult : StreamlineProxy::reflexResult;
    result = sl::Result::eErrorNotInitialized;
    output.SetOptions(true);
    result = sl::Result::eOk;
    output.SetOptions(true);
    const size_t expected = invalidateGeneration ? 3 : 2;
    require((dlssg ? StreamlineProxy::dlssgCalls.size() : StreamlineProxy::reflexCalls.size()) == expected,
            dlssg ? "failed DLSSG options must retry on the next frame"
                  : "failed Reflex options must retry on the next frame");
    output.SetOptions(true);
    require((dlssg ? StreamlineProxy::dlssgCalls.size() : StreamlineProxy::reflexCalls.size()) == expected,
            "retry success must populate the cache");
    require((dlssg ? StreamlineProxy::reflexCalls.size() : StreamlineProxy::dlssgCalls.size()) == 1,
            "one API failing must not invalidate the other API's successful cache");
}

static void generations()
{
    DLSSG_Common output;
    output.SetOptions(true);
    ++StreamlineHooks::dlssgGeneration;
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 2, "game DLSSG override must force a resend");
    require(StreamlineProxy::reflexCalls.size() == 1, "DLSSG override must not resend Reflex");
    ++StreamlineHooks::reflexGeneration;
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 2, "Reflex override must not resend DLSSG");
    require(StreamlineProxy::reflexCalls.size() == 2, "game Reflex override must force a resend");
}

static void mfgTransition()
{
    DLSSG_Common output;
    output.SetOptions(true);
    Config::Instance()->FGDLSSGInterpolationCount = 3;
    require(!output.SetOptions(true), "MFG count change must skip one dispatch");
    require(StreamlineProxy::dlssgCalls.size() == 2, "MFG count change must send eOff immediately");
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eOff, "MFG transition omitted eOff");
    require(output.SetOptions(true), "MFG must resume on the next frame");
    require(StreamlineProxy::dlssgCalls.size() == 3, "MFG transition must resend eOn");
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eOn, "MFG did not resume eOn");
    require(StreamlineProxy::dlssgCalls.back().numFramesToGenerate == 3, "MFG resumed with the wrong count");
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 3, "successful MFG restart was not cached");
}

static void menu()
{
    DLSSG_Common output;
    output.SetOptions(true);
    MenuOverlayBase::visible = true;
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 2, "menu opening must resend DLSSG options");
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eOff, "menu opening must disable DLSSG");
    require(StreamlineProxy::dlssgCalls.back().flags & sl::DLSSGFlags::eRetainResourcesWhenOff,
            "menu must retain DLSSG resources while disabled");
    require(ReflexHooks::frameCount == 0, "menu interlock must clear interpolated frame telemetry");
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 2, "unchanged menu state must stay cached");
    MenuOverlayBase::visible = false;
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 3, "closing menu must resend DLSSG options");
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eOn, "closing menu must resume DLSSG");
    require(StreamlineProxy::dlssgCalls.back().flags == sl::DLSSGFlags {}, "closing menu must clear retain flag");
}

static void dynamicReflex()
{
    DLSSG_Common output;
    ReflexHooks::markers = true;
    Config::Instance()->FGDLSSGForceDMFG = true;
    Config::Instance()->FGDLSSGReflexBoost = true;
    output.SetOptions(false);
    require(StreamlineProxy::dlssgCalls.back().mode == sl::DLSSGMode::eDynamic, "DMFG mode was ignored");
    require(StreamlineProxy::dlssgCalls.back().dynamicTargetFrameRate == 120.0f, "DMFG target was ignored");
    require(StreamlineProxy::reflexCalls.back().mode == sl::ReflexMode::eLowLatencyWithBoost,
            "Reflex boost was ignored");
    require(!StreamlineProxy::reflexCalls.back().useMarkersToOptimize,
            "Vulkan must not optimize with markers that do not reach its Streamline");
    output.SetOptions(true);
    require(StreamlineProxy::reflexCalls.size() == 2 && StreamlineProxy::reflexCalls.back().useMarkersToOptimize,
            "D3D12 marker change must resend Reflex options");
    Config::Instance()->FGDLSSGFramerateTargetDMFG = 144.0f;
    output.SetOptions(true);
    require(StreamlineProxy::dlssgCalls.size() == 2 &&
                StreamlineProxy::dlssgCalls.back().dynamicTargetFrameRate == 144.0f,
            "changed DMFG target must resend options");
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "expected a case name");
        const std::string name = argv[1];
        if (name == "unchanged")
            unchanged();
        else if (name == "dlssg-retry")
            retry(true, false);
        else if (name == "reflex-retry")
            retry(false, false);
        else if (name == "generations")
            generations();
        else if (name == "dlssg-generation-retry")
            retry(true, true);
        else if (name == "reflex-generation-retry")
            retry(false, true);
        else if (name == "mfg-transition")
            mfgTransition();
        else if (name == "menu")
            menu();
        else if (name == "dynamic-reflex")
            dynamicReflex();
        else
            throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << (argc > 1 ? argv[1] : "unknown") << ": " << error.what() << '\n';
        return 1;
    }
}
