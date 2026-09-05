int main()
{
    Config defaults;
    auto settings = defaults.GetDlssNrGpuTimingSettings();
    assert(!settings.Enabled && settings.Interval == 30);
    CSimpleIniA blank;
    defaults.Read(blank);
    assert(!defaults.GetDlssNrGpuTimingSettings().Enabled);
    defaults.Save(blank);
    assert(std::string(blank.GetValue("DlssNr", "GpuTiming")) == "auto");
    assert(std::string(blank.GetValue("DlssNr", "GpuTimingInterval")) == "auto");

    struct IntervalCase
    {
        const char* input;
        uint32_t expected;
    };
    const IntervalCase intervals[] = {
        { "auto", 30 },       { "", 30 },         { "0", 1 },         { "1", 1 },
        { "30", 30 },         { "10000", 10000 }, { "10001", 10000 }, { "4294967295", 10000 },
        { "4294967296", 30 }, { "-1", 30 },       { "+-1", 30 },      { "30x", 30 },
        { "1.5", 30 },        { "NaN", 30 },      { "+10", 10 },      { "0x20", 32 },
    };
    for (const auto& test : intervals)
    {
        Config first;
        CSimpleIniA input;
        input.SetValue("DlssNr", "GpuTiming", "true");
        input.SetValue("DlssNr", "GpuTimingInterval", test.input);
        first.Read(input);
        const auto before = first.GetDlssNrGpuTimingSettings();
        assert(before.Enabled && before.Interval == test.expected);
        first.Save(input);
        std::string encoded;
        assert(input.Save(encoded) >= 0);
        CSimpleIniA decoded;
        assert(decoded.LoadData(encoded) >= 0);
        Config second;
        second.Read(decoded);
        const auto after = second.GetDlssNrGpuTimingSettings();
        assert(after.Enabled == before.Enabled && after.Interval == before.Interval);
    }

    for (const char* value : { "auto", "garbage", "1", "false", "FALSE", "truex" })
    {
        Config config;
        CSimpleIniA input;
        input.SetValue("DlssNr", "GpuTiming", value);
        config.Read(input);
        assert(!config.GetDlssNrGpuTimingSettings().Enabled);
    }

    Config explicitValue;
    explicitValue.SetDlssNrGpuTimingEnabled(true);
    explicitValue.SetDlssNrGpuTimingInterval(17);
    CSimpleIniA onDisk;
    explicitValue.Save(onDisk);
    assert(onDisk.SaveFile("timing.ini") >= 0);
    CSimpleIniA fromDisk;
    assert(fromDisk.LoadFile("timing.ini") >= 0);
    Config restored;
    restored.Read(fromDisk);
    assert(restored.GetDlssNrGpuTimingSettings().Enabled);
    assert(restored.GetDlssNrGpuTimingSettings().Interval == 17);
    restored.SetDlssNrGpuTimingEnabled(false);
    restored.Save(fromDisk);
    Config disabled;
    disabled.Read(fromDisk);
    assert(!disabled.GetDlssNrGpuTimingSettings().Enabled);
    assert(disabled.GetDlssNrGpuTimingSettings().Interval == 17);

    // Existing CustomOptional semantics preserve a runtime edit across Reload.
    restored.SetDlssNrGpuTimingInterval(53);
    restored.Read(fromDisk);
    assert(restored.GetDlssNrGpuTimingSettings().Interval == 53);
    restored.SetDlssNrGpuTimingInterval(0);
    assert(restored.GetDlssNrGpuTimingSettings().Interval == 1);
    restored.SetDlssNrGpuTimingInterval(UINT32_MAX);
    assert(restored.GetDlssNrGpuTimingSettings().Interval == 10000);

    Config concurrent;
    std::thread writer(
        [&]
        {
            for (uint32_t i = 0; i != 5000; ++i)
            {
                concurrent.SetDlssNrGpuTimingEnabled((i & 1) != 0);
                concurrent.SetDlssNrGpuTimingInterval(i * 10);
            }
        });
    for (unsigned i = 0; i != 5000; ++i)
    {
        const auto current = concurrent.GetDlssNrGpuTimingSettings();
        assert(current.Interval >= 1 && current.Interval <= 10000);
        CSimpleIniA serialized;
        concurrent.Save(serialized);
        Config copy;
        copy.Read(serialized);
        assert(copy.GetDlssNrGpuTimingSettings().Interval >= 1);
    }
    writer.join();
    std::cout << "PASS: defaults, 16 interval round-trips, strict booleans, disk persistence, disable retention, "
                 "runtime precedence and 5000 concurrent snapshots/saves\n";
}
