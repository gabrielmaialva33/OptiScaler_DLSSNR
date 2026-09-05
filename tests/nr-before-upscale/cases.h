#include <cstdio>
using namespace std::chrono;
using DlssNr::ScopedPreUpscale;

struct Fixture
{
    ID3D12Device device;
    ID3D12Resource color{{}, &device}, depth{{}, &device}, motion{{}, &device}, output{{}, &device}, exposure{{}, &device};
    ID3D12GraphicsCommandList cmd;
    NVSDK_NGX_Parameter params;
    Config& cfg = *Config::Instance();
    Fixture()
    {
        cfg = {};
        State::Instance().frameCount = 100;
        cfg.DlssNrEnabled = true;
        cfg.DlssNrStage = 1;
        params.typed = {{"Color", &color}, {"Depth", &depth}, {"Motion", &motion}, {"Output", &output}};
        params.untyped["Exposure"] = &exposure;
        g_pre = {};
        g_preDevice = nullptr;
        g_preAllocationFailed = false;
        g_preExtent.Reset();
        g_compose.reset();
        DlssNr_Dx12::calls = 0;
        DlssNr_Dx12::writes = true;
        Stable();
    }
    void Stable()
    {
        g_preExtent.Observe(1280, 720, steady_clock::now() - milliseconds(501), color.desc.Format);
    }
    void Restored()
    {
        assert(params.typed["Color"] == &color);
        assert(color.state == cfg.ColorResourceBarrier.value_or(1));
        assert(depth.state == cfg.DepthResourceBarrier.value_or(1));
        assert(motion.state == cfg.MVResourceBarrier.value_or(1));
        assert(exposure.state == cfg.ExposureResourceBarrier.value_or(1));
        if (g_pre.scratch) assert(g_pre.scratch->state == 2);
    }
};

int main()
{
    unsigned checks = 0;
    for (bool enabled : {false, true})
        for (unsigned stage : {0u, 1u})
        {
            Fixture f; f.cfg.DlssNrEnabled = enabled; f.cfg.DlssNrStage = stage;
            { ScopedPreUpscale p(&f.cmd, &f.params, true); assert(p.Swapped() == (enabled && stage == 1)); }
            if (!enabled || stage == 0) assert(f.params.gets == 0 && f.params.sets == 0 && f.device.allocations == 0);
            f.Restored(); ++checks;
        }
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        Fixture f;
        if (mode == 1) { f.params.typed.erase("Color"); f.params.untyped["Color"] = &f.color; }
        if (mode == 2) f.params.untyped["Color"] = &f.color;
        if (mode == 3) { f.params.typed["Color"] = nullptr; f.params.untyped["Color"] = &f.color; }
        auto typed = f.params.typed; auto untyped = f.params.untyped;
        try
        {
            ScopedPreUpscale p(&f.cmd, &f.params, true);
            assert(p.Swapped());
            if (typed.count("Color")) assert(f.params.typed["Color"] == g_pre.scratch);
            if (untyped.count("Color")) assert(f.params.untyped["Color"] == g_pre.scratch);
            throw 1; // The caller failing must not leave its block referring to our scratch.
        }
        catch (int) {}
        assert(f.params.typed == typed && f.params.untyped == untyped);
        assert(g_pre.scratch->state == 2); ++checks;
    }
    { Fixture f; f.params.untyped["Color"] = &f.output;
      ScopedPreUpscale p(&f.cmd, &f.params, true); assert(!p.Swapped() && f.device.allocations == 0); ++checks; }
    for (bool wrote : {false, true})
    {
        Fixture f; DlssNr_Dx12::writes = wrote;
        bool declined;
        { ScopedPreUpscale p(&f.cmd, &f.params, true); assert(p.Swapped() == wrote); declined = p.Declined(); }
        DlssNr::EvaluateAfterUpscale(&f.cmd, &f.params, nullptr, declined);
        assert(DlssNr_Dx12::calls == 1); f.Restored(); ++checks;
    }
    { Fixture f; bool declined;
      { ScopedPreUpscale p(&f.cmd, &f.params, false); assert(!p.Swapped()); declined = p.Declined(); }
      DlssNr::EvaluateAfterUpscale(&f.cmd, &f.params, nullptr, declined);
      assert(declined && DlssNr_Dx12::calls == 1 && f.device.allocations == 0); ++checks; }
    { Fixture f; f.cfg.ColorResourceBarrier = 4; f.color.state = 4; f.color.desc.Flags = 2;
      f.cfg.DepthResourceBarrier = 16; f.depth.state = 16;
      f.cfg.MVResourceBarrier = 32; f.motion.state = 32;
      f.cfg.ExposureResourceBarrier = 8; f.exposure.state = 8;
      { ScopedPreUpscale p(&f.cmd, &f.params, true); assert(p.Swapped()); assert(g_pre.scratch->state == 4); }
      f.Restored(); ++checks; }
    { Fixture f; ScopedPreUpscale outer(&f.cmd, &f.params, true);
      auto scratch = f.params.typed["Color"];
      { ScopedPreUpscale nested(&f.cmd, &f.params, true); assert(!nested.Swapped() && !nested.Declined()); }
      assert(f.params.typed["Color"] == scratch && DlssNr_Dx12::calls == 1); ++checks; }
    for (unsigned invalid = 0; invalid < 11; ++invalid)
    {
        Fixture f;
        switch (invalid)
        {
        case 0: f.color.desc.Format = 10; break;
        case 1: f.color.desc.SampleDesc.Count = 4; break;
        case 2: f.color.desc.DepthOrArraySize = 2; break;
        case 3: f.params.numbers["X"] = 1; break;
        case 4: f.params.numbers = {{"Width", 2000}, {"Height", 720}}; break;
        case 5: f.params.numbers["Width"] = 1280; break;
        case 6: f.color.desc.Format = 99; break;
        case 7: f.color.desc.Flags = 4; break;
        case 8: f.cfg.DlssNrHoldFrame = true; break;
        case 9: f.cfg.DlssNrUseProxy = true; break;
        case 10: f.device.formatSupported = false; break;
        }
        ScopedPreUpscale p(&f.cmd, &f.params, true);
        assert(!p.Swapped() && p.Declined() && f.device.allocations == 0 && f.cmd.barriers == 0); ++checks;
    }
    { Fixture f; f.device.allocationFails = true;
      for (int i = 0; i < 1000; ++i) { ScopedPreUpscale p(&f.cmd, &f.params, true); assert(!p.Swapped()); }
      assert(f.device.allocations == 1 && f.params.sets == 0); ++checks; }
    { Fixture f; ID3D12Device other; g_preDevice = &other;
      ScopedPreUpscale p(&f.cmd, &f.params, true); assert(!p.Swapped() && !p.Declined() && f.device.allocations == 0); ++checks; }
    { Fixture f; g_preExtent.Reset();
      for (unsigned i = 0; i < 1000; ++i)
      { f.params.numbers = {{"Width", 1000 + i % 2}, {"Height", 600}};
        ScopedPreUpscale p(&f.cmd, &f.params, true); assert(!p.Swapped() && !p.Declined()); }
      assert(f.device.allocations == 0 && DlssNr_Dx12::calls == 0); ++checks; }
    { DlssNr::Detail::StableExtent e; auto t = steady_clock::now();
      assert(!e.Observe(1280, 720, t)); assert(!e.Observe(1280, 720, t + milliseconds(499)));
      assert(e.Observe(1280, 720, t + milliseconds(500)));
      assert(!e.Observe(1280, 720, t + milliseconds(501), 1));
      assert(!e.Observe(1920, 1080, t + milliseconds(502), 1)); ++checks; }
    { Fixture f; State::Instance().frameCount = 0;
      ScopedPreUpscale p(&f.cmd, &f.params, true);
      assert(!p.Swapped() && p.Declined() && f.device.allocations == 0); ++checks; }
    { DlssNr::Detail::CreationFrameGate gate; gate.Created(100);
      for (int i = 0; i < 1000; ++i) assert(!gate.Ready(100));
      assert(!gate.Ready(0)); assert(!gate.Ready(99)); assert(gate.Ready(101));
      gate.Created(101); assert(!gate.Ready(101)); assert(gate.Ready(102)); ++checks; }
    assert(checks >= 25 && "ZERO COVERAGE");
    std::printf("PASS: %u boundary cases; allocation-failure and DRS loops: 1000 each\n", checks);
}
