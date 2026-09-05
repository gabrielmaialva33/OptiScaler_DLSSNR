#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>

static unsigned cases = 0;
#define CASE(name) do { ++cases; std::printf("PASS: %s\n", name); } while (false)

int main()
{
    using namespace DlssNr::PassConfig;
    assert(!UInt("-1", 3) && !UInt("4294967296", UINT32_MAX) && !UInt("1x", 3));
    assert(!UInt("+", 3) && !UInt("0x", 3) && !UInt("auto", 3) && !UInt(nullptr, 3));
    assert(UInt("+0x3", 3) == 3 && UInt("02", 3) == 2);
    CASE("strict unsigned parsing rejects malformed and overflowing inputs");
    for (const char* text : { "nan", "NaN", "inf", "-inf", "1e99", "1e-999", "1x", "+-1", "++1", "", "auto" })
        assert(!Float(text, -1.0f, 2.0f));
    assert(Float("+1.25", 0, 2) == 1.25f && Float("-1", -1, 2) == -1.0f);
    assert(!Float("-0.1", 0, 2) && !Float("2.01", 0, 2));
    CASE("finite bounded float parsing rejects NaN, infinity, underflow and trailing input");
    assert(Count("0") == 1 && Count("7") == 3 && Count("4294967295") == 3);
    assert(!Count("4294967296") && !Count("-1") && !Count("auto"));
    CASE("requested counts are bounded to one through three without integer wraparound");
    assert(Bool("TRUE") == true && Bool("false") == false && !Bool("1"));
    CASE("boolean values accept true/false only, independent of letter case");
    assert(Index("dlssnr.PASS01") == 0 && Index("DlssNr.Pass4294967295") == UINT32_MAX - 1);
    for (const char* text : { "DlssNr.Pass0", "DlssNr.Pass+1", "DlssNr.Pass0x1", "DlssNr.Pass4294967296", "DlssNr.Pass1suffix" })
        assert(!Index(text));
    CASE("one-based section names are strict and aliases map to the same index");

    CSimpleIniA ini;
    assert(ini.LoadData("[DlssNr.Pass01]\nIntensity=1\nPreset=bad\nAutoMask=false\n"
                        "[DlssNr.Pass1]\nIntensity=0.5\nPreset=0x3\nLocalTone=auto\n"
                        "[DlssNr.Pass3]\nLocalTone=1.23456788\nLocalStructure=0.6\nSkinStructure=-1\n"
                        "[DlssNr.Pass99]\nStyle=2\n"
                        "[Unrelated]\nKeep=original\n") >= 0);
    auto overrides = Load(ini);
    assert(overrides.size() == 3 && Get(overrides, 0).Intensity == 1.0f);
    assert(Get(overrides, 0).Preset == 3 && Get(overrides, 0).AutoMask == false);
    CASE("aliases merge first valid explicit value per key in file order");
    DlssNrResolvedPassSettings master;
    auto answer = Resolve(master, Get(overrides, 0), true);
    assert(answer.Intensity == 1.0f && answer.LocalTone == 1.0f && answer.Preset == 3);
    master.Intensity = 0.2f; master.LocalTone = 0.4f;
    answer = Resolve(master, Get(overrides, 0), true);
    assert(answer.Intensity == 1.0f && answer.LocalTone == 0.4f);
    CASE("explicit master-equal values remain explicit and absent fields inherit live master changes");
    assert(Resolve(master, Get(overrides, 0), false) == master && overrides.size() == 3);
    CASE("disabling individual settings ignores but retains overrides");
    for (uint32_t i = 0; i < 1000; ++i) (void)Get(overrides, i + 100);
    assert(overrides.size() == 3);
    CASE("render reads never insert sparse entries");

    Save(ini, overrides);
    assert(ini.GetValue("DlssNr.Pass01", "Intensity") == nullptr);
    assert(ini.GetValue("DlssNr.Pass1", "Intensity") != nullptr);
    assert(ini.GetValue("DlssNr.Pass1", "LocalTone") == nullptr);
    assert(std::string(ini.GetValue("Unrelated", "Keep")) == "original");
    CASE("saving canonicalizes aliases, omits inherited keys and preserves unrelated sections");
    std::string serialized;
    assert(ini.Save(serialized) >= 0);
    CSimpleIniA reloaded;
    assert(reloaded.LoadData(serialized.c_str()) >= 0);
    assert(Load(reloaded) == overrides);
    CASE("real SimpleIni serialization and reload preserve every explicit model key");
    assert(ini.SaveFile("roundtrip.ini") >= 0);
    CSimpleIniA fromFile;
    assert(fromFile.LoadFile("roundtrip.ini") >= 0 && Load(fromFile) == overrides);
    std::remove("roundtrip.ini");
    CASE("real SimpleIni disk save/load round-trips sparse inactive higher passes");
    const float precise = std::nextafter(1.0f, 2.0f);
    for (float number : { precise, 0.000000012345678f, -0.0f, -1.0f, 2.0f })
        assert(Float(FloatText(number).c_str(), -1, 2) == number);
    CASE("NR-only float serialization preserves float precision");

    auto first = Get(overrides, 0);
    first.Intensity.reset();
    Set(overrides, 0, first);
    Save(ini, overrides);
    assert(!ini.GetValue("DlssNr.Pass1", "Intensity"));
    assert(ini.GetValue("DlssNr.Pass1", "Preset"));
    Set(overrides, 0, {});
    Save(ini, overrides);
    assert(!ini.GetValue("DlssNr.Pass1", "Preset"));
    assert(Load(ini) == overrides);
    CASE("field and whole-pass removal delete stale serialized keys");
    DlssNrPassSettings invalid;
    invalid.Intensity = std::numeric_limits<float>::quiet_NaN();
    invalid.LocalTone = std::numeric_limits<float>::infinity();
    invalid.Preset = 4; invalid.Style = UINT32_MAX;
    Set(overrides, 12, invalid);
    assert(Get(overrides, 12) == DlssNrPassSettings {});
    CASE("UI mutation validation cannot persist nonfinite or unsupported tuning");
    Set(overrides, UINT32_MAX, first);
    assert(Get(overrides, UINT32_MAX) == DlssNrPassSettings {});
    CASE("unrepresentable one-based index cannot enter the store");

    Config config;
    config.SetDlssNrPassCount(9);
    config.SetDlssNrIndividualPassSettings(true);
    config.SetDlssNrPassOverrides(2, first);
    config.SetDlssNrPassCount(1);
    assert(config.GetDlssNrPassOverrides(2) == first);
    config.SetDlssNrPassCount(3);
    assert(config.GetDlssNrPassSettings(2).Preset == first.Preset);
    CASE("production Config setters preserve overrides when count decreases and increases");
    config.SetDlssNrMasterSetting(&Config::DlssNrLocalTone, 0.7f);
    assert(config.GetDlssNrPassSettings(2).LocalTone == 0.7f);
    config.SetDlssNrIndividualPassSettings(false);
    assert(config.GetDlssNrPassSettings(2) == config.GetDlssNrMasterSettings());
    CASE("production Config snapshots honor master edits and disabled inheritance");
    std::thread writer([&] {
        for (int i = 0; i < 5000; ++i)
        {
            config.SetDlssNrPassCount(uint32_t(i % 5));
            config.SetDlssNrIndividualPassSettings((i & 1) != 0);
            config.SetDlssNrPassOverrides(0, i & 1 ? first : DlssNrPassSettings {});
            config.SetDlssNrMasterSetting(&Config::DlssNrLocalTone, float(i & 1));
        }
    });
    for (int i = 0; i < 5000; ++i)
    {
        const auto snapshot = config.GetDlssNrPassSnapshot();
        assert(snapshot.Count >= 1 && snapshot.Count <= 3);
        if (!snapshot.Individual) assert(snapshot.Settings[0] == snapshot.Settings[2]);
        (void)config.GetDlssNrPassOverrides(0);
    }
    writer.join();
    config.ClearDlssNrPassOverrides(0);
    assert(config.GetDlssNrPassOverrides(0) == DlssNrPassSettings {});
    CASE("production Config snapshot and mutation methods survive 5000 concurrent transactions");
    std::printf("PASS: %u NR pass configuration cases\n", cases);
}
