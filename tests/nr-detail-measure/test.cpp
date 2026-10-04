#include <dlssnr/DlssNr_DetailStats.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace DlssNr::DetailStats;

void TestReduceGridEmpty()
{
    std::vector<float> grid(kGridRowFloats * kGridTiles, 0.0f);
    // When pixels measured (column b[3]) is 0, ReduceGrid should report NaN
    const auto stats = ReduceGrid(grid.data());
    assert(std::isnan(stats.detailBand));
    assert(!Finite(stats));
}

void TestReduceGridUniform()
{
    std::vector<float> grid(kGridRowFloats * kGridTiles, 0.0f);

    for (unsigned y = 0; y < kGridTiles; ++y)
    {
        float* row = grid.data() + y * kGridRowFloats;
        for (unsigned x = 0; x < kGridTiles; ++x)
        {
            float* a = row + x * 4;
            float* b = row + (x + kGridTiles) * 4;
            float* c = row + (x + 2 * kGridTiles) * 4;
            float* d = row + (x + 3 * kGridTiles) * 4;

            a[0] = 0.10f;  // raw
            a[1] = 0.05f;  // detailBand
            a[2] = 0.02f;  // inputBand
            a[3] = 0.001f; // outputChange

            b[0] = 0.0005f; // inputChange
            b[1] = 0.0f;    // shoulder
            b[2] = 0.0f;    // floor
            b[3] = 16.0f;   // pixels measured in tile

            c[0] = 0.15f;  // chromaIn
            c[1] = 0.18f;  // chromaOut
            c[2] = 0.03f;  // colourShift
            c[3] = 0.002f; // warmth

            d[0] = 0.20f; // shadowShare
            d[1] = 0.05f; // shadowIn
            d[2] = 0.04f; // shadowOut
            d[3] = 0.01f; // crushed
        }
    }

    const auto stats = ReduceGrid(grid.data());
    assert(Finite(stats));
    assert(std::fabs(stats.detailRaw - 0.10f) < 1e-5f);
    assert(std::fabs(stats.detailBand - 0.05f) < 1e-5f);
    assert(std::fabs(stats.inputBand - 0.02f) < 1e-5f);
    assert(std::fabs(stats.AddedBand() - 0.03f) < 1e-5f);
    assert(std::fabs(stats.Flicker() - 0.0005f) < 1e-5f);
    assert(std::fabs(stats.Saturation() - (0.18f / 0.15f - 1.0f)) < 1e-4f);
    assert(std::fabs(stats.ShadowDarkening() - (1.0f - 0.04f / 0.05f)) < 1e-4f);
}

void TestMeasurementAggregation()
{
    Accumulator acc;
    for (int i = 0; i < 60; ++i)
    {
        Stats s {};
        s.detailRaw = 0.12f;
        s.detailBand = 0.06f;
        s.inputBand = 0.04f;
        s.outputChange = 0.002f;
        s.inputChange = 0.001f;
        s.chromaIn = 0.20f;
        s.chromaOut = 0.22f;
        s.warmth = 0.003f;
        s.shadowIn = 0.10f;
        s.shadowOut = 0.09f;
        s.crushed = 0.001f;
        acc.Add(s);
    }

    const auto m1 = acc.Finish(1920, 1080);
    assert(m1.samples == 60);
    assert(std::fabs(m1.detailAdded - 0.02f) < 1e-5f);
    assert(std::fabs(m1.flicker - 0.001f) < 1e-5f);
    assert(m1.DetailWords().find("+50.0%") != std::string::npos);

    // Second measurement with more detail
    acc.Reset();
    for (int i = 0; i < 60; ++i)
    {
        Stats s {};
        s.detailRaw = 0.15f;
        s.detailBand = 0.07f;
        s.inputBand = 0.04f;
        s.outputChange = 0.0025f;
        s.inputChange = 0.001f;
        s.chromaIn = 0.20f;
        s.chromaOut = 0.24f;
        s.warmth = 0.004f;
        s.shadowIn = 0.10f;
        s.shadowOut = 0.08f;
        s.crushed = 0.002f;
        acc.Add(s);
    }
    const auto m2 = acc.Finish(1920, 1080);
    const auto cmp = m2.Compare(m1);
    assert(!cmp.empty());
    assert(cmp.find("detail +25.0%") != std::string::npos);
}

int main()
{
    TestReduceGridEmpty();
    TestReduceGridUniform();
    TestMeasurementAggregation();
    std::printf("PASS: detail stats reduction and measurement aggregation\n");
    return 0;
}
